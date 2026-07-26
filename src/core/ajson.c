/* ajson.c — amipkg portable core. Minimal JSON tree parser. */

#include "ajson.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *p; int depth; } P;

#define AJ_MAX_DEPTH 32

static aj_node *parse_value(P *ps);

static aj_node *node_new(aj_type t)
{
    aj_node *n = (aj_node *)calloc(1, sizeof(aj_node));
    if (n) n->type = t;
    return n;
}

void ajson_free(aj_node *n)
{
    while (n) {
        aj_node *next = n->next;
        ajson_free(n->child);
        free(n->str);
        free(n->key);
        free(n);
        n = next;
    }
}

static void skip_ws(P *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
        ps->p++;
}

/* Parse a JSON string (opening quote consumed by caller? no — expects `"`).
 * Returns heap copy, NULL on error. */
static char *parse_string(P *ps)
{
    size_t cap = 32, len = 0;
    char *out;
    if (*ps->p != '"') return NULL;
    ps->p++;
    out = (char *)malloc(cap);
    if (!out) return NULL;
    while (*ps->p && *ps->p != '"') {
        char c = *ps->p;
        if (c == '\\') {
            ps->p++;
            switch (*ps->p) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                int i;
                for (i = 0; i < 4 && ps->p[1]; i++) ps->p++;
                c = '?';   /* non-ASCII escapes degrade to '?' (ids/paths are ASCII) */
                break;
            }
            default: free(out); return NULL;
            }
        }
        if (len + 2 > cap) {
            char *grown;
            cap *= 2;
            grown = (char *)realloc(out, cap);
            if (!grown) { free(out); return NULL; }
            out = grown;
        }
        out[len++] = c;
        ps->p++;
    }
    if (*ps->p != '"') { free(out); return NULL; }
    ps->p++;
    out[len] = '\0';
    return out;
}

static aj_node *parse_object(P *ps)
{
    aj_node *obj = node_new(AJ_OBJ);
    aj_node **tail = obj ? &obj->child : NULL;
    if (!obj) return NULL;
    ps->p++;                       /* consume '{' */
    skip_ws(ps);
    if (*ps->p == '}') { ps->p++; return obj; }
    for (;;) {
        char *key;
        aj_node *val;
        skip_ws(ps);
        key = parse_string(ps);
        if (!key) { ajson_free(obj); return NULL; }
        skip_ws(ps);
        if (*ps->p != ':') { free(key); ajson_free(obj); return NULL; }
        ps->p++;
        val = parse_value(ps);
        if (!val) { free(key); ajson_free(obj); return NULL; }
        val->key = key;
        *tail = val;
        tail = &val->next;
        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == '}') { ps->p++; return obj; }
        ajson_free(obj);
        return NULL;
    }
}

static aj_node *parse_array(P *ps)
{
    aj_node *arr = node_new(AJ_ARR);
    aj_node **tail = arr ? &arr->child : NULL;
    if (!arr) return NULL;
    ps->p++;                       /* consume '[' */
    skip_ws(ps);
    if (*ps->p == ']') { ps->p++; return arr; }
    for (;;) {
        aj_node *val = parse_value(ps);
        if (!val) { ajson_free(arr); return NULL; }
        *tail = val;
        tail = &val->next;
        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == ']') { ps->p++; return arr; }
        ajson_free(arr);
        return NULL;
    }
}

static aj_node *parse_value(P *ps)
{
    aj_node *n;
    skip_ws(ps);
    if (ps->depth >= AJ_MAX_DEPTH) return NULL;
    switch (*ps->p) {
    case '{': ps->depth++; n = parse_object(ps); ps->depth--; return n;
    case '[': ps->depth++; n = parse_array(ps); ps->depth--; return n;
    case '"': {
        char *s = parse_string(ps);
        if (!s) return NULL;
        n = node_new(AJ_STR);
        if (!n) { free(s); return NULL; }
        n->str = s;
        return n;
    }
    case 't':
        if (strncmp(ps->p, "true", 4) == 0) {
            ps->p += 4; n = node_new(AJ_BOOL); if (n) n->num = 1; return n;
        }
        return NULL;
    case 'f':
        if (strncmp(ps->p, "false", 5) == 0) {
            ps->p += 5; n = node_new(AJ_BOOL); return n;
        }
        return NULL;
    case 'n':
        if (strncmp(ps->p, "null", 4) == 0) { ps->p += 4; return node_new(AJ_NULL); }
        return NULL;
    default:
        if (*ps->p == '-' || isdigit((unsigned char)*ps->p)) {
            char *end;
            long v = strtol(ps->p, &end, 10);
            /* skip a fractional/exponent tail (index numbers are integral) */
            if (*end == '.') { end++; while (isdigit((unsigned char)*end)) end++; }
            if (*end == 'e' || *end == 'E') {
                end++;
                if (*end == '+' || *end == '-') end++;
                while (isdigit((unsigned char)*end)) end++;
            }
            ps->p = end;
            n = node_new(AJ_NUM);
            if (n) n->num = v;
            return n;
        }
        return NULL;
    }
}

aj_node *ajson_parse(const char *text)
{
    P ps;
    aj_node *root;
    if (!text) return NULL;
    ps.p = text;
    ps.depth = 0;
    root = parse_value(&ps);
    if (!root) return NULL;
    skip_ws(&ps);
    if (*ps.p != '\0') { ajson_free(root); return NULL; }
    return root;
}

const aj_node *ajson_get(const aj_node *obj, const char *key)
{
    const aj_node *c;
    if (!obj || obj->type != AJ_OBJ) return NULL;
    for (c = obj->child; c; c = c->next)
        if (c->key && strcmp(c->key, key) == 0) return c;
    return NULL;
}

const char *ajson_get_str(const aj_node *obj, const char *key, const char *fallback)
{
    const aj_node *n = ajson_get(obj, key);
    return (n && n->type == AJ_STR && n->str) ? n->str : fallback;
}

long ajson_get_num(const aj_node *obj, const char *key, long fallback)
{
    const aj_node *n = ajson_get(obj, key);
    return (n && (n->type == AJ_NUM || n->type == AJ_BOOL)) ? n->num : fallback;
}

size_t ajson_arr_len(const aj_node *arr)
{
    size_t n = 0;
    const aj_node *c;
    if (!arr || arr->type != AJ_ARR) return 0;
    for (c = arr->child; c; c = c->next) n++;
    return n;
}
