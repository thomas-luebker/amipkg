/*
 * main.c - amipkg, the package-manager CLI for AmigaOS 3.x.
 *
 * Runs on ANY 3.x system (the standalone dist/ bundle installs it) and on
 * every Amiga-Imager-built image. Full docs: dist/ReadMe; roadmap:
 * docs/agent/amipkg-roadmap.md (AmigaImager repo).
 *
 * Commands: update (fetch + on-device-Ed25519-verify the catalog) | list |
 * avail | check | doctor | info | fetch | install [DRYRUN] (dependency-
 * resolved, CPU-variant-aware, disk-space preflight, CPU/KS floors) |
 * upgrade (incl. self-update) | dir | verify | remove (receipt-driven:
 * digest-checked deletes, User-Startup overlay stripping, recorded
 * removeScript).
 *
 * TRUST: the catalog is Ed25519-signed (verified on-device against the baked
 * public key); every archive download must match the SHA-256 pinned in that
 * signed catalog. The transport (plain HTTP, or https via optional AmiSSL)
 * is untrusted by design.
 *
 * A 128 KB stack is guaranteed by StackSwap in main() - the launching
 * Shell's stack setting cannot matter.
 */

#include "../core/sha256.h"
#include "../core/aver.h"
#include "../core/receipts.h"
#include "../core/aindex.h"
#include "../core/resolve.h"
#include "../core/arecipe.h"
#include "../core/arun.h"
#include "../core/ajson.h"
#include "../core/store.h"   /* read_file, sha256_of_file, load_installed, load_files_for, paths */
#include "../core/averify.h" /* on-device Ed25519 verify of the fetched index */
#include "../core/arepo.h"   /* the repository list (multi-repo) */
#include "../core/adfread.h" /* extract packages distributed as a raw .adf image */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __amigaos__
/* (Stack is guaranteed by a StackSwap in main() - the newlib crt0 ignores the
 * libnix-style `__stack` global, so we do it ourselves. See main().) */

/* AmigaOS Version-command tag: `Version C:amipkg` reports the exact build. */
static const char verstag[] __attribute__((used)) = "$VER: amipkg " AMIPKG_VERSION " (" AMIPKG_VERDATE ")";

int http_available(void);
void http_cleanup(void);
int http_get(const char *url, FILE *out, long *bytes_out);
int http_get_file(const char *url, const char *path, long *bytes_out);
int http_post_json(const char *url, const char *body, char *resp, size_t respsize);
int amipkg_extract(const char *archive, const char *destdir);
size_t amipkg_run_recipe(const arecipe *recipe, const char *extract_dir,
                         const char *boot_root, char (*out_paths)[256], size_t max);
size_t amipkg_install_generic(const char *extract_dir, const char *dest_root,
                              char (*out_paths)[256], size_t max);
size_t amipkg_install_generic_recorded(const char *extract_dir, const char *dest_root,
                                       const char *id, char (*out_paths)[256], size_t max);
int amipkg_extract_single_top_dir(const char *extract_dir);
int amipkg_extract_nonempty(const char *extract_dir);
void amipkg_selfupdate_mirror(char (*paths)[256], size_t n);
void amipkg_ensure_dirs(void);
void amipkg_make_dir(const char *path);       /* create one drawer (multi-repo) */
long long amipkg_volume_free(const char *path);   /* -1 = unknown (don't block) */
int amipkg_rename(const char *from, const char *to);   /* dos Rename; 0 = ok */
long amipkg_run_inline_script(const char *script, const char *label);
int amipkg_strip_overlay(const char *script_path, const char *marker);
long amipkg_adopt_inventory(const char *id, const char *drawer);
int  amipkg_detect_version(const char *drawer, const char *id, char *out, size_t n);
size_t amipkg_install_generic_routed(const char *extract_dir, const char *sysdest,
                                     const char *companion, const char *id,
                                     char (*out_paths)[256], size_t max,
                                     size_t *n_sys, size_t *n_comp);
const char *amipkg_cpu(void);                     /* "68000".."68060" via AttnFlags */
int amipkg_ks_version(void);                      /* exec lib_Version (39 = KS 3.0) */

/* argv workaround for a bebbo m68k-amigaos-gcc newlib crt0 bug.
 *
 * newlib's crt0.c declares `char *__argv[] = {0, 0}` (an 8-byte ARRAY) and
 * calls main(__argc, &__argv[0]); the __nocommandline parser it links against
 * declares `extern char **__argv` and does `__argv = calloc(...)`, which
 * writes the fresh array's pointer INTO __argv[0] instead of repointing argv.
 * Net result on the Amiga: argc is correct, argv[0] is garbage and argv[1] is
 * always NULL - every subcommand reads as `(null)`. (libnix's crt0 declares
 * ___argv as a 4-byte pointer, so the same parser is correct there; the newlib
 * port regressed the type.) We sidestep it entirely by rebuilding argv from
 * the raw DOS argument string, which crt0 does capture correctly. */
#include <proto/dos.h>   /* GetArgStr */

static char *g_amiga_argv[64];
static char  g_amiga_argbuf[1024];

static int amiga_rebuild_argv(char ***argv_out)
{
    const char *src = (const char *)GetArgStr(); /* args AFTER the command */
    char *dst = g_amiga_argbuf;
    char *end = g_amiga_argbuf + sizeof g_amiga_argbuf - 1;
    int argc = 0;
    g_amiga_argv[argc++] = "amipkg";             /* synthesize argv[0] */
    while (src && *src && argc < 63) {
        char quote = 0;
        while (*src == ' ' || *src == '\t' || *src == '\n' || *src == '\r') src++;
        if (!*src) break;
        g_amiga_argv[argc++] = dst;
        if (*src == '"') { quote = '"'; src++; }
        while (*src && dst < end) {
            if (quote) { if (*src == quote) { src++; break; } }
            else if (*src == ' ' || *src == '\t' || *src == '\n' || *src == '\r') break;
            *dst++ = *src++;
        }
        *dst++ = '\0';
    }
    g_amiga_argv[argc] = 0;
    *argv_out = g_amiga_argv;
    return argc;
}
#else
/* Host stubs so the CLI logic compiles/tests off-Amiga (no net, no DOS). */
static int http_get(const char *url, FILE *out, long *bytes_out)
{ (void)url; (void)out; (void)bytes_out;
  printf("amipkg: networking is Amiga-only in the host build\n"); return 1; }
static int http_get_file(const char *u, const char *p, long *b)
{ (void)u; (void)p; (void)b;
  printf("amipkg: networking is Amiga-only in the host build\n"); return 1; }
static int http_post_json(const char *u, const char *b, char *r, size_t rs)
{ (void)u; (void)b; (void)r; (void)rs;
  printf("amipkg: networking is Amiga-only in the host build\n"); return 1; }
static void http_cleanup(void) {}
static int amipkg_extract(const char *a, const char *d)
{ (void)a; (void)d; return 0; }
static size_t amipkg_run_recipe(const arecipe *rc, const char *e, const char *b,
                                char (*o)[256], size_t m)
{ (void)rc; (void)e; (void)b; (void)o; (void)m;
  printf("amipkg: install is Amiga-only in the host build\n"); return 0; }
/* Superseded on the Amiga side by the _recorded variant; kept as a stub so the
 * two builds declare the same surface. Unreferenced in the host build, hence
 * the attribute (the suite is -Werror). */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static size_t amipkg_install_generic(const char *e, const char *d, char (*o)[256], size_t m)
{ (void)e; (void)d; (void)o; (void)m; return 0; }
static size_t amipkg_install_generic_recorded(const char *e, const char *d, const char *i,
                                              char (*o)[256], size_t m)
{ (void)e; (void)d; (void)i; (void)o; (void)m; return 0; }
static int amipkg_extract_single_top_dir(const char *e) { (void)e; return 1; }
static int amipkg_extract_nonempty(const char *e) { (void)e; return 1; }
static void amipkg_selfupdate_mirror(char (*p)[256], size_t n) { (void)p; (void)n; }
static void amipkg_ensure_dirs(void) {}
static long long amipkg_volume_free(const char *p) { (void)p; return -1; }
static int amipkg_rename(const char *f, const char *t) { return rename(f, t); }
static long amipkg_run_inline_script(const char *sc, const char *l)
{ (void)sc; (void)l; printf("amipkg: scripts are Amiga-only in the host build\n"); return 0; }
static int amipkg_strip_overlay(const char *p, const char *m)
{ (void)p; (void)m; return 0; }
static long amipkg_adopt_inventory(const char *i, const char *d)
{ (void)i; (void)d; return -1; }
static int amipkg_detect_version(const char *d, const char *i, char *o, size_t n)
{ (void)d; (void)i; if (n) o[0] = 0; return 0; }
static size_t amipkg_install_generic_routed(const char *e, const char *sd, const char *c,
                                            const char *i, char (*o)[256], size_t m,
                                            size_t *ns, size_t *nc)
{ (void)e; (void)sd; (void)c; (void)i; (void)o; (void)m;
  if (ns) *ns = 0; if (nc) *nc = 0; return 0; }
static const char *amipkg_cpu(void) { return ""; }
static int amipkg_ks_version(void) { return 0; }
/* adf_extract (portable) is linked from adfread.c; provide its mkdir hook. */
#include <sys/stat.h>
int adf_mkdir(const char *path) { mkdir(path, 0777); return 0; }
static void amipkg_make_dir(const char *p) { mkdir(p, 0777); }
#endif

/* Find the packages[] entry object with `id` in a parsed index tree. */
static const aj_node *find_entry_obj_by_id(const aj_node *root, const char *id)
{
    const aj_node *pkgs = ajson_get(root, "packages");
    const aj_node *c;
    if (!pkgs || pkgs->type != AJ_ARR) return 0;
    for (c = pkgs->child; c; c = c->next)
        if (c->type == AJ_OBJ && strcmp(ajson_get_str(c, "id", ""), id) == 0) return c;
    return 0;
}

/* "DH0:Programs/Foo" -> a path fopen can use. On AmigaOS the receipt's
 * absolute path IS a valid DOS path already; on the host build we leave it
 * unchanged too (host build is for logic testing only). */
#define receipt_path(p) (p)

/* The catalog every command works against: all ENABLED repos merged in
 * priority order (see arepo_load_merged). With one repo configured - the
 * default - this is exactly the old single-catalog behaviour. */
static int load_index(aidx_index *idx)
{
    if (arepo_load_merged(idx) != 0) {
        printf("amipkg: no catalog at %s\n", amipkg_data_path("packages.json"));
        printf("Bring your network up and run:  amipkg update\n");
        return 1;
    }
    return 0;
}

/* Where a package came from, for display. "" (a standalone-parsed index)
 * reads as the official repo. */
