/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#include "label_gen.h"
#include "gpu_targets.h"

#include "graph.h"
#include "hash.h"
#ifdef TARGET_GPU
#include "gpu_session.h"
#endif

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Per-thread buffers, sized for one chunk_blocks and kept between sessions
 * that use the same value.  The pool's workers live for the whole daemon. */
static pose_graph_pool_t *g_pool;
static uint8_t           *g_slots[POSE_REGION_THREADS_MAX];
static uint8_t           *g_scratch[POSE_REGION_THREADS_MAX];
static uint32_t           g_cb;        /* chunk_blocks the buffers fit; 0 = none */
static int                g_threads;

/* The GPU generator (gpu_session.h): labels computed in a GPU's HBM and
 * copied out, for sessions whose algo the GPU implements (BLAKE3).  Chosen
 * per session at prepare; POSE_LABEL_GEN=cpu|gpu|auto (default auto: the GPU
 * whenever there is one and the algo allows; gpu makes a CPU fallback an
 * error).  Kept open between sessions with the same chunk_blocks. */
#ifdef TARGET_GPU
static pose_gpu_gen_t *g_gen;
static int             g_gen_device = -1;
static uint32_t        g_gen_cb;
#endif
static int             g_use_gpu;        /* the session being prepared/run */

/* Largest label window per device buffer (two are allocated) and the pinned
 * host slice the labels are copied out through.  The slice is host RAM for
 * the generator's lifetime and sets the daemon's peak on a GPU-generated
 * session (x86 UKI mem= budget); it only has to hold one super-chunk (32 MiB
 * at the largest chunk_blocks), and a window drains in slice-sized pinned
 * DMA copies at PCIe speed, so its size does not affect throughput. */
#define GEN_WINDOW_MAX_BYTES (8ull << 30)
#define GEN_SLICE_BYTES      (256ull << 20)

typedef enum { GEN_AUTO, GEN_CPU, GEN_GPU } gen_mode_t;

static gen_mode_t gen_mode(void)
{
    const char *e = getenv("POSE_LABEL_GEN");
    if (!e || !*e || strcmp(e, "auto") == 0) return GEN_AUTO;
    if (strcmp(e, "cpu") == 0) return GEN_CPU;
    if (strcmp(e, "gpu") == 0) return GEN_GPU;
    fprintf(stderr, "label_gen: POSE_LABEL_GEN=%s is not auto, cpu or gpu; using auto\n", e);
    return GEN_AUTO;
}

/* Seconds between progress lines. */
#define PROGRESS_INTERVAL_S 10.0

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void free_buffers(void)
{
    for (int i = 0; i < POSE_REGION_THREADS_MAX; i++) {
        free(g_slots[i]);
        free(g_scratch[i]);
        g_slots[i] = g_scratch[i] = NULL;
    }
    g_cb = 0;
    g_threads = 0;
}

