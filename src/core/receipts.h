/*
 * receipts.h — amipkg portable core
 *
 * The installed-package receipt DB, C mirror of AmigaPackageKit's ReceiptDB
 * line formats (docs/agent/package-manager-plan.md in the AmigaImager repo):
 *   installed.txt        id|version|indexVersion|installEpoch
 *   files/<id>.files     path|sha256   (bare path = no digest → must ask)
 *   scripts/<id>.edits   target|overlayName|scriptVersion
 * The formats are line-oriented text precisely so THIS parser stays trivial.
 */
#ifndef AMIPKG_RECEIPTS_H
#define AMIPKG_RECEIPTS_H

#include <stddef.h>

typedef struct {
    char id[64];
    char version[48];
    long index_version;
    long install_epoch;
} rcpt_installed;

typedef struct {
    char path[256];
    char sha256[65];    /* "" = no digest recorded */
} rcpt_file;

typedef struct {
    char target[64];
    char overlay[64];
    char script_version[32];
} rcpt_edit;

/* Parse installed.txt content. Returns count parsed (<= max). */
size_t rcpt_parse_installed(const char *text, rcpt_installed *out, size_t max);
/* Parse a files/<id>.files content. Returns count parsed (<= max). */
size_t rcpt_parse_files(const char *text, rcpt_file *out, size_t max);
/* Parse a scripts/<id>.edits content. Returns count parsed (<= max). */
size_t rcpt_parse_edits(const char *text, rcpt_edit *out, size_t max);

/* Format one installed.txt line into buf (returns chars written, excl NUL). */
size_t rcpt_format_installed_line(const rcpt_installed *r, char *buf, size_t bufsize);

#endif
