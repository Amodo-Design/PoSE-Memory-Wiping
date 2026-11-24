/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * gpu_session.cu — a device's labeled HBM buffer (gpu_session.h).
 *
 * The labeling itself is pose_graph_hbm_topo_label_ex (label_gpu_common.cu),
 * which produces labels byte-identical to the CPU in-place path; this file
 * only owns the buffer, the device context and the challenge read-back.
 */

#include "../../include/gpu_session.h"
#include "../../include/gpu_label.h"
#include "../../include/graph.h"

#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_devices = -1;   /* from pose_gpu_init; -1 = not initialised */

static void cuda_why(char *why, size_t why_cap, const char *what, cudaError_t e)
{
    if (why && why_cap)
        snprintf(why, why_cap, "%s: %s", what, cudaGetErrorString(e));
}

extern "C" int pose_gpu_init(char *why, size_t why_cap)
{
    int n = 0;
    cudaError_t e = cudaGetDeviceCount(&n);
    if (e == cudaErrorNoDevice) {
        g_devices = 0;
        return 0;
    }
    if (e != cudaSuccess) {
        cuda_why(why, why_cap, "cudaGetDeviceCount", e);
        return -1;
    }
    for (int d = 0; d < n; d++) {
        /* cudaFree(0) is the documented way to force context creation. */
        if ((e = cudaSetDevice(d)) != cudaSuccess || (e = cudaFree(0)) != cudaSuccess) {
            char what[64];
            snprintf(what, sizeof(what), "device %d: context creation", d);
            cuda_why(why, why_cap, what, e);
            return -1;
        }
    }
    g_devices = n;
    return n;
}

extern "C" int pose_gpu_info(int device, pose_gpu_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (g_devices < 0 || device < 0 || device >= g_devices)
        return -1;
    cudaDeviceProp p;
    if (cudaSetDevice(device) != cudaSuccess || cudaGetDeviceProperties(&p, device) != cudaSuccess)
        return -1;
    size_t free_b = 0, total_b = 0;
    if (cudaMemGetInfo(&free_b, &total_b) != cudaSuccess)
        return -1;
    out->device = device;
    snprintf(out->name, sizeof(out->name), "%s", p.name);
    snprintf(out->pci_bus_id, sizeof(out->pci_bus_id), "%04x:%02x:%02x.0",
             p.pciDomainID, p.pciBusID, p.pciDeviceID);
    out->total_bytes = total_b;
    out->free_bytes  = free_b;
    out->sm_count    = p.multiProcessorCount;
    return 0;
}

/* pose_gpu_scratch_bytes is plain C in src/gpu_scratch.c (the verifier's
 * planner needs it without CUDA). */

extern "C" int pose_gpu_session_alloc(pose_gpu_session_t *gs, int device, uint64_t bytes,
                                      char *why, size_t why_cap)
{
    memset(gs, 0, sizeof(*gs));
    if (g_devices < 0 || device < 0 || device >= g_devices) {
        snprintf(why, why_cap, "no such GPU device %d", device);
        return -1;
    }
    if (bytes == 0 || bytes % POSE_HASH_BYTES != 0) {
        snprintf(why, why_cap, "size must be a non-zero multiple of %d bytes", POSE_HASH_BYTES);
        return -1;
    }
    cudaError_t e;
    if ((e = cudaSetDevice(device)) != cudaSuccess) {
        cuda_why(why, why_cap, "cudaSetDevice", e);
        return -1;
    }
    size_t free_b = 0, total_b = 0;
    if ((e = cudaMemGetInfo(&free_b, &total_b)) != cudaSuccess) {
        cuda_why(why, why_cap, "cudaMemGetInfo", e);
        return -1;
    }
    if (bytes + POSE_GPU_HEADROOM_BYTES > free_b) {
        snprintf(why, why_cap, "device %d has %llu MiB free, less than %llu MiB + %llu MiB headroom",
                 device, (unsigned long long)(free_b >> 20), (unsigned long long)(bytes >> 20),
                 (unsigned long long)(POSE_GPU_HEADROOM_BYTES >> 20));
        return -1;
    }
    void *hbm = NULL;
    if ((e = cudaMalloc(&hbm, (size_t)bytes)) != cudaSuccess) {
        cuda_why(why, why_cap, "cudaMalloc", e);
        return -1;
    }
    gs->device       = device;
    gs->hbm          = hbm;
    gs->hbm_len      = bytes;
    gs->total_blocks = bytes / POSE_HASH_BYTES;
    return 0;
}

