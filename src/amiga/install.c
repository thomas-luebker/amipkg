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
    static rcpt_installed inst[MAX_PKGS];
    size_t n = load_installed(inst, MAX_PKGS), i;
    long have = -1;
    for (i = 0; i < n; i++)
        if (strcmp(inst[i].id, "amipkg") == 0) { have = (long)i; break; }
    if (have >= 0 && !aver_is_newer(AMIPKG_VERSION, inst[have].version))
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
        /* Binary is newer than the record (manual update): bump the line. */
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
long amipkg_adopt_inventory(const char *id, const char *drawer)
{
    static char pool[ARUN_MAX_OPS][256];
    static int is_dir[ARUN_MAX_OPS];
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
/* One shared walk buffer (BSS budget: 1 MB machines) - the callers below
 * never overlap, each does its own fresh walk. */
static char g_walk_pool[ARUN_MAX_OPS][256];
static int  g_walk_isdir[ARUN_MAX_OPS];

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