int label_gen_prepare(const sink_session_init_t *init, char *why, size_t why_cap)
{
    if (init->algo > SINK_ALGO_AES_PRF) {
        snprintf(why, why_cap, "unknown algo %u", init->algo);
        return -1;
    }
    uint32_t cb = init->chunk_blocks;
    if (cb == 0 || cb > POSE_GRAPH_MAX_M || (cb & (cb - 1)) != 0) {
        snprintf(why, why_cap, "chunk_blocks %u must be a power of two in [1, %u]",
                 cb, (unsigned)POSE_GRAPH_MAX_M);
        return -1;
    }
    uint64_t super_bytes = pose_graph_super_chunk_blocks(cb) * POSE_HASH_BYTES;
    if (init->size == 0 || init->size % super_bytes != 0) {
        snprintf(why, why_cap, "size is not a whole number of super-chunks (%llu bytes)",
                 (unsigned long long)super_bytes);
        return -1;
    }
    if (init->base % POSE_HASH_BYTES != 0) {
        snprintf(why, why_cap, "base is not a multiple of %d", POSE_HASH_BYTES);
        return -1;
    }

    /* GPU or CPU for this session? */
    gen_mode_t mode = gen_mode();
    g_use_gpu = 0;
    if (mode != GEN_CPU) {
        const char *no_gpu = NULL;
#ifdef TARGET_GPU
        if (gpu_targets_count() == 0)
            no_gpu = "no GPU";
        else if (init->algo != SINK_ALGO_BLAKE3)
            no_gpu = "the GPU labels with BLAKE3 only";
#else
        no_gpu = "built without GPU support (TARGET_GPU=OFF)";
#endif
        if (no_gpu && mode == GEN_GPU) {
            snprintf(why, why_cap, "POSE_LABEL_GEN=gpu but %s", no_gpu);
            return -1;
        }
        if (!no_gpu)
            g_use_gpu = 1;
    }
#ifdef TARGET_GPU
    if (g_use_gpu) {
        /* The device with the most free HBM.  Two windows of n super-chunks
         * plus the labeler's pool of 2*cb labels per super-chunk in flight
         * cost 4 super-chunks' worth each, on top of the topology and the
         * headroom. */
        int      device = -1;
        uint64_t best   = 0;
        char     name[sizeof(((pose_gpu_info_t *)0)->name)] = "";
        for (int d = 0; d < gpu_targets_count(); d++) {
            pose_gpu_info_t info;
            if (pose_gpu_info(d, &info) == 0 && info.free_bytes > best) {
                best = info.free_bytes;
                device = d;
                snprintf(name, sizeof(name), "%s", info.name);
            }
        }
        uint64_t fixed  = POSE_GPU_HEADROOM_BYTES + pose_gpu_scratch_bytes(cb, 0);
        uint64_t budget = best > fixed ? best - fixed : 0;
        uint64_t n      = budget / (4 * super_bytes);
        if (n * super_bytes > GEN_WINDOW_MAX_BYTES) n = GEN_WINDOW_MAX_BYTES / super_bytes;
        if (device < 0 || n == 0) {
            snprintf(why, why_cap, "no GPU has room for a label window: gpu%d has %llu MiB free, "
                     "a super-chunk is %llu MiB and the labeler needs %llu MiB beside it",
                     device, (unsigned long long)(best >> 20), (unsigned long long)(super_bytes >> 20),
                     (unsigned long long)(fixed >> 20));
            return -1;
        }
        if (g_gen && (g_gen_device != device || g_gen_cb != cb || pose_gpu_gen_window_supers(g_gen) != n)) {
            pose_gpu_gen_close(g_gen);
            g_gen = NULL;
        }
        if (!g_gen) {
            g_gen = pose_gpu_gen_open(device, cb, n * super_bytes, GEN_SLICE_BYTES, why, why_cap);
            if (!g_gen)
                return -1;
            g_gen_device = device;
            g_gen_cb     = cb;
            printf("label_gen: gpu%d (%s), chunk_blocks %u, 2 x %llu MiB label windows (%llu super-chunks), "
                   "%llu MiB host slice\n", device, name, cb,
                   (unsigned long long)((n * super_bytes) >> 20), (unsigned long long)n,
                   (unsigned long long)(pose_gpu_gen_slice_bytes(g_gen) >> 20));
            fflush(stdout);
        }
        return 0;
    }
#endif

    if (!g_pool) {
        g_pool = pose_graph_pool_create();
        if (!g_pool) {
            snprintf(why, why_cap, "cannot start the labeling threads");
            return -1;
        }
    }
    if (g_cb == cb)
        return 0;

    free_buffers();
    int    threads       = pose_region_threads();
    size_t scratch_bytes = pose_graph_scratch_bytes_inplace(cb);
    for (int i = 0; i < threads; i++) {
        void *slot = NULL, *scratch = NULL;
        if (posix_memalign(&slot, 64, (size_t)super_bytes) != 0 ||
            posix_memalign(&scratch, 64, scratch_bytes) != 0) {
            free(slot);
            free(scratch);
            free_buffers();
            snprintf(why, why_cap,
                     "cannot allocate %llu MiB of labeling buffers (chunk_blocks %u, %d threads)",
                     (unsigned long long)(((uint64_t)super_bytes + scratch_bytes) * (uint64_t)threads >> 20),
                     cb, threads);
            return -1;
        }
        g_slots[i]   = slot;
        g_scratch[i] = scratch;
    }
    g_cb      = cb;
    g_threads = threads;
    printf("label_gen: chunk_blocks %u, %d threads, %llu MiB of buffers\n", cb, threads,
           (unsigned long long)(((uint64_t)super_bytes + scratch_bytes) * (uint64_t)threads >> 20));
    fflush(stdout);
    return 0;
}

