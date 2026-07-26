/* receipts.c - amipkg portable core. Mirror of ReceiptDB (Swift). */

#include "receipts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Copy the next |-separated field of `line` (bounded by `end`) into dst.
 * Advances *line past the separator. Returns 1 if a field existed. */
static int take_field(const char **line, const char *end, char *dst, size_t dstsize)
{
    const char *p = *line;
    size_t n = 0;
    if (p >= end) { dst[0] = '\0'; return 0; }
    while (p < end && *p != '|') {
        if (n + 1 < dstsize) dst[n++] = *p;
        p++;
    }
    dst[n] = '\0';
    *line = (p < end) ? p + 1 : p;
    return 1;
}

/* Iterate lines of `text`; cb-style loop kept inline for simplicity. */
size_t rcpt_parse_installed(const char *text, rcpt_installed *out, size_t max)
{
    size_t count = 0;
    const char *p = text ? text : "";
    while (*p && count < max) {
        const char *nl = strchr(p, '\n');
        const char *end = nl ? nl : p + strlen(p);
        if (end > p) {
            rcpt_installed *r = &out[count];
            char numbuf[24];
            const char *cur = p;
            r->index_version = r->install_epoch = 0;
            if (take_field(&cur, end, r->id, sizeof r->id) && r->id[0] != '\0') {
                take_field(&cur, end, r->version, sizeof r->version);
                if (r->version[0] == '\0') strcpy(r->version, "-");
                if (take_field(&cur, end, numbuf, sizeof numbuf))
                    r->index_version = atol(numbuf);
                if (take_field(&cur, end, numbuf, sizeof numbuf))
                    r->install_epoch = atol(numbuf);
                count++;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    return count;
}

size_t rcpt_parse_files(const char *text, rcpt_file *out, size_t max)
{
    size_t count = 0;
    const char *p = text ? text : "";
    while (*p && count < max) {
        const char *nl = strchr(p, '\n');
        const char *end = nl ? nl : p + strlen(p);
        if (end > p) {
            rcpt_file *f = &out[count];
            const char *cur = p;
            if (take_field(&cur, end, f->path, sizeof f->path) && f->path[0] != '\0') {
                take_field(&cur, end, f->sha256, sizeof f->sha256);
                count++;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    return count;
}

size_t rcpt_parse_edits(const char *text, rcpt_edit *out, size_t max)
{
    size_t count = 0;
    const char *p = text ? text : "";
    while (*p && count < max) {
        const char *nl = strchr(p, '\n');
        const char *end = nl ? nl : p + strlen(p);
        if (end > p) {
            rcpt_edit *e = &out[count];
            const char *cur = p;
            if (take_field(&cur, end, e->target, sizeof e->target) && e->target[0] != '\0') {
                take_field(&cur, end, e->overlay, sizeof e->overlay);
                take_field(&cur, end, e->script_version, sizeof e->script_version);
                if (e->overlay[0] != '\0') count++;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    return count;
}

size_t rcpt_format_installed_line(const rcpt_installed *r, char *buf, size_t bufsize)
{
    int n = snprintf(buf, bufsize, "%s|%s|%ld|%ld",
                     r->id, r->version[0] ? r->version : "-",
                     r->index_version, r->install_epoch);
    return n > 0 ? (size_t)n : 0;
}
