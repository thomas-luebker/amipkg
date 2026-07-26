/*
 * http.c - amipkg Amiga platform layer
 *
 * HTTP/1.1 GET over bsdsocket.library (Roadshow, or UAE's host emulation),
 * plus HTTPS via AmiSSL WHEN IT IS INSTALLED (amisslmaster.library v5+,
 * opened lazily on the first https:// URL; without it, https is refused with
 * a clear hint). TLS is used for TRANSPORT COMPATIBILITY only - hosts like
 * GitHub are https-only - not for trust: the signed index pins each
 * archive's SHA-256, so certificate verification is deliberately off (no
 * cert store is guaranteed on a classic Amiga, and integrity is already
 * end-to-end via the Ed25519-signed catalog).
 *
 * Redirects (301/302/303/307/308) are followed up to 3 hops - GitHub
 * release downloads always bounce to a CDN host. The SHA-256 pin makes
 * redirect-following safe: wherever the bytes come from, they must hash to
 * the pinned digest or the download is refused.
 */

#ifdef __amigaos__

/* The NDK sys/socket.h uses ssize_t, but newlib gates its typedef behind a
 * feature macro that isn't active here. Define it (with newlib's own guard, so
 * this is conflict-free) BEFORE proto/bsdsocket.h - which pulls in socket.h.
 * (32-bit on m68k.) */
#include <sys/types.h>
#ifndef _SSIZE_T_DECLARED
typedef long ssize_t;
#define _SSIZE_T_DECLARED
#endif

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* AmiSSL (vendored SDK headers; the libraries are runtime-optional). */
#include <proto/amissl.h>
#include <proto/amisslmaster.h>
#include <amissl/amissl.h>
#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>

struct Library *SocketBase = NULL;
struct Library *AmiSSLMasterBase = NULL;
struct Library *AmiSSLBase = NULL;
struct Library *UtilityBase = NULL;

static SSL_CTX *g_ssl_ctx = NULL;
static int g_amissl_tried = 0;

int http_available(void)
{
    if (SocketBase) return 1;
    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    if (!SocketBase) {
        printf("amipkg: you are offline - connect first, then retry.\n");
        return 0;
    }
    return 1;
}

/* Lazily bring up AmiSSL for https. Returns 1 when TLS is usable. */
static int amissl_available(void)
{
    if (g_ssl_ctx) return 1;
    if (g_amissl_tried) return 0;   /* failed before - don't spam retries */
    g_amissl_tried = 1;

    if (!http_available()) return 0;
    if (!UtilityBase) UtilityBase = OpenLibrary((STRPTR)"utility.library", 0);
    AmiSSLMasterBase = OpenLibrary((STRPTR)"amisslmaster.library", AMISSLMASTER_MIN_VERSION);
    if (!AmiSSLMasterBase) {
        printf("amipkg: this URL needs https, and AmiSSL is not installed.\n");
        printf("Install AmiSSL 5.x (Aminet util/libs/amissl) to enable https downloads.\n");
        return 0;
    }
    if (!InitAmiSSLMaster(AMISSL_CURRENT_VERSION, TRUE)) {
        printf("amipkg: installed AmiSSL is too old (need 5.x) - https unavailable.\n");
        return 0;
    }
    AmiSSLBase = OpenAmiSSL();
    if (!AmiSSLBase) {
        printf("amipkg: couldn't open AmiSSL - https unavailable.\n");
        return 0;
    }
    if (InitAmiSSL(AmiSSL_ErrNoPtr, (ULONG)&errno,
                   AmiSSL_SocketBase, (ULONG)SocketBase,
                   TAG_DONE) != 0) {
        printf("amipkg: couldn't initialize AmiSSL - https unavailable.\n");
        return 0;
    }
    OPENSSL_init_ssl(OPENSSL_INIT_SSL_DEFAULT
                     | OPENSSL_INIT_ADD_ALL_CIPHERS
                     | OPENSSL_INIT_ADD_ALL_DIGESTS, NULL);
    {
        /* Seed the entropy pool: system time + stack noise. Fine for a CLIENT
         * handshake (we generate no long-lived keys). */
        struct { struct DateStamp ds; void *sp; long clk; char noise[64]; } seed;
        DateStamp(&seed.ds);
        seed.sp = (void *)&seed;
        seed.clk = (long)clock();
        RAND_seed(&seed, sizeof seed);
    }
    g_ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!g_ssl_ctx) {
        printf("amipkg: SSL_CTX_new failed - https unavailable.\n");
        return 0;
    }
    /* Integrity comes from the signed index's SHA-256 pin, not the cert chain
     * (no CA store is guaranteed on a stock 3.x system). */
    SSL_CTX_set_verify(g_ssl_ctx, SSL_VERIFY_NONE, NULL);
    return 1;
}

void http_cleanup(void)
{
    if (g_ssl_ctx) { SSL_CTX_free(g_ssl_ctx); g_ssl_ctx = NULL; }
    if (AmiSSLBase) { CloseAmiSSL(); AmiSSLBase = NULL; }
    if (AmiSSLMasterBase) { CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase = NULL; }
    if (UtilityBase) { CloseLibrary(UtilityBase); UtilityBase = NULL; }
    if (SocketBase) { CloseLibrary(SocketBase); SocketBase = NULL; }
}

