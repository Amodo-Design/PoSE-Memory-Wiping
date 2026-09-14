/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#include "../include/hash.h"
#include "../include/sha256.h"
#include "../include/aes_prf.h"
#include "../vendor/blake3/blake3.h"
#include "../vendor/blake3/blake3_impl.h"

#include <stdatomic.h>
#include <string.h>

/*
 * Write one 32-byte label to dst using a regular cached store.
 *
 * STNP was previously used here with the rationale that it avoids write-
 * allocate, preventing new labels from evicting parent labels from L2.  In
 * practice the scratch buffer is 40 MB (320× the 128 KB L2), so parent labels
 * are never in L2 regardless — the write-allocate bypass has no benefit.
 * Worse, on some ARM cores STNP applies a "streaming" eviction hint that causes
 * recently-written scratch labels to be expelled from L2 sooner than normal,
 * increasing DRAM reads on subsequent predecessor lookups (visible as excess
 * LLC read misses per chunk under cachegrind).
 *
 * Regular stores give scratch writes normal LRU priority in L2.  Recently-
 * written labels stay resident long enough to be read as predecessors in the
 * same graph traversal, reducing DRAM traffic.
 */
static inline void store_label(uint8_t *dst, const uint8_t *src)
{
    memcpy(dst, src, POSE_HASH_BYTES);
}

/*
 * Precomputed key material stored as uint32_t[8] — the word format that
 * blake3_hash_many expects directly.
 *
 * blake3_hasher_init_derive_key stores the derived key in hasher.key[8]
 * (uint32_t[8], little-endian words), identical to what blake3_hash_many
 * accepts.  We derive once at first use and cache.
 */
static uint32_t g_key_src[8]; /* "pose-db/label/src" */
static uint32_t g_key_int[8]; /* "pose-db/label/int" */
static atomic_int g_keys_ready = 0;

static void ensure_keys(void)
{
    if (atomic_load_explicit(&g_keys_ready, memory_order_acquire))
        return;

    blake3_hasher h;

    blake3_hasher_init_derive_key(&h, "pose-db/label/src");
    memcpy(g_key_src, h.key, sizeof(g_key_src));

    blake3_hasher_init_derive_key(&h, "pose-db/label/int");
    memcpy(g_key_int, h.key, sizeof(g_key_int));

    atomic_store_explicit(&g_keys_ready, 1, memory_order_release);
}

void pose_label_node_keys(uint32_t key_src[8], uint32_t key_int[8])
{
    ensure_keys();
    memcpy(key_src, g_key_src, sizeof(g_key_src));
    memcpy(key_int, g_key_int, sizeof(g_key_int));
}

/* ── 192-byte node message layout ─────────────────────────────────────────── */

#define NODE_MSG_BYTES (3 * BLAKE3_BLOCK_LEN)   /* 192 */

/*
 * Build the 192-byte message for one node:
 *
 *   Block 0 [ 0.. 63]: node_index (8 B big-endian) | zeros (56 B)
 *   Block 1 [64..127]: pred0 (32 B) | pred1 (32 B)  — zero if NULL
 *   Block 2 [128..191]: seed (32 B) | descriptor (32 B)
 */
static void build_node_msg(uint8_t msg[NODE_MSG_BYTES],
                           uint64_t node_index,
                           const uint8_t *pred0,
                           const uint8_t *pred1,
                           const uint8_t *seed,
                           const uint8_t descriptor[POSE_HASH_BYTES])
{
    memset(msg, 0, NODE_MSG_BYTES);

    /* Block 0: node_index big-endian at offset 0 */
    msg[0] = (uint8_t)(node_index >> 56);
    msg[1] = (uint8_t)(node_index >> 48);
    msg[2] = (uint8_t)(node_index >> 40);
    msg[3] = (uint8_t)(node_index >> 32);
    msg[4] = (uint8_t)(node_index >> 24);
    msg[5] = (uint8_t)(node_index >> 16);
    msg[6] = (uint8_t)(node_index >>  8);
    msg[7] = (uint8_t)(node_index);

    /* Block 1: pred0 | pred1 (zero if absent) */
    if (pred0) memcpy(msg + 64, pred0, POSE_HASH_BYTES);
    if (pred1) memcpy(msg + 96, pred1, POSE_HASH_BYTES);

    /* Block 2: seed | descriptor */
    memcpy(msg + 128, seed,       POSE_HASH_BYTES);
    memcpy(msg + 160, descriptor, POSE_HASH_BYTES);
}

/* ── label derivation ─────────────────────────────────────────────────────── */

void pose_label_node(uint8_t out[POSE_HASH_BYTES],
                     int is_source,
                     uint64_t node_index,
                     const uint8_t *pred0,
                     const uint8_t *pred1,
                     const uint8_t *seed,
                     const uint8_t descriptor[POSE_HASH_BYTES],
                     pose_hash_algo_t algo)
{
    uint8_t tmp[POSE_HASH_BYTES];
    if (algo == POSE_HASH_AES_PRF) {
        const uint64_t idx = node_index;
        const uint8_t *p0 = is_source ? NULL : pred0;
        const uint8_t *p1 = is_source ? NULL : pred1;
        pose_aes_prf_many(1, tmp, is_source, &idx, &p0, &p1, seed, descriptor);
    } else {
        uint8_t msg[NODE_MSG_BYTES];
        build_node_msg(msg, node_index,
                       is_source ? NULL : pred0,
                       is_source ? NULL : pred1,
                       seed, descriptor);
        if (algo == POSE_HASH_SHA256) {
            pose_sha256(msg, NODE_MSG_BYTES, tmp);
        } else {
            ensure_keys();
            const uint8_t *inputs[1] = { msg };
            blake3_hash_many(inputs, 1, 3,
                             is_source ? g_key_src : g_key_int,
                             0, false,
                             KEYED_HASH, CHUNK_START, CHUNK_END | ROOT,
                             tmp);
        }
    }
    store_label(out, tmp);
}

