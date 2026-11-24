/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * aes_prf.c — AES-128-CBC-MAC double-pipe label PRF.
 *
 * Two hardware backends produce byte-identical labels:
 *   ARM64  — AES-CE (vaeseq_u8 / vaesmcq_u8), -march=armv8-a+crypto.
 *   x86-64 — AES-NI (_mm_aesenc_si128 / _mm_aesenclast_si128), -maes.
 * The ARM AESE+AESMC round chain is exactly FIPS-197 AES-128 encryption, the
 * same primitive AES-NI's aesenc/aesenclast compute, so a given (seed, message)
 * yields identical labels on both ISAs — required for verifier↔daemon parity.
 * On any other target the call aborts rather than emit zero labels.
 */

#include "../include/aes_prf.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define AES_MSG_BYTES  112  /* 7 × 16-byte AES blocks */
#define AES_BLOCKS       7
#define LABEL_BYTES     32

#if defined(__ARM_FEATURE_AES) || defined(__AES__)
#define POSE_AES_PRF_HAVE_BACKEND 1
#endif

/* ══════════════════════════════════════════════════════════════════════════ */
/* Shared (arch-independent) message builder + software key schedule          */
/* ══════════════════════════════════════════════════════════════════════════ */

#ifdef POSE_AES_PRF_HAVE_BACKEND

static void build_aes_msg(uint8_t msg[AES_MSG_BYTES],
                           uint64_t node_index,
                           const uint8_t *pred0,
                           const uint8_t *pred1,
                           const uint8_t *descriptor)
{
    msg[0] = (uint8_t)(node_index >> 56);
    msg[1] = (uint8_t)(node_index >> 48);
    msg[2] = (uint8_t)(node_index >> 40);
    msg[3] = (uint8_t)(node_index >> 32);
    msg[4] = (uint8_t)(node_index >> 24);
    msg[5] = (uint8_t)(node_index >> 16);
    msg[6] = (uint8_t)(node_index >>  8);
    msg[7] = (uint8_t)(node_index);
    memset(msg + 8, 0, 8); /* bytes 8–15: only field never otherwise written */
    if (pred0) memcpy(msg + 16, pred0, 32); else memset(msg + 16, 0, 32);
    if (pred1) memcpy(msg + 48, pred1, 32); else memset(msg + 48, 0, 32);
    memcpy(msg + 80, descriptor, 32);
}

/* Standard AES-128 S-box (FIPS 197). */
static const uint8_t s_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static const uint8_t s_rcon[10] = {
    0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36
};

/*
 * AES-128 key schedule into the raw 176-byte expanded-key buffer (11 round
 * keys × 16 bytes).  Neither AES-CE nor AES-NI has a single-instruction
 * AES-128 schedule that matches this layout portably, so it is computed in
 * software once per chunk_seed and each backend loads the 16-byte slices into
 * its own vector type.  The byte layout is identical across ISAs, so both
 * backends derive the same round keys.
 */
static void aes128_schedule_bytes(const uint8_t *key, uint8_t w[176])
{
    memcpy(w, key, 16);
    for (int i = 4; i < 44; i++) {
        uint8_t t[4];
        memcpy(t, w + (i - 1) * 4, 4);
        if ((i % 4) == 0) {
            uint8_t tmp = t[0]; t[0] = t[1]; t[1] = t[2]; t[2] = t[3]; t[3] = tmp;
            t[0] = s_sbox[t[0]]; t[1] = s_sbox[t[1]];
            t[2] = s_sbox[t[2]]; t[3] = s_sbox[t[3]];
            t[0] ^= s_rcon[i / 4 - 1];
        }
        w[i*4+0] = w[(i-4)*4+0] ^ t[0];
        w[i*4+1] = w[(i-4)*4+1] ^ t[1];
        w[i*4+2] = w[(i-4)*4+2] ^ t[2];
        w[i*4+3] = w[(i-4)*4+3] ^ t[3];
    }
}

#endif /* POSE_AES_PRF_HAVE_BACKEND */

/* ══════════════════════════════════════════════════════════════════════════ */
/* ARM AES-CE path                                                            */
/* ══════════════════════════════════════════════════════════════════════════ */

#ifdef __ARM_FEATURE_AES

