/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#ifndef POSE_GPU_SESSION_H
#define POSE_GPU_SESSION_H
/*
 * GPU HBM sessions: one device's labeled HBM buffer, from allocation through
 * labeling to answering challenges.  The host side of a GPU target for the
 * source-daemon, kept in pose-common's CUDA library so the daemon stays plain C.
 *
 * A session labels a cudaMalloc'd buffer in place with the paper-faithful
 * labeler (gpu_label.h), byte-identical to the CPU in-place path, so the
 * verifier recomputes it with pose_graph_label_chunk_inplace as for RAM: block
 * i of the session is the 32-byte label at hbm[i * 32], chunk k of the buffer
 * is seeded as (base / 32) / chunk_blocks + k with base 0 (identity map, rank
 * r at block r, exactly as a sink stores a stream).  The buffer stays resident
 * until freed; a challenge is one 32-byte device-to-host copy.
 *
 * cudaMalloc only ever returns user-allocatable HBM, so firmware and
 * fixed-function regions are never reachable.  POSE_GPU_HEADROOM_BYTES is
 * additionally left unallocated for the driver's own working set.  What the
 * labeler needs transiently (the resident topology and the per-super-chunk
 * scratch pools) is allocated by the library at label time from whatever HBM
 * is still free, so the caller must leave pose_gpu_scratch_bytes() free.
 *
 * Only BLAKE3 is implemented on the GPU.
 */

#include "hash.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HBM left unallocated per device for the CUDA driver and context. */
#define POSE_GPU_HEADROOM_BYTES (512ull << 20)

typedef struct {
    int      device;          /* CUDA device ordinal */
    char     name[256];       /* e.g. "NVIDIA H200 NVL" (cudaDeviceProp.name) */
    char     pci_bus_id[32];  /* e.g. "0000:25:00.0" */
    uint64_t total_bytes;     /* HBM the driver reports */
    uint64_t free_bytes;      /* free with this process's context created */
    int      sm_count;
} pose_gpu_info_t;

/* Initialise the runtime and create a context on every device, so free HBM
 * is measured with the context's own allocations already taken.  Returns the
 * device count (0 if there are none), or -1 with the reason in why (no
 * driver, no libcuda.so.1, firmware failure, ...). */
int pose_gpu_init(char *why, size_t why_cap);

/* Describe device `device` (0 <= device < pose_gpu_init()).  Returns 0, or -1. */
int pose_gpu_info(int device, pose_gpu_info_t *out);

/* Bytes the labeler holds on the device besides the session buffer while
 * labeling with chunk_blocks and `inflight` scratch pools of 2*chunk_blocks
 * labels: the resident scaffold topology, the pools, and a fixed margin.  An
 * HBM session labels its whole buffer by using the buffer's own unlabeled
 * tail as scratch (pose_gpu_session_label), so it only needs this with
 * inflight = 1 (the final super-chunk's pool) beside the buffer: the planner
 * subtracts pose_gpu_scratch_bytes(cb, 1) (and the headroom, which the
 * inventory already excludes) from a device's free HBM to size a session.
 * chunk_blocks 0 means POSE_CHUNK_BLOCKS.  Plain C (src/gpu_scratch.c), in
 * pose_common itself, so a CUDA-less verifier computes the same figure as the
 * daemon. */
uint64_t pose_gpu_scratch_bytes(uint32_t chunk_blocks, uint32_t inflight);

typedef struct {
    int      device;
    void    *hbm;             /* cudaMalloc'd device buffer, NULL if none */
    uint64_t hbm_len;         /* bytes; a whole number of super-chunks once labeled */
    uint32_t chunk_blocks;    /* 0 until labeled */
    uint64_t total_blocks;    /* hbm_len / 32 */
} pose_gpu_session_t;

/* Allocate `bytes` of HBM on device.  Returns 0, or -1 with the reason in why
 * (not enough free HBM, CUDA error).  The session holds nothing until then. */
int pose_gpu_session_alloc(pose_gpu_session_t *gs, int device, uint64_t bytes,
                           char *why, size_t why_cap);

/* Called after every batch of super-chunks is labeled, with the running total
 * (done of num_super), from the labeling thread. */
typedef void (*pose_gpu_progress_fn)(void *ctx, uint64_t done, uint64_t num_super);

