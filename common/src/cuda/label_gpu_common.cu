/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * label_gpu_common.cu — shared host driver for GPU HBM graph labeling.
 *
 * Parity strategy: the GPU does NOT
 * reimplement the DAG.  The graph topology is seed-independent, so the host
 * computes it once with common/graph.c (pose_graph_edges) plus a per-node
 * dependency level, uploads it, and the device kernel just runs keyed BLAKE3
 * (blake3_node.cuh) per node.  Because predecessors always have lower node IDs,
 * "level = 1 + max(level(pred))" is a valid parallel schedule.
 *
 * This file owns everything shared across the four labeling strategies (LEVEL /
 * BLOCK / THREAD / WARP): topology + level computation, the level-sorted schedule, the
 * output-rank map, descriptor/keys, optional slot-recycling map, the device
 * upload of all of the above, and progress timing.  The actual labeling is done
 * by the single compiled-in strategy's label_gpu_run() (see
 * label_gpu_strategy.h and the POSE_GPU_LABEL_STRATEGY build flag).
 */

#include "label_gpu_strategy.h"
#include "../../include/gpu_label.h"
#include "../../include/graph.h"

#include <cuda_runtime.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>

double pose_gpu_now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Progress/log suppression: when POSE_GPU_QUIET is set (non-empty, not "0") the
 * labeler's own stdout progress lines are silenced so a caller can render its own
 * live display without interleaving.  Off by default, so other callers of this
 * library print exactly as before.  Cached on first use. */
