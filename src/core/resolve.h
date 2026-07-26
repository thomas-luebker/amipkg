/*
 * resolve.h — amipkg portable core
 *
 * Dependency resolution: C mirror of AmigaPackageKit's DependencyResolver.
 * Topological expansion honoring `provides` (virtual ids satisfied by
 * variant packages) and CPU-variant selection (highest satisfiable minCPU
 * wins), cycle-safe, unresolvable targets reported not dropped.
 */
#ifndef AMIPKG_RESOLVE_H
#define AMIPKG_RESOLVE_H

#include "aindex.h"

#define ARES_MAX 64

typedef struct {
    /* install order: dependencies precede dependents */
    const aidx_entry *ordered[ARES_MAX];
    size_t count;
    /* unresolvable dep targets (id strings point into the index entries) */
    const char *missing[ARES_MAX];
    size_t missing_count;
} ares_result;

/* Resolve `selected_id` (one root; call repeatedly for a set — placed ids
 * carry over via the result). `cpu` e.g. "68020", NULL/"" = no CPU floor. */
void ares_resolve(const aidx_index *idx, const char *selected_id,
                  const char *cpu, ares_result *result);

/* CPU comparison helper: does `target` satisfy a `floor` (>=)? "" floor = yes. */
int ares_cpu_satisfies(const char *target, const char *floor);

#endif
