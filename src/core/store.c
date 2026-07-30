/*
 * store.c - amipkg on-image data access. See store.h.
 * Extracted from main.c so the CLI and the GUI share one implementation.
 */
#include "store.h"
#include "sha256.h"

#ifdef __amigaos__
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The resolved data prefix. It is an ABSOLUTE path since 0.4.7 (not the short
 * "AMIPKG:"), so every buffer built from it must be sized off this - GCC's
 * -Wformat-truncation caught three that were still sized for the old short
 * form. */
#define AMIPKG_PREFIX_MAX 300

char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long len;
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); fclose(f); return NULL; }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

int sha256_of_file(const char *path, char out_hex[65])
{
    FILE *f = fopen(path, "rb");
    sha256_ctx c;
    unsigned char digest[32];
    static unsigned char buf[8192];
    size_t n;
    int i;
    static const char hexd[] = "0123456789abcdef";
    if (!f) return 1;
    sha256_init(&c);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        sha256_update(&c, buf, n);
    fclose(f);
    sha256_final(&c, digest);
    for (i = 0; i < 32; i++) {
        out_hex[i*2]   = hexd[digest[i] >> 4];
        out_hex[i*2+1] = hexd[digest[i] & 0xf];
    }
    out_hex[64] = '\0';
    return 0;
}

size_t load_installed(rcpt_installed *out, size_t max)
{
    char *text = read_file(amipkg_data_path("db/installed.txt"));
    size_t n;
    if (!text) return 0;
    n = rcpt_parse_installed(text, out, max);
    free(text);
    return n;
}

size_t load_files_for(const char *id, rcpt_file *out, size_t max)
{
    /* Big enough for the resolved prefix (which is an ABSOLUTE path since
     * 0.4.7, not the short "AMIPKG:") plus the longest id. */
    char path[AMIPKG_PREFIX_MAX + 96];
    char *text;
    size_t n;
    snprintf(path, sizeof path, "%sdb/files/%s.files", amipkg_prefix(), id);
    text = read_file(path);
    if (!text) return 0;
    n = rcpt_parse_files(text, out, max);
    free(text);
    return n;
}

/* ---- runtime data prefix ---------------------------------------------- */

static char g_prefix[AMIPKG_PREFIX_MAX] = "";

#ifdef __amigaos__
static int lock_exists(const char *path)
{
    BPTR l = Lock((STRPTR)path, ACCESS_READ);
    if (l) { UnLock(l); return 1; }
    return 0;
}
#endif