static const char *entry_repo(const aidx_entry *e)
{
    return (e && e->repo[0]) ? e->repo : AREPO_OFFICIAL_ID;
}

/* Load the index a package SPEC should be resolved against, and hand back the
 * bare id.
 *
 *   "ibrowse"          -> the merged view; repo priority already picked a winner
 *   "mystuff:ibrowse"  -> ONLY that repo, so a package shadowed by a
 *                         higher-priority repo can still be asked for by name
 *
 * Returns 0 on success. Free the index with aidx_free either way. */
static int load_index_for_spec(const char *spec, aidx_index *idx, char *id_out, size_t n)
{
    char repo[AREPO_ID_MAX];
    if (arepo_split_spec(spec ? spec : "", repo, sizeof repo, id_out, n)) {
        arepo_list l;
        arepo_load(&l);
        if (arepo_find(&l, repo) < 0) {
            printf("amipkg: no repository called '%s'. Try 'amipkg repo'.\n", repo);
            return 1;
        }
        if (arepo_load_one(repo, idx) != 0) {
            printf("amipkg: '%s' has no catalog yet - run 'amipkg update'.\n", repo);
            return 1;
        }
        return 0;
    }
    return load_index(idx);
}

/* ------------------------------------------------------------------ */

static int cmd_list(void)
{
    rcpt_installed *inst = amipkg_inst_scratch;   /* shared scratch, see store.h */
    aidx_index idx;
    int have_idx;
    size_t n = load_installed(inst, MAX_PKGS), i;
    if (n == 0) { printf("No packages recorded (no receipt DB?).\n"); return 5; }
    /* Old build-time receipts carry no version ("-") - show the catalog's
     * as the best estimate (build-time installs came from the catalog). */
    have_idx = (load_index(&idx) == 0);
    printf("%-24s %s\n", "Package", "Version");
    for (i = 0; i < n; i++) {
        const char *v = inst[i].version;
        if (have_idx && aver_is_unknown(v)) {
            const aidx_entry *e = aidx_find(&idx, inst[i].id);
            if (e && !aver_is_unknown(e->version)) v = e->version;
        }
        printf("%-24s %s\n", inst[i].id, v);
    }
    if (have_idx) aidx_free(&idx);
    return 0;
}

static int cmd_check(void)
{
    rcpt_installed *inst = amipkg_inst_scratch;   /* shared scratch, see store.h */
    aidx_index idx;
    size_t n, i;
    int updates = 0;
    if (load_index(&idx) != 0) return 10;
    n = load_installed(inst, MAX_PKGS);
    for (i = 0; i < n; i++) {
        const aidx_entry *e = aidx_find(&idx, inst[i].id);
        if (!e) continue;
        if (aver_is_newer(aidx_comparable_version(e), inst[i].version)) {
            printf("%-24s %s -> %s\n", inst[i].id, inst[i].version, e->version);
            updates++;
        } else if (aver_is_unknown(inst[i].version)) {
            printf("%-24s version unknown (reinstall to adopt versioning)\n", inst[i].id);
        }
    }
    if (updates == 0) printf("Everything is up to date.\n");
    aidx_free(&idx);
    return 0;
}

static int cmd_info(const char *id)
{
    aidx_index idx;
    const aidx_entry *e;
    size_t i;
    char bare[128];
    if (load_index_for_spec(id, &idx, bare, sizeof bare) != 0) return 10;
    id = bare;
    e = aidx_find(&idx, id);
    if (!e) { printf("amipkg: '%s' is not in the index.\n", id); aidx_free(&idx); return 5; }
    printf("%s - %s (%s)\n", e->id, e->name, e->category);
    if (e->description[0]) printf("  %s\n", e->description);
    printf("  version: %s\n", e->version);
    {   /* Provenance matters once repos can shadow each other - but stay quiet
         * in the single-repo default, where it would just be noise. */
        arepo_list rl; arepo_load(&rl);
        if (rl.count > 1) {
            int at = arepo_find(&rl, entry_repo(e));
            printf("  repo: %s%s\n", entry_repo(e),
                   (at >= 0 && !arepo_is_signed(&rl.v[at])) ? " (UNSIGNED)" : "");
        }
    }
    if (e->added[0]) printf("  added: %s\n", e->added);
    /* Only worth a line when it is NOT the default everything else is. */
    if (e->arch[0] && strcmp(aidx_arch(e), "m68k-amigaos") != 0)
        printf("  architecture: %s%s\n", aidx_arch(e),
               aidx_arch_runs_on(aidx_arch(e), amipkg_host_arch())
                   ? "" : " - NOT runnable on this machine");
    if (e->dep_count) {
        printf("  needs:");
        for (i = 0; i < e->dep_count; i++) printf(" %s", e->deps[i].id);
        printf("\n");
    }
    if (e->archive_url[0]) printf("  archive: %s\n", e->archive_url);
    if (e->archive_sha256[0]) printf("  sha256: %.16s...\n", e->archive_sha256);
    printf("  install: %s\n", e->has_recipe ? "portable recipe" : "build-time only");
    aidx_free(&idx);
    return 0;
}

static int id_installed(const rcpt_installed *inst, size_t n, const char *id)
{
    size_t i;
    for (i = 0; i < n; i++) if (strcmp(inst[i].id, id) == 0) return 1;
    return 0;
}

/* Case-insensitive substring match (Amiga text is Latin-1; ASCII fold is fine
 * for the id/name/category fields we filter on). */
static int contains_ci(const char *hay, const char *needle)
{
    size_t nl = strlen(needle), i, j;
    if (!nl) return 1;
    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j]; j++) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
        }
        if (!needle[j]) return 1;
    }
    return 0;
}

/* List every package in the seeded index (optionally filtered by a term that
 * matches the id, name, or category), marking what's already installed. */
