/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * blake3_node.cuh — on-device keyed BLAKE3 for a single PoSE-DB node label.
 *
 * Computes one keyed BLAKE3 hash over the fixed 192-byte (3 × 64-byte block)
 * node message, byte-identical to the CPU path in common/src/hash.c
 * (pose_label_node → blake3_hash_many at 3 blocks).  This is a single-chunk
 * keyed hash, not a Merkle-tree hash of a long input.
 *
 * The compression core (g, round, permute, compress folding) is the BLAKE3
 * compression function as implemented in the Blaze-3 BLAKE3-gpu project
 * (https://github.com/Blaze-3/BLAKE3-gpu, MIT License, Copyright (c) 2021
 * Rehan Vipin; see NOTICE).  It is reproduced here as
 * `static __device__ __forceinline__` so it can be included from multiple .cu
 * translation units without ODR/multiple-definition conflicts and without
 * that project's host-side Chunk/Hasher machinery.
 *
 * Message layout (matches hash.h / build_node_msg):
 *   Block 0 [ 0.. 63]: node_index (8 B big-endian) | zeros (56 B)
 *   Block 1 [64..127]: pred0 (32 B) | pred1 (32 B)   — zero if absent
 *   Block 2 [128..191]: seed (32 B) | descriptor (32 B)
 *
 * Keys: source nodes use derive_key("pose-db/label/src"); internal nodes use
 * derive_key("pose-db/label/int").  The host derives both (pose_label_node_keys)
 * and passes the selected 8-word key in.
 */
#ifndef POSE_BLAKE3_NODE_CUH
#define POSE_BLAKE3_NODE_CUH

#include <cstdint>

namespace pose_gpu {

/* BLAKE3 block/flag constants (match vendor/blake3). */
static __device__ __constant__ const uint32_t kIV[8] = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
};

static __device__ __constant__ const int kMsgPermutation[16] = {
    2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8
};

enum : uint32_t {
    FLAG_CHUNK_START = 1u << 0,
    FLAG_CHUNK_END   = 1u << 1,
    FLAG_ROOT        = 1u << 3,
    FLAG_KEYED_HASH  = 1u << 4,
};

static __device__ __forceinline__ uint32_t rotr32(uint32_t v, int s)
{
    return (v >> s) | (v << (32 - s));
}

static __device__ __forceinline__ void mix(uint32_t st[16], int a, int b, int c,
                                           int d, uint32_t mx, uint32_t my)
{
    st[a] = st[a] + st[b] + mx;
    st[d] = rotr32(st[d] ^ st[a], 16);
    st[c] = st[c] + st[d];
    st[b] = rotr32(st[b] ^ st[c], 12);
    st[a] = st[a] + st[b] + my;
    st[d] = rotr32(st[d] ^ st[a], 8);
    st[c] = st[c] + st[d];
    st[b] = rotr32(st[b] ^ st[c], 7);
}

static __device__ __forceinline__ void round_fn(uint32_t st[16], const uint32_t m[16])
{
    mix(st, 0, 4,  8, 12, m[0],  m[1]);
    mix(st, 1, 5,  9, 13, m[2],  m[3]);
    mix(st, 2, 6, 10, 14, m[4],  m[5]);
    mix(st, 3, 7, 11, 15, m[6],  m[7]);
    mix(st, 0, 5, 10, 15, m[8],  m[9]);
    mix(st, 1, 6, 11, 12, m[10], m[11]);
    mix(st, 2, 7,  8, 13, m[12], m[13]);
    mix(st, 3, 4,  9, 14, m[14], m[15]);
}

static __device__ __forceinline__ void permute(uint32_t m[16])
{
    uint32_t t[16];
    for (int i = 0; i < 16; i++) t[i] = m[kMsgPermutation[i]];
    for (int i = 0; i < 16; i++) m[i] = t[i];
}

/*
 * One BLAKE3 compression.  cv[8] in/out: on return cv holds the new chaining
 * value (state[i] ^ state[i+8]) — the non-root CV, which for the ROOT block
 * also yields the first 32 output bytes.  counter is always 0 here.
 */
static __device__ __forceinline__ void compress(uint32_t cv[8],
                                                 const uint32_t block[16],
                                                 uint32_t block_len, uint32_t flags)
{
    uint32_t st[16];
    for (int i = 0; i < 8; i++) st[i] = cv[i];
    for (int i = 0; i < 8; i++) st[8 + i] = kIV[i];
    st[12] = 0;            /* counter low  */
    st[13] = 0;            /* counter high */
    st[14] = block_len;
    st[15] = flags;

    uint32_t m[16];
    for (int i = 0; i < 16; i++) m[i] = block[i];

    round_fn(st, m); permute(m);
    round_fn(st, m); permute(m);
    round_fn(st, m); permute(m);
    round_fn(st, m); permute(m);
    round_fn(st, m); permute(m);
    round_fn(st, m); permute(m);
    round_fn(st, m);

    for (int i = 0; i < 8; i++) cv[i] = st[i] ^ st[i + 8];
}

