/*
 * ajson.h — amipkg portable core
 *
 * Minimal JSON tree parser for the packages.json index — deliberately small
 * (the schema is flat by design so a 68k parser stays trivial). Supports
 * objects, arrays, strings (with \" \\ \/ \n \r \t and \uXXXX→'?' escapes),
 * numbers (long), true/false/null. Everything heap-allocated; free with
 * ajson_free on the root.
 */
#ifndef AMIPKG_AJSON_H
#define AMIPKG_AJSON_H

#include <stddef.h>

typedef enum {
    AJ_NULL, AJ_BOOL, AJ_NUM, AJ_STR, AJ_ARR, AJ_OBJ
} aj_type;

typedef struct aj_node {
    aj_type type;
    /* AJ_BOOL/AJ_NUM */
    long num;
    /* AJ_STR: NUL-terminated heap copy */
    char *str;
    /* AJ_OBJ member key (heap), else NULL */
    char *key;
    /* AJ_ARR/AJ_OBJ children (linked list), and next sibling */
    struct aj_node *child;
    struct aj_node *next;
} aj_node;

/* Parse a NUL-terminated JSON text. NULL on syntax error. */
aj_node *ajson_parse(const char *text);
void ajson_free(aj_node *n);

/* Object member by key (NULL when absent / not an object). */
const aj_node *ajson_get(const aj_node *obj, const char *key);
/* String value of an object member, or fallback. */
const char *ajson_get_str(const aj_node *obj, const char *key, const char *fallback);
/* Number value of an object member, or fallback. */
long ajson_get_num(const aj_node *obj, const char *key, long fallback);
/* Array length. */
size_t ajson_arr_len(const aj_node *arr);

#endif
