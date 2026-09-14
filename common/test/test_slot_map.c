/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * Slot-map verification (gates the GPU BLOCK / THREAD strategies).
 *
 * pose_graph_slot_map assigns every transient scaffold node a recycled scratch
 * slot so the per-chunk GPU kernels can label a super-chunk in a small pool
 * instead of the full scaffold.  Two properties must hold for the map to be
 * safe AND for the unattested scratch footprint to actually shrink:
 *
 *   1. Bound:    pool_size <= 8 * half_w  (the plan's structural high-water).
 *   2. Liveness: no two nodes whose level-granular live ranges overlap share a
 *                slot.  A node id is "live" across levels [level[id], last_use[id]]
 *                (last_use = max level of any reader; a sink dies at its own
 *                level).  Output-set nodes are written straight to the persisted
 *                region (SENTINEL slot) and excluded from the pool.
 *
 * Both checks recompute level/last_use INDEPENDENTLY from pose_graph_edges so the
 * test does not trust pose_graph_slot_map's internal arithmetic.
 *
 * Default sweep stops at 2^17 (the value used in the benchmarks) to stay within
 * host RAM; pass a max-cb power of two as argv[1] to push higher (2^18..2^20 need
 * many GB — run in the CUDA Docker image or a big-mem host).
 */
#include "../include/graph.h"
#include "../include/hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define EXPECT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s\n", msg); failures++; } \
    else         { fprintf(stdout, "PASS %s\n", msg); } \
} while (0)

typedef struct { uint32_t slot, level, last_use; } liverange_t;

static int cmp_liverange(const void *a, const void *b)
{
    const liverange_t *x = (const liverange_t *)a, *y = (const liverange_t *)b;
    if (x->slot != y->slot)   return x->slot   < y->slot   ? -1 : 1;
    if (x->level != y->level) return x->level  < y->level  ? -1 : 1;
    return 0;
}

static void check_cb(uint32_t cb)
{
    char msg[160];
    int n = pose_graph_parameter_n(cb);
    uint64_t half_w = (uint64_t)1 << n;

    uint64_t count = pose_graph_scaffold_node_count(cb);
    uint32_t *slot = (uint32_t *)malloc(count * sizeof(uint32_t));
    if (!slot) { fprintf(stderr, "FAIL cb=%u: slot malloc (%llu nodes)\n",
                         cb, (unsigned long long)count); failures++; return; }

    uint32_t pool_size = 0;
    int rc = pose_graph_slot_map(cb, slot, &pool_size);
    snprintf(msg, sizeof(msg), "cb=%u slot_map returns 0 (count=%llu)",
             cb, (unsigned long long)count);
    EXPECT(rc == 0, msg);
    if (rc != 0) { free(slot); return; }

    snprintf(msg, sizeof(msg),
             "cb=%u pool_size=%u <= 8*half_w=%llu  (%.1fx reduction vs full scaffold)",
             cb, pool_size, (unsigned long long)(8 * half_w),
             (double)count / (double)(pool_size ? pool_size : 1));
    EXPECT((uint64_t)pool_size <= 8 * half_w, msg);

    /* Independently recompute level[] + last_use[] from the topology. */
    pose_node_t *nodes = NULL;
    uint64_t ncount = 0;
    if (pose_graph_edges(cb, &nodes, &ncount) != 0 || ncount != count) {
        snprintf(msg, sizeof(msg), "cb=%u pose_graph_edges agrees on count", cb);
        EXPECT(0, msg);
        free(slot); free(nodes); return;
    }
    int *level    = (int *)malloc(count * sizeof(int));
    int *last_use = (int *)malloc(count * sizeof(int));
    if (!level || !last_use) { fprintf(stderr, "FAIL cb=%u: level malloc\n", cb);
                               failures++; free(slot); free(nodes);
                               free(level); free(last_use); return; }
    for (uint64_t id = 0; id < count; id++) {
        int np = nodes[id].num_preds, lv;
        if (np == 0)      lv = 0;
        else if (np == 1) lv = level[nodes[id].pred[0]] + 1;
        else { int a = level[nodes[id].pred[0]], b = level[nodes[id].pred[1]];
               lv = (a > b ? a : b) + 1; }
        level[id] = lv; last_use[id] = lv;
    }
    for (uint64_t id = 0; id < count; id++)
        for (int k = 0; k < nodes[id].num_preds; k++) {
            uint64_t p = nodes[id].pred[k];
            if (last_use[p] < level[id]) last_use[p] = level[id];
        }

    /* Every node (output-set included) gets a valid pool index. */
    int range_ok = 1;
    for (uint64_t id = 0; id < count; id++)
        if (slot[id] >= pool_size) range_ok = 0;
    snprintf(msg, sizeof(msg), "cb=%u all slots in [0, pool_size)", cb);
    EXPECT(range_ok, msg);

    /* Liveness: pack every node's (slot, [level,last_use]) and check that within
     * each slot the live ranges are disjoint and ordered.  A node id is live
     * across levels [level[id], last_use[id]]; two nodes sharing a slot must not
     * overlap (the next node may only reuse a slot at a level strictly after the
     * previous occupant's last reader). */
    liverange_t *lr = (liverange_t *)malloc(count * sizeof(liverange_t));
    if (!lr) { fprintf(stderr, "FAIL cb=%u: liverange malloc\n", cb); failures++; }
    else {
        for (uint64_t id = 0; id < count; id++) {
            lr[id].slot = slot[id];
            lr[id].level = (uint32_t)level[id];
            lr[id].last_use = (uint32_t)last_use[id];
        }
        qsort(lr, count, sizeof(liverange_t), cmp_liverange);
        int overlap = 0;
        for (uint64_t i = 1; i < count; i++) {
            if (lr[i].slot == lr[i - 1].slot &&
                lr[i].level <= lr[i - 1].last_use) { overlap = 1; break; }
        }
        snprintf(msg, sizeof(msg),
                 "cb=%u no live-range overlap among all %llu nodes",
                 cb, (unsigned long long)count);
        EXPECT(!overlap, msg);
        free(lr);
    }

    free(slot); free(nodes); free(level); free(last_use);
}

int main(int argc, char **argv)
{
    uint32_t max_cb = 1u << 17;            /* benchmark chunk_blocks */
    if (argc > 1) {
        unsigned long v = strtoul(argv[1], NULL, 0);
        if (v >= 1 && v <= POSE_GRAPH_MAX_M) max_cb = (uint32_t)v;
    }

    for (uint32_t cb = 4096; cb <= max_cb; cb <<= 1)
        check_cb(cb);

    if (failures) {
        fprintf(stderr, "\n%d slot-map check(s) FAILED\n", failures);
        return 1;
    }
    fprintf(stdout, "\nAll slot-map checks passed\n");
    return 0;
}