const char *amipkg_prefix(void)
{
    if (g_prefix[0]) return g_prefix;
#ifdef __amigaos__
    {
        /* Suppress "Please insert volume" requesters while probing. */
        struct Process *pr = (struct Process *)FindTask(NULL);
        APTR oldwin = pr->pr_WindowPtr;
        BPTR pd;
        pr->pr_WindowPtr = (APTR)-1;
        pd = GetProgramDir();
        if (pd && (lock_exists("PROGDIR:db") || lock_exists("PROGDIR:packages.json"))) {
            /* Our drawer IS the home. Resolve it ABSOLUTELY: "PROGDIR:" is
             * per-process and silently points elsewhere inside any child we
             * spawn (C:lha! - 0.4.7 regression: extraction failed for every
             * standalone install because lha saw ITS OWN PROGDIR:). An
             * absolute prefix is safe in-process and across children. */
            char dir[256];
            if (NameFromLock(pd, (STRPTR)dir, sizeof dir) && dir[0]) {
                size_t n = strlen(dir);
                snprintf(g_prefix, sizeof g_prefix, "%s%s",
                         dir, dir[n - 1] == ':' ? "" : "/");
            } else
                strcpy(g_prefix, "PROGDIR:");      /* degraded fallback */
        }
        else if (lock_exists("AMIPKG:"))
            strcpy(g_prefix, "AMIPKG:");           /* legacy image layout */
        else if (lock_exists("AMIGAIMAGER:")) {
            /* pre-0.4 image: alias AMIPKG: from the old assign (read-only
             * migration - the legacy name is never created by us). */
            BPTR src = Lock((STRPTR)"AMIGAIMAGER:", ACCESS_READ);
            if (src) AssignLock((STRPTR)"AMIPKG", src);   /* consumes lock */
            strcpy(g_prefix, "AMIPKG:");
        } else if (pd) {
            /* Fresh standalone: become the home right here. No assign;
             * absolute for the same child-process reason as above. */
            char dir[256];
            if (NameFromLock(pd, (STRPTR)dir, sizeof dir) && dir[0]) {
                size_t n = strlen(dir);
                snprintf(g_prefix, sizeof g_prefix, "%s%s",
                         dir, dir[n - 1] == ':' ? "" : "/");
            } else
                strcpy(g_prefix, "PROGDIR:");
        } else
            strcpy(g_prefix, "AMIPKG:");
        pr->pr_WindowPtr = oldwin;
    }
#else
    {
        /* Host build. Normally inert (logic tests only), but AMIPKG_PREFIX
         * points it at a scratch directory so the test suite and the host CLI
         * can exercise the real on-disk config/receipt paths. Amiga builds
         * never see this - it lives inside the non-__amigaos__ branch. */
        const char *env = getenv("AMIPKG_PREFIX");
        if (env && env[0]) {
            size_t n = strlen(env);
            snprintf(g_prefix, sizeof g_prefix, "%s%s",
                     env, (env[n-1] == '/' || env[n-1] == ':') ? "" : "/");
        } else
            strcpy(g_prefix, "AMIPKG:");
    }
#endif
    return g_prefix;
}

#ifndef __amigaos__
void amipkg_reset_prefix_for_test(void) { g_prefix[0] = '\0'; }
#endif

char *amipkg_data_path(const char *rel)
{
    static char ring[4][320];
    static int i = 0;
    char *b = ring[i = (i + 1) & 3];
    snprintf(b, sizeof ring[0], "%s%s", amipkg_prefix(), rel);
    return b;
}

/* Kept as the GUIs' startup hook: resolving the prefix does everything the
 * old assign bridge did (incl. the pre-0.4 image migration). No assign is
 * created on the standalone path - nothing locks the drawer. */
void amipkg_bridge_assigns(void)
{
    (void)amipkg_prefix();
}

