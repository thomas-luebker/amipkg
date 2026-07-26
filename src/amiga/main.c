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
#include "../core/adfread.h" /* extract packages distributed as a raw .adf image */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __amigaos__
/* (Stack is guaranteed by a StackSwap in main() - the newlib crt0 ignores the
 * libnix-style `__stack` global, so we do it ourselves. See main().) */

/* AmigaOS Version-command tag: `Version C:amipkg` reports the exact build. */
static const char verstag[] __attribute__((used)) = "$VER: amipkg 0.4.2 (26.7.2026)";

int http_available(void);
void http_cleanup(void);
int http_get(const char *url, FILE *out, long *bytes_out);
int http_get_file(const char *url, const char *path, long *bytes_out);
int amipkg_extract(const char *archive, const char *destdir);
size_t amipkg_run_recipe(const arecipe *recipe, const char *extract_dir,
                         const char *boot_root, char (*out_paths)[256], size_t max);
size_t amipkg_install_generic(const char *extract_dir, const char *dest_root,
                              char (*out_paths)[256], size_t max);
void amipkg_ensure_dirs(void);
long long amipkg_volume_free(const char *path);   /* -1 = unknown (don't block) */
int amipkg_rename(const char *from, const char *to);   /* dos Rename; 0 = ok */
long amipkg_run_inline_script(const char *script, const char *label);
int amipkg_strip_overlay(const char *script_path, const char *marker);
long amipkg_adopt_inventory(const char *id, const char *drawer);
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
static void http_cleanup(void) {}
static int amipkg_extract(const char *a, const char *d)
{ (void)a; (void)d; return 0; }
static size_t amipkg_run_recipe(const arecipe *rc, const char *e, const char *b,
                                char (*o)[256], size_t m)
{ (void)rc; (void)e; (void)b; (void)o; (void)m;
  printf("amipkg: install is Amiga-only in the host build\n"); return 0; }
static size_t amipkg_install_generic(const char *e, const char *d, char (*o)[256], size_t m)
{ (void)e; (void)d; (void)o; (void)m; return 0; }
static void amipkg_ensure_dirs(void) {}
static long long amipkg_volume_free(const char *p) { (void)p; return -1; }
static int amipkg_rename(const char *f, const char *t) { return rename(f, t); }
static long amipkg_run_inline_script(const char *sc, const char *l)
{ (void)sc; (void)l; printf("amipkg: scripts are Amiga-only in the host build\n"); return 0; }
static int amipkg_strip_overlay(const char *p, const char *m)
{ (void)p; (void)m; return 0; }
static long amipkg_adopt_inventory(const char *i, const char *d)
{ (void)i; (void)d; return -1; }
static const char *amipkg_cpu(void) { return ""; }
static int amipkg_ks_version(void) { return 0; }
/* adf_extract (portable) is linked from adfread.c; provide its mkdir hook. */
#include <sys/stat.h>
int adf_mkdir(const char *path) { mkdir(path, 0777); return 0; }
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

static int load_index(aidx_index *idx)
{
    char *text = read_file(AMIPKG_INDEX_PATH);
    int rc;
    if (!text) {
        printf("amipkg: no catalog at %s\n", AMIPKG_INDEX_PATH);
        printf("Bring your network up and run:  amipkg update\n");
        return 1;
    }
    rc = aidx_parse(text, idx);
    free(text);
    if (rc != 0) printf("amipkg: seeded index is unreadable (error %d)\n", rc);
    return rc;
}

/* ------------------------------------------------------------------ */

static int cmd_list(void)
{
    static rcpt_installed inst[MAX_PKGS];
    size_t n = load_installed(inst, MAX_PKGS), i;
    if (n == 0) { printf("No packages recorded (no receipt DB?).\n"); return 5; }
    printf("%-24s %s\n", "Package", "Version");
    for (i = 0; i < n; i++)
        printf("%-24s %s\n", inst[i].id, inst[i].version);
    return 0;
}

