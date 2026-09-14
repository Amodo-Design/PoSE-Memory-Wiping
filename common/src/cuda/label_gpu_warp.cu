/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * label_gpu_warp.cu — WARP labeling strategy (one CUDA warp per super-chunk).
 *
 * Motivation (measured on H100/H200 with the BLOCK strategy, cb = 2^17):
 * Nsight Compute showed HBM at ~5 % of peak, ALUs at ~19 %, and ~56 % of all
 * stall cycles spent at BLOCK's per-level __syncthreads().  The depth-robust
 * spine has many narrow levels where a handful of threads work while the other
 * ~250 idle at the barrier, and a single L2 miss on the critical path holds the
 * whole 256-thread block for a full DRAM round trip.  A clock sweep confirmed
 * the kernel is neither bandwidth- nor compute-bound: ~2/3 of the time scales
 * with SM clock (hash + barrier cycles), ~1/3 is fixed exposed memory latency.
 *
 * WARP is the middle ground between BLOCK (256 threads per chunk, block-wide
 * barrier) and THREAD (1 thread per chunk, no barrier, memory-capped far below
 * one warp/SM).  Each warp owns one super-chunk and walks the levels serially,
 * splitting each level's nodes across its 32 lanes and separating levels with
 * __syncwarp() instead of __syncthreads().  Warps are fully independent, so a
 * narrow spine level or an exposed L2 miss in one chunk idles at most 32 lanes
 * while the other resident warps on the SM keep issuing — the scheduler fills
 * the gap that BLOCK's barrier exposed.  Per-chunk scratch is the same
 * recycled slot pool as BLOCK (pool_size labels = 2*cb), but there are 8x more
 * chunks in flight (one per resident warp), so the scratch footprint is 8x
 * larger (~25 GiB at cb = 2^17 on a 132-SM part); the pool is capped by free
 * HBM and degrades gracefully to fewer concurrent chunks when memory is tight.
 *
 * Security / parity: nothing protocol-visible changes.  The graph (node ids,
 * predecessor edges, dependency levels, chunk size, gamma) is the same host-
 * computed topology every strategy consumes; each label is the same keyed
 * BLAKE3 over the same inputs; predecessors are complete before use because
 * they lie at lower levels and __syncwarp() orders the levels within the
 * owning warp (it guarantees memory ordering among the participating lanes);
 * the slot map guarantees no live label is overwritten; output-set nodes land
 * at the same challenge rank in the persisted region.  Output labels are
 * therefore byte-identical to LEVEL / BLOCK / the CPU path — validated by
 * disk-wipe-bench --verify (read-back + CPU recompute), which is the proof,
 * not this comment.  The only
 * change is the transient scratch footprint, which is zeroed and freed before
 * the challenge phase and excluded from the coverage map exactly as for BLOCK.
 *
 * Selected with POSE_GPU_LABEL_STRATEGY=warp.  POSE_GPU_WARP_MIN_BLOCKS (CMake
 * cache var, default 3) feeds __launch_bounds__: 3 matches BLOCK's register-
 * limited occupancy (no regression); 4 asks ptxas for <= 64 regs/thread to
 * reach 32 warps/SM — check `-Xptxas -v` for zero spill bytes before using it.
 */

#include "label_gpu_strategy.h"
#include "blake3_node.cuh"
#include "../../include/gpu_label.h"
#include "../../include/graph.h"

#include <cuda_runtime.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>

#ifndef POSE_GPU_WARP_MIN_BLOCKS
#define POSE_GPU_WARP_MIN_BLOCKS 3
#endif

/* `extern` forces external linkage on the `const int` (a namespace-scope `const`
 * object would otherwise default to internal linkage and be invisible to the
 * common driver).  The `const char *` is a non-const pointer → already external. */
extern const int LABEL_GPU_USES_SLOT_MAP = 1;
const char      *LABEL_GPU_STRATEGY_NAME = "warp";

