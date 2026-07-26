/*
 * arun.h - amipkg portable core
 *
 * The recipe EXECUTION PLAN: given a recipe and a listing of an extracted
 * archive tree, compute the concrete file operations to perform - which
 * source file goes to which destination, with which protection, plus the
 * strip/exec/script-inject actions. This is the portable, host-tested half
 * of on-Amiga install (mirror of RecipeRunner's decisions); the AmigaOS
 * half (src/amiga/install.c) just executes these ops with dos.library.
 *
 * The caller feeds the extracted tree as a flat list of relative paths (what
 * `lha l`/a directory walk yields). arun matches globs the same way
 * AminetInterimStager does: fnmatch-casefold, '*' spans '/', depth-matched.
 */
#ifndef AMIPKG_ARUN_H
#define AMIPKG_ARUN_H

#include "arecipe.h"
#include <stddef.h>

#define ARUN_MAX_OPS   512

typedef enum {
    ARUN_COPY,        /* copy src -> dest (a file) */
    ARUN_SET_EXEC,    /* set the script/exec protection bit on dest */
    ARUN_STRIP,       /* delete dest (junk / bundled Installer) */
    ARUN_SCRIPT       /* inject overlay into a boot script */
} arun_kind;

typedef struct {
    arun_kind kind;
    char src[256];        /* extract-relative source (COPY) */
    char dest[256];       /* volume-relative dest, or the boot script (SCRIPT) */
    char overlay[128];    /* SCRIPT: the overlay file (extract-relative) */
    char marker[64];      /* SCRIPT: ScriptEditor marker name */
} arun_op;

typedef struct {
    arun_op ops[ARUN_MAX_OPS];
    size_t count;
    int overflow;         /* the tree produced more ops than ARUN_MAX_OPS */
} arun_plan;

/* Build the execution plan. `entries`/`n_entries` is the extracted tree as
 * relative paths (files only; directories implied). `is_dir(i)` returns
 * nonzero when entry i is a directory (dirs are not copied, only descended).
 * Returns 0 on success. */
int arun_plan_build(const arecipe *recipe,
                    const char *const *entries, const int *is_dir, size_t n_entries,
                    arun_plan *out);

/* fnmatch-casefold with '*' spanning '/', matched on the whole string.
 * Exposed for its own tests. */
int arun_glob_match(const char *pattern, const char *string);

#endif
