/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * SHA-256 for the POSE_HASH_SHA256 benchmarking path.
 *
 * AArch64 with __ARM_FEATURE_SHA2: hardware sha256h / sha256h2 /
 * sha256su0 / sha256su1 instructions.  This file is compiled with
 * -march=armv8-a+sha2 so the flag is available without affecting the rest of
 * the build.
 *
 * All other targets: portable C implementation.
 *
 * The input is always the 192-byte node message used by pose_label_node*.
 * Padding: 192 + 1 (0x80) + 55 zeros + 8-byte big-endian bit count = 256
 * bytes = 4 × 64-byte SHA-256 blocks.
 */

#include "../include/sha256.h"
#include <string.h>

/* ── SHA-256 round constants ─────────────────────────────────────────────── */

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffa, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

/* ── SHA-256 initial hash values ─────────────────────────────────────────── */

static const uint32_t H0[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};

/* ── Platform-specific block compression ─────────────────────────────────── */

#ifdef __ARM_FEATURE_SHA2
#include <arm_neon.h>

/*
 * One 64-byte block of SHA-256 compression using ARMv8 SHA-2 instructions.
 * sha256h / sha256h2 each process 4 rounds; sha256su0 / sha256su1 advance the
 * message schedule.  16 groups of 4 rounds = 64 rounds total per block.
 */
static void sha256_compress(uint32_t state[8], const uint8_t block[64])
{
    uint32x4_t abcd = vld1q_u32(&state[0]);
    uint32x4_t efgh = vld1q_u32(&state[4]);
    uint32x4_t abcd_save = abcd, efgh_save = efgh;

    /* Load message words — reverse byte order (BE message → LE NEON words). */
    uint32x4_t msg0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block +  0)));
    uint32x4_t msg1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 16)));
    uint32x4_t msg2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 32)));
    uint32x4_t msg3 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 48)));

    uint32x4_t tmp0, tmp1;

/* Helper: 4 rounds of SHA-256 compression and one message schedule step.
 * Uses old abcd (via tmp1) so both sha256h and sha256h2 see the pre-round
 * state, matching the SHA-256 spec. */
#define SHA256_ROUNDS4(wk, MSG_NEW, su0_a, su0_b, su1_c, su1_d)        \
    tmp0 = (wk);                                                         \
    tmp1 = vsha256hq_u32(abcd, efgh, tmp0);                             \
    efgh = vsha256h2q_u32(efgh, abcd, tmp0);                            \
    abcd = tmp1;                                                         \
    (MSG_NEW) = vsha256su1q_u32(vsha256su0q_u32(su0_a, su0_b), su1_c, su1_d)

#define SHA256_ROUNDS4_NOSCHED(wk)                                       \
    tmp0 = (wk);                                                         \
    tmp1 = vsha256hq_u32(abcd, efgh, tmp0);                             \
    efgh = vsha256h2q_u32(efgh, abcd, tmp0);                            \
    abcd = tmp1

    /* Rounds 0-3;   schedule W[16..19] into msg0 */
    SHA256_ROUNDS4(vaddq_u32(msg0, vld1q_u32(&K[ 0])), msg0,  msg0, msg1, msg2, msg3);
    /* Rounds 4-7;   schedule W[20..23] into msg1 */
    SHA256_ROUNDS4(vaddq_u32(msg1, vld1q_u32(&K[ 4])), msg1,  msg1, msg2, msg3, msg0);
    /* Rounds 8-11;  schedule W[24..27] into msg2 */
    SHA256_ROUNDS4(vaddq_u32(msg2, vld1q_u32(&K[ 8])), msg2,  msg2, msg3, msg0, msg1);
    /* Rounds 12-15; schedule W[28..31] into msg3 */
    SHA256_ROUNDS4(vaddq_u32(msg3, vld1q_u32(&K[12])), msg3,  msg3, msg0, msg1, msg2);
    /* Rounds 16-19; schedule W[32..35] into msg0 */
    SHA256_ROUNDS4(vaddq_u32(msg0, vld1q_u32(&K[16])), msg0,  msg0, msg1, msg2, msg3);
    /* Rounds 20-23; schedule W[36..39] into msg1 */
    SHA256_ROUNDS4(vaddq_u32(msg1, vld1q_u32(&K[20])), msg1,  msg1, msg2, msg3, msg0);
    /* Rounds 24-27; schedule W[40..43] into msg2 */
    SHA256_ROUNDS4(vaddq_u32(msg2, vld1q_u32(&K[24])), msg2,  msg2, msg3, msg0, msg1);
    /* Rounds 28-31; schedule W[44..47] into msg3 */
    SHA256_ROUNDS4(vaddq_u32(msg3, vld1q_u32(&K[28])), msg3,  msg3, msg0, msg1, msg2);
    /* Rounds 32-35; schedule W[48..51] into msg0 */
    SHA256_ROUNDS4(vaddq_u32(msg0, vld1q_u32(&K[32])), msg0,  msg0, msg1, msg2, msg3);
    /* Rounds 36-39; schedule W[52..55] into msg1 */
    SHA256_ROUNDS4(vaddq_u32(msg1, vld1q_u32(&K[36])), msg1,  msg1, msg2, msg3, msg0);
    /* Rounds 40-43; schedule W[56..59] into msg2 */
    SHA256_ROUNDS4(vaddq_u32(msg2, vld1q_u32(&K[40])), msg2,  msg2, msg3, msg0, msg1);
    /* Rounds 44-47; schedule W[60..63] into msg3 */
    SHA256_ROUNDS4(vaddq_u32(msg3, vld1q_u32(&K[44])), msg3,  msg3, msg0, msg1, msg2);
    /* Rounds 48-51; no more schedule updates */
    SHA256_ROUNDS4_NOSCHED(vaddq_u32(msg0, vld1q_u32(&K[48])));
    /* Rounds 52-55 */
    SHA256_ROUNDS4_NOSCHED(vaddq_u32(msg1, vld1q_u32(&K[52])));
    /* Rounds 56-59 */
    SHA256_ROUNDS4_NOSCHED(vaddq_u32(msg2, vld1q_u32(&K[56])));
    /* Rounds 60-63 */
    SHA256_ROUNDS4_NOSCHED(vaddq_u32(msg3, vld1q_u32(&K[60])));