static __device__ __forceinline__ uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * 32-byte label <-> 8 little-endian words.
 *
 * The GPU is little-endian, so a uint4/uint32 load of bytes b0..b3 yields
 * exactly b0 | b1<<8 | b2<<16 | b3<<24 == load_le32: the words are identical
 * whichever path runs, only the instruction count differs.  All production
 * callers hand in 32-byte-aligned pointers (scratch slots, persisted labels,
 * per-chunk seeds and the descriptor are all `cudaMalloc base + k*32`), so they
 * take the two-uint4 path; the byte path exists for callers with unaligned
 * buffers (e.g. local arrays in a standalone test kernel) and is checked at
 * runtime — the branch is warp-uniform and costs nothing measurable.
 *
 * Why this matters: L1 is write-through and does not combine stores across
 * instructions, so 32 single-byte stores per label are 32 L2 write
 * transactions (Nsight measured L2 at ~70 % of peak with HBM at ~5 %).  Two
 * 16-byte stores are 2.  Loads similarly drop from ~130 LSU instructions per
 * node (preds + seed + descriptor, byte by byte) to 8.
 */
static __device__ __forceinline__ void load_label_words(uint32_t w[8], const uint8_t *p)
{
    if ((((uintptr_t)p) & 15u) == 0) {
        const uint4 *q = reinterpret_cast<const uint4 *>(p);
        uint4 a = q[0], b = q[1];
        w[0] = a.x; w[1] = a.y; w[2] = a.z; w[3] = a.w;
        w[4] = b.x; w[5] = b.y; w[6] = b.z; w[7] = b.w;
    } else {
        for (int i = 0; i < 8; i++) w[i] = load_le32(p + i * 4);
    }
}

static __device__ __forceinline__ void store_label_words(uint8_t *p, const uint32_t w[8])
{
    if ((((uintptr_t)p) & 15u) == 0) {
        uint4 *q = reinterpret_cast<uint4 *>(p);
        q[0] = make_uint4(w[0], w[1], w[2], w[3]);
        q[1] = make_uint4(w[4], w[5], w[6], w[7]);
    } else {
        for (int i = 0; i < 8; i++) {
            p[i * 4 + 0] = (uint8_t)(w[i]);
            p[i * 4 + 1] = (uint8_t)(w[i] >> 8);
            p[i * 4 + 2] = (uint8_t)(w[i] >> 16);
            p[i * 4 + 3] = (uint8_t)(w[i] >> 24);
        }
    }
}

/* Byte-reverse a 32-bit word (PRMT with selector 0x0123). */
static __device__ __forceinline__ uint32_t bswap32(uint32_t v)
{
    return __byte_perm(v, 0, 0x0123);
}

/*
 * Compute one node label into out[32].
 *
 *   key      selected 8-word key (src for source nodes, int otherwise)
 *   node_index   the node id
 *   pred0/pred1  pointers to 32-byte predecessor labels, or nullptr → zeros
 *   seed/descriptor  32 bytes each
 *
 * Message layout (192 bytes = 3 BLAKE3 blocks), identical to the CPU
 * pose_label_node:
 *   block 0: node_index as 8 big-endian bytes at offset 0, rest zero
 *   block 1: pred0 | pred1 (32 zero bytes for an absent predecessor)
 *   block 2: seed | descriptor
 * The blocks are assembled directly as little-endian words rather than via a
 * byte buffer; the word values are the same (see load_label_words).
 */
static __device__ __forceinline__ void label_node(uint8_t out[32],
                                                   const uint32_t key[8],
                                                   uint64_t node_index,
                                                   const uint8_t *pred0,
                                                   const uint8_t *pred1,
                                                   const uint8_t *seed,
                                                   const uint8_t *descriptor)
{
    uint32_t cv[8];
    for (int i = 0; i < 8; i++) cv[i] = key[i];

    uint32_t words[16];

    /* Block 0: node_index big-endian at byte 0.  Bytes 0..3 are the high half
     * (msb first), so the little-endian word 0 is bswap(high), word 1 is
     * bswap(low).  Bytes 8..63 are zero. */
    words[0] = bswap32((uint32_t)(node_index >> 32));
    words[1] = bswap32((uint32_t)(node_index));
    for (int i = 2; i < 16; i++) words[i] = 0;
    compress(cv, words, 64, FLAG_KEYED_HASH | FLAG_CHUNK_START);

    /* Block 1: pred0 | pred1 (zero if absent). */
    if (pred0) load_label_words(words, pred0);
    else       for (int i = 0; i < 8; i++) words[i] = 0;
    if (pred1) load_label_words(words + 8, pred1);
    else       for (int i = 8; i < 16; i++) words[i] = 0;
    compress(cv, words, 64, FLAG_KEYED_HASH);

    /* Block 2: seed | descriptor. */
    load_label_words(words, seed);
    load_label_words(words + 8, descriptor);
    compress(cv, words, 64, FLAG_KEYED_HASH | FLAG_CHUNK_END | FLAG_ROOT);

    /* Output first 32 bytes = cv[0..7] little-endian. */
    store_label_words(out, cv);
}

/* Copy one 32-byte label (both pointers 32-byte aligned in every production
 * caller; falls back to bytes otherwise).  Used to mirror an output-set label
 * from its scratch slot to the persisted region. */
static __device__ __forceinline__ void copy_label(uint8_t *dst, const uint8_t *src)
{
    if (((((uintptr_t)dst) | ((uintptr_t)src)) & 15u) == 0) {
        const uint4 *s = reinterpret_cast<const uint4 *>(src);
        uint4 *d = reinterpret_cast<uint4 *>(dst);
        d[0] = s[0]; d[1] = s[1];
    } else {
        for (int k = 0; k < 32; k++) dst[k] = src[k];
    }
}

} /* namespace pose_gpu */

#endif /* POSE_BLAKE3_NODE_CUH */
