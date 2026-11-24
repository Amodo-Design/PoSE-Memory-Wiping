/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * test_gpu_parity — the GPU <-> CPU label parity gate (gpu_session.h).
 *
 * Needs a CUDA device; without one it prints why and exits 77 (ctest "skip").
 *
 *   1. Node known-answer test: one source node and one internal node hashed
 *      on the GPU (pose_gpu_label_node_single) and on the CPU
 *      (pose_label_node) must agree.
 *   2. Super-chunk parity: one whole super-chunk labeled on the GPU
 *      (pose_gpu_parity_check) must equal the CPU in-place labeler's output
 *      for the same seed, at the default chunk_blocks and at a small one.
 *   3. Session round trip: a multi-super-chunk HBM session labeled through the
 *      session API, then sampled challenges read back with
 *      pose_gpu_session_lookup must equal pose_graph_label_chunk_inplace, the
 *      verifier's own recomputation, and the progress hook must have reported
 *      every super-chunk.
 *
 * Usage: test_gpu_parity [SESSION_MIB [CHUNK_BLOCKS]] (default 64, 4096)
 */
#include "gpu_session.h"
#include "gpu_label.h"
#include "graph.h"
#include "hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t SEED[32] = {
    0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe, 0xba, 0xbe, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0xfe, 0xed, 0xfa, 0xce, 0x0b, 0xad, 0xf0, 0x0d, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11,
};

static int g_fail;

static void result(const char *what, int ok, const char *detail)
{
    printf("  %-44s %s%s%s\n", what, ok ? "PASS" : "FAIL", detail ? "  " : "", detail ? detail : "");
    if (!ok) g_fail++;
}

static void node_kat(const uint32_t key_src[8], const uint32_t key_int[8], const uint8_t desc[32])
{
    uint8_t pred0[32], pred1[32];
    for (int i = 0; i < 32; i++) { pred0[i] = (uint8_t)i; pred1[i] = (uint8_t)(255 - i); }
    struct { int is_src; uint64_t idx; const uint8_t *p0, *p1; const char *name; } cases[] = {
        { 1, 0, NULL,  NULL,  "node KAT: source node" },
        { 0, 5, pred0, pred1, "node KAT: internal node" },
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        uint8_t cpu[32], gpu[32];
        pose_label_node(cpu, cases[c].is_src, cases[c].idx, cases[c].p0, cases[c].p1,
                        SEED, desc, POSE_HASH_BLAKE3);
        if (pose_gpu_label_node_single(gpu, cases[c].is_src, cases[c].idx, cases[c].p0,
                                       cases[c].p1, SEED, desc, key_src, key_int) != 0) {
            result(cases[c].name, 0, "GPU call failed");
            continue;
        }
        result(cases[c].name, memcmp(cpu, gpu, 32) == 0, NULL);
    }
}

static void super_chunk_parity(uint32_t cb, uint8_t *scratch_unused)
{
    (void)scratch_unused;
    size_t   super_bytes = (size_t)pose_graph_super_chunk_blocks(cb) * POSE_HASH_BYTES;
    uint8_t *ref         = malloc(super_bytes);
    uint8_t *scratch     = malloc(pose_graph_scratch_bytes_inplace(cb));
    char     what[64], detail[96] = "";
    snprintf(what, sizeof(what), "super-chunk parity, chunk_blocks %u (%zu KiB)", cb, super_bytes >> 10);
    int ok = 0;
    if (!ref || !scratch) {
        snprintf(detail, sizeof(detail), "out of memory");
    } else if (pose_graph_label_chunk_inplace(SEED, sizeof(SEED), /*chunk_index=*/0, ref, scratch, POSE_HASH_BLAKE3, cb) != 0) {
        snprintf(detail, sizeof(detail), "CPU labeling failed");
    } else {
        size_t diff = 0;
        int rc = pose_gpu_parity_check(0, SEED, sizeof(SEED), cb, ref, super_bytes, &diff);
        if (rc == 0) ok = 1;
        else if (rc == 1) snprintf(detail, sizeof(detail), "first difference at byte %zu (block %zu)", diff, diff / 32);
        else snprintf(detail, sizeof(detail), "GPU labeling or CUDA error");
    }
    result(what, ok, detail[0] ? detail : NULL);
    free(ref);
    free(scratch);
}

typedef struct { uint64_t calls, last_done, num_super, monotonic; } progress_t;

static void on_progress(void *ctx, uint64_t done, uint64_t num_super)
{
    progress_t *p = ctx;
    p->calls++;
    if (done < p->last_done) p->monotonic = 0;
    p->last_done  = done;
    p->num_super  = num_super;
}