#undef SHA256_ROUNDS4
#undef SHA256_ROUNDS4_NOSCHED

    abcd = vaddq_u32(abcd, abcd_save);
    efgh = vaddq_u32(efgh, efgh_save);
    vst1q_u32(&state[0], abcd);
    vst1q_u32(&state[4], efgh);
}

#else /* portable C fallback */

#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x,y,z)    (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z)   (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SIG0(x) (ROTR32(x,  2) ^ ROTR32(x, 13) ^ ROTR32(x, 22))
#define SIG1(x) (ROTR32(x,  6) ^ ROTR32(x, 11) ^ ROTR32(x, 25))
#define sig0(x) (ROTR32(x,  7) ^ ROTR32(x, 18) ^ ((x) >>  3))
#define sig1(x) (ROTR32(x, 17) ^ ROTR32(x, 19) ^ ((x) >> 10))

static void sha256_compress(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4  ] << 24)
             | ((uint32_t)block[i*4+1] << 16)
             | ((uint32_t)block[i*4+2] <<  8)
             |  (uint32_t)block[i*4+3];
    }
    for (int i = 16; i < 64; i++)
        w[i] = sig1(w[i-2]) + w[i-7] + sig0(w[i-15]) + w[i-16];

    uint32_t a=state[0], b=state[1], c=state[2], d=state[3];
    uint32_t e=state[4], f=state[5], g=state[6], h=state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + SIG1(e) + CH(e,f,g) + K[i] + w[i];
        uint32_t t2 = SIG0(a) + MAJ(a,b,c);
        h=g; g=f; f=e; e=d+t1;
        d=c; c=b; b=a; a=t1+t2;
    }

    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
    state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
}

#undef ROTR32
#undef CH
#undef MAJ
#undef SIG0
#undef SIG1
#undef sig0
#undef sig1

#endif /* __ARM_FEATURE_SHA2 */

/* ── Public entry point ──────────────────────────────────────────────────── */

void pose_sha256(const uint8_t *msg, size_t msg_len, uint8_t out[32])
{
    /*
     * Build padded message.  For 192-byte input:
     *   192 bytes message + 0x80 + 55 zero bytes + 8-byte bit count = 256 bytes
     *   = 4 × 64-byte SHA-256 blocks.
     *
     * Bit count = 192 × 8 = 1536 = 0x0000000000000600.
     */
    uint8_t padded[256];
    memcpy(padded, msg, msg_len);
    memset(padded + msg_len, 0, 256 - msg_len);
    padded[msg_len] = 0x80;
    /* 8-byte big-endian bit count at bytes 248..255 */
    uint64_t bits = (uint64_t)msg_len * 8;
    for (int i = 7; i >= 0; i--) {
        padded[248 + i] = (uint8_t)(bits & 0xff);
        bits >>= 8;
    }

    uint32_t state[8];
    for (int i = 0; i < 8; i++) state[i] = H0[i];

    int nblocks = (int)((msg_len + 1 + 8 + 63) / 64);
    for (int b = 0; b < nblocks; b++)
        sha256_compress(state, padded + b * 64);

    for (int i = 0; i < 8; i++) {
        out[i*4+0] = (uint8_t)(state[i] >> 24);
        out[i*4+1] = (uint8_t)(state[i] >> 16);
        out[i*4+2] = (uint8_t)(state[i] >>  8);
        out[i*4+3] = (uint8_t)(state[i]);
    }
}
