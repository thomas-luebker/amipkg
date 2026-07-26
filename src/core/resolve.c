/* resolve.c — amipkg portable core. Mirror of DependencyResolver (Swift). */

#include "resolve.h"
#include <string.h>

static int cpu_rank(const char *cpu)
{
    if (!cpu || !cpu[0]) return -1;
    if (strcmp(cpu, "68000") == 0) return 0;
    if (strcmp(cpu, "68010") == 0) return 1;
    if (strcmp(cpu, "68020") == 0) return 2;
    if (strcmp(cpu, "68030") == 0) return 3;
    if (strcmp(cpu, "68040") == 0) return 4;
    if (strcmp(cpu, "68060") == 0) return 5;
    if (strcmp(cpu, "68080") == 0) return 6;
    return -1;
}

int ares_cpu_satisfies(const char *target, const char *floor)
{
    int f, t;
    if (!floor || !floor[0]) return 1;      /* no floor = ok */
    f = cpu_rank(floor);
    if (f < 0) return 1;                    /* unknown floor = permissive */
    t = cpu_rank(target);
    if (t < 0) return 0;                    /* floor set, unknown target = no */
    return t >= f;
}

static int placed(const ares_result *r, const aidx_entry *e)
{
    size_t i;
    for (i = 0; i < r->count; i++)
        if (r->ordered[i] == e) return 1;
    return 0;
}

/* Pick the entry for a dep target: a concrete id, else the best provider
 * (CPU-satisfying, highest floor). NULL when unresolvable. */
static const aidx_entry *pick(const aidx_index *idx, const char *target, const char *cpu)
{
    const aidx_entry *best = NULL, *best_any = NULL;
    int best_rank = -2, best_any_rank = -2;
    size_t i, j;
    const aidx_entry *direct = aidx_find(idx, target);
    if (direct) return direct;
    for (i = 0; i < idx->count; i++) {
        const aidx_entry *e = &idx->entries[i];
        for (j = 0; j < e->provide_count; j++) {
            if (strcmp(e->provides[j], target) != 0) continue;
            {
                int r = cpu_rank(e->min_cpu);
                if (r > best_any_rank) { best_any = e; best_any_rank = r; }
                if (ares_cpu_satisfies(cpu, e->min_cpu) && r > best_rank) {
                    best = e; best_rank = r;
                }
            }
        }
    }
    return best ? best : best_any;
}

static void visit(const aidx_index *idx, const aidx_entry *e, const char *cpu,
                  ares_result *r, const aidx_entry **stack, size_t depth)
{
    size_t i;
    if (!e || placed(r, e)) return;
    for (i = 0; i < depth; i++)
        if (stack[i] == e) return;          /* cycle guard */
    if (depth >= ARES_MAX) return;
    stack[depth] = e;
    for (i = 0; i < e->dep_count; i++) {
        const aidx_entry *dep = pick(idx, e->deps[i].id, cpu);
        if (dep) {
            visit(idx, dep, cpu, r, stack, depth + 1);
        } else if (r->missing_count < ARES_MAX) {
            r->missing[r->missing_count++] = e->deps[i].id;
        }
    }
    if (r->count < ARES_MAX && !placed(r, e))
        r->ordered[r->count++] = e;
}

void ares_resolve(const aidx_index *idx, const char *selected_id,
                  const char *cpu, ares_result *result)
{
    const aidx_entry *stack[ARES_MAX];
    const aidx_entry *root = aidx_find(idx, selected_id);
    if (!root) {
        if (result->missing_count < ARES_MAX)
            result->missing[result->missing_count++] = selected_id;
        return;
    }
    visit(idx, root, cpu, result, stack, 0);
}
