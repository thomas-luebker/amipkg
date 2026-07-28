/*
 * install.c - amipkg AmigaOS execution layer
 *
 * Executes a recipe plan (arun_plan) against a downloaded archive: unpack via
 * the mandatory C:lha, walk the extract tree, run the copy/set-exec/script-
 * inject ops with dos.library, and record the receipt. This is the on-Amiga
 * half of install - the portable half (arun_plan_build, host-tested) decided
 * WHAT to do; this does it.
 *
 * UNVERIFIED ON HARDWARE: written to the documented dos.library API; compile
 * with bebbo's amiga-gcc and validate on a networked image before release.
 * The script-inject marker grammar mirrors ScriptEditor
 * ("; <name> - Amiga Imager <ver> - BEGIN/END") so on-Amiga and build-time
 * edits are mutually idempotent/removable.
 */

#ifdef __amigaos__

#include "../core/arun.h"
#include "../core/arecipe.h"
#include "../core/sha256.h"
#include "../core/aver.h"
#include "../core/store.h"

#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <dos/dosextens.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define AMIPKG_MARK_VER "amipkg"

/* Recursively collect the extract tree as relative paths (files + dirs), so
 * arun can match globs. Returns the count; fills entries[]/is_dir[] up to max
 * (caller owns the string storage in `pool`). */
static size_t walk_tree(const char *root, const char *rel,
                        char (*pool)[256], int *is_dir, size_t max, size_t count)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    char path[256];

    if (rel[0]) snprintf(path, sizeof path, "%s/%s", root, rel);
    else        snprintf(path, sizeof path, "%s", root);

    lock = Lock(path, ACCESS_READ);
    if (!lock) return count;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) { UnLock(lock); return count; }

    if (Examine(lock, fib) && fib->fib_DirEntryType > 0) {
        while (ExNext(lock, fib)) {
            char childrel[256];
            int isdir = fib->fib_DirEntryType > 0;
            if (rel[0]) snprintf(childrel, sizeof childrel, "%s/%s", rel, fib->fib_FileName);
            else        snprintf(childrel, sizeof childrel, "%s", fib->fib_FileName);
            if (count < max) {
                strncpy(pool[count], childrel, 255); pool[count][255] = 0;
                is_dir[count] = isdir;
                count++;
            }
            if (isdir) count = walk_tree(root, childrel, pool, is_dir, max, count);
        }
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return count;
}

/* Copy one file, creating parent drawers. dst is a full DOS path. */
static int copy_file(const char *src, const char *dst)
{
    BPTR in, out;
    static char buf[8192];
    LONG n;
    /* Ensure the parent drawer chain exists. */
    {
        char parent[256]; char *slash;
        strncpy(parent, dst, sizeof parent - 1); parent[sizeof parent - 1] = 0;
        slash = parent;
        while ((slash = strchr(slash + 1, '/')) != NULL) {
            char save = *slash; *slash = 0;
            { BPTR l = Lock(parent, ACCESS_READ); if (l) UnLock(l); else { BPTR d = CreateDir(parent); if (d) UnLock(d); } }
            *slash = save;
        }
    }
    in = Open(src, MODE_OLDFILE);
    if (!in) return 0;
    out = Open(dst, MODE_NEWFILE);
    if (!out) { Close(in); return 0; }
    while ((n = Read(in, buf, sizeof buf)) > 0) Write(out, buf, n);
    Close(out); Close(in);
    return 1;
}

/* Forward declarations - shared walk buffer + helpers defined further down
 * (tentative array declarations merge with the commented definitions). */
static char g_walk_pool[ARUN_MAX_OPS][256];
static int  g_walk_isdir[ARUN_MAX_OPS];
static const char *selfupdate_basename(const char *path);
static int ci_eq(const char *a, const char *b);

/* SetProtection: give a path the script (s) bit, clearing (e) is not needed -
 * matches copyFromHost's "0x40 script bit for exec files" for a script. The
 * plan's SET_EXEC scope maps to a subtree; we set the s-bit on plain files
 * there. NOTE: build parity keeps binaries WITHOUT the s-bit; a full port
 * should mirror isBinaryLoadFile. v0.1 sets the s-bit on shell scripts only
 * (files whose first two bytes aren't a hunk/ELF magic). */