/* Case-insensitive whole-word compare, for the ALL keyword. */
static int ci_eq_word(const char *a, const char *b)
{
    size_t i;
    for (i = 0; a[i] && b[i]; i++) {
        int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static int cmd_avail(const char *filter)
{
    rcpt_installed *inst = amipkg_inst_scratch;   /* shared scratch, see store.h */
    aidx_index idx;
    size_t ninst, i;
    int shown = 0, hidden = 0;
    const char *host = amipkg_host_arch();
    int show_all = 0;
    if (filter && ci_eq_word(filter, "ALL")) { show_all = 1; filter = NULL; }
    if (load_index(&idx) != 0) return 10;
    ninst = load_installed(inst, MAX_PKGS);
    printf("%-22s %-9s %-14s\n", "Package", "Version", "Category");
    for (i = 0; i < idx.count; i++) {
        const aidx_entry *e = &idx.entries[i];
        if (filter && !(contains_ci(e->id, filter) || contains_ci(e->name, filter)
                        || contains_ci(e->category, filter)))
            continue;
        /* Hide what this machine cannot run, and say how many were hidden
         * rather than silently shrinking the catalog. ALL=show everything. */
        if (!show_all && !aidx_arch_runs_on(aidx_arch(e), host)) { hidden++; continue; }
        printf("%-22s %-9s %-14s %s%s\n", e->id, e->version[0] ? e->version : "-",
               e->category, id_installed(inst, ninst, e->id) ? "[installed]" : "",
               (show_all && !aidx_arch_runs_on(aidx_arch(e), host))
                   ? aidx_arch(e) : "");
        shown++;
    }
    if (filter) printf("%d package(s) match \"%s\" (of %lu).\n", shown, filter, (unsigned long)idx.count);
    else        printf("%d package(s) available.\n", shown);
    if (hidden)
        printf("%d not runnable on this machine (%s) - 'amipkg avail ALL' shows them.\n",
               hidden, host);
    aidx_free(&idx);
    return 0;
}

/* The plain-HTTP mirror of the signed index (amipkg has no TLS; the host is
 * UNTRUSTED - the Ed25519 signature is verified on-device below, so a plain-HTTP
 * mirror is safe). amiga-imager.org is served over plain HTTP for exactly this
 * (.com/.de force HTTPS). Override with the AMIPKG_REPO_URL env var. */
#define AMIPKG_UPDATE_BASE "http://amiga-imager.org/packages"

/* `amipkg update` - fetch a fresh packages.json + .sig from the repo mirror,
 * verify the Ed25519 signature against the baked public key ON-DEVICE, and only
 * then replace the seeded AMIPKG:packages.json. This is what lets an online
 * Amiga pick up newly-published packages without a rebuild. */
/* Update ONE repository. `e` carries the url and (for a signed repo) the
 * pinned key. Returns 0 on success, nonzero on any failure - the caller keeps
 * going so one dead mirror cannot block the others. */
static int update_one_repo(const arepo_entry *e)
{
    const char *base = e->url;
    char url[512];
    char *json = NULL, *sig = NULL;
    long bytes = 0;
    size_t jl, sl = 0;
    FILE *out;
    char jnew[320]; strcpy(jnew, amipkg_data_path("cache/packages.json.new"));
    char snew[320]; strcpy(snew, amipkg_data_path("cache/packages.json.sig.new"));
    char dest[320], sdest[320], dir[320];

    arepo_catalog_path(e->id, dest, sizeof dest);
    arepo_sig_path(e->id, sdest, sizeof sdest);
    arepo_dir_path(e->id, dir, sizeof dir);
    /* repos/ must exist before repos/<id>/. Harmless for the official repo,
     * whose "dir" is the already-present prefix root. */
    { char parent[320]; snprintf(parent, sizeof parent, "%srepos", amipkg_prefix());
      amipkg_make_dir(parent); }
    amipkg_make_dir(dir);

    printf("Updating '%s' from %s ...\n", e->id, base);
    /* Cache-buster: plain-HTTP intermediaries (router/ISP) may serve the
     * index stale for minutes after a publish (Cache-Control max-age on
     * the endpoint). A unique query defeats every cache in the path.
     * dos DateStamp, not time() - newlib's gettimeofday syscall is absent. */
    unsigned long stamp = 0;
#ifdef __amigaos__
    { struct DateStamp ds; DateStamp(&ds);
      stamp = (unsigned long)ds.ds_Days * 86400UL
            + (unsigned long)ds.ds_Minute * 60UL
            + (unsigned long)ds.ds_Tick / 50UL; }
#endif
    snprintf(url, sizeof url, "%s/packages.json?t=%lu", base, stamp);
    if (!(out = fopen(jnew, "wb"))) { printf("amipkg: cannot write %s\n", jnew); return 10; }
    if (http_get(url, out, &bytes) != 0) { fclose(out); remove(jnew); return 10; }
    fclose(out);
    /* An UNSIGNED repo has no .sig to fetch - the user accepted that when they
     * added it. A SIGNED repo must produce one or nothing is installed. */
    if (arepo_is_signed(e)) {
        snprintf(url, sizeof url, "%s/packages.json.sig?t=%lu", base, stamp);
        if (!(out = fopen(snew, "wb"))) { printf("amipkg: cannot write cache\n"); remove(jnew); return 10; }
        if (http_get(url, out, &bytes) != 0) { fclose(out); remove(jnew); remove(snew); return 10; }
        fclose(out);
        sig = read_file(snew);
        if (!sig) { printf("amipkg: could not read the signature.\n"); goto fail; }
        sl = strlen(sig);
        while (sl && (sig[sl-1] == '\n' || sig[sl-1] == '\r' || sig[sl-1] == ' ' || sig[sl-1] == '\t'))
            sig[--sl] = '\0';
    }

    json = read_file(jnew);
    if (!json) { printf("amipkg: could not read the download.\n"); goto fail; }
    jl = strlen(json);                       /* JSON is text (no NUL) -> exact byte length */

    if (arepo_is_signed(e)) {
        /* Verified against THIS repo's pinned key - not the project key. A
         * signature from another repo must not vouch for this one. */
        if (!amipkg_verify_index_key((const unsigned char *)json, jl, sig, e->key)) {
            printf("amipkg: '%s' SIGNATURE DID NOT VERIFY - keeping the current catalog.\n", e->id);
            goto fail;
        }
    }

    /* Sanity-check before overwriting a good catalog with rubbish: a truncated
     * or HTML error page must not replace a working index. */
    {
        aidx_index probe;
        memset(&probe, 0, sizeof probe);
        if (aidx_parse(json, &probe) != 0 || probe.count == 0) {
            printf("amipkg: '%s' sent an unreadable catalog - keeping the current one.\n", e->id);
            aidx_free(&probe);
            goto fail;
        }
        aidx_free(&probe);
    }

    if (!(out = fopen(dest, "wb"))) { printf("amipkg: cannot write %s\n", dest); goto fail; }
    fwrite(json, 1, jl, out); fclose(out);
    if (sig && sl) { if ((out = fopen(sdest, "wb"))) { fwrite(sig, 1, sl, out); fclose(out); } }
    else remove(sdest);                      /* unsigned: leave no stale sig */
    remove(jnew); remove(snew);
    free(json); free(sig);
    if (arepo_is_signed(e)) printf("  '%s' updated + signature verified.\n", e->id);
    else                    printf("  '%s' updated (UNSIGNED - not verified).\n", e->id);
    return 0;
fail:
    free(json); free(sig);
    remove(jnew); remove(snew);
    return 10;
}

/* `amipkg update` - refresh every ENABLED repository. One failure does not
 * abort the rest; the exit code reflects whether ANY repo updated. */
static int cmd_update(void)
{
    arepo_list l;
    size_t i;
    int ok = 0, failed = 0;
    const char *override = getenv("AMIPKG_REPO_URL");

    arepo_load(&l);

    /* Back-compat: AMIPKG_REPO_URL used to point the single-repo client at a
     * different mirror. Keep honouring it, for the official repo only. */
    if (override && override[0]) {
        int at = arepo_find(&l, AREPO_OFFICIAL_ID);
        if (at >= 0 && arepo_url_valid(override) == 0) {
            strncpy(l.v[at].url, override, sizeof l.v[at].url - 1);
            l.v[at].url[sizeof l.v[at].url - 1] = '\0';
        }
    }

    for (i = 0; i < l.count; i++) {
        if (!l.v[i].enabled) { printf("Skipping '%s' (disabled).\n", l.v[i].id); continue; }
        if (update_one_repo(&l.v[i]) == 0) ok++;
        else { failed++; printf("  '%s' could not be updated.\n", l.v[i].id); }
    }

    if (ok == 0) {
        printf("amipkg: no repository could be updated.\n");
        return 10;
    }
    if (failed) printf("Updated %d repo(s); %d failed. Run 'amipkg avail' to see the catalog.\n", ok, failed);
    else        printf("Updated %d repo(s). Run 'amipkg avail' to see the catalog.\n", ok);
    return 0;
}

/* ------------------------------------------------------- repo management */

static void repo_print_list(const arepo_list *l)
{
    size_t i;
    printf("Repositories (order is PRIORITY - the first to provide a package wins):\n\n");
    for (i = 0; i < l->count; i++) {
        const arepo_entry *e = &l->v[i];
        printf("%2d. %-16s %-9s %s\n", (int)(i + 1), e->id,
               e->enabled ? "enabled" : "DISABLED",
               arepo_is_signed(e) ? "signed" : "UNSIGNED");
        printf("    %s\n", e->url);
    }
    printf("\n%lu repo(s).\n", (unsigned long)l->count);
}

/* Ask once before adding a repo with no key. This is the ONE place the
 * unsigned decision is made, so spell out what it actually costs: without a
 * signature the catalog can be rewritten in transit, and the SHA-256 pins do
 * NOT save you because they live inside the very catalog that was rewritten.
 * So it is not only the operator being trusted - it is the network path. */
static int confirm_unsigned(const char *id, const char *url)
{
    int c, first;
    printf("\n'%s' has no public key, so its catalog will NOT be verified.\n", id);
    printf("  %s\n\n", url);
    printf("That means trusting the person running it AND every hop in between\n");
    printf("(your ISP, router, any proxy) - amipkg fetches over plain HTTP and\n");
    printf("an unsigned catalog can be altered on the way to you. The archive\n");
    printf("checksums do not help here: they live inside that same catalog.\n\n");
    printf("Ask the repo owner for their public key if they have one.\n\n");
    printf("Add '%s' as an UNSIGNED repository anyway? (y/N) ", id);
    fflush(stdout);
    first = c = getchar();
    while (c != '\n' && c != EOF) c = getchar();       /* drain the line */
    return (first == 'y' || first == 'Y');
}

static void repo_usage(void)
{
    printf("amipkg repo                      list configured repositories\n");
    printf("amipkg repo add <id> <url> [key] add one (no key = unsigned, asks first)\n");
    printf("amipkg repo remove <id>          remove one\n");
    printf("amipkg repo enable <id>          include it again\n");
    printf("amipkg repo disable <id>         keep it configured but ignore it\n");
    printf("amipkg repo up|down <id>         change PRIORITY (first match wins)\n");
    printf("\nA repo's <url> is the drawer holding packages.json (and, when\n");
    printf("signed, packages.json.sig). The key is base64, as published by the\n");
    printf("repo owner. Install a specific repo's build with  repo:package\n");
}

static int cmd_repo(int argc, char **argv)
{
    arepo_list l;
    const char *sub = (argc >= 3) ? argv[2] : "list";
    const char *id  = (argc >= 4) ? argv[3] : NULL;
    int rc;

    arepo_load(&l);

    if (strcmp(sub, "list") == 0) { repo_print_list(&l); return 0; }

    if (strcmp(sub, "add") == 0) {
        const char *url = (argc >= 5) ? argv[4] : NULL;
        const char *key = (argc >= 6) ? argv[5] : NULL;
        if (!id || !url) { repo_usage(); return 20; }
        if (arepo_id_valid(id) != 0) {
            printf("amipkg: '%s' is not a valid repo name (use letters, digits, - and _).\n", id);
            return 20;
        }
        if (arepo_url_valid(url) != 0) {
            printf("amipkg: '%s' is not a valid URL (needs http:// or https://).\n", url);
            return 20;
        }
        if (arepo_find(&l, id) >= 0) { printf("amipkg: a repo called '%s' already exists.\n", id); return 20; }
        if (!key || !key[0]) {
            if (!confirm_unsigned(id, url)) { printf("Not added.\n"); return 5; }
        }
        rc = arepo_add(&l, id, url, key);
        if (rc == 5) { printf("amipkg: that does not look like a base64 public key.\n"); return 20; }
        if (rc == 1) { printf("amipkg: the repo list is full (max %d).\n", AREPO_MAX); return 20; }
        if (rc != 0) { printf("amipkg: could not add '%s' (error %d).\n", id, rc); return 20; }
        if (arepo_save(&l) != 0) { printf("amipkg: could not save the repo list.\n"); return 10; }
        printf("Added '%s' (%s), lowest priority.\n", id, (key && key[0]) ? "signed" : "UNSIGNED");
        printf("Run 'amipkg update' to fetch its catalog.\n");
        return 0;
    }

    if (!id) { repo_usage(); return 20; }

    if (strcmp(sub, "remove") == 0) {
        if (arepo_remove(&l, id) != 0) { printf("amipkg: no repo called '%s'.\n", id); return 20; }
        if (arepo_save(&l) != 0) { printf("amipkg: could not save the repo list.\n"); return 10; }
        printf("Removed '%s'. Its cached catalog is left in repos/%s.\n", id, id);
        return 0;
    }
    if (strcmp(sub, "enable") == 0 || strcmp(sub, "disable") == 0) {
        int on = (strcmp(sub, "enable") == 0);
        if (arepo_set_enabled(&l, id, on) != 0) { printf("amipkg: no repo called '%s'.\n", id); return 20; }
        if (arepo_save(&l) != 0) { printf("amipkg: could not save the repo list.\n"); return 10; }
        printf("'%s' is now %s.\n", id, on ? "enabled" : "disabled");
        return 0;
    }
    if (strcmp(sub, "up") == 0 || strcmp(sub, "down") == 0) {
        int delta = (strcmp(sub, "up") == 0) ? -1 : 1;
        if (arepo_move(&l, id, delta) != 0) { printf("amipkg: no repo called '%s'.\n", id); return 20; }
        if (arepo_save(&l) != 0) { printf("amipkg: could not save the repo list.\n"); return 10; }
        repo_print_list(&l);
        return 0;
    }

    repo_usage();
    return 20;
}

static const char *basename_of(const char *url)
{
    const char *slash = strrchr(url, '/');
    return slash ? slash + 1 : url;
}

/* Does this cached archive path end in ".adf" (case-insensitive)? */
static int archive_is_adf(const char *path)
{
    size_t n = strlen(path);
    return n >= 4
        && (path[n-4] == '.')
        && (path[n-3] == 'a' || path[n-3] == 'A')
        && (path[n-2] == 'd' || path[n-2] == 'D')
        && (path[n-1] == 'f' || path[n-1] == 'F');
}

/* Download + SHA-256-verify `e`'s archive into the cache. On success writes the
 * cache path to dest_out (size dest_sz) and returns 0. Refuses an unpinned
 * archive (trust contract). Nonzero on any failure. */
static int fetch_verified(const aidx_entry *e, char *dest_out, size_t dest_sz)
{
    char hex[65];
    static char part[300];
    long bytes = 0;
    int rc = 1;
    size_t m;
    if (!e->archive_url[0]) { printf("amipkg: '%s' has no archive.\n", e->id); return 5; }
    if (!e->archive_sha256[0]) {
        printf("amipkg: '%s' has no pinned sha256 - refusing (trust contract).\n", e->id);
        return 5;
    }
    snprintf(dest_out, dest_sz, "%scache/%s", amipkg_prefix(), basename_of(e->archive_url));
    /* Already cached + verified? skip the download. */
    if (sha256_of_file(dest_out, hex) == 0 && strcmp(hex, e->archive_sha256) == 0) {
        printf("Cached %s (sha256 verified).\n", e->id);
        return 0;
    }
    /* Download to <name>.part with resume (Range) - an interrupted transfer
     * continues where it stopped, incl. across mirrors (identical bytes by
     * contract; the SHA-256 check below catches any divergence). */
    snprintf(part, sizeof part, "%s.part", dest_out);
    printf("Fetching %s...\n", e->archive_url);
    rc = http_get_file(e->archive_url, part, &bytes);
    for (m = 0; rc != 0 && m < e->mirror_count; m++) {
        printf("Trying mirror %s...\n", e->mirrors[m]);
        rc = http_get_file(e->mirrors[m], part, &bytes);
    }
    if (rc != 0) return 10;   /* .part stays - the next attempt resumes it */
    if (sha256_of_file(part, hex) != 0 || strcmp(hex, e->archive_sha256) != 0) {
        printf("amipkg: SHA-256 MISMATCH for %s - deleting download.\n", part);
        printf("  expected %s\n  got      %s\n", e->archive_sha256, hex);
        remove(part);
        return 10;
    }
    remove(dest_out);                     /* stale/older cached copy, if any */
    if (amipkg_rename(part, dest_out) != 0) {
        printf("amipkg: cannot move %s into the cache.\n", part);
        return 10;
    }
    printf("Fetched %s (%ld bytes, sha256 verified).\n", e->id, bytes);
    return 0;
}

static int cmd_fetch(const char *id)
{
    aidx_index idx;
    const aidx_entry *e;
    char dest[256];
    int rc;
    char bare[128];
    if (load_index_for_spec(id, &idx, bare, sizeof bare) != 0) return 10;
    id = bare;
    e = aidx_find(&idx, id);
    if (!e) { printf("amipkg: '%s' not in the index.\n", id); aidx_free(&idx); return 5; }
    rc = fetch_verified(e, dest, sizeof dest);
    if (rc == 0) printf("-> %s\n", dest);
    aidx_free(&idx);
    return rc;
}

/* Append one installed path to files/<id>.files with its digest. */
static void receipt_record_file(const char *id, const char *path)
{
    char rp[192], hex[65];
    FILE *f;
    snprintf(rp, sizeof rp, "%sdb/files/%s.files", amipkg_prefix(), id);
    f = fopen(rp, "a");
    if (!f) return;
    if (sha256_of_file(path, hex) == 0) fprintf(f, "%s|%s\n", path, hex);
    else                                fprintf(f, "%s\n", path);
    fclose(f);
}

static int cmd_remove(const char *id, int force);   /* defined below */

/* Amiga filesystems are case-insensitive - compare recorded paths that way. */
static int ci_path_eq(const char *a, const char *b)
{
    size_t i = 0;
    for (;;) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
        if (!ca) return 1;
        i++;
    }
}

/* Drop a package's receipt (inventory + installed line) WITHOUT touching a
 * single file on disk. Used by the in-place self-upgrade: the old files must
 * survive until their replacements are written, but the record of them has to
 * go so the post-install append leaves exactly one entry. */
static void receipt_forget(const char *id)
{
    rcpt_installed *inst = amipkg_inst_scratch;
    size_t n = load_installed(inst, MAX_PKGS), i;
    char rp[AMIPKG_PREFIX_MAX + 32], line[192];
    FILE *f;
    snprintf(rp, sizeof rp, "%sdb/files/%s.files", amipkg_prefix(), id);
    remove(rp);
    f = fopen(amipkg_data_path("db/installed.txt"), "wb");
    if (!f) return;
    for (i = 0; i < n; i++) {
        if (strcmp(inst[i].id, id) == 0) continue;
        rcpt_format_installed_line(&inst[i], line, sizeof line);
        fprintf(f, "%s\n", line);
    }
    fclose(f);
}

/* Is `id` recorded in the installed receipt DB? */
static int pkg_installed(const char *id)
{
    rcpt_installed *inst = amipkg_inst_scratch;   /* shared scratch, see store.h */
    size_t n = load_installed(inst, MAX_PKGS), i;
    for (i = 0; i < n; i++) if (strcmp(inst[i].id, id) == 0) return 1;
    return 0;
}

/* Does this entry need a build-time-only capability (arbitrary installers /
 * icon patching / host-side machinery)? Those install with Amiga Imager at
 * image-build time; amipkg must refuse them cleanly instead of fetching. */
static int entry_build_only(const aidx_entry *e)
{
    size_t i;
    for (i = 0; i < e->cap_count; i++) {
        const char *c = e->caps[i];
        if (strcmp(c, "host-builtin-v1") == 0 || strcmp(c, "icon-patch-v1") == 0
            || strcmp(c, "adf-unwrap-v1") == 0 || strcmp(c, "installer-script-v1") == 0)
            return 1;
    }
    return 0;
}

/* Map a requirements.minKS string to the exec lib_Version it needs. 0 = no
 * parseable floor (never blocks). */
static int ks_floor_version(const char *minks)
{
    if (!minks || !minks[0]) return 0;
    if (strcmp(minks, "2.04") == 0 || strcmp(minks, "2.0") == 0) return 37;
    if (strcmp(minks, "2.1") == 0) return 38;
    if (strcmp(minks, "3.0") == 0) return 39;
    if (strcmp(minks, "3.1") == 0) return 40;
    if (strcmp(minks, "3.5") == 0) return 44;
    if (strcmp(minks, "3.9") == 0) return 45;
    if (strcmp(minks, "3.1.4") == 0) return 46;
    if (strcmp(minks, "3.2") == 0) return 47;
    return 0;
}

/* Does this machine satisfy the entry's CPU/Kickstart floors? Prints the
 * refusal reason when not. */
static int floors_ok(const aidx_entry *e)
{
    const char *cpu = amipkg_cpu();
    int ksneed = ks_floor_version(e->min_ks);
    int kshave = amipkg_ks_version();
    if (!aidx_arch_runs_on(aidx_arch(e), amipkg_host_arch())) {
        printf("amipkg: '%s' is built for %s; this machine is %s.\n",
               e->id, aidx_arch(e), amipkg_host_arch());
        return 0;
    }
    if (e->min_cpu[0] && cpu[0] && !ares_cpu_satisfies(cpu, e->min_cpu)) {
        printf("amipkg: '%s' needs a %s or better (this machine: %s).\n",
               e->id, e->min_cpu, cpu);
        return 0;
    }
    if (ksneed && kshave && kshave < ksneed) {
        printf("amipkg: '%s' needs Kickstart %s+ (exec v%d < v%d).\n",
               e->id, e->min_ks, kshave, ksneed);
        return 0;
    }
    return 1;
}

/* Disk-space preflight: refuse an install that would obviously fill the
 * volume. Extracted trees run ~2-3x the LHA size, so require 3x on the
 * destination (plus the archive itself in the cache when not yet cached).
 * amipkg_volume_free returns -1 when unknown - then we don't block. */
static int space_ok(const aidx_entry *e, const char *archive_path, const char *dest_volume)
{
    long long need, freeb;
    FILE *f;
    if (e->archive_size <= 0) return 1;
    f = fopen(archive_path, "rb");
    if (f) { fclose(f); }
    else {
        freeb = amipkg_volume_free(amipkg_data_path("cache/"));
        if (freeb >= 0 && freeb < (long long)e->archive_size + (256L * 1024)) {
            printf("amipkg: not enough space on the cache volume for %s "
                   "(need ~%ldK free).\n", e->id, (long)(e->archive_size / 1024 + 256));
            return 0;
        }
    }
    need = (long long)e->archive_size * 3;
    freeb = amipkg_volume_free(dest_volume);
    if (freeb >= 0 && freeb < need) {
        printf("amipkg: not enough space on %s to install %s "
               "(need ~%ldK free, have ~%ldK).\n", dest_volume, e->id,
               (long)(need / 1024), (long)(freeb / 1024));
        return 0;
    }
    return 1;
}

/* Install ONE resolved entry (no dependency handling here - cmd_install
 * resolves and orders; this does the fetch/verify/extract/copy/receipt). */
static int install_entry(const aidx_index *idx, const aidx_entry *e)
{
    int generic_recorded = 0;
    const char *root;
    aj_node *tree;
    const aj_node *entry_obj;
    static arecipe recipe;   /* ~32 KB - keep off the Shell stack; install_entry
                                runs sequentially, never nested */
    char archive[256];
    static char paths[256][256];
    static rcpt_file prev_files[MAX_FILES];   /* self-upgrade prune list */
    size_t n_prev = 0;
    size_t n, i;
    int rc;
    /* Replacing the running program is the one upgrade that cannot afford a
     * gap where nothing is installed - see the in-place path below. */
    const int self_upgrade = strcmp(e->id, "amipkg") == 0;

    tree = NULL;
    if (!e->archive_url[0]) {
        printf("amipkg: '%s' has no downloadable archive - install it at build time / via the Mac app.\n", e->id);
        return 5;
    }

    /* Packages with a portable recipe run it; the fetch-only Aminet entries fall
     * back to a generic install (extract into the configured install drawer). */
    if (e->has_recipe) {
        char *json = read_file(amipkg_data_path("packages.json"));
        tree = json ? ajson_parse(json) : NULL;
        free(json);
        entry_obj = tree ? find_entry_obj_by_id(tree, e->id) : NULL;
        if (!entry_obj || arecipe_parse(entry_obj, &recipe) != 0) {
            printf("amipkg: could not read '%s' recipe.\n", e->id);
            if (tree) ajson_free(tree); return 10;
        }
    }

    /* Space preflight against the actual destination volume. */
    {
        char dir[256], vol[64];
        const char *colon;
        if (e->has_recipe) { strcpy(vol, "SYS:"); }
        else {
            amipkg_get_installdir(dir, sizeof dir);
            colon = strchr(dir, ':');
            if (colon && (size_t)(colon - dir) < sizeof vol - 2) {
                memcpy(vol, dir, (size_t)(colon - dir) + 1);
                vol[(size_t)(colon - dir) + 1] = '\0';
            } else strcpy(vol, "SYS:");
        }
        snprintf(archive, sizeof archive, "%scache/%s", amipkg_prefix(), basename_of(e->archive_url));
        if (!space_ok(e, archive, vol)) { if (tree) ajson_free(tree); return 5; }
    }

    rc = fetch_verified(e, archive, sizeof archive);
    if (rc != 0) { if (tree) ajson_free(tree); return rc; }

    printf("Unpacking + installing %s...\n", e->id);
    if (archive_is_adf(archive)) {
        /* Package shipped as a raw Amiga floppy image: read its OFS/FFS tree. */
        if (adf_extract(archive, amipkg_data_path("cache/extract")) < 0) {
            printf("amipkg: not a readable OFS/FFS ADF.\n");
            if (tree) ajson_free(tree); return 10;
        }
    } else if (!amipkg_extract(archive, amipkg_data_path("cache/extract"))) {
        printf("amipkg: extraction failed (C:lha present?).\n");
        if (tree) ajson_free(tree); return 10;
    }
    /* Upgrade path: the new version is now downloaded + extracted, so it's safe
     * to remove the previously-installed version (files + receipt) first. Doing
     * it AFTER extraction means a failed download never destroys the install.
     * And never on the strength of an EMPTY extraction (A4000 incident). */
    if (!amipkg_extract_nonempty(amipkg_data_path("cache/extract"))) {
        printf("amipkg: extraction produced no files - aborting, nothing was removed.\n");
        if (tree) ajson_free(tree); return 10;
    }
    if (pkg_installed(e->id)) {
        if (self_upgrade) {
            /* Upgrading the program that is RUNNING. Removing first would mean
             * a window with no amipkg on the machine at all and no second copy
             * anywhere - and anything that then goes wrong in the copy phase
             * leaves the user with nothing (A4000 report, 2026-07-30: a GUI
             * erased from under itself mid-update). So: install straight over
             * the previous version and prune the leftovers AFTERWARDS, once
             * the replacement is known to be on disk. The receipt is cleared
             * here so the post-install record has exactly one entry; the FILES
             * stay put until they are either overwritten or pruned. */
            n_prev = load_files_for(e->id, prev_files, MAX_FILES);
            printf("Updating %s in place (the previous version stays until "
                   "the new one is installed)...\n", e->id);
            receipt_forget(e->id);
        } else {
            printf("Updating %s - removing the previous version...\n", e->id);
            cmd_remove(e->id, 1 /*force: an upgrade replaces its own files*/);
        }
    }
    if (e->has_recipe) {
        root = getenv("AMIPKG_ROOT"); if (!root || !root[0]) root = "SYS:";
        n = amipkg_run_recipe(&recipe, amipkg_data_path("cache/extract"), root, paths, 256);
    } else {
        /* Generic: drop the extracted tree into the configured install drawer
         * (default SYS:Programs; overridable via `amipkg dir` / AMIPKG_INSTALLDIR).
         * Only an archive that brings its OWN single top drawer lands as-is;
         * everything else (flat LHAs, ADF roots, multi-root archives) is
         * wrapped in a <id> drawer so loose files never pile up in the
         * install dir root. */
        char dir[256], dest[320];
        amipkg_get_pkgdir(e->id, dir, sizeof dir);   /* adopt override, else global */

        if (amipkg_system_drawer_kind(dir) != 0) {
            /* A system drawer holds flat, specific things. Copying a whole
             * archive tree here would bury the command at C:Tool/Tool - where
             * the shell never finds it - and drag the documentation in with
             * it. So: programs go in flat, the rest beside it. */
            char companion[320], global[256];
            size_t n_sys = 0, n_comp = 0;
            amipkg_get_installdir(global, sizeof global);
            snprintf(companion, sizeof companion, "%s/%s", global, e->id);
            printf("(no recipe - '%s' is a system drawer: programs go there,\n"
                   " everything else into %s)\n", dir, companion);
            n = amipkg_install_generic_routed(amipkg_data_path("cache/extract"),
                                              dir, companion, e->id, paths, 256,
                                              &n_sys, &n_comp);
            if (n_sys == 0)
                printf("amipkg: nothing here looks like a program for %s -\n"
                       "        it all went to %s.\n", dir, companion);
            else
                printf("Installed %lu file(s) into %s, %lu into %s.\n",
                       (unsigned long)n_sys, dir, (unsigned long)n_comp, companion);
        } else {
            if (amipkg_extract_single_top_dir(amipkg_data_path("cache/extract")))
                snprintf(dest, sizeof dest, "%s", dir);
            else
                snprintf(dest, sizeof dest, "%s/%s", dir, e->id);
            printf("(no recipe - installing into %s)\n", dest);
            /* Uncapped + self-recording (see amipkg_install_generic_recorded);
             * the caller-side receipt loop below stays for the RECIPE path only. */
            n = amipkg_install_generic_recorded(amipkg_data_path("cache/extract"), dest,
                                                e->id, paths, 256);
        }
        generic_recorded = 1;
    }
    if (n == 0) {
        printf("amipkg: nothing installed (no files matched / not runnable here).\n");
        if (tree) ajson_free(tree); return 10;
    }
    if (!generic_recorded)
        for (i = 0; i < n; i++) receipt_record_file(e->id, paths[i]);
    /* In-place self-upgrade: the replacement is on disk now, so anything the
     * old version shipped and the new one doesn't can go. Deliberately narrow -
     * prune ONLY inside our own install prefix. A stale file left elsewhere is
     * harmless; deleting something outside the home on the strength of an old
     * inventory is how a running binary gets erased, which is the whole reason
     * this path exists. Non-absolute (legacy PROGDIR:) lines are never pruned. */
    if (self_upgrade && n_prev) {
        size_t p, q, prefix_len = strlen(amipkg_prefix());
        int pruned = 0;
        for (p = 0; p < n_prev; p++) {
            const char *old = prev_files[p].path;
            int still_shipped = 0;
            if (!strchr(old, ':')) continue;                       /* not absolute */
            if (strncmp(old, amipkg_prefix(), prefix_len) != 0) continue;  /* outside */
            for (q = 0; q < n && !still_shipped; q++)
                if (ci_path_eq(old, paths[q])) still_shipped = 1;
            if (!still_shipped && remove(old) == 0) pruned++;
        }
        if (pruned)
            printf("Removed %d file(s) the new version no longer ships.\n", pruned);
    }
    /* Self-update: also refresh the binaries actually in use (own program
     * drawer + legacy C:/SYS:Tools homes) - see amipkg_selfupdate_mirror.
     * The NOTE line doubles as the GUIs' restart-requester marker - keep
     * the wording in sync with gui.c/mui.c poll_async. */
    if (strcmp(e->id, "amipkg") == 0) {
        amipkg_selfupdate_mirror(paths, n);
        printf("NOTE: amipkg itself was updated - please restart amipkg "
               "(and any open amipkg GUI) to run the new version.\n");
    }
    if (e->has_recipe) {
        /* Record boot-script edits (scripts/<id>.edits, target|marker|version)
         * so `remove` can strip the overlay blocks, and save any removeScript
         * lines (scripts/<id>.remove) to run at uninstall - the receipt owns
         * them so removal works even after the catalog moves on. */
        char rp[192];
        FILE *f;
        size_t k;
        snprintf(rp, sizeof rp, "%sdb/scripts/%s.edits", amipkg_prefix(), e->id);
        f = NULL;
        for (k = 0; k < recipe.op_count; k++) {
            if (recipe.ops[k].type != AROP_SCRIPT_INJECT) continue;
            if (!f) f = fopen(rp, "w");
            if (f) fprintf(f, "S:User-Startup|%s|amipkg\n", recipe.ops[k].marker);
        }
        if (f) fclose(f);
        snprintf(rp, sizeof rp, "%sdb/scripts/%s.remove", amipkg_prefix(), e->id);
        f = NULL;
        for (k = 0; k < recipe.op_count; k++) {
            if (recipe.ops[k].type != AROP_REMOVE_SCRIPT) continue;
            if (!f) f = fopen(rp, "w");
            if (f) fprintf(f, "%s\n", recipe.ops[k].script);
        }
        if (f) fclose(f);
    }
    /* Record in installed.txt. On an upgrade the old line was already removed
     * above (pkg_installed -> cmd_remove), so this append leaves exactly one. */
    { FILE *f = fopen(amipkg_data_path("db/installed.txt"), "a");
      if (f) { fprintf(f, "%s|%s|%ld|0\n", e->id, e->version, idx->index_version); fclose(f); } }
    printf("Installed %s %s: %lu file(s).\n", e->id, e->version, (unsigned long)n);
    if (e->has_recipe) {
        size_t k;
        for (k = 0; k < recipe.op_count; k++)
            if (recipe.ops[k].type == AROP_SCRIPT_INJECT) {
                printf("NOTE: S:User-Startup was extended (assigns for %s).\n"
                       "      REBOOT once to activate them before using it.\n", e->id);
                break;
            }
    }
    if (tree) ajson_free(tree);
    return 0;
}

/* `amipkg install <id> [DIR=<drawer>] [DRYRUN]` - resolve dependencies (topological,
 * CPU-variant aware) and install everything that's missing, dependencies
 * first. DRYRUN prints the plan + download sizes and changes nothing. */
static int cmd_install2(const char *id, int dry)
{
    aidx_index idx;
    const aidx_entry *e;
    static ares_result res;
    size_t i;
    long dl_bytes = 0;
    int rc = 0, installed_count = 0;

    {   /* "repo:id" forces one repository; a bare id uses repo priority. */
        static char bare[128];
        if (load_index_for_spec(id, &idx, bare, sizeof bare) != 0) return 10;
        id = bare;
    }
    e = aidx_find(&idx, id);
    if (!e) { printf("amipkg: '%s' not in the index.\n", id); aidx_free(&idx); return 5; }
    if (entry_build_only(e)) {
        printf("amipkg: '%s' is installed at build time only (Amiga Imager) - not installable here.\n", id);
        aidx_free(&idx); return 5;
    }
    if (!floors_ok(e)) { aidx_free(&idx); return 5; }

    ares_resolve(&idx, id, amipkg_cpu(), &res);
    for (i = 0; i < res.missing_count; i++)
        printf("amipkg: WARNING - dependency '%s' is not in the index; continuing without it.\n",
               res.missing[i]);

    if (dry) printf("DRY RUN - nothing will be installed.\n");
    for (i = 0; i < res.count && rc == 0; i++) {
        const aidx_entry *pe = res.ordered[i];
        int is_target = strcmp(pe->id, e->id) == 0;
        /* Dependencies already present are satisfied; the TARGET always
         * (re)installs - that's the upgrade path. */
        if (!is_target && pkg_installed(pe->id)) {
            if (dry) printf("  %-24s (already installed)\n", pe->id);
            continue;
        }
        if (!is_target && entry_build_only(pe)) {
            printf("amipkg: WARNING - dependency '%s' is build-time only and not installed;\n"
                   "        '%s' may not work until an Amiga Imager build provides it.\n",
                   pe->id, id);
            continue;
        }
        if (!is_target && !floors_ok(pe)) { rc = 5; break; }
        if (dry) {
            printf("  %-24s %-10s %ldK download%s\n", pe->id, pe->version,
                   pe->archive_size > 0 ? pe->archive_size / 1024 : 0,
                   is_target ? "" : "  (dependency)");
            if (pe->archive_size > 0) dl_bytes += pe->archive_size;
            installed_count++;
            continue;
        }
        if (!is_target) printf("Installing dependency %s...\n", pe->id);
        rc = install_entry(&idx, pe);
        if (rc == 0) installed_count++;
    }
    if (dry)
        printf("Would install %d package(s), ~%ldK to download.\n",
               installed_count, dl_bytes / 1024);
    else if (rc == 0 && installed_count > 1)
        printf("Done: %d package(s) installed (dependencies first).\n", installed_count);
    aidx_free(&idx);
    return rc;
}

static int cmd_install(const char *id) { return cmd_install2(id, 0); }

/* `amipkg doctor` - verify every receipt against the disk: missing files,
 * digest mismatches (user-modified or corrupt). Advises reinstalls. */
static int cmd_doctor(void)
{
    rcpt_installed *inst = amipkg_inst_scratch;   /* shared scratch, see store.h */
    static rcpt_file files[MAX_FILES];
    size_t ninst = load_installed(inst, MAX_PKGS), i, j;
    int broken_pkgs = 0;
    if (ninst == 0) { printf("No packages recorded (no receipt DB?).\n"); return 5; }
    for (i = 0; i < ninst; i++) {
        size_t nf = load_files_for(inst[i].id, files, MAX_FILES);
        int missing = 0, modified = 0;
        char hex[65];
        for (j = 0; j < nf; j++) {
            FILE *f = fopen(receipt_path(files[j].path), "rb");
            if (!f) { missing++; continue; }
            fclose(f);
            if (files[j].sha256[0]
                && sha256_of_file(receipt_path(files[j].path), hex) == 0
                && strcmp(hex, files[j].sha256) != 0)
                modified++;
        }
        if (missing || modified) {
            broken_pkgs++;
            printf("%-24s %d missing, %d modified of %lu file(s) -> amipkg install %s\n",
                   inst[i].id, missing, modified, (unsigned long)nf, inst[i].id);
        } else {
            printf("%-24s OK (%lu file(s))\n", inst[i].id, (unsigned long)nf);
        }
    }
    if (broken_pkgs == 0) { printf("All packages healthy.\n"); return 0; }
    printf("%d package(s) need attention (reinstall repairs them).\n", broken_pkgs);
    return 5;
}

/* `amipkg upgrade [<id>]` - reinstall every installed package whose index
 * version is newer (or just <id>). Each install is upgrade-aware (removes the
 * old version first). With no argument it upgrades everything out of date. */
static int cmd_upgrade(const char *only_id)
{
    rcpt_installed *inst = amipkg_inst_scratch;   /* shared scratch, see store.h */
    static char todo[MAX_PKGS][64];
    aidx_index idx;
    size_t ninst, i, ntodo = 0;
    int rc = 0;
    if (load_index(&idx) != 0) return 10;
    ninst = load_installed(inst, MAX_PKGS);
    /* Snapshot the ids to upgrade FIRST - installing rewrites installed.txt. */
    for (i = 0; i < ninst; i++) {
        const aidx_entry *e;
        if (only_id && strcmp(inst[i].id, only_id) != 0) continue;
        e = aidx_find(&idx, inst[i].id);
        if (!e) continue;
        if (aver_is_newer(aidx_comparable_version(e), inst[i].version)) {
            printf("Update %-20s %s -> %s\n", inst[i].id, inst[i].version, e->version);
            strncpy(todo[ntodo], inst[i].id, 63); todo[ntodo][63] = '\0';
            ntodo++;
        }
    }
    aidx_free(&idx);
    if (only_id && ntodo == 0) {
        if (pkg_installed(only_id)) printf("%s is already up to date.\n", only_id);
        else printf("amipkg: '%s' is not installed.\n", only_id);
        return 0;
    }
    if (ntodo == 0) { printf("Everything is up to date.\n"); return 0; }
    for (i = 0; i < ntodo; i++) {
        int r = cmd_install(todo[i]);
        if (r != 0) { printf("amipkg: update of '%s' failed (rc %d).\n", todo[i], r); rc = r; }
    }
    printf("Updated %lu package(s).\n", (unsigned long)ntodo);
    return rc;
}

/* "submit <id> <archive-url> [description...]" - author a catalog entry ON
 * THE AMIGA and hand it to the maintainer: fetch the archive, compute its
 * SHA-256 pin locally, harvest Version:/Short: from the Aminet .readme when
 * present, compose the packages/<id>.json draft, and POST it to the
 * submission dropbox on amiga-imager.org. From there the Pi courier
 * validates it and opens a review branch - NOTHING enters the signed catalog
 * without the usual human review + offline signature. The Amiga is the
 * authoring tool, not a trust shortcut. */
static void json_escape(const char *in, char *out, size_t outsize)
{
    size_t o = 0;
    for (; *in && o < outsize - 2; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 32) out[o++] = ' ';
        else out[o++] = (char)c;
    }
    out[o] = '\0';
}

