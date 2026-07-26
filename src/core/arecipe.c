/* arecipe.c — amipkg portable core. Recipe-op parsing (mirror of RecipeOp). */

#include "arecipe.h"
#include "ajson.h"
#include <string.h>

static void copy_str(char *dst, size_t dstsize, const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (n >= dstsize) n = dstsize - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

static arop_type type_of(const char *op)
{
    if (!op) return AROP_UNKNOWN;
    if (strcmp(op, "copyGlob") == 0) return AROP_COPY_GLOB;
    if (strcmp(op, "stripJunk") == 0) return AROP_STRIP_JUNK;
    if (strcmp(op, "mergeNested") == 0) return AROP_MERGE_NESTED;
    if (strcmp(op, "setExec") == 0) return AROP_SET_EXEC;
    if (strcmp(op, "scriptInject") == 0) return AROP_SCRIPT_INJECT;
    if (strcmp(op, "tooltypeEdit") == 0) return AROP_TOOLTYPE_EDIT;
    if (strcmp(op, "makeAssign") == 0) return AROP_MAKE_ASSIGN;
    if (strcmp(op, "placeFile") == 0) return AROP_PLACE_FILE;
    if (strcmp(op, "preScript") == 0) return AROP_PRE_SCRIPT;
    if (strcmp(op, "postScript") == 0) return AROP_POST_SCRIPT;
    if (strcmp(op, "removeScript") == 0) return AROP_REMOVE_SCRIPT;
    return AROP_UNKNOWN;
}

const char *arop_type_name(arop_type t)
{
    switch (t) {
    case AROP_COPY_GLOB: return "copyGlob";
    case AROP_STRIP_JUNK: return "stripJunk";
    case AROP_MERGE_NESTED: return "mergeNested";
    case AROP_SET_EXEC: return "setExec";
    case AROP_SCRIPT_INJECT: return "scriptInject";
    case AROP_TOOLTYPE_EDIT: return "tooltypeEdit";
    case AROP_MAKE_ASSIGN: return "makeAssign";
    case AROP_PLACE_FILE: return "placeFile";
    case AROP_PRE_SCRIPT: return "preScript";
    case AROP_POST_SCRIPT: return "postScript";
    case AROP_REMOVE_SCRIPT: return "removeScript";
    default: return "unknown";
    }
}

int arecipe_parse(const aj_node *entry_obj, arecipe *out)
{
    const aj_node *recipe = ajson_get(entry_obj, "recipe");
    const aj_node *ops, *c;

    memset(out, 0, sizeof *out);
    if (!recipe) return 1;
    out->schema = ajson_get_num(recipe, "recipeSchema", 1);
    ops = ajson_get(recipe, "ops");
    if (!ops || ops->type != AJ_ARR) return 1;

    for (c = ops->child; c && out->op_count < ARECIPE_MAX_OPS; c = c->next) {
        arecipe_op *o;
        if (c->type != AJ_OBJ) continue;
        o = &out->ops[out->op_count++];
        memset(o, 0, sizeof *o);
        o->type = type_of(ajson_get_str(c, "op", ""));
        switch (o->type) {
        case AROP_COPY_GLOB:
            copy_str(o->src, sizeof o->src, ajson_get_str(c, "src", ""));
            copy_str(o->dest, sizeof o->dest, ajson_get_str(c, "dest", ""));
            copy_str(o->rename, sizeof o->rename, ajson_get_str(c, "rename", ""));
            copy_str(o->overwrite, sizeof o->overwrite, ajson_get_str(c, "overwrite", ""));
            o->recursive = (int)ajson_get_num(c, "recursive", 0);
            break;
        case AROP_SET_EXEC:
            copy_str(o->scope, sizeof o->scope, ajson_get_str(c, "scope", ""));
            o->depth = (int)ajson_get_num(c, "depth", 0);
            break;
        case AROP_SCRIPT_INJECT:
            copy_str(o->target, sizeof o->target, ajson_get_str(c, "target", ""));
            copy_str(o->overlay, sizeof o->overlay, ajson_get_str(c, "overlay", ""));
            copy_str(o->marker, sizeof o->marker, ajson_get_str(c, "marker", ""));
            copy_str(o->mode, sizeof o->mode, ajson_get_str(c, "mode", "append"));
            break;
        case AROP_TOOLTYPE_EDIT:
            copy_str(o->dest, sizeof o->dest, ajson_get_str(c, "icon", ""));
            copy_str(o->key, sizeof o->key, ajson_get_str(c, "key", ""));
            copy_str(o->value, sizeof o->value, ajson_get_str(c, "value", ""));
            break;
        case AROP_MAKE_ASSIGN:
            copy_str(o->name, sizeof o->name, ajson_get_str(c, "name", ""));
            copy_str(o->dest, sizeof o->dest, ajson_get_str(c, "path", ""));
            break;
        case AROP_PLACE_FILE:
            copy_str(o->src, sizeof o->src, ajson_get_str(c, "src", ""));
            copy_str(o->dest, sizeof o->dest, ajson_get_str(c, "dest", ""));
            copy_str(o->overwrite, sizeof o->overwrite, ajson_get_str(c, "overwrite", ""));
            break;
        case AROP_PRE_SCRIPT:
        case AROP_POST_SCRIPT:
        case AROP_REMOVE_SCRIPT: {
            /* Join the inline "lines" array with \n into op->script. */
            const aj_node *lines = ajson_get(c, "lines");
            size_t used = 0;
            o->script[0] = '\0';
            if (lines && lines->type == AJ_ARR) {
                const aj_node *ln;
                for (ln = lines->child; ln; ln = ln->next) {
                    const char *t = (ln->type == AJ_STR && ln->str) ? ln->str : "";
                    size_t tl = strlen(t);
                    if (used + tl + 2 >= sizeof o->script) break;   /* cap: 640 */
                    if (used) o->script[used++] = '\n';
                    memcpy(o->script + used, t, tl);
                    used += tl;
                    o->script[used] = '\0';
                }
            }
            break;
        }
        case AROP_STRIP_JUNK:
        case AROP_MERGE_NESTED:
            break;
        default:
            out->has_unknown = 1;   /* a recipe we can't fully run */
            break;
        }
    }
    return 0;
}
