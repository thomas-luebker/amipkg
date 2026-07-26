/* aindex.c — amipkg portable core. packages.json subset parser. */

#include "aindex.h"
#include "ajson.h"
#include <stdlib.h>
#include <string.h>

static void copy_str(char *dst, size_t dstsize, const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (n >= dstsize) n = dstsize - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

static void parse_entry(const aj_node *e, aidx_entry *out)
{
    const aj_node *arr, *c, *archive, *reqs, *install;

    memset(out, 0, sizeof *out);
    out->user_visible = 1;
    copy_str(out->id, sizeof out->id, ajson_get_str(e, "id", ""));
    copy_str(out->version, sizeof out->version, ajson_get_str(e, "version", "-"));
    copy_str(out->sort_version, sizeof out->sort_version, ajson_get_str(e, "sortVersion", ""));
    copy_str(out->name, sizeof out->name, ajson_get_str(e, "name", out->id));
    copy_str(out->category, sizeof out->category, ajson_get_str(e, "category", ""));
    copy_str(out->description, sizeof out->description, ajson_get_str(e, "description", ""));

    reqs = ajson_get(e, "requirements");
    if (reqs) {
        copy_str(out->min_cpu, sizeof out->min_cpu, ajson_get_str(reqs, "minCPU", ""));
        copy_str(out->min_ks, sizeof out->min_ks, ajson_get_str(reqs, "minKS", ""));
    }

    arr = ajson_get(e, "deps");
    if (arr && arr->type == AJ_ARR) {
        for (c = arr->child; c && out->dep_count < AIDX_MAX_DEPS; c = c->next) {
            if (c->type != AJ_OBJ) continue;
            copy_str(out->deps[out->dep_count].id, sizeof out->deps[0].id,
                     ajson_get_str(c, "id", ""));
            copy_str(out->deps[out->dep_count].min, sizeof out->deps[0].min,
                     ajson_get_str(c, "min", ""));
            if (out->deps[out->dep_count].id[0]) out->dep_count++;
        }
    }
    arr = ajson_get(e, "provides");
    if (arr && arr->type == AJ_ARR) {
        for (c = arr->child; c && out->provide_count < AIDX_MAX_PROVIDES; c = c->next)
            if (c->type == AJ_STR && c->str && c->str[0])
                copy_str(out->provides[out->provide_count++], sizeof out->provides[0], c->str);
    }
    arr = ajson_get(e, "requiredCapabilities");
    if (arr && arr->type == AJ_ARR) {
        for (c = arr->child; c && out->cap_count < AIDX_MAX_CAPS; c = c->next)
            if (c->type == AJ_STR && c->str && c->str[0])
                copy_str(out->caps[out->cap_count++], sizeof out->caps[0], c->str);
    }
    archive = ajson_get(e, "archive");
    if (archive) {
        copy_str(out->archive_url, sizeof out->archive_url, ajson_get_str(archive, "url", ""));
        copy_str(out->archive_sha256, sizeof out->archive_sha256,
                 ajson_get_str(archive, "sha256", ""));
        out->archive_size = ajson_get_num(archive, "sizeBytes", 0);
        arr = ajson_get(archive, "mirrors");
        if (arr && arr->type == AJ_ARR) {
            for (c = arr->child; c && out->mirror_count < AIDX_MAX_MIRRORS; c = c->next)
                if (c->type == AJ_STR && c->str && c->str[0])
                    copy_str(out->mirrors[out->mirror_count++], sizeof out->mirrors[0], c->str);
        }
    }
    copy_str(out->added, sizeof out->added, ajson_get_str(e, "added", ""));
    out->has_recipe = ajson_get(e, "recipe") != NULL;
    install = ajson_get(e, "install");
    if (install) {
        const aj_node *vis = ajson_get(install, "userVisible");
        if (vis && vis->type == AJ_BOOL) out->user_visible = (int)vis->num;
    }
}

int aidx_parse(const char *json_text, aidx_index *out)
{
    aj_node *root;
    const aj_node *pkgs, *c;
    size_t i = 0;

    memset(out, 0, sizeof *out);
    root = ajson_parse(json_text);
    if (!root) return 1;
    out->schema = ajson_get_num(root, "schema", 0);
    out->index_version = ajson_get_num(root, "indexVersion", 0);
    pkgs = ajson_get(root, "packages");
    if (out->schema != 1 || !pkgs || pkgs->type != AJ_ARR) {
        ajson_free(root);
        return 2;
    }
    out->count = ajson_arr_len(pkgs);
    if (out->count > 0) {
        out->entries = (aidx_entry *)calloc(out->count, sizeof(aidx_entry));
        if (!out->entries) { ajson_free(root); return 3; }
        for (c = pkgs->child; c; c = c->next)
            if (c->type == AJ_OBJ) parse_entry(c, &out->entries[i++]);
        out->count = i;
    }
    ajson_free(root);
    return 0;
}

void aidx_free(aidx_index *idx)
{
    free(idx->entries);
    idx->entries = NULL;
    idx->count = 0;
}

const aidx_entry *aidx_find(const aidx_index *idx, const char *id)
{
    size_t i;
    for (i = 0; i < idx->count; i++)
        if (strcmp(idx->entries[i].id, id) == 0) return &idx->entries[i];
    return NULL;
}

const char *aidx_comparable_version(const aidx_entry *e)
{
    return e->sort_version[0] ? e->sort_version : e->version;
}
