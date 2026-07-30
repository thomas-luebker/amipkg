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
    /* requirements.architecture, Aminet's vocabulary ("m68k-amigaos",
     * "ppc-morphos", "ppc-amigaos", "i386-aros", "generic"), and COMMA-
     * SEPARATED when a package targets several - which is how real Aminet
     * readmes are written ("m68k-amigaos,ppc-amigaos,ppc-morphos"). "" means
     * m68k-amigaos - the whole catalog predates this field, so absent has to
     * mean the platform everything was written for. */
    char arch[72];
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
    /* Source repository id, stamped by arepo_load_merged. "" when the index
     * was parsed standalone (aidx_parse never sets it). Front-ends SHOW this,
     * because with pure-priority resolution a repo placed high can shadow a
     * package from a lower one - that must be visible, not silent. */
    char repo[32];
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

/* The architecture an entry targets, with the default applied ("" ->
 * m68k-amigaos). Never returns NULL. */
const char *aidx_arch(const aidx_entry *e);

/* Can a package built for `pkg_arch` run on a machine that is `host_arch`?
 *
 * Deliberately INCLUSIVE where the hardware is: MorphOS and AmigaOS 4 both run
 * m68k binaries (MorphOS's emulation, OS4's Petunia), so a 68k package is
 * offered there rather than hidden - that is most of the catalog and it works.
 * The reverse is never true, and AROS on i386/x86_64 runs neither. "generic"
 * (documentation, data, art) runs anywhere.
 *
 * Empty/unknown values are treated as m68k-amigaos, so an old catalog behaves
 * exactly as before. */
int aidx_arch_runs_on(const char *pkg_arch, const char *host_arch);

#endif
