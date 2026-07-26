/*
 * averify.h - on-device Ed25519 verification of the signed package index.
 *
 * Phase 4 of the package-manager roadmap: lets `amipkg update` fetch a fresh
 * packages.json over (untrusted) HTTP and verify its detached Ed25519 signature
 * against the baked-in project public key itself - so the Amiga can refresh its
 * catalog online without the host having pre-verified/seeded it. Backed by
 * TweetNaCl (public domain).
 */
#ifndef AMIPKG_AVERIFY_H
#define AMIPKG_AVERIFY_H

#include <stddef.h>

/* Verify `msg[msglen]` against `sig_base64` (a base64 detached Ed25519 signature,
 * as stored in packages.json.sig) using the baked project public key. Returns 1
 * if the signature is valid, 0 otherwise. */
int amipkg_verify_index(const unsigned char *msg, size_t msglen,
                        const char *sig_base64);

#endif /* AMIPKG_AVERIFY_H */