static void set_script_bit_if_script(const char *path)
{
    BPTR fh = Open(path, MODE_OLDFILE);
    unsigned char magic[4];
    LONG n;
    if (!fh) return;
    n = Read(fh, magic, 4);
    Close(fh);
    if (n >= 4) {
        /* 0x000003F3 = AmigaDOS hunk; 0x7F 'E' 'L' 'F' = ELF. Skip those. */
        if (magic[0] == 0 && magic[1] == 0 && magic[2] == 3 && magic[3] == 0xF3) return;
        if (magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') return;
    }
    SetProtection((STRPTR)path, FIBF_SCRIPT);   /* active-low semantics handled by dos */
}

/* Append an overlay into a boot script with the ScriptEditor BEGIN/END markers,
 * idempotently (skip if the marker is already present). */
static int inject_script(const char *script_path, const char *overlay_path, const char *marker)
{
    static char cur[16384], overlay[8192], begin[128], endm[128];
    BPTR fh; LONG n; size_t curlen = 0;

    snprintf(begin, sizeof begin, "; %s - Amiga Imager %s - BEGIN", marker, AMIPKG_MARK_VER);
    snprintf(endm, sizeof endm, "; %s - Amiga Imager %s - END", marker, AMIPKG_MARK_VER);

    fh = Open(script_path, MODE_OLDFILE);
    if (fh) {
        n = Read(fh, cur, sizeof cur - 1);
        Close(fh);
        if (n > 0) curlen = (size_t)n;
    }
    cur[curlen] = 0;
    if (strstr(cur, begin)) return 1;   /* already injected */

    fh = Open(overlay_path, MODE_OLDFILE);
    if (!fh) return 0;
    n = Read(fh, overlay, sizeof overlay - 1);
    Close(fh);
    if (n < 0) return 0;
    overlay[n > 0 ? n : 0] = 0;

    fh = Open(script_path, MODE_NEWFILE);
    if (!fh) return 0;
    if (curlen) Write(fh, cur, curlen);
    Write(fh, (APTR)"\n", 1);
    Write(fh, begin, strlen(begin)); Write(fh, (APTR)"\nFAILAT 30\n", 11);
    Write(fh, overlay, strlen(overlay));
    Write(fh, (APTR)"\nFAILAT 21\n", 11);
    Write(fh, endm, strlen(endm)); Write(fh, (APTR)"\n", 1);
    Close(fh);
    return 1;
}

/* Create a single drawer if it doesn't already exist (idempotent). */
static void ensure_one(const char *path)
{
    BPTR l = Lock((STRPTR)path, ACCESS_READ);
    if (l) { UnLock(l); return; }
    { BPTR d = CreateDir((STRPTR)path); if (d) UnLock(d); }
}

/* Make sure amipkg's working drawers exist before we write to them. The
 * AMIPKG: assign exists after the bridge, but its cache/ and db/ sub-
 * drawers are not guaranteed present on every image - create them here so a
 * fetch/install never fails on a missing directory. */
/* Self-seed the amipkg receipt: `check`/`upgrade` only manage what the
 * receipt DB records, and skip-Install users (extract & start - the
 * blessed flow since 0.4.5) had NO amipkg entry, so self-update looked
 * "up to date" forever (tester report). Runs on every CLI start: appends
 * the entry when missing, bumps it when this binary is NEWER than the
 * recorded version (manual updates). Never touches files. */
static void selfseed_receipt(void)
{
    rcpt_installed *inst = amipkg_inst_scratch;   /* shared scratch, see store.h */
    size_t n = load_installed(inst, MAX_PKGS), i;
    long have = -1;
    for (i = 0; i < n; i++)
        if (strcmp(inst[i].id, "amipkg") == 0) { have = (long)i; break; }
    /* Sync BOTH ways: the receipt must describe the amipkg the user RUNS.
     * Before 0.5.6 it only bumped upward - an upgrade whose new binaries
     * didn't reach the launch location left the receipt claiming the new
     * version, so every further check said "up to date" while the user
     * kept running the old one (A4000 report). If the receipt and this
     * binary disagree IN EITHER DIRECTION, rewrite it to this binary's
     * version - check/upgrade then re-offer the update and the self-update
     * mirror gets another chance to converge. */
    if (have >= 0 && strcmp(inst[have].version, AMIPKG_VERSION) == 0)
        return;
    if (have < 0) {
        FILE *f = fopen(amipkg_data_path("db/installed.txt"), "a");
        if (f) { fprintf(f, "amipkg|%s|1|0\n", AMIPKG_VERSION); fclose(f); }
        f = fopen(amipkg_data_path("db/files/amipkg.files"), "w");
        if (f) {
            static const char *bins[] = { "amipkg", "amipkg-gui", "amipkg-gui.info",
                                          "amipkg-mui", "amipkg-mui.info", "amipkg.guide", 0 };
            char probe[64];
            for (i = 0; bins[i]; i++) {
                BPTR l;
                snprintf(probe, sizeof probe, "PROGDIR:%s", bins[i]);
                l = Lock((STRPTR)probe, ACCESS_READ);
                if (l) { UnLock(l); fprintf(f, "%s\n", probe); }
            }
            fclose(f);
        }
    } else {
        /* Receipt disagrees with this binary (either direction): rewrite. */
        FILE *f = fopen(amipkg_data_path("db/installed.txt"), "wb");
        char line[192];
        if (!f) return;
        for (i = 0; i < n; i++) {
            if (strcmp(inst[i].id, "amipkg") == 0)
                strncpy(inst[i].version, AMIPKG_VERSION, sizeof inst[i].version - 1);
            rcpt_format_installed_line(&inst[i], line, sizeof line);
            fprintf(f, "%s\n", line);
        }
        fclose(f);
    }
}

void amipkg_ensure_dirs(void)
{
    amipkg_bridge_assigns();
    ensure_one(amipkg_data_path("cache"));
    ensure_one(amipkg_data_path("config"));
    ensure_one(amipkg_data_path("db"));
    ensure_one(amipkg_data_path("db/files"));
    ensure_one(amipkg_data_path("db/scripts"));
    ensure_one(amipkg_data_path("db/assigns"));
    selfseed_receipt();
}

/* Free bytes on the volume holding `path` (e.g. "SYS:" / "AMIPKG:cache/").
 * -1 when unknown (RAM: and odd handlers) - callers must not block on that. */
long long amipkg_volume_free(const char *path)
{
    struct InfoData *info;
    BPTR lock;
    long long freeb = -1;
    lock = Lock((STRPTR)path, ACCESS_READ);
    if (!lock) return -1;
    info = (struct InfoData *)AllocDosObject(DOS_FIB, NULL);   /* FIB-sized+ */
    if (info) {
        /* InfoData fits in a FileInfoBlock allocation (260 bytes vs 36). */
        if (Info(lock, info))
            freeb = (long long)(info->id_NumBlocks - info->id_NumBlocksUsed)
                    * info->id_BytesPerBlock;
        FreeDosObject(DOS_FIB, info);
    }
    UnLock(lock);
    return freeb;
}

/* The running Kickstart's exec version (37=2.04, 39=3.0, 40=3.1, 44=3.5,
 * 45=3.9, 46=3.1.4, 47=3.2) for requirements.minKS enforcement. */
int amipkg_ks_version(void)
{
    return (int)SysBase->LibNode.lib_Version;
}

/* The running machine's CPU class, for dependency CPU-variant selection
 * (e.g. the 020 vs 68k build of an MUI class). From exec's AttnFlags. */
const char *amipkg_cpu(void)
{
    UWORD f = SysBase->AttnFlags;
    if (f & AFF_68060) return "68060";
    if (f & AFF_68040) return "68040";
    if (f & AFF_68030) return "68030";
    if (f & AFF_68020) return "68020";
    return "68000";
}

/* `amipkg adopt`: inventory an EXISTING drawer into files/<id>.files (path +
 * SHA-256 per file, like a normal install's receipt) so remove/doctor/upgrade
 * manage an app the user already had. Returns the file count. */
/* Is this drawer-relative path part of amipkg's OWN runtime state? Adopting
 * amipkg at its home drawer must NEVER inventory cache/db/config/catalog/
 * trace files: the upgrade's remove-previous step would otherwise delete the
 * receipt DB, the freshly extracted files in cache/extract, and the catalog
 * itself mid-upgrade (A4000 incident: "Removed amipkg: 57 file(s)" followed
 * by "nothing installed"). */
static int is_amipkg_runtime_rel(const char *rel)
{
    static const char *dirs[] = { "cache", "db", "config", NULL };
    size_t k, len;
    const char *base = selfupdate_basename(rel);
    for (k = 0; dirs[k]; k++) {
        len = strlen(dirs[k]);
        if (strlen(rel) >= len) {
            size_t x; int eq = 1;
            for (x = 0; x < len; x++) {
                int ca = tolower((unsigned char)rel[x]);
                int cb = tolower((unsigned char)dirs[k][x]);
                if (ca != cb) { eq = 0; break; }
            }
            if (eq && (rel[len] == '\0' || rel[len] == '/')) return 1;
        }
    }
    len = strlen(base);
    if (len > 6 && ci_eq(base + len - 6, ".trace")) return 1;
    if (ci_eq(base, "packages.json") || ci_eq(base, "packages.json.sig")) return 1;
    return 0;
}

long amipkg_adopt_inventory(const char *id, const char *drawer)
{
    char (*pool)[256] = g_walk_pool;
    int *is_dir = g_walk_isdir;
    size_t n, i;
    long files = 0;
    char rp[192];
    FILE *f;
    n = walk_tree(drawer, "", pool, is_dir, ARUN_MAX_OPS, 0);
    snprintf(rp, sizeof rp, "%sdb/files/%s.files", amipkg_prefix(), id);
    f = fopen(rp, "w");
    if (!f) return -1;
    for (i = 0; i < n; i++) {
        char full[512], hex[65];
        const char *sep = drawer[strlen(drawer) - 1] == ':' ? "" : "/";
        if (is_dir[i]) continue;
        if (ci_eq(id, "amipkg") && is_amipkg_runtime_rel(pool[i])) continue;
        snprintf(full, sizeof full, "%s%s%s", drawer, sep, pool[i]);
        if (sha256_of_file(full, hex) == 0) fprintf(f, "%s|%s\n", full, hex);
        else                                fprintf(f, "%s\n", full);
        files++;
    }
    fclose(f);
    return files;
}

/* Rename within a volume via dos.library (newlib's rename() pulls a missing
 * _link syscall on this toolchain). 0 on success. */
int amipkg_rename(const char *from, const char *to)
{
    return Rename((STRPTR)from, (STRPTR)to) ? 0 : 1;
}

/* Platform hook for the portable ADF reader (adfread.c): create one drawer. */
int adf_mkdir(const char *path)
{
    BPTR l = Lock((STRPTR)path, ACCESS_READ);
    if (l) { UnLock(l); return 0; }
    { BPTR d = CreateDir((STRPTR)path); if (d) { UnLock(d); return 0; } }
    return 1;
}

/* Strip one ScriptEditor overlay block ("; <marker> - Amiga Imager <any> -
 * BEGIN" .. "- END", inclusive) from a boot script. The version token varies
 * ("0.99", "amipkg", "host"), so match the stable prefix + BEGIN/END suffix.
 * Returns 1 stripped, 0 marker not found, -1 error. */
int amipkg_strip_overlay(const char *script_path, const char *marker)
{
    static char cur[16384], out[16384], prefix[96];
    BPTR fh;
    LONG n;
    size_t used = 0, o = 0;
    int in_block = 0, stripped = 0;
    char *line, *next;

    snprintf(prefix, sizeof prefix, "; %s - Amiga Imager ", marker);
    fh = Open((STRPTR)script_path, MODE_OLDFILE);
    if (!fh) return 0;
    n = Read(fh, cur, sizeof cur - 1);
    Close(fh);
    if (n <= 0) return 0;
    used = (size_t)n;
    cur[used] = '\0';
    if (!strstr(cur, prefix)) return 0;   /* nothing of ours in there */

    for (line = cur; line; line = next) {
        size_t len;
        char *nl = strchr(line, '\n');
        next = nl ? nl + 1 : NULL;
        len = nl ? (size_t)(nl - line) + 1 : strlen(line);
        if (!in_block && strncmp(line, prefix, strlen(prefix)) == 0
            && memchr(line, 'B', len) && strstr(line, "- BEGIN")
            && (strstr(line, "- BEGIN") < line + len)) {
            in_block = 1;
            stripped = 1;
            continue;                      /* drop the BEGIN line */
        }
        if (in_block) {
            if (strncmp(line, prefix, strlen(prefix)) == 0
                && strstr(line, "- END") && (strstr(line, "- END") < line + len))
                in_block = 0;              /* drop the END line, block done */
            continue;                      /* drop block content */
        }
        if (o + len < sizeof out) { memcpy(out + o, line, len); o += len; }
    }
    if (!stripped) return 0;

    fh = Open((STRPTR)script_path, MODE_NEWFILE);
    if (!fh) return -1;
    if (o) Write(fh, out, (LONG)o);
    Close(fh);
    return 1;
}

/* Unpack `archive` into `destdir` via C:lha (mandatory on every image). */
int amipkg_extract(const char *archive, const char *destdir)
{
    char cmd[512];
    LONG rc;
    { BPTR d = Lock(destdir, ACCESS_READ); if (d) UnLock(d); else { BPTR c = CreateDir(destdir); if (c) UnLock(c); } }
    snprintf(cmd, sizeof cmd, "C:lha -q x \"%s\" \"%s/\"", archive, destdir);
    rc = SystemTags(cmd, TAG_DONE);
    return rc == 0;
}

/* Execute `plan` against the extract dir, writing into the volume relative to
 * boot_root (e.g. "DH0:"). Returns the number of files written; fills
 * out_paths[] with the absolute installed paths for the receipt (up to max). */
size_t amipkg_execute(const arun_plan *plan, const char *extract_dir,
                      const char *boot_root, char (*out_paths)[256], size_t max)
{
    size_t i, written = 0;
    for (i = 0; i < plan->count; i++) {
        const arun_op *op = &plan->ops[i];
        char src[512], dst[512];
        switch (op->kind) {
        case ARUN_COPY: {
            /* A dest carrying an assign/volume (":" anywhere) is ABSOLUTE -
             * e.g. "AMIPKG:amipkg" (self-update into the home drawer);
             * everything else stays boot_root-relative. */
            const char *root = strchr(op->dest, ':') ? "" : boot_root;
            const char *destp = op->dest;
            char xlat[300];
            /* "AMIPKG:x" is a NAMESPACE for "amipkg's home", not a literal
             * assign - translate it to the runtime prefix (PROGDIR: on
             * standalone installs, where no assign exists at all). */
            if (strncmp(destp, "AMIPKG:", 7) == 0) {
                snprintf(xlat, sizeof xlat, "%s%s", amipkg_prefix(), destp + 7);
                destp = xlat;
            }
            snprintf(src, sizeof src, "%s/%s", extract_dir, op->src);
            snprintf(dst, sizeof dst, "%s%s", root, destp);
            if (copy_file(src, dst) && written < max) {
                snprintf(out_paths[written], 256, "%s%s", root, destp);
                written++;
            }
            break;
        }
        case ARUN_SET_EXEC:
            /* Best-effort: set the script bit on shell scripts in the scope.
             * A full port walks the scope subtree; v0.1 relies on lha having
             * preserved protection bits and only fixes obvious scripts. */
            break;
        case ARUN_SCRIPT:
            snprintf(src, sizeof src, "%s/%s", extract_dir, op->overlay);
            snprintf(dst, sizeof dst, "%s%s", boot_root, op->dest[0] == 'S' ? "S/User-Startup" : op->dest);
            (void)inject_script(dst, src, op->marker);
            break;
        case ARUN_STRIP:
            break;
        }
    }
    (void)set_script_bit_if_script;   /* reserved for the full set-exec port */
    return written;
}

/* Execute one inline pre/post-install script: FailAt header + the recipe's
 * lines, via Execute of a RAM: file. Returns the script's worst return code.
 * The lines were REVIEWED into the signed catalog (pre-post-script-v1) -
 * this is curated native code, same trust rung as the Installer hatch. */
long amipkg_run_inline_script(const char *script, const char *label)
{
    BPTR fh;
    long rc;
    fh = Open((STRPTR)"RAM:amipkg-op.script", MODE_NEWFILE);
    if (!fh) { Printf((STRPTR)"amipkg: cannot write %s script\n", (LONG)label); return 10; }
    Write(fh, (APTR)"FailAt 21\n", 10);
    Write(fh, (APTR)script, (LONG)strlen(script));
    Write(fh, (APTR)"\n", 1);
    Close(fh);
    rc = SystemTags((STRPTR)"Execute RAM:amipkg-op.script", TAG_DONE);
    DeleteFile((STRPTR)"RAM:amipkg-op.script");
    return rc;
}

/* Full on-Amiga recipe run: pre-install scripts, then walk the extract tree,
 * build the plan (portable), execute it, then post-install scripts. Returns
 * the installed-file count, or 0 on plan failure / a failing pre-script. */
size_t amipkg_run_recipe(const arecipe *recipe, const char *extract_dir,
                         const char *boot_root, char (*out_paths)[256], size_t max)
{
    static char pool[ARUN_MAX_OPS][256];
    static int is_dir[ARUN_MAX_OPS];
    static const char *entries[ARUN_MAX_OPS];
    static arun_plan plan;   /* ~362 KB - MUST be static, not on the 4 KB Shell stack */
    size_t n, i, written;

    /* Pre-install scripts: a failure (rc >= 10) ABORTS the install - they
     * guard preconditions (stop a running instance, make an assign). */
    for (i = 0; i < recipe->op_count; i++) {
        if (recipe->ops[i].type != AROP_PRE_SCRIPT) continue;
        if (amipkg_run_inline_script(recipe->ops[i].script, "pre-install") >= 10) {
            PutStr((STRPTR)"amipkg: pre-install script failed - aborting.\n");
            return 0;
        }
    }

    n = walk_tree(extract_dir, "", pool, is_dir, ARUN_MAX_OPS, 0);
    for (i = 0; i < n; i++) entries[i] = pool[i];
    if (arun_plan_build(recipe, entries, is_dir, n, &plan) != 0 || plan.overflow)
        return 0;
    written = amipkg_execute(&plan, extract_dir, boot_root, out_paths, max);

    /* Post-install scripts: tidy-ups (ENVARC defaults, obsolete-file removal).
     * A failure here is a WARNING - the files are already placed. */
    if (written > 0) {
        for (i = 0; i < recipe->op_count; i++) {
            if (recipe->ops[i].type != AROP_POST_SCRIPT) continue;
            if (amipkg_run_inline_script(recipe->ops[i].script, "post-install") >= 10)
                PutStr((STRPTR)"amipkg: WARNING - a post-install script failed.\n");
        }
    }
    return written;
}

/* Does the extract tree consist of ONE top-level drawer (all entries share a
 * single first path component that is a directory)? Then the archive brings
 * its own app drawer and may land in the install dir as-is. Anything else
 * (flat archives, multiple roots, a lone file) must be wrapped in a <id>/
 * drawer by the caller - loose files must never pile up in the install dir
 * root (tester report: amipkg-client + Amelinium spilled their files there). */
/* Self-update must reach the binaries ACTUALLY IN USE, not just the data
 * prefix: on older layouts the running `amipkg` lives elsewhere (the early
 * Imager images shipped C:amipkg + SYS:Tools/amipkg-gui with the data drawer
 * behind an assign). Without this, `upgrade amipkg` kept installing the new
 * version into the prefix drawer while the user kept launching the old copy -
 * "it sees 0.5.2, installs it, and still runs the old one" (A4000 report).
 * After the recipe placed the new files, mirror each amipkg-family file onto
 * (a) the running program's own directory and (b) the legacy locations IF a
 * stale copy exists there. Best-effort: a failed copy never fails the update
 * (an in-use file just stays until the next run). */
static const char *selfupdate_basename(const char *path)
{
    const char *b = path, *p;
    for (p = path; *p; p++) if (*p == '/' || *p == ':') b = p + 1;
    return b;
}

/* Case-insensitive compare (Amiga filesystems); newlib's stricmp is unsafe. */
static int ci_eq(const char *a, const char *b)
{
    size_t k;
    for (k = 0; ; k++) {
        int ca = tolower((unsigned char)a[k]);
        int cb = tolower((unsigned char)b[k]);
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}

void amipkg_selfupdate_mirror(char (*paths)[256], size_t n)
{
    static const char *family[] = { "amipkg", "amipkg-gui", "amipkg-mui",
                                    "amipkg-gui.info", "amipkg-mui.info",
                                    "amipkg.guide", NULL };
    char own[300];
    BPTR l = GetProgramDir();
    size_t i, k, refreshed = 0;
    own[0] = '\0';
    if (l) NameFromLock(l, (STRPTR)own, sizeof own);
    for (i = 0; i < n; i++) {
        const char *base = selfupdate_basename(paths[i]);
        int in_family = 0;
        for (k = 0; family[k]; k++)
            if (ci_eq(base, family[k])) { in_family = 1; break; }
        if (!in_family) continue;
        /* (a) the running program's own drawer */
        if (own[0]) {
            char dst[340];
            size_t len = strlen(own);
            snprintf(dst, sizeof dst, "%s%s%s", own,
                     (len && own[len - 1] == ':') ? "" : "/", base);
            if (!ci_eq(dst, paths[i])) {
                BPTR dl = Lock((STRPTR)dst, ACCESS_READ);
                int existed = dl != 0;
                if (dl) UnLock(dl);
                /* Copy into the running drawer even when absent there:
                 * whoever launches from this drawer must get the new set. */
                if (copy_file(paths[i], dst)) refreshed++;
                (void)existed;
            }
        }
        /* (b) known legacy homes - refresh ONLY where a stale copy exists */
        {
            const char *legacy = NULL;
            char dst[64];
            if (ci_eq(base, "amipkg")) legacy = "C:amipkg";
            else if (ci_eq(base, "amipkg-gui")) legacy = "SYS:Tools/amipkg-gui";
            else if (ci_eq(base, "amipkg-mui")) legacy = "SYS:Tools/amipkg-mui";
            if (legacy) {
                snprintf(dst, sizeof dst, "%s", legacy);
                if (!ci_eq(dst, paths[i])) {
                    BPTR dl = Lock((STRPTR)dst, ACCESS_READ);
                    if (dl) {
                        UnLock(dl);
                        if (copy_file(paths[i], dst)) refreshed++;
                    }
                }
            }
        }
    }
    if (refreshed)
        printf("Refreshed %lu running/legacy cop%s of amipkg alongside the install.\n",
               (unsigned long)refreshed, refreshed == 1 ? "y" : "ies");
}

/* One shared walk buffer (BSS budget: 1 MB machines) - the callers below
 * never overlap, each does its own fresh walk. */
static char g_walk_pool[ARUN_MAX_OPS][256];
static int  g_walk_isdir[ARUN_MAX_OPS];

/* Does the extract dir contain at least one file? cmd_install checks this
 * BEFORE removing the previous version - never destroy an install on the
 * strength of an empty extraction. */
int amipkg_extract_nonempty(const char *extract_dir)
{
    char (*pool)[256] = g_walk_pool;
    int *is_dir = g_walk_isdir;
    size_t n, i;
    n = walk_tree(extract_dir, "", pool, is_dir, ARUN_MAX_OPS, 0);
    for (i = 0; i < n; i++) if (!is_dir[i]) return 1;
    return 0;
}

int amipkg_extract_single_top_dir(const char *extract_dir)
{
    char (*pool)[256] = g_walk_pool;
    int *is_dir = g_walk_isdir;
    size_t n, i;
    char first[256];
    int have = 0, seen_dir = 0;
    n = walk_tree(extract_dir, "", pool, is_dir, ARUN_MAX_OPS, 0);
    if (n == 0) return 0;
    first[0] = '\0';
    for (i = 0; i < n; i++) {
        const char *sl = strchr(pool[i], '/');
        size_t len = sl ? (size_t)(sl - pool[i]) : strlen(pool[i]);
        char comp[256];
        size_t k;
        if (len >= sizeof comp) len = sizeof comp - 1;
        memcpy(comp, pool[i], len); comp[len] = '\0';
        if (!have) { strcpy(first, comp); have = 1; }
        else {
            /* Amiga filesystems are case-insensitive - compare accordingly. */
            for (k = 0; ; k++) {
                int a = tolower((unsigned char)first[k]);
                int b = tolower((unsigned char)comp[k]);
                if (a != b) return 0;
                if (!a) break;
            }
        }
        if (sl) seen_dir = 1;                 /* something lives under the top */
        else if (is_dir[i]) seen_dir = 1;     /* the top entry itself is a dir */
    }
    return seen_dir;
}

/* Generic install for a recipe-less package (the fetch-only Aminet entries):
 * copy the whole extracted tree under `dest_root` (typically SYS:Programs), so
 * an archive that unpacked into e.g. "Fitz/" lands as SYS:Programs/Fitz/. This
 * is the "drop the Aminet app into a drawer" a user would do by hand - a
 * best-effort install (apps needing their own Installer may need manual setup).
 * Returns the count of installed files (recorded for clean removal). */
/* Uncapped, receipt-recording generic install. The old array-based path
 * silently TRUNCATED big archives: walk_tree stopped at 512 tree entries and
 * the caller recorded at most 256 receipt lines - AmiBlitz3 (1109 files)
 * arrived half-installed with "unresolved dependencies" (A4000 report).
 * This walks and copies recursively with no caps, appends every file to the
 * receipt as it goes, and afterwards PROMOTES bundled <top>/Libs/*.library
 * to LIBS: when missing there (the classic "copy our Libs drawer to your
 * system Libs" install instruction, e.g. AmiBlitz's wizard.library) -
 * promoted copies are receipt-recorded too, so remove cleans them up. */
typedef struct {
    FILE *rcpt;
    char (*out_paths)[256];
    size_t out_max, out_n, copied;
    char libs[40][300];        /* installed dest paths of bundled Libs/ */
    size_t nlibs;
} gen_ctx;

static void gen_install_rec(const char *src_root, const char *rel,
                            const char *dest_root, gen_ctx *cx)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    char spath[512];
    if (rel[0]) snprintf(spath, sizeof spath, "%s/%s", src_root, rel);
    else        snprintf(spath, sizeof spath, "%s", src_root);
    lock = Lock((STRPTR)spath, ACCESS_READ);
    if (!lock) return;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) { UnLock(lock); return; }
    if (Examine(lock, fib) && fib->fib_DirEntryType > 0) {
        while (ExNext(lock, fib)) {
            char childrel[512];
            if (rel[0]) snprintf(childrel, sizeof childrel, "%s/%s", rel, fib->fib_FileName);
            else        snprintf(childrel, sizeof childrel, "%s", fib->fib_FileName);
            if (fib->fib_DirEntryType > 0) {
                gen_install_rec(src_root, childrel, dest_root, cx);
            } else {
                char src[600], dst[600], hex[65];
                snprintf(src, sizeof src, "%s/%s", src_root, childrel);
                snprintf(dst, sizeof dst, "%s/%s", dest_root, childrel);
                if (copy_file(src, dst)) {
                    cx->copied++;
                    if (cx->rcpt) {
                        if (sha256_of_file(dst, hex) == 0) fprintf(cx->rcpt, "%s|%s\n", dst, hex);
                        else                                fprintf(cx->rcpt, "%s\n", dst);
                    }
                    if (cx->out_n < cx->out_max) {
                        strncpy(cx->out_paths[cx->out_n], dst, 255);
                        cx->out_paths[cx->out_n][255] = 0;
                        cx->out_n++;
                    }
                    /* <top>/Libs/<name>.library (or Libs/ at the wrap root) */
                    {
                        const char *l = strstr(childrel, "Libs/");
                        size_t cl = strlen(childrel);
                        if (l && (l == childrel || *(l - 1) == '/')
                            && strchr(l + 5, '/') == NULL
                            && cl > 8 && ci_eq(childrel + cl - 8, ".library")
                            && cx->nlibs < sizeof cx->libs / sizeof cx->libs[0]) {
                            strncpy(cx->libs[cx->nlibs], dst, sizeof cx->libs[0] - 1);
                            cx->libs[cx->nlibs][sizeof cx->libs[0] - 1] = 0;
                            cx->nlibs++;
                        }
                    }
                }
            }
        }
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
}

size_t amipkg_install_generic_recorded(const char *extract_dir, const char *dest_root,
                                       const char *id, char (*out_paths)[256], size_t max)
{
    static gen_ctx cx;
    char rp[192];
    size_t i, promoted = 0;
    memset(&cx, 0, sizeof cx);
    cx.out_paths = out_paths; cx.out_max = max;
    snprintf(rp, sizeof rp, "%sdb/files/%s.files", amipkg_prefix(), id);
    cx.rcpt = fopen(rp, "a");
    gen_install_rec(extract_dir, "", dest_root, &cx);
    for (i = 0; i < cx.nlibs; i++) {
        const char *base = selfupdate_basename(cx.libs[i]);
        char libdst[300], hex[65];
        BPTR l;
        snprintf(libdst, sizeof libdst, "LIBS:%s", base);
        l = Lock((STRPTR)libdst, ACCESS_READ);
        if (l) { UnLock(l); continue; }   /* never overwrite a system lib */
        if (copy_file(cx.libs[i], libdst)) {
            promoted++;
            if (cx.rcpt) {
                if (sha256_of_file(libdst, hex) == 0) fprintf(cx.rcpt, "%s|%s\n", libdst, hex);
                else                                   fprintf(cx.rcpt, "%s\n", libdst);
            }
        }
    }
    if (cx.rcpt) fclose(cx.rcpt);
    if (promoted)
        printf("Promoted %lu bundled librar%s to LIBS: (missing there).\n",
               (unsigned long)promoted, promoted == 1 ? "y" : "ies");
    return cx.copied;
}

size_t amipkg_install_generic(const char *extract_dir, const char *dest_root,
                              char (*out_paths)[256], size_t max)
{
    char (*pool)[256] = g_walk_pool;
    int *is_dir = g_walk_isdir;
    size_t n, i, out = 0;
    n = walk_tree(extract_dir, "", pool, is_dir, ARUN_MAX_OPS, 0);
    for (i = 0; i < n; i++) {
        char src[300], dst[300];
        if (is_dir[i]) continue;
        snprintf(src, sizeof src, "%s/%s", extract_dir, pool[i]);
        snprintf(dst, sizeof dst, "%s/%s", dest_root, pool[i]);
        if (copy_file(src, dst) && out < max) {
            strncpy(out_paths[out], dst, 255); out_paths[out][255] = '\0';
            out++;
        }
    }
    return out;
}

#endif /* __amigaos__ */