/* ── in-order delivery gate ──────────────────────────────────────────────── */

typedef struct {
    pthread_mutex_t  mu;
    pthread_cond_t   cv;
    uint64_t         next;      /* absolute byte offset to deliver next */
    int              failed;    /* a delivery failed: wake and fail everyone */
    char             why[SINK_REASON_MAX];
    uint64_t         base;
    uint32_t         session;
    label_deliver_fn deliver;
    void            *ctx;
} gate_t;

/* pose_label_sink_fn: called by each pool worker once its super-chunk is
 * labeled.  Waits until every lower offset is delivered, then delivers this
 * one.  Delivery happens under the mutex, so only one call runs at a time. */
static int gate_sink(void *c, const uint8_t *buf, size_t len, uint64_t off)
{
    gate_t *g = c;
    int rc;
    pthread_mutex_lock(&g->mu);
    while (!g->failed && off != g->next)
        pthread_cond_wait(&g->cv, &g->mu);
    if (g->failed) {
        rc = -1;
    } else {
        rc = g->deliver(g->ctx, g->session, off - g->base, buf, len, g->why, sizeof(g->why));
        if (rc != 0)
            g->failed = 1;
        else
            g->next += len;
    }
    pthread_cond_broadcast(&g->cv);
    pthread_mutex_unlock(&g->mu);
    return rc;
}

#ifdef TARGET_GPU
/* pose_gpu_gen_sink_fn: a slice of labels from the GPU, delivered one
 * super-chunk at a time through the session's label_deliver_fn. */
typedef struct {
    const sink_session_init_t *init;
    label_deliver_fn           deliver;
    void                      *ctx;
    uint64_t                   super_bytes;
    char                       why[SINK_REASON_MAX];
    double                     t0, t_last;
} gpu_sink_t;

static int gpu_sink(void *c, uint64_t offset, const uint8_t *buf, size_t len)
{
    gpu_sink_t *s = c;
    for (size_t off = 0; off < len; off += (size_t)s->super_bytes) {
        if (s->deliver(s->ctx, s->init->session, offset + off, buf + off, (size_t)s->super_bytes,
                       s->why, sizeof(s->why)) != 0)
            return -1;
    }
    uint64_t done = offset + len;
    double   t    = now_s();
    if (t - s->t_last >= PROGRESS_INTERVAL_S || done == s->init->size) {
        printf("Session %u: %llu / %llu MiB (%.0f%%), %.1f MiB/s (GPU)\n", s->init->session,
               (unsigned long long)(done >> 20), (unsigned long long)(s->init->size >> 20),
               100.0 * (double)done / (double)s->init->size,
               (double)done / (1 << 20) / (t - s->t0 > 0 ? t - s->t0 : 1e-9));
        fflush(stdout);
        s->t_last = t;
    }
    return 0;
}

static int label_gen_run_gpu(const sink_session_init_t *init, label_deliver_fn deliver, void *ctx,
                             char *why, size_t why_cap)
{
    const uint32_t cb          = init->chunk_blocks;
    const uint64_t super_bytes = pose_graph_super_chunk_blocks(cb) * POSE_HASH_BYTES;
    const uint64_t supers      = init->size / super_bytes;
    gpu_sink_t s = { .init = init, .deliver = deliver, .ctx = ctx, .super_bytes = super_bytes,
                     .why = "", .t0 = now_s(), .t_last = now_s() };
    printf("Session %u: generating %llu MiB on gpu%d (%llu super-chunks, chunk_blocks %u, blake3)\n",
           init->session, (unsigned long long)(init->size >> 20), g_gen_device,
           (unsigned long long)supers, cb);
    fflush(stdout);
    if (pose_gpu_gen_run(g_gen, init->seed, SINK_SEED_BYTES, init->base, 0, supers, gpu_sink, &s,
                         why, why_cap) != 0) {
        if (s.why[0])
            snprintf(why, why_cap, "%s", s.why);
        return -1;
    }
    return 0;
}
#endif