static const char *g_submit_cats[] = {
    "Utilities", "Games", "Internet", "Audio", "Text", "Network",
    "Graphics", "Development", "Libraries", "Emulation", "System", NULL };

static int cmd_submit(const char *id, const char *url, const char *cat,
                      const char *desc)
{
    static char json[3072], resp[512], hex[65], readme_url[560];
    static char version[48] = "-";
    static char shortdesc[160] = "";
    char descesc[512];
    const char *p;
    long size = 0;
    FILE *f;

    for (p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) {
            printf("amipkg: id must be lowercase letters/digits/dashes: %s\n", id);
            return 5;
        }
    if (strlen(id) < 2 || strlen(id) > 32) { printf("amipkg: id length 2..32.\n"); return 5; }
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        printf("amipkg: the archive URL must start with http:// or https://\n");
        return 5;
    }
    {
        size_t ul = strlen(url);
        if (!(ul > 4 && (strcmp(url + ul - 4, ".lha") == 0 || strcmp(url + ul - 4, ".adf") == 0))) {
            printf("amipkg: the client can only install .lha or .adf archives.\n");
            return 5;
        }
    }
    if (cat && cat[0]) {
        int k, okcat = 0;
        for (k = 0; g_submit_cats[k]; k++)
            if (strcmp(cat, g_submit_cats[k]) == 0) okcat = 1;
        if (!okcat) {
            printf("amipkg: unknown category '%s'. Known: ", cat);
            for (k = 0; g_submit_cats[k]; k++)
                printf("%s%s", k ? ", " : "", g_submit_cats[k]);
            printf("\n");
            return 5;
        }
    }

    /* Instant feedback for the most common rejection: the id already exists.
     * Checked against the LOCAL catalog - offline, before any upload. */
    {
        aidx_index idx;
        if (load_index(&idx) == 0) {
            const aidx_entry *e = aidx_find(&idx, id);
            aidx_free(&idx);
            if (e) {
                printf("amipkg: '%s' is already in the catalog (version %s).\n", id, e->version);
                printf("New versions are picked up automatically by the nightly refresh -\n");
                printf("if this is DIFFERENT software, pick another id.\n");
                return 5;
            }
        }
    }

    amipkg_ensure_dirs();
    printf("Fetching the archive to pin its SHA-256...\n");
    if (http_get_file(url, amipkg_data_path("cache/submit.tmp"), &size) != 0 || size <= 0) {
        printf("amipkg: could not download the archive - nothing submitted.\n");
        return 10;
    }
    if (sha256_of_file(amipkg_data_path("cache/submit.tmp"), hex) != 0) {
        printf("amipkg: cannot hash the download.\n");
        return 10;
    }
    remove(amipkg_data_path("cache/submit.tmp"));

    /* Aminet convention: <archive>.readme next to the archive. Best-effort. */
    snprintf(readme_url, sizeof readme_url, "%s.readme", url);
    f = fopen(amipkg_data_path("cache/submit.readme"), "wb");
    if (f) {
        long rn = 0;
        int ok = http_get(readme_url, f, &rn) == 0;
        fclose(f);
        if (ok && rn > 0) {
            char *text = read_file(amipkg_data_path("cache/submit.readme"));
            if (text) {
                char *ln = strstr(text, "Version:");
                if (ln) { ln += 8; while (*ln == ' ') ln++;
                    { size_t k = 0; while (ln[k] && ln[k] != '\n' && ln[k] != '\r' && k < sizeof version - 1) { version[k] = ln[k]; k++; } version[k] = 0; } }
                ln = strstr(text, "Short:");
                if (ln) { ln += 6; while (*ln == ' ') ln++;
                    { size_t k = 0; while (ln[k] && ln[k] != '\n' && ln[k] != '\r' && k < sizeof shortdesc - 1) { shortdesc[k] = ln[k]; k++; } shortdesc[k] = 0; } }
                free(text);
            }
        }
        remove(amipkg_data_path("cache/submit.readme"));
    }

    json_escape(desc && desc[0] ? desc : (shortdesc[0] ? shortdesc : "(no description - maintainer, please fill in)"),
                descesc, sizeof descesc);
    {
        char veresc[64], urlesc[560];
        json_escape(version, veresc, sizeof veresc);
        json_escape(url, urlesc, sizeof urlesc);
        snprintf(json, sizeof json,
            "{\"id\":\"%s\",\"name\":\"%s\",\"version\":\"%s\","
            "\"category\":\"%s\",\"description\":\"%s\","
            "\"archive\":{\"url\":\"%s\",\"sha256\":\"%s\",\"sizeBytes\":%ld},"
            "\"deps\":[],\"requirements\":{},\"tier\":\"A\","
            "\"submittedVia\":\"amipkg " AMIPKG_VERSION " on-Amiga\"}",
            id, id, veresc, (cat && cat[0]) ? cat : "Utilities",
            descesc, urlesc, hex, size);
    }

    printf("Submitting %s (%s, %ld bytes, sha %.12s...) for review...\n", id, version, size, hex);
    if (http_post_json("http://amiga-imager.org/packages/submit", json, resp, sizeof resp) != 0) {
        printf("amipkg: submission failed%s%s\n", resp[0] ? ": " : ".", resp);
        return 10;
    }
    printf("Submitted! %s\n", resp[0] ? resp : "");
    printf("A maintainer reviews every submission before it is signed into\n");
    printf("the catalog - watch https://github.com/thomas-luebker/amiga-pkg\n");
    return 0;
}