#include <arm_neon.h>

/*
 * Per-thread round-key cache.  chunk_seed is constant for POSE_CHUNK_BLOCKS
 * nodes, so the two key expansions need to happen once per chunk, not once
 * per 4-label batch.  A 32-byte memcmp on a cache hit replaces two full
 * software S-box walks.
 */
typedef struct {
    uint8_t    seed[32];
    uint8x16_t rk_hi[11];
    uint8x16_t rk_lo[11];
    int        valid;
} aes_key_cache_t;

static _Thread_local aes_key_cache_t tl_key_cache;

static void aes128_expand(const uint8_t *key, uint8x16_t rk[11])
{
    uint8_t w[176];
    aes128_schedule_bytes(key, w);
    for (int i = 0; i < 11; i++)
        rk[i] = vld1q_u8(w + i * 16);
}

static inline const aes_key_cache_t *get_round_keys(const uint8_t *seed)
{
    if (!tl_key_cache.valid || memcmp(tl_key_cache.seed, seed, 32) != 0) {
        aes128_expand(seed,      tl_key_cache.rk_hi);
        aes128_expand(seed + 16, tl_key_cache.rk_lo);
        memcpy(tl_key_cache.seed, seed, 32);
        tl_key_cache.valid = 1;
    }
    return &tl_key_cache;
}

/*
 * AES-128-CBC-MAC over one 112-byte (7-block) message, single chain.
 * vaeseq_u8(state, rk) = ShiftRows(SubBytes(state XOR rk)); the AR()+final
 * sequence is exactly FIPS-197 AES-128 encryption of each CBC-chained block.
 */
static uint8x16_t cbc_mac_single(const uint8x16_t rk[11],
                                  const uint8_t msg[AES_MSG_BYTES])
{
    uint8x16_t s = vdupq_n_u8(0);
    for (int b = 0; b < AES_BLOCKS; b++) {
        s = veorq_u8(s, vld1q_u8(msg + b * 16));   /* CBC XOR */
#define AR(r) s = vaesmcq_u8(vaeseq_u8(s, rk[r]))
        AR(0); AR(1); AR(2); AR(3); AR(4); AR(5); AR(6); AR(7); AR(8);
#undef AR
        s = veorq_u8(vaeseq_u8(s, rk[9]), rk[10]); /* final round */
    }
    return s;
}

/*
 * 4-wide × 2-key (8-chain) interleaved CBC-MAC for a batch of exactly 4 messages.
 * All 8 chains are issued per AES round so the CPU's SIMD pipelines stay
 * saturated, hiding the AES-CE instruction latency.
 */