int label_gen_run(const sink_session_init_t *init, label_deliver_fn deliver, void *ctx,
                  char *why, size_t why_cap)
{
#ifdef TARGET_GPU
    if (g_use_gpu) {
        if (!g_gen || g_gen_cb != init->chunk_blocks) {
            snprintf(why, why_cap, "internal: label_gen_run (GPU) without label_gen_prepare");
            return -1;
        }
        return label_gen_run_gpu(init, deliver, ctx, why, why_cap);
    }
#endif
    if (g_cb != init->chunk_blocks || !g_pool) {
        snprintf(why, why_cap, "internal: label_gen_run without label_gen_prepare");
        return -1;
    }
    const uint32_t cb          = init->chunk_blocks;
    const uint64_t super_bytes = pose_graph_super_chunk_blocks(cb) * POSE_HASH_BYTES;
    const uint64_t supers      = init->size / super_bytes;

    gate_t g;
    memset(&g, 0, sizeof(g));
    pthread_mutex_init(&g.mu, NULL);
    pthread_cond_init(&g.cv, NULL);
    g.next    = init->base;
    g.base    = init->base;
    g.session = init->session;
    g.deliver = deliver;
    g.ctx     = ctx;

    printf("Session %u: generating %llu MiB (%llu super-chunks, chunk_blocks %u, %s)\n",
           init->session, (unsigned long long)(init->size >> 20), (unsigned long long)supers,
           cb, pose_hash_to_string((pose_hash_algo_t)init->algo));
    fflush(stdout);

    int    rc     = 0;
    double t0     = now_s();
    double t_last = t0;
    for (uint64_t j = 0; j < supers; ) {
        int n = (int)(supers - j < (uint64_t)g_threads ? supers - j : (uint64_t)g_threads);
        /* region_block_offset = base / 32 and byte_start = base: the placement
         * contract that makes the verifier's recomputation match. */
        if (pose_graph_label_disk_wave(init->seed, SINK_SEED_BYTES, g_slots, g_scratch, g_pool,
                                       (pose_hash_algo_t)init->algo, cb, n, j,
                                       init->base / POSE_HASH_BYTES, init->base,
                                       (size_t)super_bytes, gate_sink, &g) != 0) {
            if (g.failed)
                snprintf(why, why_cap, "%s", g.why);
            else
                snprintf(why, why_cap, "labeling failed at super-chunk %llu",
                         (unsigned long long)j);
            rc = -1;
            break;
        }
        j += (uint64_t)n;

        double t = now_s();
        if (t - t_last >= PROGRESS_INTERVAL_S || j == supers) {
            uint64_t done = j * super_bytes;
            printf("Session %u: %llu / %llu MiB (%.0f%%), %.1f MiB/s\n", init->session,
                   (unsigned long long)(done >> 20), (unsigned long long)(init->size >> 20),
                   100.0 * (double)done / (double)init->size,
                   (double)done / (1 << 20) / (t - t0 > 0 ? t - t0 : 1e-9));
            fflush(stdout);
            t_last = t;
        }
    }

    if (rc == 0 && g.next != init->base + init->size) {
        snprintf(why, why_cap, "internal: delivered %llu of %llu bytes",
                 (unsigned long long)(g.next - init->base), (unsigned long long)init->size);
        rc = -1;
    }
    pthread_cond_destroy(&g.cv);
    pthread_mutex_destroy(&g.mu);
    return rc;
}

void label_gen_release(void)
{
#ifdef TARGET_GPU
    if (g_gen) {
        pose_gpu_gen_close(g_gen);
        g_gen = NULL;
    }
#endif
    free_buffers();
    if (g_pool) {
        pose_graph_pool_destroy(g_pool);
        g_pool = NULL;
    }
}