static int cmd_verify(const char *file, const char *expected)
{
    char hex[65];
    if (sha256_of_file(file, hex) != 0) { printf("amipkg: cannot read %s\n", file); return 10; }
    if (strcmp(hex, expected) == 0) { printf("%s: OK\n", file); return 0; }
    printf("%s: MISMATCH\n  expected %s\n  got      %s\n", file, expected, hex);
    return 5;
}

static int cmd_remove(const char *id, int force)
{
    rcpt_installed *inst = amipkg_inst_scratch;   /* shared scratch, see store.h */
    static rcpt_file files[MAX_FILES], other[MAX_FILES];
    size_t ninst = load_installed(inst, MAX_PKGS);
    size_t nfiles, i, j, k;
    int found = 0;
    int deleted = 0, kept_shared = 0, blocked = 0;
    long total_lines = 0;

    for (i = 0; i < ninst; i++)
        if (strcmp(inst[i].id, id) == 0) found = 1;
    if (!found) { printf("amipkg: '%s' is not installed.\n", id); return 5; }

    nfiles = load_files_for(id, files, MAX_FILES);
    if (nfiles == 0)
        printf("amipkg: no file inventory for '%s' - only the DB entry will be removed.\n", id);
    /* Inventories can exceed MAX_FILES (AmiBlitz3: 1100+). This pass handles
     * the first MAX_FILES; the tail is rewritten below and a re-run
     * continues - never silently drop files from a removal. */
    {
        char rp[192], ln[400];
        FILE *tf;
        total_lines = 0;
        snprintf(rp, sizeof rp, "%sdb/files/%s.files", amipkg_prefix(), id);
        tf = fopen(rp, "r");
        if (tf) {
            while (fgets(ln, sizeof ln, tf))
                if (ln[0] && ln[0] != '\n') total_lines++;
            fclose(tf);
        }
    }

    /* First pass: classify. Shared = the path appears in another package's
     * inventory (case-insensitive compare via strcasecmp fallback loop). */
    for (i = 0; i < nfiles; i++) {
        char hex[65];
        int shared = 0;
        for (j = 0; j < ninst && !shared; j++) {
            size_t nother;
            if (strcmp(inst[j].id, id) == 0) continue;
            nother = load_files_for(inst[j].id, other, MAX_FILES);
            for (k = 0; k < nother; k++) {
                /* Amiga FS is case-insensitive. */
                const char *a = files[i].path, *b = other[k].path;
                size_t x = 0;
                while (a[x] && b[x]) {
                    char ca = a[x], cb = b[x];
                    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
                    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
                    if (ca != cb) break;
                    x++;
                }
                if (!a[x] && !b[x]) { shared = 1; break; }
            }
        }
        if (shared) { kept_shared++; files[i].path[0] = '\0'; continue; }
        if (files[i].sha256[0] == '\0') {
            if (!force) { printf("  no digest: %s\n", files[i].path); blocked++; }
            continue;
        }
        if (sha256_of_file(receipt_path(files[i].path), hex) != 0)
            { files[i].path[0] = '\0'; continue; }   /* already gone */
        if (strcmp(hex, files[i].sha256) != 0) {
            if (!force) { printf("  modified: %s\n", files[i].path); blocked++; }
        }
    }
    if (blocked && !force) {
        printf("amipkg: %d file(s) were modified after install or have no digest.\n", blocked);
        printf("Nothing removed. Re-run with FORCE to delete them anyway.\n");
        return 5;
    }

    /* Uninstall tidy-up script (recorded at install from the recipe's
     * removeScript op): runs BEFORE deletion so it can still reference the
     * package's files (undo assigns, ENVARC cleanup). Failure = warning. */
    {
        char rp[192];
        char *sc;
        snprintf(rp, sizeof rp, "%sdb/scripts/%s.remove", amipkg_prefix(), id);
        sc = read_file(rp);
        if (sc) {
            printf("Running %s's uninstall script...\n", id);
            if (amipkg_run_inline_script(sc, "uninstall") >= 10)
                printf("amipkg: WARNING - the uninstall script failed; continuing.\n");
            free(sc);
            remove(rp);
        }
    }

    /* Second pass: delete. amipkg's OWN runtime state (cache/db/config under
     * the prefix) is NEVER deleted through a receipt - a bad inventory (e.g.
     * an old adopt of the home drawer) must not take out the receipt DB, the
     * catalog, or an in-flight extraction. */
    {
        static const char *rt[] = { "cache", "db", "config", NULL };
        int kept_runtime = 0;
        for (i = 0; i < nfiles; i++) {
            int is_rt = 0;
            size_t k;
            if (files[i].path[0] == '\0') continue;
            for (k = 0; rt[k] && !is_rt; k++) {
                const char *root = amipkg_data_path(rt[k]);
                size_t rl = strlen(root), x;
                int eq = strlen(files[i].path) >= rl;
                for (x = 0; eq && x < rl; x++) {
                    char ca = files[i].path[x], cb = root[x];
                    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
                    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
                    if (ca != cb) eq = 0;
                }
                if (eq && (files[i].path[rl] == '\0' || files[i].path[rl] == '/')) is_rt = 1;
            }
            if (is_rt) { kept_runtime++; continue; }
            if (remove(receipt_path(files[i].path)) == 0) deleted++;
        }
        if (kept_runtime)
            printf("Kept %d amipkg runtime file(s) (cache/db/config are never receipt-deleted).\n",
                   kept_runtime);
    }

    /* Continuation: more inventory lines than this pass could load - keep
     * the DB entry, rewrite the inventory to the UNPROCESSED tail, and ask
     * for another run. (Processed kept-shared files were intentionally left
     * on disk and are dropped from the inventory like a completed pass.) */
    if (total_lines > (long)nfiles) {
        char rp[192], tmp[192], ln[400];
        FILE *in, *outf;
        long skip = (long)nfiles, kept = 0;
        snprintf(rp, sizeof rp, "%sdb/files/%s.files", amipkg_prefix(), id);
        snprintf(tmp, sizeof tmp, "%sdb/files/%s.files.new", amipkg_prefix(), id);
        in = fopen(rp, "r"); outf = fopen(tmp, "w");
        if (in && outf) {
            while (fgets(ln, sizeof ln, in)) {
                if (!(ln[0] && ln[0] != '\n')) continue;
                if (skip > 0) { skip--; continue; }
                fputs(ln, outf); kept++;
            }
        }
        if (in) fclose(in);
        if (outf) fclose(outf);
        if (kept > 0) {
            remove(rp);
            amipkg_rename(tmp, rp);
            printf("Removed %d file(s); %ld remain in this large package -\n"
                   "run `amipkg remove %s%s` again to continue.\n",
                   deleted, kept, id, force ? " FORCE" : "");
            return 0;
        }
        remove(tmp);
    }

    /* Rewrite installed.txt without this id. */
    {
        FILE *f = fopen(amipkg_data_path("db/installed.txt"), "wb");
        char line[192];
        if (f) {
            for (i = 0; i < ninst; i++) {
                if (strcmp(inst[i].id, id) == 0) continue;
                rcpt_format_installed_line(&inst[i], line, sizeof line);
                fprintf(f, "%s\n", line);
            }
            fclose(f);
        }
        {
            char p[160];
            char *text;
            snprintf(p, sizeof p, "%sdb/files/%s.files", amipkg_prefix(), id);
            remove(p);
            /* Strip the package's boot-script overlay blocks (ScriptEditor
             * BEGIN/END markers recorded in scripts/<id>.edits). */
            snprintf(p, sizeof p, "%sdb/scripts/%s.edits", amipkg_prefix(), id);
            text = read_file(p);
            if (text) {
                static rcpt_edit edits[32];
                size_t nedits = rcpt_parse_edits(text, edits, 32), k;
                int stripped = 0;
                free(text);
                for (k = 0; k < nedits; k++) {
                    const char *script = edits[k].target[0] ? edits[k].target
                                                            : "S:User-Startup";
                    if (amipkg_strip_overlay(script, edits[k].overlay) == 1)
                        stripped++;
                }
                if (stripped)
                    printf("Stripped %d boot-script overlay block(s).\n", stripped);
                remove(p);
            }
        }
    }
    printf("Removed %s: %d file(s) deleted, %d shared kept.\n", id, deleted, kept_shared);
    return 0;
}

