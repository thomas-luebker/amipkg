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
    char *text = read_file(AMIPKG_DB_PREFIX "installed.txt");
    size_t n;
    if (!text) return 0;
    n = rcpt_parse_installed(text, out, max);
    free(text);
    return n;
}

size_t load_files_for(const char *id, rcpt_file *out, size_t max)
{
    char path[128];
    char *text;
    size_t n;
    snprintf(path, sizeof path, AMIPKG_DB_PREFIX "files/%s.files", id);
    text = read_file(path);
    if (!text) return 0;
    n = rcpt_parse_files(text, out, max);
    free(text);
    return n;
}

void amipkg_bridge_assigns(void)
{
#ifdef __amigaos__
    /* Suppress the system "Please insert volume ..." requester while we
     * PROBE assigns that may not exist - without this, the probes themselves
     * spam requesters on systems that were never set up (tester report). */
    struct Process *pr = (struct Process *)FindTask(NULL);
    APTR oldwin = pr->pr_WindowPtr;
    BPTR l, src;
    pr->pr_WindowPtr = (APTR)-1;

    l = Lock((STRPTR)"AMIPKG:", ACCESS_READ);
    if (l) UnLock(l);
    else {
        /* Migration read (never created): an Amiga-Imager-built image
         * provides AMIGAIMAGER: - alias AMIPKG: to the same drawer. */
        src = Lock((STRPTR)"AMIGAIMAGER:", ACCESS_READ);
        if (src) AssignLock((STRPTR)"AMIPKG", src);   /* consumes the lock */
        else {
            /* Standalone: amipkg lives WHERE IT WAS UNPACKED OR COPIED -
             * home is the drawer holding the running binary (PROGDIR:),
             * like MUI:. Catalog, cache and receipt DB sit next to it. */
            BPTR home = GetProgramDir();     /* shared lock - do NOT unlock */
            src = home ? DupLock(home) : Lock((STRPTR)"", ACCESS_READ);
            if (src) AssignLock((STRPTR)"AMIPKG", src);
        }
    }

    /* NO persistence: amipkg never writes to S:User-Startup or any other
     * system file. The bridge runs on EVERY start (CLI dispatch + both GUI
     * launches), so the assign is simply re-created per session - zero
     * footprint outside the home drawer. */
    pr->pr_WindowPtr = oldwin;
#endif
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
    text = read_file(AMIPKG_INSTALLDIR_FILE);
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
    char cfg[192];
    char *text;
    snprintf(cfg, sizeof cfg, AMIPKG_CONFIG_DIR "dir-%s", id);
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
    char cfg[192];
    FILE *f;
    snprintf(cfg, sizeof cfg, AMIPKG_CONFIG_DIR "dir-%s", id);
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
    if (!path || !path[0]) { remove(AMIPKG_INSTALLDIR_FILE); return 0; }
    strncpy(buf, path, sizeof buf - 1); buf[sizeof buf - 1] = '\0';
    trim(buf);
    f = fopen(AMIPKG_INSTALLDIR_FILE, "wb");
    if (!f) return 1;
    fprintf(f, "%s\n", buf);
    fclose(f);
    return 0;
}
