/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#include "gpu_sessions.h"
#include "gpu_targets.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int reject(sink_init_ack_t *ack, const char *why)
{
    ack->status = SINK_STATUS_ERROR;
    snprintf(ack->error, sizeof(ack->error), "%s", why);
    fprintf(stderr, "session: rejected: %s\n", ack->error);
    return 0;
}

#ifdef TARGET_GPU

#include "gpu_session.h"
#include "graph.h"

/* An HBM session labels its whole buffer, using the buffer's own unlabeled
 * tail as the labeler's scratch (gpu_session.h); beside the buffer it needs
 * only the resident topology, the margin and one pool for the final
 * super-chunk — pose_gpu_scratch_bytes(cb, 1), which the planner leaves. */

typedef struct {
    int                used;
    pose_gpu_session_t gs;
} slot_t;

static slot_t g_slots[SINK_MAX_SESSIONS];

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static slot_t *slot_of(uint32_t session)
{
    if (session >= SINK_MAX_SESSIONS || !g_slots[session].used)
        return NULL;
    return &g_slots[session];
}

int gpu_sessions_init(const sink_session_init_t *init, sink_init_ack_t *ack)
{
    memset(ack, 0, sizeof(*ack));
    char why[SINK_REASON_MAX];

    int device = gpu_targets_parse(init->target);
    if (device < 0 || device >= gpu_targets_count()) {
        snprintf(why, sizeof(why), "no such GPU target '%s'", init->target);
        return reject(ack, why);
    }
    if (init->session >= SINK_MAX_SESSIONS) {
        snprintf(why, sizeof(why), "session number %u is out of range", init->session);
        return reject(ack, why);
    }
    if (g_slots[init->session].used) {
        snprintf(why, sizeof(why), "session %u already exists", init->session);
        return reject(ack, why);
    }
    for (uint32_t s = 0; s < SINK_MAX_SESSIONS; s++) {
        if (g_slots[s].used && g_slots[s].gs.device == device) {
            snprintf(why, sizeof(why), "%s already has session %u", init->target, s);
            return reject(ack, why);
        }
    }
    if (init->algo != SINK_ALGO_BLAKE3)
        return reject(ack, "GPU sessions label with BLAKE3 only");
    if (init->base != 0)
        return reject(ack, "a GPU session starts at base 0");
    uint32_t cb = init->chunk_blocks;
    if (cb == 0 || (cb & (cb - 1)) != 0 || cb > POSE_GRAPH_MAX_M) {
        snprintf(why, sizeof(why), "chunk_blocks %u must be a power of two in [1, %u]",
                 cb, POSE_GRAPH_MAX_M);
        return reject(ack, why);
    }
    uint64_t super_bytes = pose_graph_super_chunk_blocks(cb) * POSE_HASH_BYTES;
    if (init->size == 0 || init->size % super_bytes != 0) {
        snprintf(why, sizeof(why), "size must be a non-zero multiple of the %llu-byte super-chunk",
                 (unsigned long long)super_bytes);
        return reject(ack, why);
    }

    pose_gpu_info_t info;
    if (pose_gpu_info(device, &info) != 0) {
        snprintf(why, sizeof(why), "%s: cannot query the device", init->target);
        return reject(ack, why);
    }
    uint64_t scratch = pose_gpu_scratch_bytes(cb, 1);
    uint64_t need    = init->size + scratch + POSE_GPU_HEADROOM_BYTES;
    if (need > info.free_bytes) {
        snprintf(why, sizeof(why), "%s has %llu MiB free: %llu MiB session + %llu MiB labeling scratch "
                 "(chunk_blocks %u) + %llu MiB headroom do not fit",
                 init->target, (unsigned long long)(info.free_bytes >> 20),
                 (unsigned long long)(init->size >> 20), (unsigned long long)(scratch >> 20), cb,
                 (unsigned long long)(POSE_GPU_HEADROOM_BYTES >> 20));
        return reject(ack, why);
    }

    slot_t *s = &g_slots[init->session];
    if (pose_gpu_session_alloc(&s->gs, device, init->size, why, sizeof(why)) != 0)
        return reject(ack, why);
    s->used = 1;
    printf("Session %u: %s (%s) %llu MiB of HBM allocated, %llu MiB left for labeling scratch\n",
           init->session, init->target, info.name, (unsigned long long)(init->size >> 20),
           (unsigned long long)((info.free_bytes - init->size) >> 20));
    fflush(stdout);
    ack->status = SINK_STATUS_OK;
    return 1;
}

typedef struct {
    uint32_t                 session;
    uint64_t                 super_bytes;
    gpu_sessions_progress_fn fn;
    void                    *ctx;
    int                      failed;
} progress_ctx_t;