/* `amipkg adopt <id> <drawer> [<version>]` - take over managing an app the
 * user ALREADY has, wherever it lives: inventory the drawer into the receipt
 * DB (digests included, so remove/doctor work) and remember its PARENT as the
 * package's install dir so upgrades land in place. */
/* Drop one id's line from installed.txt (receipt only - files untouched). */
static void receipt_remove_installed(const char *id)
{
    rcpt_installed *inst = amipkg_inst_scratch;   /* shared scratch, see store.h */
    size_t ninst = load_installed(inst, MAX_PKGS), i;
    FILE *f = fopen(amipkg_data_path("db/installed.txt"), "wb");
    char line[192];
    if (!f) return;
    for (i = 0; i < ninst; i++) {
        if (strcmp(inst[i].id, id) == 0) continue;
        rcpt_format_installed_line(&inst[i], line, sizeof line);
        fprintf(f, "%s\n", line);
    }
    fclose(f);
}

static int cmd_adopt(const char *id, const char *drawer, const char *version)
{
    aidx_index idx;
    const aidx_entry *e;
    long files;
    char parent[256];

    if (pkg_installed(id)) {
        /* Re-adopt (tester request: a mistaken adopt must be correctable
         * without deleting anything): drop the old RECEIPT - files on disk
         * are untouched - and inventory the new drawer. */
        char rp[192];
        printf("amipkg: '%s' is already recorded - re-adopting (old receipt replaced, no files touched).\n", id);
        receipt_remove_installed(id);
        snprintf(rp, sizeof rp, "%sdb/files/%s.files", amipkg_prefix(), id);
        remove(rp);
    }
    if (load_index(&idx) != 0) return 10;
    e = aidx_find(&idx, id);
    if (!e) {
        printf("amipkg: '%s' is not in the catalog - adoption links an app to its\n"
               "catalog entry for updates. Check the id with: amipkg avail <term>\n", id);
        aidx_free(&idx);
        return 5;
    }

    files = amipkg_adopt_inventory(id, drawer);
    if (files < 0) { printf("amipkg: cannot read %s\n", drawer); aidx_free(&idx); return 10; }
    if (files == 0) {
        printf("amipkg: %s contains no files - nothing to adopt.\n", drawer);
        aidx_free(&idx);
        return 5;
    }

    /* Remember WHERE it lives: the drawer's parent becomes the package's
     * install dir (a generic install recreates the app drawer inside it). */
    {
        const char *slash = strrchr(drawer, '/');
        if (slash) {
            size_t n = (size_t)(slash - drawer);
            if (n >= sizeof parent) n = sizeof parent - 1;
            memcpy(parent, drawer, n); parent[n] = '\0';
        } else {
            const char *colon = strchr(drawer, ':');
            if (colon) { size_t n = (size_t)(colon - drawer) + 1;
                         if (n >= sizeof parent) n = sizeof parent - 1;
                         memcpy(parent, drawer, n); parent[n] = '\0'; }
            else strcpy(parent, drawer);
        }
        amipkg_set_pkgdir(id, parent);
    }

    /* No version given? Read it off the binaries, the way C:Version does.
     * Recording "-" made the GUIs substitute the CATALOG version, which reads
     * as the installed one until the repo is disabled and it disappears
     * (djbase, 2026-07-29). Detecting it is the honest answer. */
    {
        static char detected[48];
        const char *rec = (version && version[0]) ? version : NULL;
        int auto_found = 0;
        if (!rec && amipkg_detect_version(drawer, id, detected, sizeof detected)) {
            rec = detected;
            auto_found = 1;
        }
        { FILE *f = fopen(amipkg_data_path("db/installed.txt"), "a");
          if (f) { fprintf(f, "%s|%s|%ld|0\n", id, rec ? rec : "-",
                           idx.index_version); fclose(f); } }

        printf("Adopted %s: %ld file(s) at %s\n", id, files, drawer);
        if (rec && auto_found)
            printf("Version detected on disk: %s\n", rec);
        else if (rec)
            printf("Version recorded: %s\n", rec);
        else
            printf("Version recorded: unknown (no $VER found)"
                   " - pass it as `amipkg adopt %s <drawer> <version>`\n", id);
    }
    printf("Future updates install into: %s\n", parent);
    aidx_free(&idx);
    return 0;
}

