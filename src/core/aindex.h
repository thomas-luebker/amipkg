/*
 * aindex.h - amipkg portable core
 *
 * The packages.json subset the on-Amiga client needs, parsed via ajson.
 * The index the client reads is the HOST-VERIFIED SEEDED copy (Phase-3
 * trust model): amipkg never refreshes it over the network - SHA-256 of
 * downloaded archives is checked against this trusted local index.
 */
#ifndef AMIPKG_AINDEX_H
#define AMIPKG_AINDEX_H

#include <stddef.h>

#define AIDX_MAX_DEPS      8
#define AIDX_MAX_PROVIDES  4
#define AIDX_MAX_CAPS      8
#define AIDX_MAX_MIRRORS   4

typedef struct {
    char id[64];
    char min[48];              /* "" = any version */
} aidx_dep;

typedef struct {
    char id[64];
    char version[48];
    char sort_version[48];     /* "" = use version */
    char name[64];
    char category[32];
    char description[128];     /* short catalog description (truncated) */
    char min_cpu[8];           /* "" = any */
    char min_ks[12];           /* requirements.minKS, "" = any (e.g. "3.0") */
    aidx_dep deps[AIDX_MAX_DEPS];
    size_t dep_count;
    char provides[AIDX_MAX_PROVIDES][64];
    size_t provide_count;
    char caps[AIDX_MAX_CAPS][32];
    size_t cap_count;
    char archive_url[256];
    char mirrors[AIDX_MAX_MIRRORS][256];
    size_t mirror_count;
    char archive_sha256[65];   /* "" = unpinned (client refuses install) */
    long archive_size;
    char added[12];            /* "YYYY-MM-DD" repo-added date, "" = unknown */
    int has_recipe;
    int user_visible;
} aidx_entry;

typedef struct {
    long schema;
    long index_version;
    aidx_entry *entries;
    size_t count;
} aidx_index;

/* Parse packages.json text. Returns 0 on success (fills *out; free with
 * aidx_free), nonzero on parse/schema error. */
int aidx_parse(const char *json_text, aidx_index *out);
void aidx_free(aidx_index *idx);

const aidx_entry *aidx_find(const aidx_index *idx, const char *id);
/* The comparable version string (sortVersion when present). */
const char *aidx_comparable_version(const aidx_entry *e);

#endif
