/* arun.c — amipkg portable core. Recipe execution PLAN (mirror of RecipeRunner). */

#include "arun.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* fnmatch with FNM_CASEFOLD and '*' spanning '/' — the exact matching
 * AminetInterimStager uses (POSIX fnmatch(..., FNM_CASEFOLD), no FNM_PATHNAME).
 * Iterative backtracking; '?' matches one char, '*' matches any run. */
int arun_glob_match(const char *pat, const char *str)
{
    const char *star = 0, *ss = 0;
    while (*str) {
        int pc = (unsigned char)*pat, sc = (unsigned char)*str;
        if (pc == '*') { star = pat++; ss = str; continue; }
        if (pc == '?' ||
            tolower(pc) == tolower(sc)) { pat++; str++; continue; }
        if (star) { pat = star + 1; str = ++ss; continue; }
        return 0;
    }
    while (*pat == '*') pat++;
    return *pat == '\0';
}

static void copy_str(char *dst, size_t dstsize, const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (n >= dstsize) n = dstsize - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

/* '\' → '/' normalization (recipes may still carry raw CSV separators). */
static void normalize(char *s)
{
    for (; *s; s++) if (*s == '\\') *s = '/';
}

/* Component count of a path ('/'-separated). */
static size_t depth_of(const char *s)
{
    size_t d = 1;
    for (; *s; s++) if (*s == '/') d++;
    return d;
}

/* Last path component. */
static const char *basename_of(const char *s)
{
    const char *slash = strrchr(s, '/');
    return slash ? slash + 1 : s;
}

static int push(arun_plan *p, arun_kind kind)
{
    arun_op *o;
    if (p->count >= ARUN_MAX_OPS) { p->overflow = 1; return 0; }
    o = &p->ops[p->count++];
    memset(o, 0, sizeof *o);
    o->kind = kind;
    return 1;
}

static const char JUNK_PREFIX[] = "__uae___";

static int is_junk(const char *base)
{
    if (strcmp(base, ".DS_Store") == 0) return 1;
    if (strncmp(base, JUNK_PREFIX, sizeof JUNK_PREFIX - 1) == 0) return 1;
    /* bundled Installer files (case-insensitive), files only */
    if (arun_glob_match("Install", base) || arun_glob_match("Install.info", base)
        || arun_glob_match("Install *", base) || arun_glob_match("Install *.info", base)
        || arun_glob_match("*.installer", base) || arun_glob_match("*.installer.info", base))
        return 1;
    return 0;
}

/* copyGlob for one op: match entries against the (normalized) glob at its
 * depth, appending COPY ops dest = "<dest>/<basename>". Mirrors stageWildcard
 * (files) and stageExact (a literal path with an optional rename). */
static void plan_copy(const arecipe_op *op, arun_plan *plan,
                      const char *const *entries, const int *is_dir, size_t n)
{
    char glob[256], dest[256], rename[64];
    size_t i, gdepth;
    int has_star;
    copy_str(glob, sizeof glob, op->src); normalize(glob);
    copy_str(dest, sizeof dest, op->dest); normalize(dest);
    copy_str(rename, sizeof rename, op->rename); normalize(rename);
    has_star = strchr(glob, '*') != 0;
    gdepth = depth_of(glob);

    for (i = 0; i < n; i++) {
        const char *e = entries[i];
        const char *base = basename_of(e);
        if (has_star) {
            if (depth_of(e) != gdepth) continue;
            if (!arun_glob_match(glob, e)) continue;
            if (is_dir && is_dir[i]) continue;            /* files only (dirs implied) */
            if (is_junk(base)) continue;
        } else {
            if (strcmp(e, glob) != 0) continue;           /* exact path */
        }
        if (push(plan, ARUN_COPY)) {
            arun_op *o = &plan->ops[plan->count - 1];
            copy_str(o->src, sizeof o->src, e);
            {
                const char *leaf = (!has_star && rename[0]) ? basename_of(rename) : base;
                if (dest[0] && dest[strlen(dest) - 1] == ':')
                    /* assign-absolute dest ("AMIPKG:"): join without '/' —
                     * "X:/y" would mean the PARENT of X: in AmigaDOS. */
                    snprintf(o->dest, sizeof o->dest, "%s%s", dest, leaf);
                else if (dest[0])
                    snprintf(o->dest, sizeof o->dest, "%s/%s", dest, leaf);
                else
                    copy_str(o->dest, sizeof o->dest, leaf);
            }
        }
    }
}

int arun_plan_build(const arecipe *recipe,
                    const char *const *entries, const int *is_dir, size_t n_entries,
                    arun_plan *out)
{
    size_t i;
    memset(out, 0, sizeof *out);
    if (recipe->has_unknown) return 1;   /* refuse a recipe we can't fully run */
    /* Schema 2 = assign-absolute placeFile dests ("AMIPKG:x"). Refuse
     * anything newer than we understand — a silent mis-run is worse. */
    if (recipe->schema > 2) return 1;

    for (i = 0; i < recipe->op_count; i++) {
        const arecipe_op *op = &recipe->ops[i];
        switch (op->type) {
        case AROP_COPY_GLOB:
        case AROP_PLACE_FILE:
            plan_copy(op, out, entries, is_dir, n_entries);
            break;
        case AROP_STRIP_JUNK:
            /* handled implicitly during copy (junk is never copied) — no op */
            break;
        case AROP_MERGE_NESTED:
            /* Foo/Foo folding is a dest-tree concern the AmigaOS layer applies
             * after copy; represented as no planned op here. */
            break;
        case AROP_SET_EXEC:
            if (push(out, ARUN_SET_EXEC))
                copy_str(out->ops[out->count - 1].dest, sizeof out->ops[0].dest, op->scope);
            break;
        case AROP_SCRIPT_INJECT:
            if (push(out, ARUN_SCRIPT)) {
                arun_op *o = &out->ops[out->count - 1];
                copy_str(o->dest, sizeof o->dest, op->target);
                copy_str(o->overlay, sizeof o->overlay, op->overlay);
                copy_str(o->marker, sizeof o->marker, op->marker);
            }
            break;
        case AROP_TOOLTYPE_EDIT:
        case AROP_MAKE_ASSIGN:
            /* deferred (parity with the host RecipeRunner's deferredOps) */
            break;
        case AROP_PRE_SCRIPT:
        case AROP_POST_SCRIPT:
        case AROP_REMOVE_SCRIPT:
            /* not copy-plan ops: the AmigaOS layer Executes them around the
             * plan (amipkg_run_recipe); nothing to place here */
            break;
        default:
            return 1;
        }
    }
    return 0;
}