/* Scratch pools the first pass of an HBM session starts with (the labeler
 * caps it at the device's resident warps anyway). */
#define SESSION_START_INFLIGHT 1024ull

/* The labeler reports progress per pass; the session reports it over the
 * whole buffer. */
typedef struct {
    pose_gpu_progress_fn fn;
    void                *ctx;
    uint64_t             offset;   /* first super-chunk of the current pass */
    uint64_t             total;
} session_progress_t;

static void session_progress(void *vctx, uint64_t done, uint64_t num_super)
{
    (void)num_super;
    session_progress_t *s = (session_progress_t *)vctx;
    if (s->fn) s->fn(s->ctx, s->offset + done, s->total);
}

static int pose_gpu_quiet_env(void)
{
    const char *q = getenv("POSE_GPU_QUIET");
    return q && *q;
}

extern "C" int pose_gpu_session_label(pose_gpu_session_t *gs, const uint8_t *seed, size_t seed_len,
                                      uint32_t chunk_blocks, pose_hash_algo_t algo,
                                      pose_gpu_progress_fn progress, void *ctx,
                                      char *why, size_t why_cap)
{
    if (!gs->hbm) {
        snprintf(why, why_cap, "session has no HBM buffer");
        return -1;
    }
    if (algo != POSE_HASH_BLAKE3) {
        snprintf(why, why_cap, "only BLAKE3 is implemented on the GPU");
        return -1;
    }
    if (chunk_blocks == 0) chunk_blocks = POSE_CHUNK_BLOCKS;
    uint64_t super_bytes = pose_graph_super_chunk_blocks(chunk_blocks) * POSE_HASH_BYTES;
    if (gs->hbm_len % super_bytes != 0) {
        snprintf(why, why_cap, "buffer of %llu bytes is not a whole number of super-chunks (%llu bytes)",
                 (unsigned long long)gs->hbm_len, (unsigned long long)super_bytes);
        return -1;
    }
    cudaError_t e;
    if ((e = cudaSetDevice(gs->device)) != cudaSuccess) {
        cuda_why(why, why_cap, "cudaSetDevice", e);
        return -1;
    }
    size_t free_b = 0, total_b = 0;
    if (cudaMemGetInfo(&free_b, &total_b) == cudaSuccess) {
        uint64_t need = pose_gpu_scratch_bytes(chunk_blocks, 1) + POSE_GPU_HEADROOM_BYTES;
        if (free_b < need) {
            snprintf(why, why_cap, "device %d has %llu MiB free for labeling scratch, needs %llu MiB "
                     "(chunk_blocks %u): the session is too large for this GPU",
                     gs->device, (unsigned long long)(free_b >> 20), (unsigned long long)(need >> 20),
                     chunk_blocks);
            return -1;
        }
    }

    pose_hbm_topo_t *topo = pose_graph_hbm_topo_build(chunk_blocks, algo);
    if (!topo) {
        snprintf(why, why_cap, "cannot build the scaffold topology for chunk_blocks %u on device %d",
                 chunk_blocks, gs->device);
        return -1;
    }

    /* Geometric passes over the buffer, the unlabeled tail serving as scratch
     * (see gpu_session.h).  base 0: chunk k of the buffer is keyed
     * (0 / 32) / chunk_blocks + k, and each pass starts at super-chunk lo, so
     * its region_block_offset is lo * chunk_blocks. */
    const uint64_t N          = gs->hbm_len / super_bytes;
    uint8_t       *buf        = (uint8_t *)gs->hbm;
    session_progress_t sp     = { progress, ctx, 0, N };
    uint64_t p  = N / 4 < SESSION_START_INFLIGHT ? N / 4 : SESSION_START_INFLIGHT;
    uint64_t lo = 0;
    int      rc = 0;
    int      pass = 0;
    while (lo < N && rc == 0) {
        uint64_t left = N - lo;
        /* scratch = the last 2p super-chunks, leaving at least one to label */
        if (p > 0 && 2 * p >= left) p = (left - 1) / 2;
        uint64_t count = p > 0 ? left - 2 * p : left;
        void    *scratch = p > 0 ? buf + (N - 2 * p) * super_bytes : NULL;
        size_t   scratch_bytes = p > 0 ? (size_t)(2 * p * super_bytes) : 0;
        if (!pose_gpu_quiet_env()) {
            size_t fb = 0, tb = 0;
            cudaMemGetInfo(&fb, &tb);
            printf("  GPU session pass %d: super-chunks %llu-%llu with %llu pools%s; %zu MiB HBM free\n", pass++,
                   (unsigned long long)lo, (unsigned long long)(lo + count - 1),
                   (unsigned long long)(p > 0 ? p : 1), p > 0 ? " in the unlabeled tail" : " from free HBM",
                   fb >> 20);
        }
        sp.offset = lo;
        rc = pose_graph_hbm_topo_label_in(topo, seed, seed_len, buf + lo * super_bytes,
                                          (size_t)(count * super_bytes), lo * chunk_blocks,
                                          scratch, scratch_bytes, session_progress, &sp);
        lo += count;
        p  /= 2;
    }
    pose_graph_hbm_topo_free(topo);
    if (!pose_gpu_quiet_env()) {
        size_t fb = 0, tb = 0;
        cudaMemGetInfo(&fb, &tb);
        printf("  GPU session labeled: %zu MiB HBM free beside the %llu MiB buffer\n", fb >> 20,
               (unsigned long long)(gs->hbm_len >> 20));
    }
    if (rc != 0) {
        e = cudaGetLastError();
        snprintf(why, why_cap, "GPU labeling failed on device %d at super-chunk %llu%s%s", gs->device,
                 (unsigned long long)lo, e != cudaSuccess ? ": " : "",
                 e != cudaSuccess ? cudaGetErrorString(e) : "");
        return -1;
    }
    gs->chunk_blocks = chunk_blocks;
    return 0;
}

