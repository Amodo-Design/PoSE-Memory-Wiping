/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * label_gpu_level.cu — LEVEL labeling strategy (default).
 *
 * Materializes the FULL transient scaffold for a wave of super-chunks in an HBM
 * work buffer and labels all nodes of a dependency level in parallel across that
 * wave (one kernel launch per level, or one cooperative launch walking every
 * level with grid.sync()).  After a wave is labeled, k_extract_outputs copies
 * each scaffold's output set into the persisted region in challenge-rank order.
 *
 * This is the byte-parity reference the BLOCK / THREAD / WARP strategies are
 * validated against (disk-wipe-bench --verify checks every strategy against the
 * CPU path).  Selected when POSE_GPU_LABEL_STRATEGY is unset
 * or "level".  Trade-off: the wave (the only cross-super-chunk parallelism on the
 * serial depth-robust spine) leaves the GPU's cores idle on the graph's narrow
 * spine levels, and the wave×scaffold work buffer is large/unattested — the
 * per-chunk strategies attack both.
 */

#include "label_gpu_strategy.h"
#include "blake3_node.cuh"
#include "../../include/gpu_label.h"
#include "../../include/graph.h"

#include <cuda_runtime.h>
#include <cooperative_groups.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace cg = cooperative_groups;

/* `extern` forces external linkage on the `const int` (a namespace-scope `const`
 * object would otherwise default to internal linkage and be invisible to the
 * common driver).  The `const char *` is a non-const pointer → already external. */
extern const int LABEL_GPU_USES_SLOT_MAP = 0;
const char      *LABEL_GPU_STRATEGY_NAME = "level";

namespace {

/* One kernel launch per dependency level, covering every super-chunk.
 * thread t → super-chunk j = t / n_in_level, level-local node = t % n_in_level. */
__global__ void k_label_level(uint8_t *hbm, uint64_t super_bytes,
                              uint32_t num_super,
                              const uint32_t *level_order, uint32_t n_in_level,
                              const uint8_t *num_preds,
                              const uint32_t *pred0, const uint32_t *pred1,
                              const uint8_t *seeds, const uint8_t *descriptor,
                              const uint32_t *key_src, const uint32_t *key_int)
{
    uint64_t tid   = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x;
    uint64_t total = (uint64_t)n_in_level * num_super;
    if (tid >= total) return;

    uint32_t local = (uint32_t)(tid % n_in_level);
    uint32_t j     = (uint32_t)(tid / n_in_level);
    uint32_t id    = level_order[local];

    uint8_t       *base = hbm + (uint64_t)j * super_bytes;
    const uint8_t *seed = seeds + (uint64_t)j * 32;

    uint8_t np = num_preds[id];
    const uint8_t *p0 = (np >= 1) ? base + (uint64_t)pred0[id] * 32 : nullptr;
    const uint8_t *p1 = (np >= 2) ? base + (uint64_t)pred1[id] * 32 : nullptr;
    const uint32_t *key = (np == 0) ? key_src : key_int;

    pose_gpu::label_node(base + (uint64_t)id * 32, key, id, p0, p1, seed, descriptor);
}

/* Single cooperative launch that walks every dependency level in one kernel,
 * synchronising the whole grid between levels with grid.sync() instead of
 * relaunching per level.  Eliminates the L× kernel-launch overhead that
 * dominates the deep (depth-robust) spine, where each level holds only a
 * handful of nodes.  Memory ordering is preserved: grid.sync() is a grid-wide
 * barrier with acquire/release semantics, so writes from level L are visible to
 * level L+1.  Output is byte-identical (same node order, same hash math). */
__global__ void k_label_all_levels(uint8_t *work, uint64_t work_super_bytes,
                                   uint32_t w_count,
                                   const uint32_t *order,
                                   const uint32_t *level_start, int num_levels,
                                   const uint8_t *num_preds,
                                   const uint32_t *pred0, const uint32_t *pred1,
                                   const uint8_t *seeds, const uint8_t *descriptor,
                                   const uint32_t *key_src, const uint32_t *key_int)
{
    cg::grid_group grid = cg::this_grid();
    uint64_t gtid    = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x;
    uint64_t gstride = (uint64_t)gridDim.x * blockDim.x;

    for (int lv = 0; lv < num_levels; lv++) {
        uint32_t lstart     = level_start[lv];
        uint32_t n_in_level = level_start[lv + 1] - lstart;
        uint64_t total      = (uint64_t)n_in_level * w_count;
        for (uint64_t t = gtid; t < total; t += gstride) {
            uint32_t local = (uint32_t)(t % n_in_level);
            uint32_t j     = (uint32_t)(t / n_in_level);
            uint32_t id    = order[lstart + local];

            uint8_t       *base = work + (uint64_t)j * work_super_bytes;
            const uint8_t *seed = seeds + (uint64_t)j * 32;

            uint8_t np = num_preds[id];
            const uint8_t *p0 = (np >= 1) ? base + (uint64_t)pred0[id] * 32 : nullptr;
            const uint8_t *p1 = (np >= 2) ? base + (uint64_t)pred1[id] * 32 : nullptr;
            const uint32_t *key = (np == 0) ? key_src : key_int;

            pose_gpu::label_node(base + (uint64_t)id * 32, key, id, p0, p1, seed, descriptor);
        }
        grid.sync();
    }
}

/* Copy the output-set labels of a wave of w_count scaffolds out of the transient
 * work buffer into the persisted region, in challenge-rank order.  Persisted
 * super-chunk (wave_base + w) block `rank` receives work scaffold node out_id[rank].
 * thread t → wave-local scaffold w = t / cb, output rank = t % cb. */
__global__ void k_extract_outputs(uint8_t *persist, uint64_t persist_super_bytes,
                                  uint64_t wave_base,
                                  const uint8_t *work, uint64_t work_super_bytes,
                                  uint32_t w_count, const uint32_t *out_id,
                                  uint32_t cb)
{
    uint64_t tid   = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x;
    uint64_t total = (uint64_t)cb * w_count;
    if (tid >= total) return;

    uint32_t rank = (uint32_t)(tid % cb);
    uint32_t w    = (uint32_t)(tid / cb);

    const uint8_t *src = work + (uint64_t)w * work_super_bytes
                              + (uint64_t)out_id[rank] * 32;
    uint8_t *dst = persist + (wave_base + (uint64_t)w) * persist_super_bytes
                           + (uint64_t)rank * 32;
    for (int i = 0; i < 32; i++) dst[i] = src[i];
}

/* Cap on the number of scaffolds held in the transient work buffer at once.
 * Bounds kernel grid sizes and work-buffer HBM; waves below this are merged for
 * GPU occupancy when free HBM allows. */
#define POSE_GPU_WAVE_CAP 64

} /* namespace */

