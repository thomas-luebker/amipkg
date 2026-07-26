/*
 * aver.h - amipkg portable core
 *
 * Version ordering: the C mirror of AmigaPackageKit's PackageVersionOrder.
 * The two implementations MUST agree (shared contract, shared test vectors):
 *  - segments are runs of digits or runs of letters; other chars separate
 *  - numeric segments compare numerically ("47.115" > "47.9")
 *  - letter segments compare case-insensitively
 *  - numeric beats text at the same position ("2.10" > "2.b")
 *  - longer prefix-equal version is newer ("2.6b" > "2.6")
 *  - "-" or "" is UNKNOWN: never auto-update against it
 */
#ifndef AMIPKG_AVER_H
#define AMIPKG_AVER_H

int aver_is_unknown(const char *v);
/* -1 / 0 / +1 like strcmp; callers handle unknown themselves. */
int aver_compare(const char *a, const char *b);
/* Update decision: strictly newer, and neither side unknown. */
int aver_is_newer(const char *candidate, const char *installed);

#endif