extern "C" int pose_gpu_session_lookup(const pose_gpu_session_t *gs, uint64_t block_index,
                                       uint8_t label[POSE_HASH_BYTES])
{
    if (!gs->hbm || block_index >= gs->total_blocks)
        return -1;
    if (cudaSetDevice(gs->device) != cudaSuccess)
        return -1;
    if (cudaMemcpy(label, (const uint8_t *)gs->hbm + block_index * POSE_HASH_BYTES,
                   POSE_HASH_BYTES, cudaMemcpyDeviceToHost) != cudaSuccess)
        return -1;
    return 0;
}

extern "C" void pose_gpu_session_free(pose_gpu_session_t *gs)
{
    if (gs->hbm) {
        cudaSetDevice(gs->device);
        cudaFree(gs->hbm);
    }
    memset(gs, 0, sizeof(*gs));
}

/* ── label generator ─────────────────────────────────────────────────────── */

#include <pthread.h>

struct pose_gpu_gen {
    int              device;
    uint32_t         cb;
    uint64_t         super_bytes;
    uint64_t         window_supers;
    uint64_t         slice_bytes;
    pose_hbm_topo_t *topo;
    void            *win[2];
    uint8_t         *slice;        /* pinned */
    cudaStream_t     copy;         /* non-blocking: D2H overlaps the labeler's kernels */

    /* labeling thread <-> caller, per run */
    pthread_mutex_t  mu;
    pthread_cond_t   cv;
    int              labeled[2];   /* window holds a labeled batch, not yet drained */
    uint64_t         batch_first[2], batch_count[2];
    int              stop;         /* caller asks the thread to quit */
    int              failed;       /* the thread hit a CUDA error */
    const uint8_t   *seed;
    size_t           seed_len;
    uint64_t         base_blocks;  /* base / 32 */
    uint64_t         first, count; /* the run */
};