static void cbc_mac_4x2(const uint8x16_t rk_hi[11], const uint8x16_t rk_lo[11],
                         const uint8_t *msgs, uint8_t *outs)
{
    uint8x16_t h0=vdupq_n_u8(0), l0=vdupq_n_u8(0);
    uint8x16_t h1=vdupq_n_u8(0), l1=vdupq_n_u8(0);
    uint8x16_t h2=vdupq_n_u8(0), l2=vdupq_n_u8(0);
    uint8x16_t h3=vdupq_n_u8(0), l3=vdupq_n_u8(0);

    for (int b = 0; b < AES_BLOCKS; b++) {
        uint8x16_t m0 = vld1q_u8(msgs +                  b * 16);
        uint8x16_t m1 = vld1q_u8(msgs +   AES_MSG_BYTES + b * 16);
        uint8x16_t m2 = vld1q_u8(msgs + 2*AES_MSG_BYTES + b * 16);
        uint8x16_t m3 = vld1q_u8(msgs + 3*AES_MSG_BYTES + b * 16);

        h0=veorq_u8(h0,m0); l0=veorq_u8(l0,m0);
        h1=veorq_u8(h1,m1); l1=veorq_u8(l1,m1);
        h2=veorq_u8(h2,m2); l2=veorq_u8(l2,m2);
        h3=veorq_u8(h3,m3); l3=veorq_u8(l3,m3);

#define AR8(r) \
        h0=vaesmcq_u8(vaeseq_u8(h0,rk_hi[r])); l0=vaesmcq_u8(vaeseq_u8(l0,rk_lo[r])); \
        h1=vaesmcq_u8(vaeseq_u8(h1,rk_hi[r])); l1=vaesmcq_u8(vaeseq_u8(l1,rk_lo[r])); \
        h2=vaesmcq_u8(vaeseq_u8(h2,rk_hi[r])); l2=vaesmcq_u8(vaeseq_u8(l2,rk_lo[r])); \
        h3=vaesmcq_u8(vaeseq_u8(h3,rk_hi[r])); l3=vaesmcq_u8(vaeseq_u8(l3,rk_lo[r]));
        AR8(0); AR8(1); AR8(2); AR8(3); AR8(4); AR8(5); AR8(6); AR8(7); AR8(8);
#undef AR8

        h0=veorq_u8(vaeseq_u8(h0,rk_hi[9]),rk_hi[10]); l0=veorq_u8(vaeseq_u8(l0,rk_lo[9]),rk_lo[10]);
        h1=veorq_u8(vaeseq_u8(h1,rk_hi[9]),rk_hi[10]); l1=veorq_u8(vaeseq_u8(l1,rk_lo[9]),rk_lo[10]);
        h2=veorq_u8(vaeseq_u8(h2,rk_hi[9]),rk_hi[10]); l2=veorq_u8(vaeseq_u8(l2,rk_lo[9]),rk_lo[10]);
        h3=veorq_u8(vaeseq_u8(h3,rk_hi[9]),rk_hi[10]); l3=veorq_u8(vaeseq_u8(l3,rk_lo[9]),rk_lo[10]);
    }

    vst1q_u8(outs +   0, h0); vst1q_u8(outs +  16, l0);
    vst1q_u8(outs +  32, h1); vst1q_u8(outs +  48, l1);
    vst1q_u8(outs +  64, h2); vst1q_u8(outs +  80, l2);
    vst1q_u8(outs +  96, h3); vst1q_u8(outs + 112, l3);
}