/* Split "http[s]://host[:port]/path". Returns 0 on success, sets *tls. */
static int parse_url(const char *url, char *host, size_t hostsize,
                     long *port, char *path, size_t pathsize, int *tls)
{
    const char *p, *slash, *colon;
    size_t hl;
    if (strncmp(url, "http://", 7) == 0)       { p = url + 7;  *tls = 0; *port = 80; }
    else if (strncmp(url, "https://", 8) == 0) { p = url + 8;  *tls = 1; *port = 443; }
    else return 1;
    slash = strchr(p, '/');
    if (!slash) slash = p + strlen(p);
    colon = memchr(p, ':', (size_t)(slash - p));
    if (colon) {
        *port = atol(colon + 1);
        hl = (size_t)(colon - p);
    } else {
        hl = (size_t)(slash - p);
    }
    if (hl == 0 || hl >= hostsize) return 1;
    memcpy(host, p, hl);
    host[hl] = '\0';
    if (*slash) {
        strncpy(path, slash, pathsize - 1);
        path[pathsize - 1] = '\0';
    } else {
        strcpy(path, "/");
    }
    return 0;
}

/* Transport read/write that goes through TLS when `ssl` is set. */
static long net_write(SSL *ssl, int sock, const char *buf, long len)
{
    if (ssl) return SSL_write(ssl, buf, len);
    return send(sock, (char *)buf, len, 0);
}
static long net_read(SSL *ssl, int sock, char *buf, long len)
{
    if (ssl) return SSL_read(ssl, buf, len);
    return recv(sock, buf, len, 0);
}

/* Pull a "Location:" value out of a raw header block (case-insensitive). */
static int header_location(const char *header, char *out, size_t outsize)
{
    const char *p = header;
    while ((p = strchr(p, '\n')) != NULL) {
        p++;
        if (strncasecmp(p, "Location:", 9) == 0) {
            const char *v = p + 9;
            size_t o = 0;
            while (*v == ' ' || *v == '\t') v++;
            while (*v && *v != '\r' && *v != '\n' && o < outsize - 1) out[o++] = *v++;
            out[o] = '\0';
            return o > 0;
        }
    }
    return 0;
}

/* One GET; on a 3xx fills `redirect` and returns 2. 0 = 200/206 body written,
 * 1 = hard failure, 3 = server ignored our Range (sent 200) - caller must
 * restart the file from zero. `resume_from` > 0 adds a Range header. */
static int do_get(const char *url, FILE *out, long *bytes_out,
                  char *redirect, size_t redirsize, long resume_from)
{
    char host[128], path[512];
    static char req[768], buf[4096];   /* off the small Shell stack */
    static char header[8192];
    long port;
    int tls = 0;
    struct hostent *he;
    struct sockaddr_in sa;
    int sock, ok = 1;
    long n, total = 0;
    long expect = 0;            /* Content-Length when the server sends it */
    long next_tick = 262144;    /* progress line every 256 KB */
    int header_done = 0, status = 0;
    size_t header_used = 0;
    SSL *ssl = NULL;

    if (!http_available()) return 1;
    if (parse_url(url, host, sizeof host, &port, path, sizeof path, &tls) != 0) {
        printf("amipkg: unsupported URL: %s\n", url);
        return 1;
    }
    if (tls && !amissl_available()) return 1;

    he = gethostbyname(host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        printf("amipkg: cannot resolve %s\n", host);
        return 1;
    }
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { printf("amipkg: socket() failed\n"); return 1; }
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof sa.sin_addr);
    if (connect(sock, (struct sockaddr *)&sa, sizeof sa) < 0) {
        printf("amipkg: cannot connect to %s:%ld\n", host, port);
        CloseSocket(sock);
        return 1;
    }

    if (tls) {
        ssl = SSL_new(g_ssl_ctx);
        if (!ssl) { printf("amipkg: SSL_new failed\n"); CloseSocket(sock); return 1; }
        SSL_set_fd(ssl, sock);
        /* SNI: virtually every https host (GitHub CDN included) requires it. */
        SSL_set_tlsext_host_name(ssl, host);
        if (SSL_connect(ssl) <= 0) {
            printf("amipkg: TLS handshake with %s failed\n", host);
            SSL_free(ssl); CloseSocket(sock);
            return 1;
        }
    }

    if (resume_from > 0)
        snprintf(req, sizeof req,
                 "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: amipkg/0.4.6\r\n"
                 "Range: bytes=%ld-\r\nConnection: close\r\n\r\n",
                 path, host, resume_from);
    else
        snprintf(req, sizeof req,
                 "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: amipkg/0.4.6\r\n"
                 "Connection: close\r\n\r\n", path, host);
    if (net_write(ssl, sock, req, (long)strlen(req)) < 0) {
        printf("amipkg: send failed\n");
        goto fail;
    }
    while ((n = net_read(ssl, sock, buf, sizeof buf)) > 0) {
        char *body = buf;
        long body_len = n;
        if (!header_done) {
            size_t take = (size_t)n;
            char *sep;
            if (header_used + take >= sizeof header) take = sizeof header - header_used - 1;
            memcpy(header + header_used, buf, take);
            header_used += take;
            header[header_used] = '\0';
            sep = strstr(header, "\r\n\r\n");
            if (!sep) continue;
            header_done = 1;
            if (sscanf(header, "HTTP/%*s %d", &status) != 1) status = 0;
            if (status >= 301 && status <= 308 && status != 304 && status != 306) {
                if (header_location(header, redirect, redirsize)) {
                    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
                    CloseSocket(sock);
                    return 2;   /* caller follows */
                }
            }
            if (resume_from > 0 && status == 200) {
                /* Server ignored the Range: the body is the FULL file - the
                 * caller must truncate + restart, not append. */
                if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
                CloseSocket(sock);
                return 3;
            }
            if (status != 200 && !(resume_from > 0 && status == 206)) {
                printf("amipkg: HTTP %d from %s\n", status, host);
                goto fail;
            }
            {
                /* Content-Length (case-insensitive) for progress reporting. */
                const char *cl = header;
                while ((cl = strchr(cl, '\n')) != NULL) {
                    cl++;
                    if (strncasecmp(cl, "Content-Length:", 15) == 0) {
                        expect = atol(cl + 15);
                        break;
                    }
                }
            }
            body = sep + 4;
            body_len = (long)(header_used - (size_t)(body - header));
            if (body_len > 0) {
                fwrite(body, 1, (size_t)body_len, out);
                total += body_len;
            }
            continue;
        }
        fwrite(body, 1, (size_t)body_len, out);
        total += body_len;
        if (total >= next_tick) {
            /* One full line per tick: the GUI streams the LAST line into its
             * progress row, so this is the live download indicator. */
            if (expect > 0)
                printf("  %ld/%ldK (%ld%%)\n",
                       (resume_from + total) / 1024, (resume_from + expect) / 1024,
                       (total * 100) / expect);
            else
                printf("  %ldK downloaded\n", (resume_from + total) / 1024);
            fflush(stdout);
            next_tick += 262144;
        }
    }
    if (!header_done) { printf("amipkg: empty response from %s\n", host); goto fail; }
    ok = 0;
    if (bytes_out) *bytes_out = total;