/* Trim leading/trailing whitespace (incl. CR/LF) in place. */
static void trim(char *s)
{
    size_t len, i = 0;
    while (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n') i++;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
    len = strlen(s);
    while (len && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\r' || s[len-1] == '\n'))
        s[--len] = '\0';
}

void amipkg_get_installdir(char *out, size_t n)
{
    char *text;
    const char *env;
    if (n == 0) return;
    /* 1. config file wins */
    text = read_file(amipkg_data_path("config/installdir"));
    if (text) {
        trim(text);
        if (text[0]) { strncpy(out, text, n - 1); out[n-1] = '\0'; free(text); return; }
        free(text);
    }
    /* 2. environment override */
    env = getenv("AMIPKG_INSTALLDIR");
    if (env && env[0]) { strncpy(out, env, n - 1); out[n-1] = '\0'; return; }
    /* 3. default */
    strncpy(out, AMIPKG_DEFAULT_INSTALLDIR, n - 1); out[n-1] = '\0';
}

void amipkg_get_pkgdir(const char *id, char *out, size_t n)
{
    char cfg[AMIPKG_PREFIX_MAX + 96];
    char *text;
    snprintf(cfg, sizeof cfg, "%sconfig/dir-%s", amipkg_prefix(), id);
    text = read_file(cfg);
    if (text) {
        trim(text);
        if (text[0]) { strncpy(out, text, n - 1); out[n-1] = '\0'; free(text); return; }
        free(text);
    }
    amipkg_get_installdir(out, n);
}

int amipkg_set_pkgdir(const char *id, const char *path)
{
    char cfg[AMIPKG_PREFIX_MAX + 96];
    FILE *f;
    snprintf(cfg, sizeof cfg, "%sconfig/dir-%s", amipkg_prefix(), id);
    if (!path || !path[0]) { remove(cfg); return 0; }
    f = fopen(cfg, "wb");
    if (!f) return 1;
    fprintf(f, "%s\n", path);
    fclose(f);
    return 0;
}

int amipkg_set_installdir(const char *path)
{
    FILE *f;
    char buf[256];
    if (!path || !path[0]) { remove(amipkg_data_path("config/installdir")); return 0; }
    strncpy(buf, path, sizeof buf - 1); buf[sizeof buf - 1] = '\0';
    trim(buf);
    f = fopen(amipkg_data_path("config/installdir"), "wb");
    if (!f) return 1;
    fprintf(f, "%s\n", buf);
    fclose(f);
    return 0;
}

/* See store.h - the one shared installed-receipt scratch buffer. */
rcpt_installed amipkg_inst_scratch[MAX_PKGS];

/* Which system drawer is this, if any?
 *
 * Installing a recipe-less package into a system drawer used to copy the whole
 * archive tree there - documentation included - AND nest it, so a command
 * landed at C:Tool/Tool where the shell will never find it (yelworC/djbase,
 * 2026-07-28). System drawers hold flat, specific things; this is how we know
 * which. Ordinary app drawers return SD_NONE and nothing changes for them. */
static int sd_ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

int amipkg_system_drawer_kind(const char *path)
{
    static const struct { const char *name; int kind; } tab[] = {
        { "c",         SD_C },      { "libs",    SD_LIBS },
        { "devs",      SD_DEVS },   { "l",       SD_L },
        { "s",         SD_S },      { "fonts",   SD_FONTS },
        { "classes",   SD_CLASSES },{ "prefs",   SD_PREFS },
        { NULL, 0 }
    };
    char leaf[64];
    const char *p;
    size_t n = 0, i;
    if (!path || !path[0]) return SD_NONE;

    /* Take the last component, with or without a trailing colon/slash:
     * "C:", "SYS:C", "DH0:System/Libs" all reduce to the leaf. */
    p = path + strlen(path);
    while (p > path && (p[-1] == '/' || p[-1] == ':')) p--;
    { const char *q = p;
      while (q > path && q[-1] != '/' && q[-1] != ':') q--;
      while (q < p && n < sizeof leaf - 1) leaf[n++] = *q++; }
    leaf[n] = '\0';
    if (!n) return SD_NONE;
    for (i = 0; tab[i].name; i++)
        if (sd_ci_eq(leaf, tab[i].name)) return tab[i].kind;
    return SD_NONE;
}

/* Which platform are we actually running on?
 *
 * amipkg is an m68k binary, but MorphOS and AmigaOS 4 both execute those (the
 * MorphOS emulation layer, OS4's Petunia), so it can genuinely find itself on
 * a machine that is not a classic Amiga. Reporting that honestly is what lets
 * the catalog carry non-68k packages without offering them to the wrong
 * machine - and, more usefully, lets a MorphOS user still see the 68k packages
 * that DO run there.
 *
 * Probe order matters: MorphOS also reports a high exec version, so it must be
 * identified by its own library BEFORE the version test.
 *
 * Aminet's vocabulary, so a repo owner writes what they already know. */
#ifdef __amigaos__
const char *amipkg_host_arch(void)
{
    static const char *cached = NULL;
    struct Library *l;
    if (cached) return cached;

    if ((l = OpenLibrary((STRPTR)"morphos.library", 0))) {
        CloseLibrary(l);
        cached = "ppc-morphos";
    } else if ((l = OpenLibrary((STRPTR)"aros.library", 0))) {
        CloseLibrary(l);
        cached = "i386-aros";          /* AROS flavour is not distinguished */
    } else if (SysBase->LibNode.lib_Version >= 50) {
        /* AmigaOS 4's exec is 52+; 3.x tops out at 47 (3.2). */
        cached = "ppc-amigaos";
    } else {
        cached = "m68k-amigaos";
    }
    return cached;
}
#else
const char *amipkg_host_arch(void) { return "m68k-amigaos"; }
#endif
