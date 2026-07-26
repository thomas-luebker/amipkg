/*
 * sha256.h - amipkg portable core
 *
 * Minimal SHA-256 (FIPS 180-4). Portable C99, no dependencies - runs on
 * 68000 up (no alignment tricks, no 64-bit requirements beyond a length
 * counter kept as two 32-bit words). This is the integrity primitive the
 * Phase-3 trust model needs on-Amiga: archives are verified against the
 * sha256 pinned in the host-verified seeded index.
 */
#ifndef AMIPKG_SHA256_H
#define AMIPKG_SHA256_H

#include <stddef.h>

typedef unsigned int u32;   /* 32-bit on all supported targets (ILP32 68k, LP64 host) */

typedef struct {
    u32 state[8];
    u32 len_hi, len_lo;     /* total bit length, as two 32-bit words */
    unsigned char buf[64];
    size_t buf_used;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, unsigned char out[32]);

/* One-shot: hex digest (lowercase, 64 chars + NUL) of a buffer. */
void sha256_hex(const void *data, size_t len, char out_hex[65]);

#endif
