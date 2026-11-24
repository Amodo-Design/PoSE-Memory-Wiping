/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * pose-label — print the expected label(s) for blocks of a session.
 *
 *   pose-label --seed HEX64 [--algo blake3|sha256|aes_prf] [--chunk-blocks N]
 *              --base PHYS_BASE INDEX [COUNT]
 *
 * Prints COUNT (default 1) labels, one lowercase hex line each, for the
 * session-relative block indices INDEX .. INDEX+COUNT-1 of a session whose
 * SessionInit carried this seed, algo, chunk_blocks and base.  A block is one
 * 32-byte label; block i of the session is stored at byte base + 32*i.
 *
 * The result is the verifier's own (pose-verifier/src/labels.rs, expected();
 * its test matches_region_labeler checks the two agree), so a label printed
 * here is exactly what a challenge for that block must return:
 *
 *   global_chunk = (base / 32) / chunk_blocks + INDEX / chunk_blocks
 *   labels       = pose_graph_label_region(seed, chunk_blocks blocks,
 *                                          region_block_offset =
 *                                            global_chunk * chunk_blocks)
 *   label        = labels[INDEX % chunk_blocks]
 *
 * pose_graph_label_region derives the chunk's own seed with pose_chunk_seed()
 * from that offset, which is how the base address enters the label.  The
 * protocol requires session sizes to be whole super-chunks, so every chunk is
 * a full chunk_blocks long.  Consecutive indices in the same chunk reuse one
 * labeling pass.
 *
 * chunk_blocks 0 means the library default (POSE_CHUNK_BLOCKS), as on the
 * wire.  Labeling a chunk costs the full scaffold, so large chunk_blocks
 * (2^17 and up) take seconds and hundreds of MB per chunk, as for the
 * verifier.
 */
#include "graph.h"
#include "hash.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *f)
{
    fprintf(f,
        "usage: pose-label --seed HEX64 [--algo blake3|sha256|aes_prf] [--chunk-blocks N]\n"
        "                  --base PHYS_BASE INDEX [COUNT]\n"
        "Prints the expected 32-byte label of session blocks INDEX..INDEX+COUNT-1\n"
        "(COUNT default 1), one hex line each, computed as the verifier does.\n"
        "Numbers accept 0x hex.  --chunk-blocks 0 (default) = %u.\n",
        (unsigned)POSE_CHUNK_BLOCKS);
}

static int parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno || !end || end == s || *end || s[0] == '-')
        return -1;
    *out = (uint64_t)v;
    return 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_seed(const char *s, uint8_t out[POSE_HASH_BYTES])
{
    if (strlen(s) != 2 * POSE_HASH_BYTES)
        return -1;
    for (int i = 0; i < POSE_HASH_BYTES; i++) {
        int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}

static int parse_algo(const char *s, pose_hash_algo_t *out)
{
    if (strcmp(s, "blake3") == 0)  { *out = POSE_HASH_BLAKE3;  return 0; }
    if (strcmp(s, "sha256") == 0)  { *out = POSE_HASH_SHA256;  return 0; }
    if (strcmp(s, "aes_prf") == 0) { *out = POSE_HASH_AES_PRF; return 0; }
    return -1;
}

int main(int argc, char **argv)
{
    uint8_t seed[POSE_HASH_BYTES];
    int have_seed = 0, have_base = 0;
    pose_hash_algo_t algo = POSE_HASH_BLAKE3;
    uint64_t cb = 0, base = 0, index = 0, count = 1;
    const char *pos[2];
    int npos = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(a, "--seed") == 0 && v) {
            if (parse_seed(v, seed)) { fprintf(stderr, "pose-label: --seed must be %d hex digits\n", 2 * POSE_HASH_BYTES); return 2; }
            have_seed = 1; i++;
        } else if (strcmp(a, "--algo") == 0 && v) {
            if (parse_algo(v, &algo)) { fprintf(stderr, "pose-label: --algo must be blake3, sha256 or aes_prf\n"); return 2; }
            i++;
        } else if (strcmp(a, "--chunk-blocks") == 0 && v) {
            if (parse_u64(v, &cb)) { fprintf(stderr, "pose-label: bad --chunk-blocks '%s'\n", v); return 2; }
            i++;
        } else if (strcmp(a, "--base") == 0 && v) {
            if (parse_u64(v, &base)) { fprintf(stderr, "pose-label: bad --base '%s'\n", v); return 2; }
            have_base = 1; i++;
        } else if (a[0] == '-' && a[1] == '-') {
            fprintf(stderr, "pose-label: unknown or incomplete option '%s'\n", a);
            usage(stderr);
            return 2;
        } else if (npos < 2) {
            pos[npos++] = a;
        } else {
            usage(stderr);
            return 2;
        }
    }

    if (!have_seed || !have_base || npos < 1) {
        usage(stderr);
        return 2;
    }
    if (parse_u64(pos[0], &index)) { fprintf(stderr, "pose-label: bad INDEX '%s'\n", pos[0]); return 2; }
    if (npos == 2 && (parse_u64(pos[1], &count) || count == 0)) {
        fprintf(stderr, "pose-label: bad COUNT '%s'\n", pos[1]);
        return 2;
    }

    if (cb == 0)
        cb = POSE_CHUNK_BLOCKS;
    if (cb > POSE_GRAPH_MAX_M || (cb & (cb - 1))) {
        fprintf(stderr, "pose-label: --chunk-blocks must be a power of two in [1, %u]\n",
                (unsigned)POSE_GRAPH_MAX_M);
        return 2;
    }
    if (base % POSE_HASH_BYTES)
        fprintf(stderr, "pose-label: warning: base 0x%llx is not a multiple of %d\n",
                (unsigned long long)base, POSE_HASH_BYTES);

    size_t chunk_bytes = (size_t)cb * POSE_HASH_BYTES;
    uint8_t *labels = malloc(chunk_bytes);
    if (!labels) {
        fprintf(stderr, "pose-label: out of memory (%zu bytes)\n", chunk_bytes);
        return 1;
    }

    const uint64_t first_global_chunk = (base / POSE_HASH_BYTES) / cb;
    uint64_t have_chunk = UINT64_MAX;

    for (uint64_t k = 0; k < count; k++) {
        uint64_t idx = index + k;
        uint64_t chunk = first_global_chunk + idx / cb;
        if (chunk != have_chunk) {
            if (pose_graph_label_region(seed, POSE_HASH_BYTES, labels, chunk_bytes,
                                        chunk * cb, algo, (uint32_t)cb) != 0) {
                fprintf(stderr, "pose-label: pose_graph_label_region failed for chunk %llu\n",
                        (unsigned long long)chunk);
                free(labels);
                return 1;
            }
            have_chunk = chunk;
        }
        const uint8_t *l = labels + (size_t)(idx % cb) * POSE_HASH_BYTES;
        for (int i = 0; i < POSE_HASH_BYTES; i++)
            printf("%02x", l[i]);
        putchar('\n');
    }

    free(labels);
    return ferror(stdout) ? 1 : 0;
}