/* A known command was given without its required argument(s). */
static int usage_needs(const char *cmd, const char *args)
{
    printf("amipkg: '%s' needs %s\n", cmd, args);
    printf("usage: amipkg %s %s\n", cmd, args);
    return 5;
}

/* `amipkg dir [<path>]` - show or set where recipe-less packages install to.
 * With no argument, prints the current destination; with a path, persists it
 * (a lone "-" reverts to the default). */
/* amipkg dir                  show the global install drawer
 * amipkg dir <path>            set it (- clears back to the default)
 * amipkg dir <id> <path>       set ONE package's drawer (- clears)
 * amipkg dir <id> -show        show ONE package's drawer
 *
 * The per-package override already existed - `adopt` writes it so upgrades
 * land where the app really lives - but nothing could set it for a fresh
 * install. That is the gap testers hit: one global drawer is not enough when
 * a tool belongs in C: and everything else belongs in SYS:Programs. */
static int cmd_dir(int argc, char **argv)
{
    char cur[256];
    const char *a1 = (argc >= 3) ? argv[2] : NULL;
    const char *a2 = (argc >= 4) ? argv[3] : NULL;

    if (!a1) {
        amipkg_get_installdir(cur, sizeof cur);
        printf("install dir: %s\n", cur);
        return 0;
    }
    if (a2) {                                  /* per-package form */
        const char *id = a1;
        const char *path = a2;
        if (strcmp(path, "-show") == 0) {
            amipkg_get_pkgdir(id, cur, sizeof cur);
            printf("install dir for '%s': %s\n", id, cur);
            return 0;
        }
        if (strcmp(path, "-") == 0) path = NULL;   /* clear -> follow global */
        if (amipkg_set_pkgdir(id, path) != 0) {
            printf("amipkg: could not save the drawer for '%s'.\n", id);
            return 10;
        }
        amipkg_get_pkgdir(id, cur, sizeof cur);
        if (path) printf("'%s' will install into: %s\n", id, cur);
        else      printf("'%s' now follows the global install dir: %s\n", id, cur);
        return 0;
    }
    if (strcmp(a1, "-") == 0) a1 = NULL;       /* clear -> default */
    if (amipkg_set_installdir(a1) != 0) {
        printf("amipkg: could not write %s\n", amipkg_data_path("config/installdir"));
        return 10;
    }
    amipkg_get_installdir(cur, sizeof cur);
    printf("install dir set to: %s\n", cur);
    return 0;
}