/* Label the whole allocated buffer in place, every super-chunk of it.  The
 * labeler needs 2*chunk_blocks labels of scratch per super-chunk in flight,
 * so the buffer's own not-yet-labeled tail is used: the first pass labels
 * all but the last 2p super-chunks with p pools living in those 2p; the next
 * labels the freed p with p/2 pools in the last p; and so on, halving, down
 * to one pool; the final super-chunk's pool comes from free HBM (the margin).
 * Each halving step costs about the same time, so the whole tail costs ~log2
 * p steps — ~9 minutes on an H200 at chunk_blocks 2^18 against a 7-minute
 * first pass — and leaves only the topology and margin unlabeled.  bytes
 * must be a whole number of super-chunks; algo must be POSE_HASH_BLAKE3.
 * Blocks until done.  Returns 0, or -1 with the reason in why. */
int pose_gpu_session_label(pose_gpu_session_t *gs, const uint8_t *seed, size_t seed_len,
                           uint32_t chunk_blocks, pose_hash_algo_t algo,
                           pose_gpu_progress_fn progress, void *ctx,
                           char *why, size_t why_cap);

/* Copy the label of block_index (0 <= block_index < total_blocks) to label.
 * Returns 0, or -1 (out of range, CUDA error). */
int pose_gpu_session_lookup(const pose_gpu_session_t *gs, uint64_t block_index,
                            uint8_t label[POSE_HASH_BYTES]);

/* Free the device buffer; zeroes *gs. */
void pose_gpu_session_free(pose_gpu_session_t *gs);

/* ── label generator: labels computed in HBM, delivered to the host ──────────
 *
 * For sessions whose labels live elsewhere (a disk, a remote sink's RAM): the
 * GPU labels a window of super-chunks in a device buffer, byte-identical to
 * what it would hold as an HBM session at the same chunk indices, and the
 * window is copied out in pinned host slices to a callback, in order.  Two
 * device windows ping-pong on a labeling thread so window N+1 is labeled
 * while N is being copied and delivered (a slow consumer, such as a sink
 * behind a 90 MiB/s relay, would otherwise leave the GPU idle).  The
 * topology is built once at open and reused for every window.  BLAKE3 only.
 */
typedef struct pose_gpu_gen pose_gpu_gen_t;

/* Open a generator on `device` for chunk_blocks, with two device windows of
 * `window_bytes` (rounded down to whole super-chunks, at least one) and a
 * pinned host slice of `slice_bytes` (likewise; capped at the window).  The
 * labeler's own scratch is allocated per window call from what is left free,
 * so leave pose_gpu_scratch_bytes(chunk_blocks, window super-chunks) + the
 * headroom unallocated.  Returns NULL with the reason in why. */
pose_gpu_gen_t *pose_gpu_gen_open(int device, uint32_t chunk_blocks, uint64_t window_bytes,
                                  uint64_t slice_bytes, char *why, size_t why_cap);

/* Super-chunks per window and bytes per slice, as opened. */
uint64_t pose_gpu_gen_window_supers(const pose_gpu_gen_t *gen);
uint64_t pose_gpu_gen_slice_bytes(const pose_gpu_gen_t *gen);

/* Called with each slice of labels as it arrives, in order: the session's
 * bytes [offset, offset + len), len a whole number of super-chunks.  Return
 * -1 to stop (the run then fails). */
typedef int (*pose_gpu_gen_sink_fn)(void *ctx, uint64_t offset, const uint8_t *buf, size_t len);

/* Label super-chunks [first, first + count) of a session at byte `base` (the
 * placement contract: super-chunk j has chunk index (base/32)/chunk_blocks +
 * j, as the CPU path and the verifier compute it) and deliver them through
 * sink, offsets counted from the session's byte 0.  Blocks until done.
 * Returns 0, or -1 with the reason in why (a sink refusal is reported as
 * "delivery refused"). */
int pose_gpu_gen_run(pose_gpu_gen_t *gen, const uint8_t *seed, size_t seed_len, uint64_t base,
                     uint64_t first, uint64_t count, pose_gpu_gen_sink_fn sink, void *ctx,
                     char *why, size_t why_cap);

/* Zero and free the windows (they held labels of other targets), the slice
 * and the topology. */
void pose_gpu_gen_close(pose_gpu_gen_t *gen);

/* Parity gate: label one super-chunk of HBM on `device` for (seed,
 * chunk_blocks) and compare it byte for byte with cpu_ref, a super-chunk the
 * CPU labeled for the same seed at chunk index 0.  super_bytes must be
 * chunk_blocks * 32.  Returns 0 if identical, 1 on a mismatch (the first
 * differing byte offset in *first_diff if non-NULL), -1 on a CUDA error. */
int pose_gpu_parity_check(int device, const uint8_t *seed, size_t seed_len,
                          uint32_t chunk_blocks, const uint8_t *cpu_ref,
                          size_t super_bytes, size_t *first_diff);

#ifdef __cplusplus
}
#endif

#endif /* POSE_GPU_SESSION_H */
