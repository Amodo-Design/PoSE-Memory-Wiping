/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * analyze_peak_live.c — empirical peak simultaneously-live label analysis.
 *
 * Design gate for the paper-faithful in-place rewrite (persist only O(G)).
 *
 * The rewritten labeler keeps internal scaffold labels in a bounded transient
 * pool and recycles a slot once every successor that consumes it has been
 * emitted.  This program measures the EXACT peak number of labels that must be
 * live simultaneously, using the real graph topology from pose_graph_edges().
 *
 * Emission order == node-id order (the emitter assigns ids via next_id++).
 * A label is live from the moment its node is emitted until its last successor
 * is emitted.  lastuse[id] = max id among successors of `id` (or id itself when
 * the node has no successors — a true sink, freed immediately).
 *
 *   peak_live = max over t of |{ id : id <= t < lastuse(id) }|
 *
 * We report peak_live and the ratio peak_live / half_w (half_w = 2^n), which is
 * the multiplier the pool must cover.  POSE_ARENA_FACTOR in graph.c must exceed
 * the worst observed ratio.
 *
 * Build (native, against the static lib):
 *   cc -O2 -I common/include common/test/analyze_peak_live.c \
 *      common/build/libpose_common.a -lpthread -o /tmp/analyze_peak_live
 * Run:
 *   /tmp/analyze_peak_live            # sweeps m = 4, 8, ... up to the cap
 *   /tmp/analyze_peak_live 1048576    # single m
 */

#include "graph.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int analyze_one(uint64_t m)
{
    pose_node_t *nodes = NULL;
    uint64_t count = 0;
    if (pose_graph_edges(m, &nodes, &count) != 0) {
        fprintf(stderr, "pose_graph_edges failed for m=%" PRIu64 "\n", m);
        return -1;
    }

    int n = pose_graph_parameter_n(m);
    uint64_t half_w = (uint64_t)1 << n;

    /* lastuse[id] = max successor id (init to id => sink frees immediately). */
    uint64_t *lastuse = (uint64_t *)malloc(count * sizeof(uint64_t));
    /* death[t] = how many labels have their last use at step t. */
    uint64_t *death = (uint64_t *)calloc(count, sizeof(uint64_t));
    if (!lastuse || !death) {
        fprintf(stderr, "alloc failed for m=%" PRIu64 " (count=%" PRIu64 ")\n", m, count);
        free(nodes); free(lastuse); free(death);
        return -1;
    }

    for (uint64_t id = 0; id < count; id++)
        lastuse[id] = id;
    for (uint64_t j = 0; j < count; j++) {
        for (int k = 0; k < nodes[j].num_preds; k++) {
            uint64_t p = nodes[j].pred[k];
            if (j > lastuse[p]) lastuse[p] = j;
        }
    }
    for (uint64_t id = 0; id < count; id++)
        death[lastuse[id]]++;

    /* Sweep emission order: birth t, then retire everything dying at t. */
    uint64_t alive = 0, peak = 0;
    uint64_t peak_at = 0;
    for (uint64_t t = 0; t < count; t++) {
        alive++;                 /* node t born */
        if (alive > peak) { peak = alive; peak_at = t; }
        alive -= death[t];       /* retire labels whose last use is t */
    }

    double ratio = (double)peak / (double)half_w;
    printf("m=%-9" PRIu64 " n=%-2d half_w=%-9" PRIu64
           " total_nodes=%-12" PRIu64 " peak_live=%-12" PRIu64
           " peak/half_w=%6.3f  peak_at_id=%" PRIu64 "\n",
           m, n, half_w, count, peak, ratio, peak_at);

    free(nodes);
    free(lastuse);
    free(death);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        uint64_t m = strtoull(argv[1], NULL, 0);
        return analyze_one(m) == 0 ? 0 : 1;
    }
    /* Sweep powers of two.  Large m needs lots of RAM (node array is
     * total_nodes * sizeof(pose_node_t)); stop where allocation fails. */
    for (int e = 2; e <= 20; e++) {
        if (analyze_one((uint64_t)1 << e) != 0)
            break;
    }
    return 0;
}