extern "C" pose_gpu_gen_t *pose_gpu_gen_open(int device, uint32_t chunk_blocks, uint64_t window_bytes,
                                             uint64_t slice_bytes, char *why, size_t why_cap)
{
    if (g_devices < 0 || device < 0 || device >= g_devices) {
        snprintf(why, why_cap, "no such GPU device %d", device);
        return NULL;
    }
    if (chunk_blocks == 0) chunk_blocks = POSE_CHUNK_BLOCKS;
    uint64_t super_bytes = pose_graph_super_chunk_blocks(chunk_blocks) * POSE_HASH_BYTES;
    uint64_t supers = window_bytes / super_bytes;
    if (supers == 0) supers = 1;
    uint64_t slice = slice_bytes - slice_bytes % super_bytes;
    if (slice == 0) slice = super_bytes;
    if (slice > supers * super_bytes) slice = supers * super_bytes;

    pose_gpu_gen_t *g = (pose_gpu_gen_t *)calloc(1, sizeof(*g));
    if (!g) {
        snprintf(why, why_cap, "out of memory");
        return NULL;
    }
    g->device = device;
    g->cb = chunk_blocks;
    g->super_bytes = super_bytes;
    g->window_supers = supers;
    g->slice_bytes = slice;
    pthread_mutex_init(&g->mu, NULL);
    pthread_cond_init(&g->cv, NULL);

    cudaError_t e;
    if ((e = cudaSetDevice(device)) != cudaSuccess) {
        cuda_why(why, why_cap, "cudaSetDevice", e);
        goto fail;
    }
    for (int i = 0; i < 2; i++) {
        if ((e = cudaMalloc(&g->win[i], (size_t)(supers * super_bytes))) != cudaSuccess) {
            snprintf(why, why_cap, "cudaMalloc(%llu MiB label window %d): %s",
                     (unsigned long long)((supers * super_bytes) >> 20), i, cudaGetErrorString(e));
            goto fail;
        }
    }
    if ((e = cudaMallocHost((void **)&g->slice, (size_t)slice)) != cudaSuccess) {
        snprintf(why, why_cap, "cudaMallocHost(%llu MiB slice): %s",
                 (unsigned long long)(slice >> 20), cudaGetErrorString(e));
        goto fail;
    }
    if ((e = cudaStreamCreateWithFlags(&g->copy, cudaStreamNonBlocking)) != cudaSuccess) {
        cuda_why(why, why_cap, "cudaStreamCreate", e);
        goto fail;
    }
    g->topo = pose_graph_hbm_topo_build(chunk_blocks, POSE_HASH_BLAKE3);
    if (!g->topo) {
        snprintf(why, why_cap, "cannot build the scaffold topology for chunk_blocks %u on device %d",
                 chunk_blocks, device);
        goto fail;
    }
    return g;
fail:
    pose_gpu_gen_close(g);
    return NULL;
}

extern "C" uint64_t pose_gpu_gen_window_supers(const pose_gpu_gen_t *g) { return g->window_supers; }
extern "C" uint64_t pose_gpu_gen_slice_bytes(const pose_gpu_gen_t *g)   { return g->slice_bytes; }

/* The labeling thread: fills windows 0, 1, 0, 1, ... with consecutive
 * batches of the run, waiting for each to be drained before reusing it. */
static void *gen_thread(void *arg)
{
    pose_gpu_gen_t *g = (pose_gpu_gen_t *)arg;
    cudaSetDevice(g->device);
    uint64_t next = g->first, end = g->first + g->count;
    int w = 0;
    while (next < end) {
        pthread_mutex_lock(&g->mu);
        while (!g->stop && g->labeled[w])
            pthread_cond_wait(&g->cv, &g->mu);
        int stop = g->stop;
        pthread_mutex_unlock(&g->mu);
        if (stop)
            return NULL;
        uint64_t n = end - next < g->window_supers ? end - next : g->window_supers;
        /* region_block_offset = base/32 + j*cb: chunk index (base/32)/cb + j */
        int rc = pose_graph_hbm_topo_label(g->topo, g->seed, g->seed_len, g->win[w],
                                           (size_t)(n * g->super_bytes),
                                           g->base_blocks + next * g->cb);
        pthread_mutex_lock(&g->mu);
        if (rc != 0) {
            g->failed = 1;
        } else {
            g->batch_first[w] = next;
            g->batch_count[w] = n;
            g->labeled[w] = 1;
        }
        pthread_cond_broadcast(&g->cv);
        pthread_mutex_unlock(&g->mu);
        if (rc != 0)
            return NULL;
        next += n;
        w ^= 1;
    }
    return NULL;
}