int pose_gpu_quiet(void)
{
    static int q = -1;
    if (q < 0) {
        const char *e = getenv("POSE_GPU_QUIET");
        q = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return q;
}

/*
 * Resident scaffold topology (seed-independent; depends only on chunk_blocks).
 * Built + uploaded once, reused across many label calls so the dominant fixed
 * cost — host-side graph construction and the H2D upload — is not repeated per
 * HBM window.  Bound to the CUDA device current at build time.
 */
struct pose_hbm_topo {
    uint32_t  chunk_blocks;
    size_t    super_bytes;
    uint64_t  count;
    int       num_levels;
    uint32_t *level_count;    /* host [L]     */
    uint32_t *level_start;    /* host [L + 1] */
    uint32_t  pool_size;
    uint8_t  *d_np, *d_desc;
    uint32_t *d_p0, *d_p1, *d_order, *d_level_start, *d_out_id, *d_ksrc, *d_kint, *d_slot;
    int32_t  *d_rank;
};

extern "C" pose_hbm_topo_t *pose_graph_hbm_topo_build(uint32_t chunk_blocks,
                                                      pose_hash_algo_t algo)
{
    if (algo != POSE_HASH_BLAKE3) {
        fprintf(stderr, "pose_graph_hbm_topo_build: only BLAKE3 supported on GPU\n");
        return nullptr;
    }
    if (chunk_blocks == 0) chunk_blocks = POSE_CHUNK_BLOCKS;

    uint64_t super_blocks = pose_graph_super_chunk_blocks(chunk_blocks);  /* == cb */
    size_t   super_bytes  = (size_t)super_blocks * 32;
    if (super_blocks == 0 || super_blocks > 0xFFFFFFFFull) {
        fprintf(stderr, "pose_graph_hbm_topo_build: super_blocks out of range\n");
        return nullptr;
    }

    /* ── host-side topology + dependency levels (full scaffold) ── */
    pose_node_t *nodes = nullptr;
    uint64_t count = 0;
    if (pose_graph_edges(chunk_blocks, &nodes, &count) != 0)
        return nullptr;
    if (count == 0 || count > 0xFFFFFFFFull) {
        fprintf(stderr, "pose_graph_hbm_topo_build: scaffold count %llu out of range\n",
                (unsigned long long)count);
        free(nodes);
        return nullptr;
    }

    int      *level    = (int *)malloc(count * sizeof(int));
    uint8_t  *h_np     = (uint8_t *)malloc(count);
    uint32_t *h_p0     = (uint32_t *)malloc(count * sizeof(uint32_t));
    uint32_t *h_p1     = (uint32_t *)malloc(count * sizeof(uint32_t));
    uint32_t *h_order  = (uint32_t *)malloc(count * sizeof(uint32_t));
    uint32_t *h_out_id = (uint32_t *)malloc((size_t)chunk_blocks * sizeof(uint32_t));
    /* id -> challenge rank (-1 internal), only needed by per-chunk strategies. */
    int32_t  *h_rank   = LABEL_GPU_USES_SLOT_MAP
                       ? (int32_t *)malloc(count * sizeof(int32_t)) : nullptr;
    uint32_t *level_count = nullptr, *level_start = nullptr, *cursor = nullptr;
    uint32_t *h_slot   = nullptr;
    uint8_t   h_desc[32];
    uint32_t  h_ksrc[8], h_kint[8];
    uint32_t  pool_size = 0;
    pose_hbm_topo_t *t = nullptr;

    uint8_t  *d_np = nullptr, *d_desc = nullptr;
    uint32_t *d_p0 = nullptr, *d_p1 = nullptr, *d_order = nullptr;
    uint32_t *d_ksrc = nullptr, *d_kint = nullptr, *d_out_id = nullptr;
    uint32_t *d_level_start = nullptr, *d_slot = nullptr;
    int32_t  *d_rank = nullptr;

    if (!level || !h_np || !h_p0 || !h_p1 || !h_order || !h_out_id ||
        (LABEL_GPU_USES_SLOT_MAP && !h_rank))
        goto fail;

    {
        int maxlevel = 0;
        for (uint64_t id = 0; id < count; id++) {
            int np = nodes[id].num_preds;
            int lv;
            if (np == 0) {
                lv = 0;
            } else if (np == 1) {
                lv = level[nodes[id].pred[0]] + 1;
            } else {
                int a = level[nodes[id].pred[0]];
                int b = level[nodes[id].pred[1]];
                lv = (a > b ? a : b) + 1;
            }
            level[id] = lv;
            if (lv > maxlevel) maxlevel = lv;
            h_np[id] = (uint8_t)np;
            h_p0[id] = (uint32_t)nodes[id].pred[0];
            h_p1[id] = (uint32_t)nodes[id].pred[1];
            if (nodes[id].challenge_rank >= 0)
                h_out_id[nodes[id].challenge_rank] = (uint32_t)id;
            if (h_rank) h_rank[id] = nodes[id].challenge_rank;
        }
        free(nodes); nodes = nullptr;

        int L = maxlevel + 1;
        level_count = (uint32_t *)calloc((size_t)L, sizeof(uint32_t));
        level_start = (uint32_t *)malloc((size_t)(L + 1) * sizeof(uint32_t));
        cursor      = (uint32_t *)malloc((size_t)L * sizeof(uint32_t));
        if (!level_count || !level_start || !cursor) goto fail;
        for (uint64_t id = 0; id < count; id++) level_count[level[id]]++;
        level_start[0] = 0;
        for (int i = 0; i < L; i++) { level_start[i + 1] = level_start[i] + level_count[i];
                                      cursor[i] = level_start[i]; }
        for (uint64_t id = 0; id < count; id++) h_order[cursor[level[id]]++] = (uint32_t)id;
        free(level);  level  = nullptr;
        free(cursor); cursor = nullptr;

        /* descriptor + keys (depend only on chunk_blocks) */
        pose_graph_descriptor(h_desc, chunk_blocks,
                              (uint64_t)pose_graph_parameter_n(chunk_blocks));
        pose_label_node_keys(h_ksrc, h_kint);

        /* Slot-recycling map (per-chunk strategies only). */
        if (LABEL_GPU_USES_SLOT_MAP) {
            h_slot = (uint32_t *)malloc(count * sizeof(uint32_t));
            if (!h_slot) goto fail;
            if (pose_graph_slot_map(chunk_blocks, h_slot, &pool_size) != 0) {
                fprintf(stderr, "pose_graph_hbm_topo_build: pose_graph_slot_map failed\n");
                goto fail;
            }
        }

        /* ── upload the shared device topology ── */
        #define CK(call) do { if ((call) != cudaSuccess) goto fail; } while (0)
        CK(cudaMalloc(&d_np, count));
        CK(cudaMalloc(&d_p0, count * sizeof(uint32_t)));
        CK(cudaMalloc(&d_p1, count * sizeof(uint32_t)));
        CK(cudaMalloc(&d_order, count * sizeof(uint32_t)));
        CK(cudaMalloc(&d_level_start, (size_t)(L + 1) * sizeof(uint32_t)));
        CK(cudaMalloc(&d_out_id, (size_t)chunk_blocks * sizeof(uint32_t)));
        CK(cudaMalloc(&d_desc, 32));
        CK(cudaMalloc(&d_ksrc, 32));
        CK(cudaMalloc(&d_kint, 32));
        if (h_slot) CK(cudaMalloc(&d_slot, count * sizeof(uint32_t)));
        if (h_rank) CK(cudaMalloc(&d_rank, count * sizeof(int32_t)));

        CK(cudaMemcpy(d_np, h_np, count, cudaMemcpyHostToDevice));
        CK(cudaMemcpy(d_p0, h_p0, count * sizeof(uint32_t), cudaMemcpyHostToDevice));
        CK(cudaMemcpy(d_p1, h_p1, count * sizeof(uint32_t), cudaMemcpyHostToDevice));
        CK(cudaMemcpy(d_order, h_order, count * sizeof(uint32_t), cudaMemcpyHostToDevice));
        CK(cudaMemcpy(d_level_start, level_start, (size_t)(L + 1) * sizeof(uint32_t),
                      cudaMemcpyHostToDevice));
        CK(cudaMemcpy(d_out_id, h_out_id, (size_t)chunk_blocks * sizeof(uint32_t),
                      cudaMemcpyHostToDevice));
        CK(cudaMemcpy(d_desc, h_desc, 32, cudaMemcpyHostToDevice));
        CK(cudaMemcpy(d_ksrc, h_ksrc, 32, cudaMemcpyHostToDevice));
        CK(cudaMemcpy(d_kint, h_kint, 32, cudaMemcpyHostToDevice));
        if (h_slot)
            CK(cudaMemcpy(d_slot, h_slot, count * sizeof(uint32_t), cudaMemcpyHostToDevice));
        if (h_rank)
            CK(cudaMemcpy(d_rank, h_rank, count * sizeof(int32_t), cudaMemcpyHostToDevice));
        #undef CK

        t = (pose_hbm_topo_t *)calloc(1, sizeof(*t));
        if (!t) goto fail;
        t->chunk_blocks  = chunk_blocks;
        t->super_bytes   = super_bytes;
        t->count         = count;
        t->num_levels    = L;
        t->level_count   = level_count;
        t->level_start   = level_start;
        t->pool_size     = pool_size;
        t->d_np          = d_np;
        t->d_p0          = d_p0;
        t->d_p1          = d_p1;
        t->d_order       = d_order;
        t->d_level_start = d_level_start;
        t->d_out_id      = d_out_id;
        t->d_desc        = d_desc;
        t->d_ksrc        = d_ksrc;
        t->d_kint        = d_kint;
        t->d_slot        = d_slot;
        t->d_rank        = d_rank;

        /* transient host arrays are only needed for the upload above */
        free(h_np); free(h_p0); free(h_p1); free(h_order); free(h_out_id);
        free(h_slot); free(h_rank);
        return t;
    }

fail:
    {
        cudaError_t e = cudaGetLastError();
        if (e != cudaSuccess)
            fprintf(stderr, "pose_graph_hbm_topo_build: CUDA error: %s\n",
                    cudaGetErrorString(e));
    }
    free(nodes); free(level); free(cursor);
    free(h_np); free(h_p0); free(h_p1); free(h_order); free(h_out_id);
    free(h_slot); free(h_rank);
    free(level_count); free(level_start);
    cudaFree(d_np); cudaFree(d_p0); cudaFree(d_p1); cudaFree(d_order);
    cudaFree(d_level_start); cudaFree(d_out_id);
    cudaFree(d_desc); cudaFree(d_ksrc); cudaFree(d_kint); cudaFree(d_slot);
    cudaFree(d_rank);
    free(t);
    return nullptr;
}

extern "C" int pose_graph_hbm_topo_label(const pose_hbm_topo_t *t,
                                         const uint8_t *session_seed, size_t seed_len,
                                         void *hbm_dev_ptr, size_t hbm_len,
                                         uint64_t region_block_offset)
{
    if (!t) return -1;
    uint64_t num_super = hbm_len / t->super_bytes;
    if (num_super == 0)
        return 0; /* buffer smaller than one super-chunk — nothing to label */

    label_gpu_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.session_seed        = session_seed;
    ctx.seed_len            = seed_len;
    ctx.chunk_blocks        = t->chunk_blocks;
    ctx.region_block_offset = region_block_offset;
    ctx.persist             = (uint8_t *)hbm_dev_ptr;
    ctx.super_bytes         = t->super_bytes;
    ctx.num_super           = num_super;
    ctx.count               = t->count;
    ctx.num_levels          = t->num_levels;
    ctx.level_count         = t->level_count;
    ctx.level_start         = t->level_start;
    ctx.d_np                = t->d_np;
    ctx.d_p0                = t->d_p0;
    ctx.d_p1                = t->d_p1;
    ctx.d_order             = t->d_order;
    ctx.d_level_start       = t->d_level_start;
    ctx.d_out_id            = t->d_out_id;
    ctx.d_desc              = t->d_desc;
    ctx.d_ksrc              = t->d_ksrc;
    ctx.d_kint              = t->d_kint;
    ctx.d_slot              = t->d_slot;
    ctx.d_rank              = t->d_rank;
    ctx.pool_size           = t->pool_size;

    if (!pose_gpu_quiet()) {
        printf("  GPU label: strategy=%s  %llu super-chunks  scaffold=%llu nodes\n",
               LABEL_GPU_STRATEGY_NAME, (unsigned long long)num_super,
               (unsigned long long)t->count);
        fflush(stdout);
    }

    double t0 = pose_gpu_now_sec();
    int rc = label_gpu_run(&ctx);
    /* label_gpu_run launches are async; sync so the elapsed time covers the
     * actual device work, not just the launch. */
    cudaDeviceSynchronize();
    double elapsed = pose_gpu_now_sec() - t0;

    if (rc != 0) {
        cudaError_t e = cudaGetLastError();
        if (e != cudaSuccess)
            fprintf(stderr, "pose_graph_hbm_topo_label: CUDA error: %s\n",
                    cudaGetErrorString(e));
    } else if (elapsed > 0.0 && !pose_gpu_quiet()) {
        /* Honest end-to-end GPU wipe rate over the persisted output set. */
        double persisted_mib = (double)(num_super * t->super_bytes) / (double)(1u << 20);
        printf("  GPU wipe: %.0f MiB in %.3f s = %.1f MiB/s\n",
               persisted_mib, elapsed, persisted_mib / elapsed);
        fflush(stdout);
    }
    return rc;
}

extern "C" void pose_graph_hbm_topo_free(pose_hbm_topo_t *t)
{
    if (!t) return;
    cudaFree(t->d_np); cudaFree(t->d_p0); cudaFree(t->d_p1); cudaFree(t->d_order);
    cudaFree(t->d_level_start); cudaFree(t->d_out_id);
    cudaFree(t->d_desc); cudaFree(t->d_ksrc); cudaFree(t->d_kint); cudaFree(t->d_slot);
    cudaFree(t->d_rank);
    free(t->level_count);
    free(t->level_start);
    free(t);
}

/*
 * Original one-shot entry point, now a thin wrapper: build the topology, label
 * the single buffer, free the topology.  Behavior is identical to before for
 * callers that label one buffer at a time.
 */
extern "C" int pose_graph_label_hbm(const uint8_t *session_seed, size_t seed_len,
                                    uint32_t chunk_blocks,
                                    void *hbm_dev_ptr, size_t hbm_len,
                                    uint64_t region_block_offset,
                                    pose_hash_algo_t algo)
{
    if (algo != POSE_HASH_BLAKE3) {
        fprintf(stderr, "pose_graph_label_hbm: only BLAKE3 supported on GPU\n");
        return -1;
    }
    if (chunk_blocks == 0) chunk_blocks = POSE_CHUNK_BLOCKS;

    uint64_t super_blocks = pose_graph_super_chunk_blocks(chunk_blocks);
    size_t   super_bytes  = (size_t)super_blocks * 32;
    if (super_blocks == 0 || super_blocks > 0xFFFFFFFFull) {
        fprintf(stderr, "pose_graph_label_hbm: super_blocks out of range\n");
        return -1;
    }
    if (hbm_len / super_bytes == 0)
        return 0; /* buffer smaller than one super-chunk — nothing to label */

    pose_hbm_topo_t *t = pose_graph_hbm_topo_build(chunk_blocks, algo);
    if (!t) return -1;
    int rc = pose_graph_hbm_topo_label(t, session_seed, seed_len,
                                       hbm_dev_ptr, hbm_len, region_block_offset);
    pose_graph_hbm_topo_free(t);
    return rc;
}
