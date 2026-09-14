/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * label_gpu_thread.cu — THREAD labeling strategy (one CUDA thread per
 * super-chunk).
 *
 * Each thread labels one whole super-chunk on its own, walking the level-sorted
 * node schedule (order[]) in full topological order and writing each node into
 * its private recycled scratch region of pool_size labels (the host slot map,
 * ctx->d_slot / ctx->pool_size).  No intra-chunk synchronisation is needed — a
 * single thread serialises the whole DAG.  Output-set nodes (rank[id] >= 0) are
 * copied straight to the persisted region at compute time (before slot recycling
 * overwrites the slot).
 *
 * Honest expectation: at a large
 * chunk_blocks the per-thread scratch is large (pool_size*32 = 64*cb bytes,
 * ~8 MiB at cb=2^17), so the number of resident threads is MEMORY-capped far
 * below one warp/SM → poor occupancy and throughput; THREAD only becomes
 * memory-feasible at small cb.  It is provided so the operator can
 * MEASURE it against BLOCK/LEVEL, not because it is expected to win.
 *
 * Output labels are byte-identical to LEVEL / the CPU path (same node ids, same
 * hash inputs; only the scratch byte offset differs).  Validated by
 * disk-wipe-bench --verify.  Selected with POSE_GPU_LABEL_STRATEGY=thread.
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
const char      *LABEL_GPU_STRATEGY_NAME = "thread";

namespace {

/* One thread per super-chunk in the current batch.  thread t labels chunk
 * (batch_base + t) into its private scratch region t, walking order[] in
 * topological order.  Output nodes are copied to persist as computed. */
__global__ void k_label_chunk_thread(uint8_t *persist, uint64_t persist_super_bytes,
                                     uint64_t batch_base, uint32_t bc,
                                     uint8_t *scratch, uint32_t pool_size,
                                     const uint32_t *order, uint64_t count,
                                     const uint8_t *num_preds,
                                     const uint32_t *pred0, const uint32_t *pred1,
                                     const uint32_t *slot, const int32_t *rank,
                                     const uint8_t *seeds, const uint8_t *descriptor,
                                     const uint32_t *key_src, const uint32_t *key_int)
{
    uint64_t t = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (t >= bc) return;

    uint8_t       *sbase = scratch + t * (uint64_t)pool_size * 32;
    const uint8_t *seed  = seeds + t * 32;
    uint8_t       *pbase = persist + (batch_base + t) * persist_super_bytes;

    for (uint64_t i = 0; i < count; i++) {
        uint32_t id = order[i];
        uint8_t  np = num_preds[id];
        const uint8_t *p0 = (np >= 1) ? sbase + (uint64_t)slot[pred0[id]] * 32 : nullptr;
        const uint8_t *p1 = (np >= 2) ? sbase + (uint64_t)slot[pred1[id]] * 32 : nullptr;
        const uint32_t *key = (np == 0) ? key_src : key_int;

        uint8_t *dst = sbase + (uint64_t)slot[id] * 32;
        pose_gpu::label_node(dst, key, id, p0, p1, seed, descriptor);

        int32_t r = rank[id];
        if (r >= 0) pose_gpu::copy_label(pbase + (uint64_t)r * 32, dst);
    }
}

} /* namespace */

