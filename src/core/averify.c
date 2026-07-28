/*
 * averify.c - on-device Ed25519 verification (see averify.h). Uses TweetNaCl's
 * crypto_sign_open (Ed25519, RFC 8032; SHA-512 included). Standard Ed25519, so
 * it verifies signatures produced by the app's CryptoKit Curve25519.Signing.
 */
#include "averify.h"
#include "arepo.h"
#include "tweetnacl.h"

#include <stdlib.h>
#include <string.h>

/* The official project key now has its single definition in arepo.h
 * (AMIPKG_OFFICIAL_PUBKEY) so the repo list and this verifier cannot drift
 * apart - the official repo entry and the default wrapper below are the same
 * key by construction. */
static const char AMIPKG_PUBKEY_B64[] = AMIPKG_OFFICIAL_PUBKEY;

/* TweetNaCl references randombytes for keygen/sign; verification never calls it,
 * but the symbol must resolve at link time. */
void randombytes(unsigned char *p, unsigned long long n) { (void)p; (void)n; }

static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode base64 `in` into `out` (capacity outmax). Returns byte count, or -1. */
static int b64decode(const char *in, unsigned char *out, int outmax)
{
    int n = 0, bits = 0, val = 0, i;
    for (i = 0; in[i]; i++) {
        int c = (unsigned char)in[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        int v = b64val(c);
        if (v < 0) return -1;
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= outmax) return -1;
            out[n++] = (unsigned char)((val >> bits) & 0xFF);
        }
    }
    return n;
}

int amipkg_verify_index(const unsigned char *msg, size_t msglen, const char *sig_base64)
{
    return amipkg_verify_index_key(msg, msglen, sig_base64, AMIPKG_PUBKEY_B64);
}

int amipkg_verify_index_key(const unsigned char *msg, size_t msglen,
                            const char *sig_base64, const char *pubkey_base64)
{
    unsigned char pk[32], sig[64];
    unsigned char *sm, *m;
    unsigned long long mlen = 0;
    int rc = 0;
    if (!pubkey_base64 || !pubkey_base64[0]) return 0;   /* unsigned != verified */
    if (!sig_base64 || !sig_base64[0]) return 0;
    if (b64decode(pubkey_base64, pk, 32) != 32) return 0;
    if (b64decode(sig_base64, sig, 64) != 64) return 0;
    /* crypto_sign_open expects a "signed message" = signature(64) || message. */
    sm = (unsigned char *)malloc(msglen + 64);
    m  = (unsigned char *)malloc(msglen + 64);
    if (!sm || !m) { free(sm); free(m); return 0; }
    memcpy(sm, sig, 64);
    memcpy(sm + 64, msg, msglen);
    if (crypto_sign_open(m, &mlen, sm, (unsigned long long)(msglen + 64), pk) == 0)
        rc = 1;
    free(sm);
    free(m);
    return rc;
}