namespace {

constexpr int kBlockThreads   = 256;
constexpr int kWarpSize       = 32;
constexpr int kWarpsPerBlock  = kBlockThreads / kWarpSize;

/* One warp per super-chunk in the current batch.  Global warp w labels chunk
 * (batch_base + w) into its private scratch region w, walking levels serially
 * and splitting each level's nodes across its 32 lanes.  Output nodes are
 * copied to the persisted region as they are computed.  The early-out on
 * w >= batch_count is warp-uniform, so every lane that stays reaches every
 * __syncwarp() (full default mask). */
__global__ void __launch_bounds__(kBlockThreads, POSE_GPU_WARP_MIN_BLOCKS)
k_label_chunk_warp(uint8_t *persist, uint64_t persist_super_bytes,
                   uint64_t batch_base, uint32_t batch_count,
                   uint8_t *scratch, uint32_t pool_size,
                   const uint32_t *order,
                   const uint32_t *level_start, int num_levels,
                   const uint8_t *num_preds,
                   const uint32_t *pred0, const uint32_t *pred1,
                   const uint32_t *slot, const int32_t *rank,
                   const uint8_t *seeds, const uint8_t *descriptor,
                   const uint32_t *key_src, const uint32_t *key_int)
{
    uint32_t w    = (blockIdx.x * (uint32_t)blockDim.x + threadIdx.x) / kWarpSize;
    uint32_t lane = threadIdx.x % kWarpSize;
    if (w >= batch_count) return;

    uint8_t       *sbase = scratch + (uint64_t)w * pool_size * 32;
    const uint8_t *seed  = seeds + (uint64_t)w * 32;
    uint8_t       *pbase = persist + (batch_base + w) * persist_super_bytes;

    for (int lv = 0; lv < num_levels; lv++) {
        uint32_t lstart = level_start[lv];
        uint32_t n      = level_start[lv + 1] - lstart;
        for (uint32_t i = lane; i < n; i += kWarpSize) {
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
        /* Level boundary: reconverge the (possibly diverged) lane loop and
         * order this level's scratch stores before the next level's loads. */
        __syncwarp();
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
        fprintf(stderr, "label_gpu_run(warp): pool_size is 0 (slot map missing)\n");
        return -1;
    }

    size_t   region_bytes = (size_t)pool_size * 32;  /* one warp's scratch */
    size_t   free_b = 0, total_b = 0, budget = 0;
    uint64_t pool_warps = 0;                           /* P: concurrent chunks */
    int      rc = -1;

    uint8_t *d_scratch = nullptr, *d_seeds = nullptr, *h_seeds = nullptr;

    #define CK(call) do { if ((call) != cudaSuccess) goto cleanup; } while (0)

    /* P = how many warp-private scratch regions fit, capped at the device's
     * resident-warp capacity (more regions than can run concurrently buys no
     * extra parallelism, only HBM) and at num_super. */
    cudaMemGetInfo(&free_b, &total_b);
    budget = (free_b > (64u << 20)) ? (free_b - (64u << 20)) : 0;
    pool_warps = region_bytes ? budget / region_bytes : 0;
    {
        int dev = 0, numSm = 0, blocksPerSm = 0;
        cudaGetDevice(&dev);
        cudaDeviceGetAttribute(&numSm, cudaDevAttrMultiProcessorCount, dev);
        cudaOccupancyMaxActiveBlocksPerMultiprocessor(
            &blocksPerSm, (void *)k_label_chunk_warp, kBlockThreads, 0);
        uint64_t resident = (uint64_t)numSm * (blocksPerSm > 0 ? blocksPerSm : 1)
                            * kWarpsPerBlock;
        if (pool_warps > resident) pool_warps = resident;
    }
    if (pool_warps > num_super) pool_warps = num_super;
    if (pool_warps == 0)        pool_warps = 1; /* try one even if tight */

    while (pool_warps >= 1) {
        if (cudaMalloc(&d_scratch, (size_t)pool_warps * region_bytes) == cudaSuccess)
            break;
        d_scratch = nullptr;
        if (pool_warps == 1) {
            fprintf(stderr, "label_gpu_run(warp): scratch region (%zu MiB) alloc failed\n",
                    region_bytes >> 20);
            goto cleanup;
        }
        pool_warps /= 2;
    }

    CK(cudaMalloc(&d_seeds, (size_t)pool_warps * 32));
    h_seeds = (uint8_t *)malloc((size_t)pool_warps * 32);
    if (!h_seeds) goto cleanup;

    {
        const double PROGRESS_INTERVAL_S = 5.0;
        double t_start = pose_gpu_now_sec();
        double t_last  = t_start;
        if (!pose_gpu_quiet()) {
            printf("  GPU label[warp]: %llu super-chunks, pool=%llu warps, "
                   "scratch=%zu MiB (%llu labels/chunk), min_blocks/SM=%d\n",
                   (unsigned long long)num_super, (unsigned long long)pool_warps,
                   ((size_t)pool_warps * region_bytes) >> 20,
                   (unsigned long long)pool_size, (int)POSE_GPU_WARP_MIN_BLOCKS);
            fflush(stdout);
        }

        for (uint64_t base = 0; base < num_super; base += pool_warps) {
            uint32_t bc = (uint32_t)((num_super - base < pool_warps)
                                     ? (num_super - base) : pool_warps);

            /* Per-super-chunk seeds (chunk_index matches the CPU path). */
            for (uint32_t w = 0; w < bc; w++) {
                uint64_t j = base + w;
                uint64_t chunk_index =
                    (ctx->region_block_offset + j * (uint64_t)chunk_blocks) / chunk_blocks;
                pose_chunk_seed(h_seeds + (size_t)w * 32, ctx->session_seed,
                                ctx->seed_len, chunk_index);
            }
            CK(cudaMemcpy(d_seeds, h_seeds, (size_t)bc * 32, cudaMemcpyHostToDevice));

            uint32_t grid = (bc + kWarpsPerBlock - 1) / kWarpsPerBlock;
            k_label_chunk_warp<<<grid, kBlockThreads>>>(
                ctx->persist, ctx->super_bytes, base, bc,
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
                printf("  GPU label[warp]: %llu/%llu super-chunks (%.1f%%)  "
                       "elapsed %.0fs  eta %.0fs\n",
                       (unsigned long long)done, (unsigned long long)num_super,
                       frac * 100.0, elapsed, eta);
                fflush(stdout);
                t_last = tnow;
            }
        }

        /* Zero the transient scratch before freeing so it cannot serve as a label
         * cache during the challenge phase (it is excluded from coverage). */
        CK(cudaMemset(d_scratch, 0, (size_t)pool_warps * region_bytes));
    }
    rc = 0;

cleanup:
    #undef CK
    cudaFree(d_scratch);
    cudaFree(d_seeds);
    free(h_seeds);
    return rc;
}