static int dispatch(int argc, char **argv)
{
    int rc = 5;
    if (argc < 2) {
        printf("amipkg " AMIPKG_VERSION " - the AmigaPKG package manager\n");
        printf("usage: amipkg update | list | avail [term|ALL] | check | doctor | info <id> | fetch <id> | install <id> [DIR=<drawer>] [DRYRUN] | submit <id> <url> [CAT=<Category>] [desc] | adopt <id> <drawer> [<ver>] | upgrade [<id>] | repo ... | dir [<path>] | dir <id> <path> | verify <file> <sha256> | remove <id> [FORCE]\n");
        return 5;
    }
    amipkg_ensure_dirs();   /* create AMIPKG:cache + db + config drawers if absent */
    if (strcmp(argv[1], "update") == 0) rc = cmd_update();
    else if (strcmp(argv[1], "repo") == 0) rc = cmd_repo(argc, argv);
    else if (strcmp(argv[1], "upgrade") == 0) rc = cmd_upgrade(argc >= 3 ? argv[2] : NULL);
    else if (strcmp(argv[1], "dir") == 0) rc = cmd_dir(argc, argv);
    else if (strcmp(argv[1], "list") == 0) rc = cmd_list();
    else if (strcmp(argv[1], "avail") == 0) rc = cmd_avail(argc >= 3 ? argv[2] : NULL);
    else if (strcmp(argv[1], "check") == 0) rc = cmd_check();
    else if (strcmp(argv[1], "submit") == 0) {
        if (argc < 4) { printf("usage: amipkg submit <id> <archive-url> [description]\n"); rc = 5; }
        else {
            static char d[512]; static char cat[32]; int i2;
            d[0] = 0; cat[0] = 0;
            for (i2 = 4; i2 < argc; i2++) {
                if (strncmp(argv[i2], "CAT=", 4) == 0) {
                    strncpy(cat, argv[i2] + 4, sizeof cat - 1); cat[sizeof cat - 1] = 0;
                    continue;
                }
                if (d[0]) strncat(d, " ", sizeof d - strlen(d) - 1);
                strncat(d, argv[i2], sizeof d - strlen(d) - 1);
            }
            rc = cmd_submit(argv[2], argv[3], cat, d);
        }
    }
    else if (strcmp(argv[1], "info") == 0)
        rc = argc >= 3 ? cmd_info(argv[2]) : usage_needs("info", "<id>");
    else if (strcmp(argv[1], "fetch") == 0)
        rc = argc >= 3 ? cmd_fetch(argv[2]) : usage_needs("fetch", "<id>");
    else if (strcmp(argv[1], "install") == 0) {
        if (argc < 3) rc = usage_needs("install", "<id> [DIR=<drawer>] [DRYRUN]");
        else {
            int dry = 0, i;
            const char *want_dir = NULL;
            for (i = 3; i < argc; i++) {
                if (strcmp(argv[i], "DRYRUN") == 0) dry = 1;
                else if (strncmp(argv[i], "DIR=", 4) == 0) want_dir = argv[i] + 4;
            }
            /* Recorded BEFORE the install so the choice also governs later
             * upgrades - the same file `adopt` writes. */
            if (want_dir && want_dir[0]) {
                char bare[128], repo[AREPO_ID_MAX];
                arepo_split_spec(argv[2], repo, sizeof repo, bare, sizeof bare);
                if (amipkg_set_pkgdir(bare, want_dir) != 0)
                    printf("amipkg: could not remember the drawer for '%s'.\n", bare);
                else
                    printf("'%s' will install into %s (remembered for updates).\n",
                           bare, want_dir);
            }
            rc = cmd_install2(argv[2], dry);
        }
    }
    else if (strcmp(argv[1], "doctor") == 0) rc = cmd_doctor();
    else if (strcmp(argv[1], "adopt") == 0)
        rc = argc >= 4 ? cmd_adopt(argv[2], argv[3], argc >= 5 ? argv[4] : NULL)
                       : usage_needs("adopt", "<id> <drawer> [<version>]");
    else if (strcmp(argv[1], "verify") == 0)
        rc = argc >= 4 ? cmd_verify(argv[2], argv[3]) : usage_needs("verify", "<file> <sha256>");
    else if (strcmp(argv[1], "remove") == 0)
        rc = argc >= 3 ? cmd_remove(argv[2], argc >= 4 && strcmp(argv[3], "FORCE") == 0)
                       : usage_needs("remove", "<id> [FORCE]");
    else printf("amipkg: unknown command '%s'\n", argv[1]);
    http_cleanup();
    return rc;
}

#ifdef __amigaos__
/* The launching Shell gives us its stack (default 4 KB), which the install path
 * (recursive walk_tree + JSON parse + a few KB of locals) can overflow -> a
 * corrupted return address -> "illegal instruction" (Guru 8000_0004) on real
 * hardware. bebbo's newlib crt0 does NOT honour the `__stack` global (that's a
 * libnix feature), so we guarantee a big stack ourselves: allocate 128 KB and
 * StackSwap onto it before doing any work. Globals (not stack locals) carry
 * argc/argv/rc across the swap - after StackSwap the SP-relative locals of this
 * frame are invalid under -fomit-frame-pointer, so we must not touch them. */
#include <exec/tasks.h>
#include <proto/exec.h>

#define AMIPKG_STACK_BYTES (128UL * 1024UL)

static struct StackSwapStruct g_sss;
static char  *g_stk;
static int    g_argc;
static char **g_argv;
static int    g_rc;

int main(int argc, char **argv)
{
    argc = amiga_rebuild_argv(&argv);   /* newlib crt0 argv fix */
    g_argc = argc; g_argv = argv;
    g_stk = (char *)AllocMem(AMIPKG_STACK_BYTES, MEMF_ANY);
    if (!g_stk) return dispatch(g_argc, g_argv);   /* no RAM: run on the shell stack */
    g_sss.stk_Lower   = (APTR)g_stk;
    g_sss.stk_Upper   = (ULONG)g_stk + AMIPKG_STACK_BYTES;
    g_sss.stk_Pointer = (APTR)((ULONG)g_stk + AMIPKG_STACK_BYTES);
    StackSwap(&g_sss);
    g_rc = dispatch(g_argc, g_argv);    /* all work runs on the 128 KB stack */
    StackSwap(&g_sss);                  /* back to the shell stack */
    FreeMem(g_stk, AMIPKG_STACK_BYTES);
    return g_rc;
}
#else
int main(int argc, char **argv) { return dispatch(argc, argv); }
#endif