fail:
    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
    CloseSocket(sock);
    return ok;
}

/* GET `url` into open FILE `out`, following up to 3 redirects. Returns 0 on
 * success (HTTP 200, body written), nonzero otherwise. */
int http_get(const char *url, FILE *out, long *bytes_out)
{
    static char current[512], redirect[512];
    int hops, rc;
    strncpy(current, url, sizeof current - 1);
    current[sizeof current - 1] = '\0';
    for (hops = 0; hops < 4; hops++) {
        rc = do_get(current, out, bytes_out, redirect, sizeof redirect, 0);
        if (rc != 2) return rc;
        /* Restart the body for the new location (out may have header spill -
         * it can't: body only written on 200; just rewind for cleanliness). */
        fseek(out, 0, SEEK_SET);
        printf("amipkg: following redirect -> %s\n", redirect);
        strncpy(current, redirect, sizeof current - 1);
        current[sizeof current - 1] = '\0';
    }
    printf("amipkg: too many redirects for %s\n", url);
    return 1;
}

/* GET `url` into file `path`, RESUMING a previous partial download via a
 * Range request (206 appends; a 200 answer to a Range restarts from zero).
 * Follows up to 3 redirects. 0 = complete body on disk. */
int http_get_file(const char *url, const char *path, long *bytes_out)
{
    static char current[512], redirect[512];
    long have = 0, got = 0;
    int hops, rc;
    FILE *out;
    strncpy(current, url, sizeof current - 1);
    current[sizeof current - 1] = '\0';

    out = fopen(path, "rb");
    if (out) { fseek(out, 0, SEEK_END); have = ftell(out); fclose(out); }
    if (have > 0) printf("amipkg: resuming at %ld bytes...\n", have);
    out = fopen(path, have > 0 ? "ab" : "wb");
    if (!out) { printf("amipkg: cannot write %s\n", path); return 10; }

    for (hops = 0; hops < 4; hops++) {
        rc = do_get(current, out, &got, redirect, sizeof redirect, have);
        if (rc == 2) {   /* redirect: keep the partial, re-request at the target */
            printf("amipkg: following redirect -> %s\n", redirect);
            strncpy(current, redirect, sizeof current - 1);
            current[sizeof current - 1] = '\0';
            continue;
        }
        if (rc == 3) {   /* Range ignored - restart the file from zero */
            fclose(out);
            out = fopen(path, "wb");
            if (!out) { printf("amipkg: cannot rewrite %s\n", path); return 10; }
            have = 0;
            continue;    /* same hop budget; a server answers 200 at most once per hop */
        }
        fclose(out);
        if (rc == 0 && bytes_out) *bytes_out = have + got;
        return rc;
    }
    fclose(out);
    printf("amipkg: too many redirects for %s\n", url);
    return 1;
}

#endif /* __amigaos__ */
