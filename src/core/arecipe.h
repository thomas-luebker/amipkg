/*
 * arecipe.h - amipkg portable core
 *
 * Parse a package's install recipe (the "recipe":{ops:[...]} block of an index
 * entry) into typed ops - the C mirror of AmigaPackageKit's PackageIndex
 * RecipeOp vocabulary. Parsing is portable/host-tested here; EXECUTING the
 * ops (file copy, protect bits, script inject) is the AmigaOS-only next
 * milestone in src/amiga/. Only the portable ops are represented - build-only
 * transforms (icon-patch/adf-unwrap) never appear in a Tier-A recipe.
 */
#ifndef AMIPKG_ARECIPE_H
#define AMIPKG_ARECIPE_H

#include <stddef.h>

struct aj_node;   /* forward decl (ajson.h) */

typedef enum {
    AROP_COPY_GLOB, AROP_STRIP_JUNK, AROP_MERGE_NESTED,
    AROP_SET_EXEC, AROP_SCRIPT_INJECT, AROP_TOOLTYPE_EDIT,
    AROP_MAKE_ASSIGN, AROP_PLACE_FILE,
    AROP_PRE_SCRIPT, AROP_POST_SCRIPT, AROP_REMOVE_SCRIPT,
    AROP_UNKNOWN
} arop_type;

typedef struct {
    arop_type type;
    /* copyGlob / placeFile */
    char src[128];
    char dest[128];
    char rename[64];
    char overwrite[16];      /* "" = default(replace) */
    int recursive;
    /* setExec */
    char scope[32];
    int depth;
    /* scriptInject */
    char target[64];
    char overlay[128];
    char marker[64];
    char mode[16];
    /* tooltypeEdit / makeAssign */
    char key[64];
    char value[128];
    char name[64];
    /* preScript / postScript: the inline AmigaDOS lines joined with \n.
     * Deliberately in arecipe (<=32 ops), NOT arun_plan (512 ops) - keeping
     * arun_op small keeps the plan's static BSS ~362 KB (1MB-machine budget). */
    char script[640];
} arecipe_op;

#define ARECIPE_MAX_OPS 32

typedef struct {
    long schema;
    arecipe_op ops[ARECIPE_MAX_OPS];
    size_t op_count;
    int has_unknown;         /* a recipe with an unknown op is NOT runnable */
} arecipe;

/* Parse the "recipe" member of an index-entry aj_node. Returns 0 on success
 * (fills *out), nonzero when there is no recipe. */
int arecipe_parse(const struct aj_node *entry_obj, arecipe *out);

const char *arop_type_name(arop_type t);

#endif