static void session_round_trip(uint32_t cb, uint64_t mib)
{
    char why[256], what[96], detail[160] = "";
    uint64_t super_bytes = pose_graph_super_chunk_blocks(cb) * POSE_HASH_BYTES;
    uint64_t bytes       = (mib << 20) - (mib << 20) % super_bytes;
    uint64_t num_super   = bytes / super_bytes;
    snprintf(what, sizeof(what), "session round trip, %llu MiB, %llu super-chunks",
             (unsigned long long)(bytes >> 20), (unsigned long long)num_super);

    pose_gpu_session_t gs;
    {
        pose_gpu_info_t info;
        if (pose_gpu_info(0, &info) == 0)
            printf("  session: %llu MiB free before alloc; reserve(cb %u, 1 pool) = %llu MiB\n",
                   (unsigned long long)(info.free_bytes >> 20), cb,
                   (unsigned long long)(pose_gpu_scratch_bytes(cb, 1) >> 20));
    }
    if (pose_gpu_session_alloc(&gs, 0, bytes, why, sizeof(why)) != 0) {
        result(what, 0, why);
        return;
    }
    progress_t prog = { 0, 0, 0, 1 };
    if (pose_gpu_session_label(&gs, SEED, sizeof(SEED), cb, POSE_HASH_BLAKE3, on_progress, &prog,
                               why, sizeof(why)) != 0) {
        result(what, 0, why);
        pose_gpu_session_free(&gs);
        return;
    }

    /* Challenge every super-chunk once at a varying rank, the first and last
     * included, capped at 64 recomputations. */
    uint8_t *ref     = malloc((size_t)super_bytes);
    uint8_t *scratch = malloc(pose_graph_scratch_bytes_inplace(cb));
    uint64_t samples = num_super < 64 ? num_super : 64, mism = 0, errs = 0;
    for (uint64_t s = 0; ref && scratch && s < samples; s++) {
        uint64_t chunk = samples <= 1 ? 0 : (s * (num_super - 1)) / (samples - 1);
        uint64_t rank  = (chunk * 1103515245ull + 12345ull) % cb;
        uint8_t  got[32];
        if (pose_gpu_session_lookup(&gs, chunk * cb + rank, got) != 0) { errs++; continue; }
        if (pose_graph_label_chunk_inplace(SEED, sizeof(SEED), chunk, ref, scratch, POSE_HASH_BLAKE3, cb) != 0) { errs++; continue; }
        if (memcmp(got, ref + rank * 32, 32) != 0) mism++;
    }
    int ok = ref && scratch && mism == 0 && errs == 0;
    if (!ref || !scratch) snprintf(detail, sizeof(detail), "out of memory");
    else snprintf(detail, sizeof(detail), "%llu/%llu sampled labels match pose_graph_label_chunk_inplace",
                  (unsigned long long)(samples - mism - errs), (unsigned long long)samples);
    result(what, ok, detail);

    uint8_t past[32];
    result("lookup past the end is refused", pose_gpu_session_lookup(&gs, gs.total_blocks, past) != 0, NULL);
    snprintf(detail, sizeof(detail), "%llu calls, last %llu of %llu", (unsigned long long)prog.calls,
             (unsigned long long)prog.last_done, (unsigned long long)prog.num_super);
    result("progress hook reported every super-chunk",
           prog.calls > 0 && prog.monotonic && prog.last_done == num_super && prog.num_super == num_super,
           detail);
    free(ref);
    free(scratch);
    pose_gpu_session_free(&gs);
}

int main(int argc, char **argv)
{
    uint64_t mib = argc > 1 ? strtoull(argv[1], NULL, 0) : 64;
    uint32_t session_cb = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 4096;
    char why[256];
    int n = pose_gpu_init(why, sizeof(why));
    if (n < 0) {
        printf("no usable CUDA device: %s\nSKIP\n", why);
        return 77;
    }
    if (n == 0) {
        printf("no CUDA device present\nSKIP\n");
        return 77;
    }
    for (int d = 0; d < n; d++) {
        pose_gpu_info_t info;
        if (pose_gpu_info(d, &info) == 0)
            printf("gpu%d: %s at %s, %llu MiB total, %llu MiB free, %d SMs\n", d, info.name,
                   info.pci_bus_id, (unsigned long long)(info.total_bytes >> 20),
                   (unsigned long long)(info.free_bytes >> 20), info.sm_count);
    }
    printf("scratch for chunk_blocks 4096 x 1024 in flight: %llu MiB; 2^18 x 1024: %llu MiB\n",
           (unsigned long long)(pose_gpu_scratch_bytes(4096, 1024) >> 20),
           (unsigned long long)(pose_gpu_scratch_bytes(1u << 18, 1024) >> 20));

    uint32_t key_src[8], key_int[8];
    uint8_t  desc[32];
    pose_label_node_keys(key_src, key_int);
    pose_graph_descriptor(desc, POSE_CHUNK_BLOCKS, (uint64_t)pose_graph_parameter_n(POSE_CHUNK_BLOCKS));
    node_kat(key_src, key_int, desc);
    super_chunk_parity(POSE_CHUNK_BLOCKS, NULL);
    super_chunk_parity(1024, NULL);
    session_round_trip(session_cb, mib);

    printf("%s\n", g_fail ? "FAILED" : "PASSED");
    return g_fail ? 1 : 0;
}