static int cmd_check(void)
{
    static rcpt_installed inst[MAX_PKGS];
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
    if (load_index(&idx) != 0) return 10;
    e = aidx_find(&idx, id);
    if (!e) { printf("amipkg: '%s' is not in the index.\n", id); aidx_free(&idx); return 5; }
    printf("%s - %s (%s)\n", e->id, e->name, e->category);
    if (e->description[0]) printf("  %s\n", e->description);
    printf("  version: %s\n", e->version);
    if (e->added[0]) printf("  added: %s\n", e->added);
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
static int cmd_avail(const char *filter)
{
    static rcpt_installed inst[MAX_PKGS];
    aidx_index idx;
    size_t ninst, i;
    int shown = 0;
    if (load_index(&idx) != 0) return 10;
    ninst = load_installed(inst, MAX_PKGS);
    printf("%-22s %-9s %-14s\n", "Package", "Version", "Category");
    for (i = 0; i < idx.count; i++) {
        const aidx_entry *e = &idx.entries[i];
        if (filter && !(contains_ci(e->id, filter) || contains_ci(e->name, filter)
                        || contains_ci(e->category, filter)))
            continue;
        printf("%-22s %-9s %-14s %s\n", e->id, e->version[0] ? e->version : "-",
               e->category, id_installed(inst, ninst, e->id) ? "[installed]" : "");
        shown++;
    }
    if (filter) printf("%d package(s) match \"%s\" (of %lu).\n", shown, filter, (unsigned long)idx.count);
    else        printf("%lu package(s) available.\n", (unsigned long)idx.count);
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
static int cmd_update(void)
{
    const char *base = getenv("AMIPKG_REPO_URL");
    char url[512];
    char *json = NULL, *sig = NULL;
    long bytes = 0;
    size_t jl, sl;
    FILE *out;
    const char *jnew = AMIPKG_CACHE_DIR "packages.json.new";
    const char *snew = AMIPKG_CACHE_DIR "packages.json.sig.new";
    if (!base || !base[0]) base = AMIPKG_UPDATE_BASE;

    printf("Updating catalog from %s ...\n", base);
    snprintf(url, sizeof url, "%s/packages.json", base);
    if (!(out = fopen(jnew, "wb"))) { printf("amipkg: cannot write %s\n", jnew); return 10; }
    if (http_get(url, out, &bytes) != 0) { fclose(out); remove(jnew); return 10; }
    fclose(out);
    snprintf(url, sizeof url, "%s/packages.json.sig", base);
    if (!(out = fopen(snew, "wb"))) { printf("amipkg: cannot write cache\n"); remove(jnew); return 10; }
    if (http_get(url, out, &bytes) != 0) { fclose(out); remove(jnew); remove(snew); return 10; }
    fclose(out);

    json = read_file(jnew);
    sig  = read_file(snew);
    if (!json || !sig) { printf("amipkg: could not read the download.\n"); goto fail; }
    jl = strlen(json);                       /* JSON is text (no NUL) -> exact byte length */
    sl = strlen(sig);
    while (sl && (sig[sl-1] == '\n' || sig[sl-1] == '\r' || sig[sl-1] == ' ' || sig[sl-1] == '\t'))
        sig[--sl] = '\0';

    if (!amipkg_verify_index((const unsigned char *)json, jl, sig)) {
        printf("amipkg: SIGNATURE DID NOT VERIFY - keeping the current catalog.\n");
        goto fail;
    }
    /* Verified: install it as the new seeded index (+ keep the .sig alongside). */
    if (!(out = fopen(AMIPKG_INDEX_PATH, "wb"))) { printf("amipkg: cannot write %s\n", AMIPKG_INDEX_PATH); goto fail; }
    fwrite(json, 1, jl, out); fclose(out);
    if ((out = fopen(AMIPKG_INDEX_PATH ".sig", "wb"))) { fwrite(sig, 1, sl, out); fclose(out); }
    remove(jnew); remove(snew);
    free(json); free(sig);
    printf("Catalog updated + signature verified. Run 'amipkg avail' to see it.\n");
    return 0;
fail:
    free(json); free(sig);
    remove(jnew); remove(snew);
    return 10;
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
    snprintf(dest_out, dest_sz, AMIPKG_CACHE_DIR "%s", basename_of(e->archive_url));
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
    if (load_index(&idx) != 0) return 10;
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
    snprintf(rp, sizeof rp, AMIPKG_DB_PREFIX "files/%s.files", id);
    f = fopen(rp, "a");
    if (!f) return;
    if (sha256_of_file(path, hex) == 0) fprintf(f, "%s|%s\n", path, hex);
    else                                fprintf(f, "%s\n", path);
    fclose(f);
}

static int cmd_remove(const char *id, int force);   /* defined below */

/* Is `id` recorded in the installed receipt DB? */
static int pkg_installed(const char *id)
{
    static rcpt_installed inst[MAX_PKGS];
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
        freeb = amipkg_volume_free(AMIPKG_CACHE_DIR);
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
    const char *root;
    aj_node *tree;
    const aj_node *entry_obj;
    static arecipe recipe;   /* ~32 KB - keep off the Shell stack; install_entry
                                runs sequentially, never nested */
    char archive[256];
    static char paths[256][256];
    size_t n, i;
    int rc;

    tree = NULL;
    if (!e->archive_url[0]) {
        printf("amipkg: '%s' has no downloadable archive - install it at build time / via the Mac app.\n", e->id);
        return 5;
    }

    /* Packages with a portable recipe run it; the fetch-only Aminet entries fall
     * back to a generic install (extract into the configured install drawer). */
    if (e->has_recipe) {
        char *json = read_file(AMIPKG_INDEX_PATH);
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
        snprintf(archive, sizeof archive, AMIPKG_CACHE_DIR "%s", basename_of(e->archive_url));
        if (!space_ok(e, archive, vol)) { if (tree) ajson_free(tree); return 5; }
    }

    rc = fetch_verified(e, archive, sizeof archive);
    if (rc != 0) { if (tree) ajson_free(tree); return rc; }

    printf("Unpacking + installing %s...\n", e->id);
    if (archive_is_adf(archive)) {
        /* Package shipped as a raw Amiga floppy image: read its OFS/FFS tree. */
        if (adf_extract(archive, AMIPKG_CACHE_DIR "extract") < 0) {
            printf("amipkg: not a readable OFS/FFS ADF.\n");
            if (tree) ajson_free(tree); return 10;
        }
    } else if (!amipkg_extract(archive, AMIPKG_CACHE_DIR "extract")) {
        printf("amipkg: extraction failed (C:lha present?).\n");
        if (tree) ajson_free(tree); return 10;
    }
    /* Upgrade path: the new version is now downloaded + extracted, so it's safe
     * to remove the previously-installed version (files + receipt) first. Doing
     * it AFTER extraction means a failed download never destroys the install. */
    if (pkg_installed(e->id)) {
        printf("Upgrading %s - removing the previous version...\n", e->id);
        cmd_remove(e->id, 1 /*force: an upgrade replaces its own files*/);
    }
    if (e->has_recipe) {
        root = getenv("AMIPKG_ROOT"); if (!root || !root[0]) root = "SYS:";
        n = amipkg_run_recipe(&recipe, AMIPKG_CACHE_DIR "extract", root, paths, 256);
    } else {
        /* Generic: drop the extracted tree into the configured install drawer
         * (default SYS:Programs; overridable via `amipkg dir` / AMIPKG_INSTALLDIR).
         * An ADF unpacks LOOSE files at its root (no top drawer), so wrap those
         * in a <id> drawer; an LHA usually already carries its own top drawer. */
        char dir[256], dest[320];
        amipkg_get_pkgdir(e->id, dir, sizeof dir);   /* adopt override, else global */
        if (archive_is_adf(archive)) snprintf(dest, sizeof dest, "%s/%s", dir, e->id);
        else                         snprintf(dest, sizeof dest, "%s", dir);
        printf("(no recipe - installing into %s)\n", dest);
        n = amipkg_install_generic(AMIPKG_CACHE_DIR "extract", dest, paths, 256);
    }
    if (n == 0) {
        printf("amipkg: nothing installed (no files matched / not runnable here).\n");
        if (tree) ajson_free(tree); return 10;
    }
    for (i = 0; i < n; i++) receipt_record_file(e->id, paths[i]);
    if (e->has_recipe) {
        /* Record boot-script edits (scripts/<id>.edits, target|marker|version)
         * so `remove` can strip the overlay blocks, and save any removeScript
         * lines (scripts/<id>.remove) to run at uninstall - the receipt owns
         * them so removal works even after the catalog moves on. */
        char rp[192];
        FILE *f;
        size_t k;
        snprintf(rp, sizeof rp, AMIPKG_DB_PREFIX "scripts/%s.edits", e->id);
        f = NULL;
        for (k = 0; k < recipe.op_count; k++) {
            if (recipe.ops[k].type != AROP_SCRIPT_INJECT) continue;
            if (!f) f = fopen(rp, "w");
            if (f) fprintf(f, "S:User-Startup|%s|amipkg\n", recipe.ops[k].marker);
        }
        if (f) fclose(f);
        snprintf(rp, sizeof rp, AMIPKG_DB_PREFIX "scripts/%s.remove", e->id);
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
    { FILE *f = fopen(AMIPKG_DB_PREFIX "installed.txt", "a");
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

/* `amipkg install <id> [DRYRUN]` - resolve dependencies (topological,
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

    if (load_index(&idx) != 0) return 10;
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
    static rcpt_installed inst[MAX_PKGS];
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
    static rcpt_installed inst[MAX_PKGS];
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
            printf("Upgrade %-20s %s -> %s\n", inst[i].id, inst[i].version, e->version);
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
        if (r != 0) { printf("amipkg: upgrade of '%s' failed (rc %d).\n", todo[i], r); rc = r; }
    }
    printf("Upgraded %lu package(s).\n", (unsigned long)ntodo);
    return rc;
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
    static rcpt_installed inst[MAX_PKGS];
    static rcpt_file files[MAX_FILES], other[MAX_FILES];
    size_t ninst = load_installed(inst, MAX_PKGS);
    size_t nfiles, i, j, k;
    int found = 0;
    int deleted = 0, kept_shared = 0, blocked = 0;

    for (i = 0; i < ninst; i++)
        if (strcmp(inst[i].id, id) == 0) found = 1;
    if (!found) { printf("amipkg: '%s' is not installed.\n", id); return 5; }

    nfiles = load_files_for(id, files, MAX_FILES);
    if (nfiles == 0)
        printf("amipkg: no file inventory for '%s' - only the DB entry will be removed.\n", id);

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
        snprintf(rp, sizeof rp, AMIPKG_DB_PREFIX "scripts/%s.remove", id);
        sc = read_file(rp);
        if (sc) {
            printf("Running %s's uninstall script...\n", id);
            if (amipkg_run_inline_script(sc, "uninstall") >= 10)
                printf("amipkg: WARNING - the uninstall script failed; continuing.\n");
            free(sc);
            remove(rp);
        }
    }

    /* Second pass: delete. */
    for (i = 0; i < nfiles; i++) {
        if (files[i].path[0] == '\0') continue;
        if (remove(receipt_path(files[i].path)) == 0) deleted++;
    }

    /* Rewrite installed.txt without this id. */
    {
        FILE *f = fopen(AMIPKG_DB_PREFIX "installed.txt", "wb");
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
            snprintf(p, sizeof p, AMIPKG_DB_PREFIX "files/%s.files", id);
            remove(p);
            /* Strip the package's boot-script overlay blocks (ScriptEditor
             * BEGIN/END markers recorded in scripts/<id>.edits). */
            snprintf(p, sizeof p, AMIPKG_DB_PREFIX "scripts/%s.edits", id);
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
    static rcpt_installed inst[MAX_PKGS];
    size_t ninst = load_installed(inst, MAX_PKGS), i;
    FILE *f = fopen(AMIPKG_DB_PREFIX "installed.txt", "wb");
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
        snprintf(rp, sizeof rp, AMIPKG_DB_PREFIX "files/%s.files", id);
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

    { FILE *f = fopen(AMIPKG_DB_PREFIX "installed.txt", "a");
      if (f) { fprintf(f, "%s|%s|%ld|0\n", id,
                       version && version[0] ? version : "-",
                       idx.index_version); fclose(f); } }

    printf("Adopted %s: %ld file(s) at %s\n", id, files, drawer);
    printf("Version recorded: %s%s\n",
           version && version[0] ? version : "unknown",
           version && version[0] ? "" : " - `amipkg check` will offer a reinstall");
    printf("Future upgrades install into: %s\n", parent);
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
static int cmd_dir(const char *path)
{
    char cur[256];
    if (!path) {
        amipkg_get_installdir(cur, sizeof cur);
        printf("install dir: %s\n", cur);
        return 0;
    }
    if (strcmp(path, "-") == 0) path = NULL;   /* clear -> default */
    if (amipkg_set_installdir(path) != 0) {
        printf("amipkg: could not write %s\n", AMIPKG_INSTALLDIR_FILE);
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
        printf("amipkg 0.4.2 - the AmigaPKG package manager\n");
        printf("usage: amipkg update | list | avail [term] | check | doctor | info <id> | fetch <id> | install <id> [DRYRUN] | adopt <id> <drawer> [<ver>] | upgrade [<id>] | dir [<path>] | verify <file> <sha256> | remove <id> [FORCE]\n");
        return 5;
    }
    amipkg_ensure_dirs();   /* create AMIPKG:cache + db + config drawers if absent */
    if (strcmp(argv[1], "update") == 0) rc = cmd_update();
    else if (strcmp(argv[1], "upgrade") == 0) rc = cmd_upgrade(argc >= 3 ? argv[2] : NULL);
    else if (strcmp(argv[1], "dir") == 0) rc = cmd_dir(argc >= 3 ? argv[2] : NULL);
    else if (strcmp(argv[1], "list") == 0) rc = cmd_list();
    else if (strcmp(argv[1], "avail") == 0) rc = cmd_avail(argc >= 3 ? argv[2] : NULL);
    else if (strcmp(argv[1], "check") == 0) rc = cmd_check();
    else if (strcmp(argv[1], "info") == 0)
        rc = argc >= 3 ? cmd_info(argv[2]) : usage_needs("info", "<id>");
    else if (strcmp(argv[1], "fetch") == 0)
        rc = argc >= 3 ? cmd_fetch(argv[2]) : usage_needs("fetch", "<id>");
    else if (strcmp(argv[1], "install") == 0)
        rc = argc >= 3 ? cmd_install2(argv[2], argc >= 4 && strcmp(argv[3], "DRYRUN") == 0)
                       : usage_needs("install", "<id> [DRYRUN]");
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
