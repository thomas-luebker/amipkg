/*
 * arepo.h - amipkg portable core. The REPOSITORY LIST.
 *
 * amipkg started single-repo: one baked-in Ed25519 key, one hardcoded URL,
 * one packages.json. This module generalises that to N repositories so people
 * can host their own, while keeping the property the whole transport story
 * rests on: the index is signature-verified ON-DEVICE, and archive SHA-256
 * pins live INSIDE that verified index, which is why plain HTTP is safe.
 *
 * Two kinds of repo:
 *
 *   SIGNED   - carries an Ed25519 public key, pinned by the user when the repo
 *              was added. `update` refuses any catalog that does not verify
 *              against THAT repo's key. Same guarantee as the official repo.
 *
 *   UNSIGNED - key is "". The user explicitly opted in (one confirmation at
 *              add time). Understand what this costs: an unsigned catalog can
 *              be rewritten in transit, and the SHA-256 pins do NOT rescue it
 *              because they live inside the rewritten catalog. So the user is
 *              trusting the operator AND every hop in between. Kept clearly
 *              labelled everywhere it is shown, never the default.
 *
 * ORDER IS PRIORITY. A bare package id resolves against repos top-down, first
 * match wins - deliberately with NO special-casing of signed over unsigned
 * (a product decision: the user's ordering is the user's business). Front-ends
 * therefore SHOW which repo a package came from so shadowing stays visible.
 * "repo:id" forces one specific repo.
 *
 * Portable C: the list is a small line-oriented text file, like every other
 * amipkg config, precisely so this parser stays trivial.
 */
#ifndef AMIPKG_AREPO_H
#define AMIPKG_AREPO_H

#include <stddef.h>
#include "aindex.h"

/* Deliberately small - this is a 1 MB-machine package manager, and the list
 * is walked one catalog at a time (see arepo_catalog_path). */
#define AREPO_MAX      8
#define AREPO_ID_MAX   32
#define AREPO_URL_MAX  256
#define AREPO_KEY_MAX  64      /* base64 Ed25519 public key is 44 chars */

/* The official repo. Its id is reserved, and it is what a client with no
 * config/repos file falls back to, so an image built before multi-repo keeps
 * working with no migration step. */
#define AREPO_OFFICIAL_ID "official"

/* The project Ed25519 public key (base64) and plain-HTTP mirror of the signed
 * index. THE single definition - averify.c uses this for the default verify
 * wrapper, and arepo.c to synthesise the official entry. Duplicating it is how
 * you get a drift bug, so don't. MUST match the app's
 * PackageCatalogLoader.bakedPublicKeyBase64 and the key that signed the repo. */
#define AMIPKG_OFFICIAL_PUBKEY "tqZXIleRDYeU69ZsLNdvN790MUYdEKqvHctivyIhLEY="
/* amiga-imager.org is served over plain HTTP on purpose (.com/.de force
 * HTTPS); the on-device signature check is what makes that safe. */
#define AMIPKG_OFFICIAL_URL    "http://amiga-imager.org/packages"

typedef struct {
    char id[AREPO_ID_MAX];
    char url[AREPO_URL_MAX];
    char key[AREPO_KEY_MAX];   /* "" = UNSIGNED (explicit user opt-in) */
    int  enabled;
} arepo_entry;

typedef struct {
    arepo_entry v[AREPO_MAX];
    size_t count;
} arepo_list;

/* Load config/repos. A missing/empty/unparsable file yields the one-entry
 * default list (the official repo, enabled) rather than an error - amipkg must
 * always have somewhere to install from. Never fails. */
void arepo_load(arepo_list *out);

/* Persist the list to config/repos. Returns 0 on success. */
int arepo_save(const arepo_list *l);

/* Index of `id` in the list, or -1. Case-insensitive (Amiga habit). */
int arepo_find(const arepo_list *l, const char *id);

/* Is this repo signature-checked? (i.e. does it carry a pinned key) */
int arepo_is_signed(const arepo_entry *e);