void pose_aes_prf_many(size_t n,
                        uint8_t *outs,
                        int is_source,
                        const uint64_t *node_indices,
                        const uint8_t *const *pred0s,
                        const uint8_t *const *pred1s,
                        const uint8_t *seed,
                        const uint8_t *descriptor)
{
    const aes_key_cache_t *kc = get_round_keys(seed);

    if (n == 4) {
        uint8_t msgs[4 * AES_MSG_BYTES];
        for (size_t i = 0; i < 4; i++) {
            const uint8_t *p0 = (is_source || !pred0s) ? NULL : pred0s[i];
            const uint8_t *p1 = (is_source || !pred1s) ? NULL : pred1s[i];
            build_aes_msg(msgs + i * AES_MSG_BYTES,
                          node_indices[i], p0, p1, descriptor);
        }
        cbc_mac_4x2(kc->rk_hi, kc->rk_lo, msgs, outs);
    } else {
        for (size_t i = 0; i < n; i++) {
            const uint8_t *p0 = (is_source || !pred0s) ? NULL : pred0s[i];
            const uint8_t *p1 = (is_source || !pred1s) ? NULL : pred1s[i];
            uint8_t msg[AES_MSG_BYTES];
            build_aes_msg(msg, node_indices[i], p0, p1, descriptor);
            uint8x16_t hi = cbc_mac_single(kc->rk_hi, msg);
            uint8x16_t lo = cbc_mac_single(kc->rk_lo, msg);
            vst1q_u8(outs + i * LABEL_BYTES,      hi);
            vst1q_u8(outs + i * LABEL_BYTES + 16, lo);
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════ */
/* x86-64 AES-NI path                                                         */
/* ══════════════════════════════════════════════════════════════════════════ */

#elif defined(__AES__)

#include <immintrin.h>

typedef struct {
    uint8_t seed[32];
    __m128i rk_hi[11];
    __m128i rk_lo[11];
    int     valid;
} aes_key_cache_t;

static _Thread_local aes_key_cache_t tl_key_cache;

static void aes128_expand(const uint8_t *key, __m128i rk[11])
{
    uint8_t w[176];
    aes128_schedule_bytes(key, w);
    for (int i = 0; i < 11; i++)
        rk[i] = _mm_loadu_si128((const __m128i *)(w + i * 16));
}

static inline const aes_key_cache_t *get_round_keys(const uint8_t *seed)
{
    if (!tl_key_cache.valid || memcmp(tl_key_cache.seed, seed, 32) != 0) {
        aes128_expand(seed,      tl_key_cache.rk_hi);
        aes128_expand(seed + 16, tl_key_cache.rk_lo);
        memcpy(tl_key_cache.seed, seed, 32);
        tl_key_cache.valid = 1;
    }
    return &tl_key_cache;
}

/*
 * AES-128-CBC-MAC over one 112-byte (7-block) message, single chain.
 * The xor(rk[0]) + aesenc(rk[1..9]) + aesenclast(rk[10]) sequence is FIPS-197
 * AES-128 encryption — byte-identical to the ARM AESE+AESMC chain above.
 */
static __m128i cbc_mac_single(const __m128i rk[11],
                               const uint8_t msg[AES_MSG_BYTES])
{
    __m128i s = _mm_setzero_si128();
    for (int b = 0; b < AES_BLOCKS; b++) {
        s = _mm_xor_si128(s, _mm_loadu_si128((const __m128i *)(msg + b * 16))); /* CBC */
        s = _mm_xor_si128(s, rk[0]);               /* initial AddRoundKey */
        s = _mm_aesenc_si128(s, rk[1]);
        s = _mm_aesenc_si128(s, rk[2]);
        s = _mm_aesenc_si128(s, rk[3]);
        s = _mm_aesenc_si128(s, rk[4]);
        s = _mm_aesenc_si128(s, rk[5]);
        s = _mm_aesenc_si128(s, rk[6]);
        s = _mm_aesenc_si128(s, rk[7]);
        s = _mm_aesenc_si128(s, rk[8]);
        s = _mm_aesenc_si128(s, rk[9]);
        s = _mm_aesenclast_si128(s, rk[10]);       /* final round */
    }
    return s;
}

/*
 * 4-wide × 2-key (8-chain) interleaved CBC-MAC for a batch of exactly 4 messages.
 * All 8 chains are issued per AES round so the aesenc pipeline stays saturated,
 * hiding the ~4-cycle AES-NI latency.
 */
static void cbc_mac_4x2(const __m128i rk_hi[11], const __m128i rk_lo[11],
                        const uint8_t *msgs, uint8_t *outs)
{
    __m128i h0=_mm_setzero_si128(), l0=_mm_setzero_si128();
    __m128i h1=_mm_setzero_si128(), l1=_mm_setzero_si128();
    __m128i h2=_mm_setzero_si128(), l2=_mm_setzero_si128();
    __m128i h3=_mm_setzero_si128(), l3=_mm_setzero_si128();

    for (int b = 0; b < AES_BLOCKS; b++) {
        __m128i m0 = _mm_loadu_si128((const __m128i *)(msgs +                  b * 16));
        __m128i m1 = _mm_loadu_si128((const __m128i *)(msgs +   AES_MSG_BYTES + b * 16));
        __m128i m2 = _mm_loadu_si128((const __m128i *)(msgs + 2*AES_MSG_BYTES + b * 16));
        __m128i m3 = _mm_loadu_si128((const __m128i *)(msgs + 3*AES_MSG_BYTES + b * 16));

        /* CBC XOR followed by initial AddRoundKey (rk[0]). */
        h0=_mm_xor_si128(_mm_xor_si128(h0,m0),rk_hi[0]); l0=_mm_xor_si128(_mm_xor_si128(l0,m0),rk_lo[0]);
        h1=_mm_xor_si128(_mm_xor_si128(h1,m1),rk_hi[0]); l1=_mm_xor_si128(_mm_xor_si128(l1,m1),rk_lo[0]);
        h2=_mm_xor_si128(_mm_xor_si128(h2,m2),rk_hi[0]); l2=_mm_xor_si128(_mm_xor_si128(l2,m2),rk_lo[0]);
        h3=_mm_xor_si128(_mm_xor_si128(h3,m3),rk_hi[0]); l3=_mm_xor_si128(_mm_xor_si128(l3,m3),rk_lo[0]);

#define AR8E(r) \
        h0=_mm_aesenc_si128(h0,rk_hi[r]); l0=_mm_aesenc_si128(l0,rk_lo[r]); \
        h1=_mm_aesenc_si128(h1,rk_hi[r]); l1=_mm_aesenc_si128(l1,rk_lo[r]); \
        h2=_mm_aesenc_si128(h2,rk_hi[r]); l2=_mm_aesenc_si128(l2,rk_lo[r]); \
        h3=_mm_aesenc_si128(h3,rk_hi[r]); l3=_mm_aesenc_si128(l3,rk_lo[r])
        AR8E(1); AR8E(2); AR8E(3); AR8E(4); AR8E(5); AR8E(6); AR8E(7); AR8E(8); AR8E(9);
#undef AR8E

        h0=_mm_aesenclast_si128(h0,rk_hi[10]); l0=_mm_aesenclast_si128(l0,rk_lo[10]);
        h1=_mm_aesenclast_si128(h1,rk_hi[10]); l1=_mm_aesenclast_si128(l1,rk_lo[10]);
        h2=_mm_aesenclast_si128(h2,rk_hi[10]); l2=_mm_aesenclast_si128(l2,rk_lo[10]);
        h3=_mm_aesenclast_si128(h3,rk_hi[10]); l3=_mm_aesenclast_si128(l3,rk_lo[10]);
    }

    _mm_storeu_si128((__m128i *)(outs +   0), h0); _mm_storeu_si128((__m128i *)(outs +  16), l0);
    _mm_storeu_si128((__m128i *)(outs +  32), h1); _mm_storeu_si128((__m128i *)(outs +  48), l1);
    _mm_storeu_si128((__m128i *)(outs +  64), h2); _mm_storeu_si128((__m128i *)(outs +  80), l2);
    _mm_storeu_si128((__m128i *)(outs +  96), h3); _mm_storeu_si128((__m128i *)(outs + 112), l3);
}

void pose_aes_prf_many(size_t n,
                        uint8_t *outs,
                        int is_source,
                        const uint64_t *node_indices,
                        const uint8_t *const *pred0s,
                        const uint8_t *const *pred1s,
                        const uint8_t *seed,
                        const uint8_t *descriptor)
{
    const aes_key_cache_t *kc = get_round_keys(seed);

    if (n == 4) {
        uint8_t msgs[4 * AES_MSG_BYTES];
        for (size_t i = 0; i < 4; i++) {
            const uint8_t *p0 = (is_source || !pred0s) ? NULL : pred0s[i];
            const uint8_t *p1 = (is_source || !pred1s) ? NULL : pred1s[i];
            build_aes_msg(msgs + i * AES_MSG_BYTES,
                          node_indices[i], p0, p1, descriptor);
        }
        cbc_mac_4x2(kc->rk_hi, kc->rk_lo, msgs, outs);
    } else {
        for (size_t i = 0; i < n; i++) {
            const uint8_t *p0 = (is_source || !pred0s) ? NULL : pred0s[i];
            const uint8_t *p1 = (is_source || !pred1s) ? NULL : pred1s[i];
            uint8_t msg[AES_MSG_BYTES];
            build_aes_msg(msg, node_indices[i], p0, p1, descriptor);
            __m128i hi = cbc_mac_single(kc->rk_hi, msg);
            __m128i lo = cbc_mac_single(kc->rk_lo, msg);
            _mm_storeu_si128((__m128i *)(outs + i * LABEL_BYTES),      hi);
            _mm_storeu_si128((__m128i *)(outs + i * LABEL_BYTES + 16), lo);
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════ */
/* No hardware AES backend                                                    */
/* ══════════════════════════════════════════════════════════════════════════ */

#else

void pose_aes_prf_many(size_t n,
                        uint8_t *outs,
                        int is_source,
                        const uint64_t *node_indices,
                        const uint8_t *const *pred0s,
                        const uint8_t *const *pred1s,
                        const uint8_t *seed,
                        const uint8_t *descriptor)
{
    (void)n; (void)outs; (void)is_source; (void)node_indices;
    (void)pred0s; (void)pred1s; (void)seed; (void)descriptor;
    fprintf(stderr, "FATAL: POSE_HASH_AES_PRF requires hardware AES "
                    "(ARM: -march=armv8-a+crypto; x86: -maes)\n");
    abort();
}

#endif