void pose_label_node_many(size_t n,
                          int is_source,
                          const uint64_t *node_indices,
                          const uint8_t *const *pred0s,
                          const uint8_t *const *pred1s,
                          const uint8_t *seed,
                          const uint8_t descriptor[POSE_HASH_BYTES],
                          uint8_t *outs,
                          pose_hash_algo_t algo)
{
    uint8_t tmp[POSE_LABEL_BATCH * POSE_HASH_BYTES];

    if (algo == POSE_HASH_AES_PRF) {
        /* The AES-CE kernel runs exactly 4 chains at a time, so feed it in
         * 4-wide groups (full groups hit the fast path; the tail goes scalar). */
        for (size_t base = 0; base < n; base += 4) {
            size_t b = (n - base < 4) ? (n - base) : 4;
            pose_aes_prf_many(b, tmp + base * POSE_HASH_BYTES, is_source,
                              node_indices + base,
                              pred0s ? pred0s + base : NULL,
                              pred1s ? pred1s + base : NULL,
                              seed, descriptor);
        }
    } else {
        uint8_t msgs[POSE_LABEL_BATCH][NODE_MSG_BYTES];
        const uint8_t *inputs[POSE_LABEL_BATCH];
        for (size_t i = 0; i < n; i++) {
            const uint8_t *p0 = (is_source || !pred0s) ? NULL : pred0s[i];
            const uint8_t *p1 = (is_source || !pred1s) ? NULL : pred1s[i];
            build_node_msg(msgs[i], node_indices[i], p0, p1, seed, descriptor);
            inputs[i] = msgs[i];
        }

        if (algo == POSE_HASH_SHA256) {
            for (size_t i = 0; i < n; i++)
                pose_sha256(msgs[i], NODE_MSG_BYTES, tmp + i * POSE_HASH_BYTES);
        } else {
            ensure_keys();
            blake3_hash_many(inputs, n, 3,
                             is_source ? g_key_src : g_key_int,
                             0, false,
                             KEYED_HASH, CHUNK_START, CHUNK_END | ROOT,
                             tmp);
        }
    }

    for (size_t i = 0; i < n; i++)
        store_label(outs + i * POSE_HASH_BYTES, tmp + i * POSE_HASH_BYTES);
}

/* ── graph descriptor and chunk seed (unchanged) ─────────────────────────── */

void pose_graph_descriptor(uint8_t out[POSE_HASH_BYTES], uint64_t m, uint64_t n)
{
    uint8_t buf[8 + 8 + 8];
    uint8_t *p = buf;

    /* put_u64_be inline */
    p[0]=(uint8_t)(m>>56); p[1]=(uint8_t)(m>>48); p[2]=(uint8_t)(m>>40); p[3]=(uint8_t)(m>>32);
    p[4]=(uint8_t)(m>>24); p[5]=(uint8_t)(m>>16); p[6]=(uint8_t)(m>> 8); p[7]=(uint8_t)(m);
    p += 8;
    p[0]=(uint8_t)(n>>56); p[1]=(uint8_t)(n>>48); p[2]=(uint8_t)(n>>40); p[3]=(uint8_t)(n>>32);
    p[4]=(uint8_t)(n>>24); p[5]=(uint8_t)(n>>16); p[6]=(uint8_t)(n>> 8); p[7]=(uint8_t)(n);
    p += 8;
    p[0]=0; p[1]=0; p[2]=0; p[3]=0; p[4]=0; p[5]=0; p[6]=1; p[7]=0; /* 256 big-endian */

    blake3_hasher h;
    blake3_hasher_init_derive_key(&h, "pose-db/descriptor");
    blake3_hasher_update(&h, buf, sizeof(buf));
    blake3_hasher_finalize(&h, out, POSE_HASH_BYTES);
}

void pose_chunk_seed(uint8_t out[POSE_HASH_BYTES],
                     const uint8_t *session_seed, size_t seed_len,
                     uint64_t chunk_index)
{
    uint8_t idx[8];
    idx[0]=(uint8_t)(chunk_index>>56); idx[1]=(uint8_t)(chunk_index>>48);
    idx[2]=(uint8_t)(chunk_index>>40); idx[3]=(uint8_t)(chunk_index>>32);
    idx[4]=(uint8_t)(chunk_index>>24); idx[5]=(uint8_t)(chunk_index>>16);
    idx[6]=(uint8_t)(chunk_index>> 8); idx[7]=(uint8_t)(chunk_index);

    blake3_hasher h;
    blake3_hasher_init_derive_key(&h, "pose-db/chunk-seed");
    blake3_hasher_update(&h, session_seed, seed_len);
    blake3_hasher_update(&h, idx, sizeof(idx));
    blake3_hasher_finalize(&h, out, POSE_HASH_BYTES);
}