extern "C" int pose_gpu_gen_run(pose_gpu_gen_t *g, const uint8_t *seed, size_t seed_len, uint64_t base,
                                uint64_t first, uint64_t count, pose_gpu_gen_sink_fn sink, void *ctx,
                                char *why, size_t why_cap)
{
    if (count == 0)
        return 0;
    if (base % POSE_HASH_BYTES != 0) {
        snprintf(why, why_cap, "base is not a multiple of %d", POSE_HASH_BYTES);
        return -1;
    }
    cudaError_t e;
    if ((e = cudaSetDevice(g->device)) != cudaSuccess) {
        cuda_why(why, why_cap, "cudaSetDevice", e);
        return -1;
    }
    g->seed = seed; g->seed_len = seed_len;
    g->base_blocks = base / POSE_HASH_BYTES;
    g->first = first; g->count = count;
    g->labeled[0] = g->labeled[1] = 0;
    g->stop = g->failed = 0;

    pthread_t th;
    if (pthread_create(&th, NULL, gen_thread, g) != 0) {
        snprintf(why, why_cap, "cannot start the GPU labeling thread");
        return -1;
    }

    int rc = 0;
    uint64_t next = first, end = first + count;
    int w = 0;
    while (next < end && rc == 0) {
        pthread_mutex_lock(&g->mu);
        while (!g->failed && !g->labeled[w])
            pthread_cond_wait(&g->cv, &g->mu);
        int failed = g->failed;
        pthread_mutex_unlock(&g->mu);
        if (failed) {
            e = cudaGetLastError();
            snprintf(why, why_cap, "GPU labeling failed at super-chunk %llu on device %d%s%s",
                     (unsigned long long)next, g->device,
                     e != cudaSuccess ? ": " : "", e != cudaSuccess ? cudaGetErrorString(e) : "");
            rc = -1;
            break;
        }
        /* drain window w in slices, delivering as we go */
        uint64_t n = g->batch_count[w];
        for (uint64_t done = 0; done < n && rc == 0; ) {
            uint64_t take = g->slice_bytes / g->super_bytes;
            if (take > n - done) take = n - done;
            size_t bytes = (size_t)(take * g->super_bytes);
            if ((e = cudaMemcpyAsync(g->slice, (const uint8_t *)g->win[w] + done * g->super_bytes, bytes,
                                     cudaMemcpyDeviceToHost, g->copy)) != cudaSuccess ||
                (e = cudaStreamSynchronize(g->copy)) != cudaSuccess) {
                cuda_why(why, why_cap, "copying labels to the host", e);
                rc = -1;
                break;
            }
            if (sink(ctx, (next + done) * g->super_bytes, g->slice, bytes) != 0) {
                snprintf(why, why_cap, "delivery refused");
                rc = -1;
                break;
            }
            done += take;
        }
        pthread_mutex_lock(&g->mu);
        g->labeled[w] = 0;
        pthread_cond_broadcast(&g->cv);
        pthread_mutex_unlock(&g->mu);
        next += n;
        w ^= 1;
    }
    pthread_mutex_lock(&g->mu);
    g->stop = 1;
    pthread_cond_broadcast(&g->cv);
    pthread_mutex_unlock(&g->mu);
    pthread_join(th, NULL);
    return rc;
}

extern "C" void pose_gpu_gen_close(pose_gpu_gen_t *g)
{
    if (!g) return;
    cudaSetDevice(g->device);
    for (int i = 0; i < 2; i++) {
        if (g->win[i]) {
            cudaMemset(g->win[i], 0, (size_t)(g->window_supers * g->super_bytes));
            cudaFree(g->win[i]);
        }
    }
    if (g->slice) {
        memset(g->slice, 0, (size_t)g->slice_bytes);
        cudaFreeHost(g->slice);
    }
    if (g->copy) cudaStreamDestroy(g->copy);
    if (g->topo) pose_graph_hbm_topo_free(g->topo);
    pthread_cond_destroy(&g->cv);
    pthread_mutex_destroy(&g->mu);
    free(g);
}

extern "C" int pose_gpu_parity_check(int device, const uint8_t *seed, size_t seed_len,
                                     uint32_t chunk_blocks, const uint8_t *cpu_ref,
                                     size_t super_bytes, size_t *first_diff)
{
    if (cudaSetDevice(device) != cudaSuccess)
        return -1;
    void    *hbm = NULL;
    uint8_t *got = NULL;
    int      rc  = -1;
    if (cudaMalloc(&hbm, super_bytes) != cudaSuccess)
        return -1;
    got = (uint8_t *)malloc(super_bytes);
    if (!got)
        goto out;
    if (pose_graph_label_hbm(seed, seed_len, chunk_blocks, hbm, super_bytes,
                             /*region_block_offset=*/0, POSE_HASH_BLAKE3) != 0)
        goto out;
    if (cudaMemcpy(got, hbm, super_bytes, cudaMemcpyDeviceToHost) != cudaSuccess)
        goto out;
    rc = 0;
    for (size_t i = 0; i < super_bytes; i++) {
        if (got[i] != cpu_ref[i]) {
            if (first_diff) *first_diff = i;
            rc = 1;
            break;
        }
    }
out:
    free(got);
    cudaFree(hbm);
    return rc;
}
