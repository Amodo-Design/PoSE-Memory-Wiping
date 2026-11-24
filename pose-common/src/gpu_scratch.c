/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * gpu_scratch.c — what the GPU labeler allocates on the device besides the
 * session buffer (gpu_session.h, pose_gpu_scratch_bytes).  Plain C, built
 * into pose_common with or without CUDA, because the verifier's planner
 * (which never links the CUDA library) sizes GPU sessions with it and must
 * agree with the daemon's own check.
 *
 * The figures mirror label_gpu_common.cu (pose_graph_hbm_topo_build: the
 * resident scaffold topology) and the warp/block strategies' per-super-chunk
 * scratch pool of 2 * chunk_blocks labels; keep them in step.
 */
#include "../include/gpu_session.h"
#include "../include/graph.h"

/* Fixed margin on top of the computed scratch for the library's small
 * allocations (per-pass seeds, keys, descriptor) and cudaMalloc page
 * rounding. */
#define SCRATCH_MARGIN_BYTES (32ull << 20)

uint64_t pose_gpu_scratch_bytes(uint32_t chunk_blocks, uint32_t inflight)
{
    if (chunk_blocks == 0) chunk_blocks = POSE_CHUNK_BLOCKS;
    uint64_t count = pose_graph_scaffold_node_count(chunk_blocks);
    /* Resident topology: per scaffold node np (1) + p0, p1, order (4 each)
     * + slot (4) + rank (4), the level table, and the rank -> node map. */
    uint64_t topo = count * (1 + 4 * 5) + (uint64_t)chunk_blocks * 4 + (64ull << 10);
    /* One recycled pool of 2 * chunk_blocks labels per in-flight super-chunk. */
    uint64_t pools = (uint64_t)inflight * 2 * chunk_blocks * POSE_HASH_BYTES;
    return topo + pools + SCRATCH_MARGIN_BYTES;
}