static void on_progress(void *vctx, uint64_t done, uint64_t num_super)
{
    (void)num_super;
    progress_ctx_t *p = vctx;
    if (p->failed || !p->fn)
        return;
    if (p->fn(p->ctx, p->session, done * p->super_bytes) != 0)
        p->failed = 1;      /* the labeler has no abort; the caller fails after */
}

int gpu_sessions_label(uint32_t session, const sink_session_init_t *init,
                       gpu_sessions_progress_fn progress, void *ctx, char *why, size_t why_cap)
{
    slot_t *s = slot_of(session);
    if (!s) {
        snprintf(why, why_cap, "session %u is not a GPU session", session);
        return -1;
    }
    const uint32_t cb = init->chunk_blocks;
    progress_ctx_t p = {
        .session = session, .super_bytes = pose_graph_super_chunk_blocks(cb) * POSE_HASH_BYTES,
        .fn = progress, .ctx = ctx, .failed = 0,
    };
    printf("Session %u: labeling %llu MiB of HBM on gpu%d (%llu super-chunks, chunk_blocks %u, blake3)\n",
           session, (unsigned long long)(s->gs.hbm_len >> 20), s->gs.device,
           (unsigned long long)(s->gs.hbm_len / p.super_bytes), cb);
    fflush(stdout);
    double t0 = now_s();
    if (pose_gpu_session_label(&s->gs, init->seed, SINK_SEED_BYTES, cb, POSE_HASH_BLAKE3,
                               on_progress, &p, why, why_cap) != 0)
        return -1;
    double dt = now_s() - t0;
    if (p.failed) {
        snprintf(why, why_cap, "client connection lost sending Progress");
        return -1;
    }
    printf("Session %u: labeled %llu MiB in %.1f s (%.0f MiB/s)\n", session,
           (unsigned long long)(s->gs.hbm_len >> 20), dt,
           dt > 0 ? (double)(s->gs.hbm_len >> 20) / dt : 0.0);
    fflush(stdout);
    return 0;
}

int gpu_sessions_coverage(uint32_t session, sink_coverage_t *cov, char *why, size_t why_cap)
{
    slot_t *s = slot_of(session);
    if (!s || s->gs.chunk_blocks == 0) {
        snprintf(why, why_cap, "session %u is not a labeled GPU session", session);
        return -1;
    }
    memset(cov, 0, sizeof(*cov));
    cov->count = 1;
    cov->regions[0].phys_base   = 0;
    cov->regions[0].block_count = s->gs.total_blocks;
    return 0;
}

int gpu_sessions_owns(uint32_t session)
{
    return slot_of(session) != NULL;
}

int gpu_sessions_challenge(uint32_t session, uint64_t block_index,
                           uint8_t label[SINK_LABEL_BYTES], char *why, size_t why_cap)
{
    memset(label, 0, SINK_LABEL_BYTES);
    slot_t *s = slot_of(session);
    if (!s) {
        snprintf(why, why_cap, "challenge for unknown session %u", session);
        return -1;
    }
    if (block_index >= s->gs.total_blocks) {
        snprintf(why, why_cap, "session %u: block %llu is past the end (%llu blocks)", session,
                 (unsigned long long)block_index, (unsigned long long)s->gs.total_blocks);
        return -1;
    }
    if (pose_gpu_session_lookup(&s->gs, block_index, label) != 0) {
        memset(label, 0, SINK_LABEL_BYTES);
        snprintf(why, why_cap, "session %u: cannot read block %llu from gpu%d", session,
                 (unsigned long long)block_index, s->gs.device);
        return -1;
    }
    return 0;
}

#else /* !TARGET_GPU */

int gpu_sessions_init(const sink_session_init_t *init, sink_init_ack_t *ack)
{
    (void)init;
    memset(ack, 0, sizeof(*ack));
    return reject(ack, "built without GPU support (TARGET_GPU=OFF)");
}

int gpu_sessions_label(uint32_t session, const sink_session_init_t *init,
                       gpu_sessions_progress_fn progress, void *ctx, char *why, size_t why_cap)
{
    (void)init; (void)progress; (void)ctx;
    snprintf(why, why_cap, "session %u is not a GPU session", session);
    return -1;
}

int gpu_sessions_coverage(uint32_t session, sink_coverage_t *cov, char *why, size_t why_cap)
{
    (void)cov;
    snprintf(why, why_cap, "session %u is not a GPU session", session);
    return -1;
}

int gpu_sessions_owns(uint32_t session)
{
    (void)session;
    return 0;
}

int gpu_sessions_challenge(uint32_t session, uint64_t block_index,
                           uint8_t label[SINK_LABEL_BYTES], char *why, size_t why_cap)
{
    (void)block_index;
    memset(label, 0, SINK_LABEL_BYTES);
    snprintf(why, why_cap, "challenge for unknown session %u", session);
    return -1;
}

#endif