/* Validate a proposed repo id. Returns 0 if OK, nonzero if it is empty, too
 * long, reserved, or contains anything but [A-Za-z0-9_-].
 *
 * This is a SECURITY check, not just tidiness: the id becomes a path component
 * under repos/, so a "../.." or "SYS:" id would let a repo write outside its
 * own drawer. */
int arepo_id_valid(const char *id);

/* Validate a repo URL: must be http:// or https://. (https needs AmiSSL at
 * runtime; http is fine for a SIGNED repo and is the norm here.) */
int arepo_url_valid(const char *url);

/* Add a repo at the END of the list (lowest priority). `key` may be NULL or ""
 * for an unsigned repo - callers MUST have confirmed that with the user first.
 * Returns 0 on success, or:
 *   1 list full, 2 bad id, 3 duplicate id, 4 bad url, 5 bad key */
int arepo_add(arepo_list *l, const char *id, const char *url, const char *key);

/* Remove by id. Returns 0 on success, 1 if not found. */
int arepo_remove(arepo_list *l, const char *id);

/* Enable/disable. Returns 0 on success, 1 if not found. */
int arepo_set_enabled(arepo_list *l, const char *id, int enabled);

/* Move a repo up (delta<0) or down (delta>0) in PRIORITY order, clamped at the
 * ends. Returns 0 on success, 1 if not found. */
int arepo_move(arepo_list *l, const char *id, int delta);

/* Where this repo's catalog and detached signature live.
 *
 * The official repo deliberately keeps the LEGACY top-level paths
 * (PROGDIR:packages.json), so every image built before multi-repo keeps
 * working untouched - no migration, no re-seed. Other repos get
 * repos/<id>/packages.json. One asymmetry, contained in these two functions. */
void arepo_catalog_path(const char *id, char *out, size_t n);
void arepo_sig_path(const char *id, char *out, size_t n);

/* Directory a non-official repo's catalog lives in ("" for official, which
 * uses the prefix root). Callers that write need to create it. */
void arepo_dir_path(const char *id, char *out, size_t n);

/* Split "repo:id" into its parts. Returns 1 if `spec` was qualified (fills
 * repo_out and id_out), 0 if it is a bare id (repo_out set to ""). Anything
 * with no colon, or a colon inside an Amiga path-looking string, is treated as
 * a bare id. */
int arepo_split_spec(const char *spec, char *repo_out, size_t repo_n,
                     char *id_out, size_t id_n);

/* Load and MERGE the catalogs of every ENABLED repo, in priority order, into
 * one index. The first repo to provide an id wins - pure priority, with NO
 * special-casing of signed over unsigned (a deliberate product decision: the
 * ordering is the user's business). Each entry is stamped with its source repo
 * so front-ends can show it.
 *
 * Merging (rather than searching repo-by-repo) is what makes dependency
 * resolution work ACROSS repos: a package in one repo can satisfy a dep
 * declared in another.
 *
 * Memory: catalogs are appended one at a time and each source is freed before
 * the next is read, so peak use is roughly the merged result plus the single
 * largest catalog - not the sum of all of them. That matters on the small
 * machines this runs on.
 *
 * Returns 0 if at least one catalog loaded, nonzero if none did (fresh install
 * with no seeded index, or every repo disabled). Free with aidx_free. */
int arepo_load_merged(aidx_index *out);

/* Load ONE repository's catalog (entries stamped with its id). This is what
 * makes "repo:id" work: the merged view keeps only the winning provider of an
 * id, so a package shadowed by a higher-priority repo can ONLY be reached by
 * going to its repo directly. Returns 0 on success. Free with aidx_free. */
int arepo_load_one(const char *repo_id, aidx_index *out);

/* Merge one already-parsed catalog into `out`, stamping `repo_id` on the
 * entries that are actually taken. Exposed for callers that already hold a
 * parsed index. Returns the number of entries added. */
size_t arepo_merge_index(aidx_index *out, const aidx_index *src, const char *repo_id);

#endif /* AMIPKG_AREPO_H */
