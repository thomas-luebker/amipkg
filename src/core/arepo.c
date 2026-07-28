/* arepo.c - amipkg portable core. The repository list (see arepo.h). */

#include "arepo.h"
#include "store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- helpers */

/* Case-insensitive compare. Not strcasecmp: that is a POSIX extension and this
 * core is compiled by two very different toolchains. ASCII is all we need -
 * repo ids are [A-Za-z0-9_-] by arepo_id_valid. */
static int ci_eq(const char *a, const char *b)
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

static void copy_into(char *dst, size_t n, const char *src)
{
    if (n == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
}

static int is_space(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* Trim trailing whitespace in place. */
static void rtrim(char *s)
{
    size_t n = strlen(s);
    while (n && is_space((unsigned char)s[n - 1])) s[--n] = '\0';
}

static void default_list(arepo_list *out)
{
    out->count = 1;
    copy_into(out->v[0].id,  sizeof out->v[0].id,  AREPO_OFFICIAL_ID);
    copy_into(out->v[0].url, sizeof out->v[0].url, AMIPKG_OFFICIAL_URL);
    copy_into(out->v[0].key, sizeof out->v[0].key, AMIPKG_OFFICIAL_PUBKEY);
    out->v[0].enabled = 1;
}

/* ------------------------------------------------------------- validation */

int arepo_id_valid(const char *id)
{
    size_t i;
    if (!id || !id[0]) return 1;
    if (strlen(id) >= AREPO_ID_MAX) return 1;
    for (i = 0; id[i]; i++) {
        int c = (unsigned char)id[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return 1;   /* keeps '/', ':' and ".." out of a path component */
    }
    return 0;
}

int arepo_url_valid(const char *url)
{
    if (!url || !url[0]) return 1;
    if (strlen(url) >= AREPO_URL_MAX) return 1;
    if (strncmp(url, "http://", 7) == 0 && url[7])  return 0;
    if (strncmp(url, "https://", 8) == 0 && url[8]) return 0;
    return 1;
}

/* A pinned key must look like a 32-byte Ed25519 key in base64 (44 chars with
 * the trailing '='). Cheap shape check so a typo is caught at add time rather
 * than as a mystifying verification failure on the next update. */
static int key_valid(const char *key)
{
    size_t i, n;
    if (!key || !key[0]) return 1;
    n = strlen(key);
    if (n != 44 || key[43] != '=') return 1;
    for (i = 0; i < 43; i++) {
        int c = (unsigned char)key[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
              || (c >= '0' && c <= '9') || c == '+' || c == '/';
        if (!ok) return 1;
    }
    return 0;
}

int arepo_is_signed(const arepo_entry *e) { return e && e->key[0] != '\0'; }

/* ------------------------------------------------------------- load / save */

/* One config line: "<enabled> <id> <key|-> <url...>". Returns 1 if it filled
 * *e, 0 to skip the line. Unparsable lines are SKIPPED, never fatal - a
 * corrupt list must not lock the user out of their package manager. */
static int parse_line(char *line, arepo_entry *e)
{
    char *p = line, *tok;
    char *fields[3];
    int i;

    while (*p && is_space((unsigned char)*p)) p++;
    if (*p == '\0' || *p == '#') return 0;

    for (i = 0; i < 3; i++) {
        tok = p;
        while (*p && !is_space((unsigned char)*p)) p++;
        if (*p) *p++ = '\0';
        while (*p && is_space((unsigned char)*p)) p++;
        if (!tok[0]) return 0;
        fields[i] = tok;
    }
    /* Remainder is the URL: taken whole so nothing in it can confuse the
     * tokeniser (it is the last field for exactly this reason). */
    rtrim(p);
    if (!p[0]) return 0;

    if (arepo_id_valid(fields[1]) != 0) return 0;
    if (arepo_url_valid(p) != 0) return 0;

    e->enabled = (fields[0][0] == '1');
    copy_into(e->id,  sizeof e->id,  fields[1]);
    copy_into(e->url, sizeof e->url, p);
    /* "-" is the unsigned marker; a malformed key is dropped to unsigned
     * rather than silently trusted. */
    if (strcmp(fields[2], "-") == 0 || key_valid(fields[2]) != 0) e->key[0] = '\0';
    else copy_into(e->key, sizeof e->key, fields[2]);
    return 1;
}

void arepo_load(arepo_list *out)
{
    char *text, *line, *next;

    out->count = 0;
    text = read_file(amipkg_data_path("config/repos"));
    if (!text) { default_list(out); return; }

    for (line = text; line && *line && out->count < AREPO_MAX; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        if (parse_line(line, &out->v[out->count])) out->count++;
    }
    free(text);

    /* An empty or entirely unparsable file must not leave the user with no
     * repo at all. */
    if (out->count == 0) default_list(out);
}

int arepo_save(const arepo_list *l)
{
    FILE *f;
    size_t i;
    f = fopen(amipkg_data_path("config/repos"), "wb");
    if (!f) return 1;
    fprintf(f, "# amipkg repositories - ORDER IS PRIORITY (first match wins).\n");
    fprintf(f, "# <enabled 0|1> <id> <public-key-base64 or - for unsigned> <url>\n");
    fprintf(f, "# Edit with 'amipkg repo' rather than by hand where you can.\n");
    for (i = 0; i < l->count; i++) {
        const arepo_entry *e = &l->v[i];
        fprintf(f, "%d %s %s %s\n", e->enabled ? 1 : 0, e->id,
                e->key[0] ? e->key : "-", e->url);
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ lookup */

int arepo_find(const arepo_list *l, const char *id)
{
    size_t i;
    if (!id || !id[0]) return -1;
    for (i = 0; i < l->count; i++)
        if (ci_eq(l->v[i].id, id)) return (int)i;
    return -1;
}

/* ------------------------------------------------------------------ mutate */

int arepo_add(arepo_list *l, const char *id, const char *url, const char *key)
{
    arepo_entry *e;
    if (l->count >= AREPO_MAX)      return 1;
    if (arepo_id_valid(id) != 0)    return 2;
    if (arepo_find(l, id) >= 0)     return 3;
    if (arepo_url_valid(url) != 0)  return 4;
    if (key && key[0] && key_valid(key) != 0) return 5;

    e = &l->v[l->count++];
    copy_into(e->id,  sizeof e->id,  id);
    copy_into(e->url, sizeof e->url, url);
    copy_into(e->key, sizeof e->key, (key && key[0]) ? key : "");
    e->enabled = 1;
    return 0;
}

int arepo_remove(arepo_list *l, const char *id)
{
    int at = arepo_find(l, id);
    size_t i;
    if (at < 0) return 1;
    for (i = (size_t)at; i + 1 < l->count; i++) l->v[i] = l->v[i + 1];
    l->count--;
    return 0;
}

int arepo_set_enabled(arepo_list *l, const char *id, int enabled)
{
    int at = arepo_find(l, id);
    if (at < 0) return 1;
    l->v[at].enabled = enabled ? 1 : 0;
    return 0;
}

int arepo_move(arepo_list *l, const char *id, int delta)
{
    int at = arepo_find(l, id);
    int to;
    arepo_entry tmp;
    if (at < 0) return 1;
    to = at + delta;
    if (to < 0) to = 0;
    if (to >= (int)l->count) to = (int)l->count - 1;
    while (at < to) { tmp = l->v[at]; l->v[at] = l->v[at+1]; l->v[at+1] = tmp; at++; }
    while (at > to) { tmp = l->v[at]; l->v[at] = l->v[at-1]; l->v[at-1] = tmp; at--; }
    return 0;
}

/* ------------------------------------------------------------------- paths */

void arepo_dir_path(const char *id, char *out, size_t n)
{
    if (n == 0) return;
    if (!id || ci_eq(id, AREPO_OFFICIAL_ID)) { copy_into(out, n, amipkg_prefix()); return; }
    snprintf(out, n, "%srepos/%s", amipkg_prefix(), id);
}

void arepo_catalog_path(const char *id, char *out, size_t n)
{
    if (n == 0) return;
    if (!id || ci_eq(id, AREPO_OFFICIAL_ID)) {
        copy_into(out, n, amipkg_data_path("packages.json"));
        return;
    }
    snprintf(out, n, "%srepos/%s/packages.json", amipkg_prefix(), id);
}

void arepo_sig_path(const char *id, char *out, size_t n)
{
    if (n == 0) return;
    if (!id || ci_eq(id, AREPO_OFFICIAL_ID)) {
        copy_into(out, n, amipkg_data_path("packages.json.sig"));
        return;
    }
    snprintf(out, n, "%srepos/%s/packages.json.sig", amipkg_prefix(), id);
}

/* ------------------------------------------------------------------ merge */

size_t arepo_merge_index(aidx_index *out, const aidx_index *src, const char *repo_id)
{
    size_t i, added = 0;
    aidx_entry *grown;

    if (!src || src->count == 0) return 0;

    /* Grow once for the whole source rather than per entry - realloc churn is
     * expensive on the machines this runs on. Slight over-allocation when some
     * entries are shadowed is a fair trade. */
    grown = (aidx_entry *)realloc(out->entries,
                                  (out->count + src->count) * sizeof(aidx_entry));
    if (!grown) return 0;                    /* keep what we had */
    out->entries = grown;

    for (i = 0; i < src->count; i++) {
        const aidx_entry *e = &src->entries[i];
        /* First repo to provide an id wins. Note this find is over the entries
         * accumulated SO FAR, which is exactly priority order. */
        if (aidx_find(out, e->id)) continue;
        out->entries[out->count] = *e;
        copy_into(out->entries[out->count].repo,
                  sizeof out->entries[out->count].repo, repo_id);
        out->count++;
        added++;
    }
    return added;
}

int arepo_load_one(const char *repo_id, aidx_index *out)
{
    char path[320];
    char *text;
    int rc;

    memset(out, 0, sizeof *out);
    arepo_catalog_path(repo_id, path, sizeof path);
    text = read_file(path);
    if (!text) return 1;
    rc = aidx_parse(text, out);
    free(text);
    if (rc != 0) { memset(out, 0, sizeof *out); return rc; }
    /* Stamp provenance so callers can display it exactly as with the merged
     * view - a single-repo load is otherwise indistinguishable. */
    {
        size_t i;
        for (i = 0; i < out->count; i++)
            copy_into(out->entries[i].repo, sizeof out->entries[i].repo, repo_id);
    }
    return 0;
}

int arepo_load_merged(aidx_index *out)
{
    arepo_list l;
    size_t i;

    memset(out, 0, sizeof *out);
    arepo_load(&l);

    for (i = 0; i < l.count; i++) {
        char path[320];
        char *text;
        aidx_index tmp;

        if (!l.v[i].enabled) continue;
        arepo_catalog_path(l.v[i].id, path, sizeof path);
        text = read_file(path);
        if (!text) continue;                 /* never fetched/seeded yet */

        memset(&tmp, 0, sizeof tmp);
        if (aidx_parse(text, &tmp) == 0) {
            /* Schema/index version come from the highest-priority repo that
             * actually had a catalog. */
            if (out->count == 0) { out->schema = tmp.schema; out->index_version = tmp.index_version; }
            arepo_merge_index(out, &tmp, l.v[i].id);
            aidx_free(&tmp);
        }
        free(text);                          /* freed before the next repo */
    }
    return out->count ? 0 : 1;
}

/* -------------------------------------------------------------- spec split */

int arepo_split_spec(const char *spec, char *repo_out, size_t repo_n,
                     char *id_out, size_t id_n)
{
    const char *colon;
    char repo[AREPO_ID_MAX];
    size_t rl;

    if (repo_n) repo_out[0] = '\0';
    copy_into(id_out, id_n, spec ? spec : "");
    if (!spec || !spec[0]) return 0;

    colon = strchr(spec, ':');
    if (!colon || colon == spec || !colon[1]) return 0;

    rl = (size_t)(colon - spec);
    if (rl >= sizeof repo) return 0;
    memcpy(repo, spec, rl);
    repo[rl] = '\0';

    /* Only treat it as a qualifier if the prefix is a legal repo id. That also
     * keeps an Amiga path ("SYS:Programs") from being mistaken for one, since
     * those are not offered as package specs anyway. */
    if (arepo_id_valid(repo) != 0) return 0;

    copy_into(repo_out, repo_n, repo);
    copy_into(id_out, id_n, colon + 1);
    return 1;
}
