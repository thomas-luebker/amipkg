/* aver.c - amipkg portable core. Mirror of PackageVersionOrder (Swift). */

#include "aver.h"
#include <ctype.h>
#include <string.h>

int aver_is_unknown(const char *v)
{
    if (!v) return 1;
    while (*v == ' ' || *v == '\t') v++;
    if (*v == '\0') return 1;
    if (v[0] == '-') {
        const char *p = v + 1;
        while (*p == ' ' || *p == '\t') p++;
        return *p == '\0';
    }
    return 0;
}

/* Advance past one segment starting at *p (digits or letters). Returns
 * 0=end reached, 1=numeric segment, 2=letter segment. Segment bounds in
 * seg_start/seg_len. Skips separator characters first. */
static int next_segment(const char **p, const char **seg_start, size_t *seg_len)
{
    const char *s = *p;
    while (*s && !isalnum((unsigned char)*s)) s++;
    if (!*s) { *p = s; return 0; }
    *seg_start = s;
    if (isdigit((unsigned char)*s)) {
        while (isdigit((unsigned char)*s)) s++;
        *seg_len = (size_t)(s - *seg_start);
        *p = s;
        return 1;
    }
    while (isalpha((unsigned char)*s)) s++;
    *seg_len = (size_t)(s - *seg_start);
    *p = s;
    return 2;
}

static int cmp_numeric(const char *a, size_t alen, const char *b, size_t blen)
{
    /* strip leading zeros, then longer wins, then lexicographic */
    while (alen > 1 && *a == '0') { a++; alen--; }
    while (blen > 1 && *b == '0') { b++; blen--; }
    if (alen != blen) return alen < blen ? -1 : 1;
    {
        size_t i;
        for (i = 0; i < alen; i++)
            if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

static int cmp_alpha_ci(const char *a, size_t alen, const char *b, size_t blen)
{
    size_t n = alen < blen ? alen : blen;
    size_t i;
    for (i = 0; i < n; i++) {
        int ca = tolower((unsigned char)a[i]);
        int cb = tolower((unsigned char)b[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (alen != blen) return alen < blen ? -1 : 1;
    return 0;
}

int aver_compare(const char *a, const char *b)
{
    const char *pa = a ? a : "", *pb = b ? b : "";
    for (;;) {
        const char *sa = 0, *sb = 0;
        size_t la = 0, lb = 0;
        int ta = next_segment(&pa, &sa, &la);
        int tb = next_segment(&pb, &sb, &lb);
        if (ta == 0 && tb == 0) return 0;
        if (ta == 0) return -1;          /* b longer -> b newer */
        if (tb == 0) return 1;           /* a longer -> a newer */
        if (ta == 1 && tb == 1) {
            int c = cmp_numeric(sa, la, sb, lb);
            if (c) return c;
        } else if (ta == 2 && tb == 2) {
            int c = cmp_alpha_ci(sa, la, sb, lb);
            if (c) return c;
        } else {
            return ta == 1 ? 1 : -1;     /* numeric beats text */
        }
    }
}

int aver_is_newer(const char *candidate, const char *installed)
{
    if (aver_is_unknown(candidate) || aver_is_unknown(installed)) return 0;
    return aver_compare(candidate, installed) > 0;
}
