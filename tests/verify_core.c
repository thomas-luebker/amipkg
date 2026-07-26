/*
 * verify_core.c — host check for the on-device Ed25519 index verification.
 *
 * Verifies the REAL signed index against the baked public key: a good signature
 * must verify, a one-byte tamper must NOT. Points at a local amiga-pkg checkout
 * by default; override with AMIGAPKG_INDEX / AMIGAPKG_SIG. If the files aren't
 * present (e.g. CI without the repo), it skips (this is a machine-local guard,
 * like the env-gated goldens).
 */
#include "averify.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *p, size_t *len)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    b[n] = '\0'; *len = (size_t)n; fclose(f);
    return b;
}

int main(void)
{
    const char *ip = getenv("AMIGAPKG_INDEX");
    const char *sp = getenv("AMIGAPKG_SIG");
    static char ipbuf[512], spbuf[512];
    if (!ip) { snprintf(ipbuf, sizeof ipbuf, "%s/Development/amiga-pkg/docs/packages.json", getenv("HOME") ? getenv("HOME") : "."); ip = ipbuf; }
    if (!sp) { snprintf(spbuf, sizeof spbuf, "%s/Development/amiga-pkg/docs/packages.json.sig", getenv("HOME") ? getenv("HOME") : "."); sp = spbuf; }

    size_t jl, sl;
    char *json = slurp(ip, &jl), *sig = slurp(sp, &sl);
    if (!json || !sig) {
        printf("amipkg verify: SKIPPED (no signed index at %s)\n", ip);
        free(json); free(sig);
        return 0;
    }
    while (sl && (sig[sl-1] == '\n' || sig[sl-1] == '\r' || sig[sl-1] == ' ')) sig[--sl] = '\0';

    int good = amipkg_verify_index((const unsigned char *)json, jl, sig);
    json[jl / 2] ^= 1;                         /* tamper one byte */
    int bad = amipkg_verify_index((const unsigned char *)json, jl, sig);
    free(json); free(sig);

    if (good == 1 && bad == 0) {
        printf("amipkg verify: PASSED (valid sig accepted, tamper rejected)\n");
        return 0;
    }
    printf("amipkg verify: FAILED (good=%d, tamper=%d)\n", good, bad);
    return 1;
}