int label_gpu_run(const label_gpu_ctx_t *ctx)
{
    const uint32_t chunk_blocks = ctx->chunk_blocks;
    const uint64_t count        = ctx->count;
    const uint64_t num_super    = ctx->num_super;
    const int      L            = ctx->num_levels;

    size_t work_super_bytes = (size_t)count * 32; /* one scaffold's HBM footprint */
    size_t free_b = 0, total_b = 0, budget = 0;
    uint64_t wave = 0;
    int rc = -1;

    uint8_t *d_work = nullptr, *d_seeds = nullptr, *h_seeds = nullptr;

    #define CK(call) do { if ((call) != cudaSuccess) goto cleanup; } while (0)

    /* Decide the wave width from the HBM left after topology, then allocate the
     * transient work buffer (halving the wave on OOM down to a single scaffold,
     * with a margin so the CUDA driver working set still fits). */
    cudaMemGetInfo(&free_b, &total_b);
    budget = (free_b > (64u << 20)) ? (free_b - (64u << 20)) : 0;
    wave = work_super_bytes ? budget / work_super_bytes : 0;
    if (wave > num_super)         wave = num_super;
    if (wave > POSE_GPU_WAVE_CAP) wave = POSE_GPU_WAVE_CAP;
    if (wave == 0)                wave = 1; /* try one scaffold even if tight */

    while (wave >= 1) {
        if (cudaMalloc(&d_work, (size_t)wave * work_super_bytes) == cudaSuccess)
            break;
        d_work = nullptr;
        if (wave == 1) {
            fprintf(stderr, "label_gpu_run(level): work buffer (%zu MiB) alloc failed\n",
                    work_super_bytes >> 20);
            goto cleanup;
        }
        wave /= 2;
    }

    CK(cudaMalloc(&d_seeds, (size_t)wave * 32));
    h_seeds = (uint8_t *)malloc((size_t)wave * 32);
    if (!h_seeds) goto cleanup;

    {
        const int block = 256;

        /* Prefer a single cooperative launch walking all levels (k_label_all_levels)
         * over one launch per level.  Size the grid to the resident block count so
         * every block stays co-resident (required by cudaLaunchCooperativeKernel).
         * coop_grid == 0 → device lacks cooperative launch → per-level path. */
        int coop_grid = 0;
        {
            int dev = 0, coop = 0, numSm = 0, blocksPerSm = 0;
            cudaGetDevice(&dev);
            cudaDeviceGetAttribute(&coop, cudaDevAttrCooperativeLaunch, dev);
            if (coop) {
                cudaDeviceGetAttribute(&numSm, cudaDevAttrMultiProcessorCount, dev);
                cudaOccupancyMaxActiveBlocksPerMultiprocessor(
                    &blocksPerSm, (void *)k_label_all_levels, block, 0);
                coop_grid = numSm * blocksPerSm;
            }
            if (coop_grid < 1) coop_grid = 0;
        }

        const double PROGRESS_INTERVAL_S = 5.0;
        double t_start = pose_gpu_now_sec();
        double t_last  = t_start;
        if (!pose_gpu_quiet()) {
            printf("  GPU label[level]: %llu super-chunks, %llu/wave (%llu waves)\n",
                   (unsigned long long)num_super, (unsigned long long)wave,
                   (unsigned long long)((num_super + wave - 1) / wave));
            fflush(stdout);
        }

        for (uint64_t wave_base = 0; wave_base < num_super; wave_base += wave) {
            uint32_t w_count = (uint32_t)((num_super - wave_base < wave)
                                          ? (num_super - wave_base) : wave);

            /* Per-super-chunk seeds for this wave (chunk_index matches the CPU
             * path so the verifier recomputes identical labels). */
            for (uint32_t w = 0; w < w_count; w++) {
                uint64_t j = wave_base + w;
                uint64_t chunk_index =
                    (ctx->region_block_offset + j * (uint64_t)chunk_blocks) / chunk_blocks;
                pose_chunk_seed(h_seeds + (size_t)w * 32, ctx->session_seed,
                                ctx->seed_len, chunk_index);
            }
            CK(cudaMemcpy(d_seeds, h_seeds, (size_t)w_count * 32,
                          cudaMemcpyHostToDevice));

            if (coop_grid > 0) {
                uint8_t       *work  = d_work;
                uint64_t       wsb   = work_super_bytes;
                uint32_t       wc    = w_count;
                const uint32_t *order = ctx->d_order;
                const uint32_t *lstart = ctx->d_level_start;
                int            nlev  = L;
                const uint8_t  *np   = ctx->d_np;
                const uint32_t *p0   = ctx->d_p0;
                const uint32_t *p1   = ctx->d_p1;
                uint8_t        *seeds = d_seeds;
                const uint8_t  *desc = ctx->d_desc;
                const uint32_t *ksrc = ctx->d_ksrc;
                const uint32_t *kint = ctx->d_kint;
                void *kargs[] = {
                    &work, &wsb, &wc,
                    &order, &lstart, &nlev,
                    &np, &p0, &p1,
                    &seeds, &desc, &ksrc, &kint
                };
                CK(cudaLaunchCooperativeKernel((void *)k_label_all_levels,
                                               dim3((unsigned)coop_grid), dim3(block),
                                               kargs, 0, 0));
            } else {
                for (int lv = 0; lv < L; lv++) {
                    uint32_t n_in_level = ctx->level_count[lv];
                    if (n_in_level == 0) continue;
                    uint64_t total = (uint64_t)n_in_level * w_count;
                    uint64_t grid  = (total + block - 1) / block;
                    if (grid > 0x7FFFFFFFull) {
                        fprintf(stderr, "label_gpu_run(level): grid too large\n");
                        goto cleanup;
                    }
                    k_label_level<<<(unsigned)grid, block>>>(
                        d_work, work_super_bytes, w_count,
                        ctx->d_order + ctx->level_start[lv], n_in_level,
                        ctx->d_np, ctx->d_p0, ctx->d_p1, d_seeds,
                        ctx->d_desc, ctx->d_ksrc, ctx->d_kint);
                }
            }
            CK(cudaGetLastError());

            /* Persist only the output set (rank order) into the labeled region. */
            {
                uint64_t total = (uint64_t)chunk_blocks * w_count;
                uint64_t grid  = (total + block - 1) / block;
                if (grid > 0x7FFFFFFFull) {
                    fprintf(stderr, "label_gpu_run(level): extract grid too large\n");
                    goto cleanup;
                }
                k_extract_outputs<<<(unsigned)grid, block>>>(
                    ctx->persist, ctx->super_bytes, wave_base,
                    d_work, work_super_bytes, w_count, ctx->d_out_id, chunk_blocks);
            }
            CK(cudaGetLastError());
            CK(cudaDeviceSynchronize());

            uint64_t done = wave_base + w_count;
            double   tnow = pose_gpu_now_sec();
            if (!pose_gpu_quiet() &&
                (wave_base == 0 || done >= num_super ||
                 tnow - t_last >= PROGRESS_INTERVAL_S)) {
                double elapsed = tnow - t_start;
                double frac    = (double)done / (double)num_super;
                double eta     = (frac > 0.0) ? elapsed * (1.0 - frac) / frac : 0.0;
                printf("  GPU label[level]: %llu/%llu super-chunks (%.1f%%)  "
                       "elapsed %.0fs  eta %.0fs\n",
                       (unsigned long long)done, (unsigned long long)num_super,
                       frac * 100.0, elapsed, eta);
                fflush(stdout);
                t_last = tnow;
            }
        }

        /* Zero the transient scaffold before freeing so it cannot serve as a
         * label cache during the challenge phase (it is excluded from coverage). */
        CK(cudaMemset(d_work, 0, (size_t)wave * work_super_bytes));
    }
    rc = 0;

cleanup:
    #undef CK
    cudaFree(d_work);
    cudaFree(d_seeds);
    free(h_seeds);
    return rc;
}
