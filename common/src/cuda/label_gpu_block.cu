/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * label_gpu_block.cu — BLOCK labeling strategy (one CUDA thread block per
 * super-chunk).
 *
 * Each block labels one whole super-chunk into a block-private recycled scratch
 * region of pool_size labels (the host slot map, ctx->d_slot / ctx->pool_size),
 * instead of materializing the full scaffold like LEVEL.  Blocks are
 * embarrassingly parallel across super-chunks, so while one block is stuck on
 * the depth-robust spine (the narrow levels that starve LEVEL's wave), the other
 * blocks keep every SM busy on different chunks.  Per-chunk scratch collapses
 * from the full scaffold (count labels) to pool_size labels (= 4*half_w = 2*cb,
 * ~68-138x smaller), so the unattested work region shrinks proportionally.
 *
 * Within a block, levels are walked serially (level_start[]/order[]); the level's
 * nodes are split across threads and labeled in parallel into distinct recycled
 * slots (the slot map guarantees no two simultaneously-live nodes share a slot,
 * and preds are always at lower levels → still live), with a __syncthreads()
 * between levels.  Output-set nodes (rank[id] >= 0) are ALSO copied straight to
 * the persisted region at compute time (before slot recycling can overwrite the
 * slot), so no separate extract kernel is needed.
 *
 * Output labels are byte-identical to LEVEL / the CPU path: same node ids, same
 * hash inputs (pred labels, seed, descriptor, keys), only the scratch byte offset
 * each transient label lives at differs.  Validated by disk-wipe-bench --verify.
 * Selected with POSE_GPU_LABEL_STRATEGY=block.
 */

#include "label_gpu_strategy.h"
#include "blake3_node.cuh"
#include "../../include/gpu_label.h"
#include "../../include/graph.h"

#include <cuda_runtime.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>

/* `extern` forces external linkage on the `const int` (a namespace-scope `const`
 * object would otherwise default to internal linkage and be invisible to the
 * common driver).  The `const char *` is a non-const pointer → already external. */
extern const int LABEL_GPU_USES_SLOT_MAP = 1;
const char      *LABEL_GPU_STRATEGY_NAME = "block";

namespace {

/* One block per super-chunk in the current batch.  block b labels chunk
 * (batch_base + b) into its private scratch region b, walking levels serially and
 * splitting each level's nodes across threads.  Output nodes are copied to the
 * persisted region as they are computed. */
__global__ void k_label_chunk_block(uint8_t *persist, uint64_t persist_super_bytes,
                                    uint64_t batch_base,
                                    uint8_t *scratch, uint32_t pool_size,
                                    const uint32_t *order,
                                    const uint32_t *level_start, int num_levels,
                                    const uint8_t *num_preds,
                                    const uint32_t *pred0, const uint32_t *pred1,
                                    const uint32_t *slot, const int32_t *rank,
                                    const uint8_t *seeds, const uint8_t *descriptor,
                                    const uint32_t *key_src, const uint32_t *key_int)
{
    uint32_t b = blockIdx.x;
    uint8_t       *sbase = scratch + (uint64_t)b * pool_size * 32;
    const uint8_t *seed  = seeds + (uint64_t)b * 32;
    uint8_t       *pbase = persist + (batch_base + b) * persist_super_bytes;

    for (int lv = 0; lv < num_levels; lv++) {
        uint32_t lstart = level_start[lv];
        uint32_t n      = level_start[lv + 1] - lstart;
        for (uint32_t i = threadIdx.x; i < n; i += blockDim.x) {
            uint32_t id = order[lstart + i];
            uint8_t  np = num_preds[id];
            const uint8_t *p0 = (np >= 1) ? sbase + (uint64_t)slot[pred0[id]] * 32 : nullptr;
            const uint8_t *p1 = (np >= 2) ? sbase + (uint64_t)slot[pred1[id]] * 32 : nullptr;
            const uint32_t *key = (np == 0) ? key_src : key_int;

            uint8_t *dst = sbase + (uint64_t)slot[id] * 32;
            pose_gpu::label_node(dst, key, id, p0, p1, seed, descriptor);

            int32_t r = rank[id];
            if (r >= 0) pose_gpu::copy_label(pbase + (uint64_t)r * 32, dst);
        }
        __syncthreads();
    }
}

} /* namespace */

int label_gpu_run(const label_gpu_ctx_t *ctx)
{
    const uint32_t chunk_blocks = ctx->chunk_blocks;
    const uint64_t num_super    = ctx->num_super;
    const int      L            = ctx->num_levels;
    const uint32_t pool_size    = ctx->pool_size;

    if (pool_size == 0) {
        fprintf(stderr, "label_gpu_run(block): pool_size is 0 (slot map missing)\n");
        return -1;
    }

    size_t   region_bytes = (size_t)pool_size * 32;  /* one block's scratch */
    size_t   free_b = 0, total_b = 0, budget = 0;
    uint64_t pool_blocks = 0;                          /* P: concurrent chunks */
    int      rc = -1;

    uint8_t *d_scratch = nullptr, *d_seeds = nullptr, *h_seeds = nullptr;

    #define CK(call) do { if ((call) != cudaSuccess) goto cleanup; } while (0)

    const int block = 256;

    /* P = how many block-private scratch regions fit, capped at the device's
     * resident-block capacity (more regions than can run concurrently buys no
     * extra parallelism, only HBM) and at num_super. */
    cudaMemGetInfo(&free_b, &total_b);
    budget = (free_b > (64u << 20)) ? (free_b - (64u << 20)) : 0;
    pool_blocks = region_bytes ? budget / region_bytes : 0;
    {
        int dev = 0, numSm = 0, blocksPerSm = 0;
        cudaGetDevice(&dev);
        cudaDeviceGetAttribute(&numSm, cudaDevAttrMultiProcessorCount, dev);
        cudaOccupancyMaxActiveBlocksPerMultiprocessor(
            &blocksPerSm, (void *)k_label_chunk_block, block, 0);
        uint64_t resident = (uint64_t)numSm * (blocksPerSm > 0 ? blocksPerSm : 1);
        if (pool_blocks > resident) pool_blocks = resident;
    }
    if (pool_blocks > num_super) pool_blocks = num_super;
    if (pool_blocks == 0)        pool_blocks = 1; /* try one even if tight */

    while (pool_blocks >= 1) {
        if (cudaMalloc(&d_scratch, (size_t)pool_blocks * region_bytes) == cudaSuccess)
            break;
        d_scratch = nullptr;
        if (pool_blocks == 1) {
            fprintf(stderr, "label_gpu_run(block): scratch region (%zu MiB) alloc failed\n",
                    region_bytes >> 20);
            goto cleanup;
        }
        pool_blocks /= 2;
    }

    CK(cudaMalloc(&d_seeds, (size_t)pool_blocks * 32));
    h_seeds = (uint8_t *)malloc((size_t)pool_blocks * 32);
    if (!h_seeds) goto cleanup;

    {
        const double PROGRESS_INTERVAL_S = 5.0;
        double t_start = pose_gpu_now_sec();
        double t_last  = t_start;
        if (!pose_gpu_quiet()) {
            printf("  GPU label[block]: %llu super-chunks, pool=%llu blocks, "
                   "scratch=%zu MiB (%llu labels/chunk)\n",
                   (unsigned long long)num_super, (unsigned long long)pool_blocks,
                   ((size_t)pool_blocks * region_bytes) >> 20,
                   (unsigned long long)pool_size);
            fflush(stdout);
        }

        for (uint64_t base = 0; base < num_super; base += pool_blocks) {
            uint32_t bc = (uint32_t)((num_super - base < pool_blocks)
                                     ? (num_super - base) : pool_blocks);

            /* Per-super-chunk seeds (chunk_index matches the CPU path). */
            for (uint32_t b = 0; b < bc; b++) {
                uint64_t j = base + b;
                uint64_t chunk_index =
                    (ctx->region_block_offset + j * (uint64_t)chunk_blocks) / chunk_blocks;
                pose_chunk_seed(h_seeds + (size_t)b * 32, ctx->session_seed,
                                ctx->seed_len, chunk_index);
            }
            CK(cudaMemcpy(d_seeds, h_seeds, (size_t)bc * 32, cudaMemcpyHostToDevice));

            k_label_chunk_block<<<bc, block>>>(
                ctx->persist, ctx->super_bytes, base,
                d_scratch, pool_size,
                ctx->d_order, ctx->d_level_start, L,
                ctx->d_np, ctx->d_p0, ctx->d_p1,
                ctx->d_slot, ctx->d_rank,
                d_seeds, ctx->d_desc, ctx->d_ksrc, ctx->d_kint);
            CK(cudaGetLastError());
            CK(cudaDeviceSynchronize());

            uint64_t done = base + bc;
            double   tnow = pose_gpu_now_sec();
            if (!pose_gpu_quiet() &&
                (base == 0 || done >= num_super ||
                 tnow - t_last >= PROGRESS_INTERVAL_S)) {
                double elapsed = tnow - t_start;
                double frac    = (double)done / (double)num_super;
                double eta     = (frac > 0.0) ? elapsed * (1.0 - frac) / frac : 0.0;
                printf("  GPU label[block]: %llu/%llu super-chunks (%.1f%%)  "
                       "elapsed %.0fs  eta %.0fs\n",
                       (unsigned long long)done, (unsigned long long)num_super,
                       frac * 100.0, elapsed, eta);
                fflush(stdout);
                t_last = tnow;
            }
        }

        /* Zero the transient scratch before freeing so it cannot serve as a label
         * cache during the challenge phase (it is excluded from coverage). */
        CK(cudaMemset(d_scratch, 0, (size_t)pool_blocks * region_bytes));
    }
    rc = 0;

cleanup:
    #undef CK
    cudaFree(d_scratch);
    cudaFree(d_seeds);
    free(h_seeds);
    return rc;
}