int label_gpu_run(const label_gpu_ctx_t *ctx)
{
    const uint32_t chunk_blocks = ctx->chunk_blocks;
    const uint64_t num_super    = ctx->num_super;
    const uint64_t count        = ctx->count;
    const uint32_t pool_size    = ctx->pool_size;

    if (pool_size == 0) {
        fprintf(stderr, "label_gpu_run(thread): pool_size is 0 (slot map missing)\n");
        return -1;
    }

    size_t   region_bytes = (size_t)pool_size * 32;  /* one thread's scratch */
    size_t   free_b = 0, total_b = 0, budget = 0;
    uint64_t pool_threads = 0;                         /* T: concurrent chunks */
    int      rc = -1;

    uint8_t *d_scratch = nullptr, *d_seeds = nullptr, *h_seeds = nullptr;

    #define CK(call) do { if ((call) != cudaSuccess) goto cleanup; } while (0)

    const int block = 256;

    /* T = how many per-thread scratch regions fit, capped at the device's
     * resident-thread capacity and at num_super.  At a secure cb the memory term
     * (region ~8 MiB) is the binding constraint → far below one warp/SM. */
    cudaMemGetInfo(&free_b, &total_b);
    budget = (free_b > (64u << 20)) ? (free_b - (64u << 20)) : 0;
    pool_threads = region_bytes ? budget / region_bytes : 0;
    {
        int dev = 0, numSm = 0, blocksPerSm = 0;
        cudaGetDevice(&dev);
        cudaDeviceGetAttribute(&numSm, cudaDevAttrMultiProcessorCount, dev);
        cudaOccupancyMaxActiveBlocksPerMultiprocessor(
            &blocksPerSm, (void *)k_label_chunk_thread, block, 0);
        uint64_t resident = (uint64_t)numSm * (blocksPerSm > 0 ? blocksPerSm : 1)
                          * (uint64_t)block;
        if (pool_threads > resident) pool_threads = resident;
    }
    if (pool_threads > num_super) pool_threads = num_super;
    if (pool_threads == 0)        pool_threads = 1; /* try one even if tight */

    while (pool_threads >= 1) {
        if (cudaMalloc(&d_scratch, (size_t)pool_threads * region_bytes) == cudaSuccess)
            break;
        d_scratch = nullptr;
        if (pool_threads == 1) {
            fprintf(stderr, "label_gpu_run(thread): scratch region (%zu MiB) alloc failed\n",
                    region_bytes >> 20);
            goto cleanup;
        }
        pool_threads /= 2;
    }

    CK(cudaMalloc(&d_seeds, (size_t)pool_threads * 32));
    h_seeds = (uint8_t *)malloc((size_t)pool_threads * 32);
    if (!h_seeds) goto cleanup;

    {
        const double PROGRESS_INTERVAL_S = 5.0;
        double t_start = pose_gpu_now_sec();
        double t_last  = t_start;
        if (!pose_gpu_quiet()) {
            printf("  GPU label[thread]: %llu super-chunks, pool=%llu threads, "
                   "scratch=%zu MiB (%llu labels/chunk)\n",
                   (unsigned long long)num_super, (unsigned long long)pool_threads,
                   ((size_t)pool_threads * region_bytes) >> 20,
                   (unsigned long long)pool_size);
            fflush(stdout);
        }

        for (uint64_t base = 0; base < num_super; base += pool_threads) {
            uint32_t bc = (uint32_t)((num_super - base < pool_threads)
                                     ? (num_super - base) : pool_threads);

            /* Per-super-chunk seeds (chunk_index matches the CPU path). */
            for (uint32_t t = 0; t < bc; t++) {
                uint64_t j = base + t;
                uint64_t chunk_index =
                    (ctx->region_block_offset + j * (uint64_t)chunk_blocks) / chunk_blocks;
                pose_chunk_seed(h_seeds + (size_t)t * 32, ctx->session_seed,
                                ctx->seed_len, chunk_index);
            }
            CK(cudaMemcpy(d_seeds, h_seeds, (size_t)bc * 32, cudaMemcpyHostToDevice));

            uint32_t grid = (bc + block - 1) / block;
            k_label_chunk_thread<<<grid, block>>>(
                ctx->persist, ctx->super_bytes, base, bc,
                d_scratch, pool_size,
                ctx->d_order, count,
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
                printf("  GPU label[thread]: %llu/%llu super-chunks (%.1f%%)  "
                       "elapsed %.0fs  eta %.0fs\n",
                       (unsigned long long)done, (unsigned long long)num_super,
                       frac * 100.0, elapsed, eta);
                fflush(stdout);
                t_last = tnow;
            }
        }

        /* Zero the transient scratch before freeing so it cannot serve as a label
         * cache during the challenge phase (it is excluded from coverage). */
        CK(cudaMemset(d_scratch, 0, (size_t)pool_threads * region_bytes));
    }
    rc = 0;

cleanup:
    #undef CK
    cudaFree(d_scratch);
    cudaFree(d_seeds);
    free(h_seeds);
    return rc;
}
