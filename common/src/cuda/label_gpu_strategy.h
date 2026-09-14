/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#ifndef POSE_LABEL_GPU_STRATEGY_H
#define POSE_LABEL_GPU_STRATEGY_H

/*
 * Internal contract between the shared GPU-labeling host driver
 * (label_gpu_common.cu) and exactly one compiled-in strategy translation unit
 * (label_gpu_level.cu / label_gpu_block.cu / label_gpu_thread.cu /
 * label_gpu_warp.cu, selected by
 * the POSE_GPU_LABEL_STRATEGY build flag — see common/CMakeLists.txt).
 *
 * The common driver computes the seed-independent scaffold topology once
 * (pose_graph_edges + dependency levels + level-sorted order + output-rank map),
 * uploads it to the device, optionally computes/uploads the slot-recycling map
 * (pose_graph_slot_map, for the per-chunk strategies), fills a label_gpu_ctx_t,
 * and hands it to label_gpu_run().  Each strategy owns ONLY the labeling itself:
 * its transient scratch buffer, per-chunk seed derivation, kernel launches,
 * output-to-persist, progress, and the zero+free of its scratch before the
 * challenge phase.  The hash inputs, node order, keys, and descriptor are
 * identical across strategies, so every strategy's persisted output labels are
 * byte-identical to the CPU path (checked by disk-wipe-bench --verify, which
 * reads labels back and recomputes them with graph.c).
 */

#include <stddef.h>
#include <stdint.h>

typedef struct {
    /* session / persisted region */
    const uint8_t *session_seed;
    size_t         seed_len;
    uint32_t       chunk_blocks;
    uint64_t       region_block_offset;
    uint8_t       *persist;        /* persisted HBM (cb output labels/super-chunk) */
    size_t         super_bytes;    /* persisted super-chunk bytes = chunk_blocks*32 */
    uint64_t       num_super;      /* complete super-chunks to label */

    /* scaffold topology (device) — the byte-parity source of truth, shared by
     * every strategy */
    uint64_t        count;         /* scaffold node count per super-chunk */
    int             num_levels;    /* dependency-level count (L) */
    const uint32_t *level_count;   /* host [num_levels]   nodes per level */
    const uint32_t *level_start;   /* host [num_levels+1] prefix sums */
    const uint8_t  *d_np;          /* [count]   predecessor count 0/1/2 */
    const uint32_t *d_p0;          /* [count]   predecessor 0 id */
    const uint32_t *d_p1;          /* [count]   predecessor 1 id */
    const uint32_t *d_order;       /* [count]   level-sorted node schedule */
    const uint32_t *d_level_start; /* [num_levels+1] device copy of level_start */
    const uint32_t *d_out_id;      /* [chunk_blocks] challenge rank -> scaffold id */
    const uint8_t  *d_desc;        /* [32] graph descriptor digest */
    const uint32_t *d_ksrc;        /* [8]  source-node key */
    const uint32_t *d_kint;        /* [8]  internal-node key */

    /* slot recycling (device) — populated only when LABEL_GPU_USES_SLOT_MAP != 0.
     * Every node has a real recycled slot; output-set nodes additionally carry a
     * challenge rank so the kernel copies them to persist when computed. */
    const uint32_t *d_slot;        /* [count] recycled scratch slot; nullptr for LEVEL */
    const int32_t  *d_rank;        /* [count] challenge rank or -1; nullptr for LEVEL */
    uint32_t        pool_size;     /* recycled labels per chunk; 0 for LEVEL */
} label_gpu_ctx_t;

/* Each strategy file defines these three symbols. */
extern const int   LABEL_GPU_USES_SLOT_MAP;   /* 1 if the driver must build d_slot */
extern const char *LABEL_GPU_STRATEGY_NAME;   /* "level" / "block" / "thread" / "warp" */
int label_gpu_run(const label_gpu_ctx_t *ctx);  /* returns 0 on success, -1 on error */

/* Monotonic wall-clock seconds, shared progress helper (defined in common). */
double pose_gpu_now_sec(void);

/* Non-zero when POSE_GPU_QUIET is set: strategies should skip their stdout
 * progress lines so a caller can render its own live display (defined in common). */
int pose_gpu_quiet(void);

#endif /* POSE_LABEL_GPU_STRATEGY_H */
