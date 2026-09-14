/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#include "../include/graph.h"

#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/*
 * Formula-driven topological emitter for pose-db-drg-v1.
 *
 * Follows the depth-robust graph construction of the PoSE-DB paper (Bursuc,
 * Gil-Pons, Mauw, Trujillo-Rasua, 2024, arXiv:2401.06626).
 *
 * Nodes are numbered in the order they are emitted.  The emitter visits
 * every node exactly once, in topological order, and computes its label
 * immediately using the predecessor labels already in the label buffer.
 *
 * Graph parameter relationships:
 *   n   = pose_graph_parameter_n(m)
 *   level = n + 1                (each standalone copy)
 *   total_nodes = 2 * S(level)   where S is _standalone_node_count
 *   challenge nodes:
 *     left copy:  second half of standalone_base_nodes(level, 0)
 *     right copy: first retained_from_right of standalone_base_nodes(level, left_total)
 *     retained_from_right = m - 2^n
 */

/* ── node-count formulae ─────────────────────────────────────────────────── */

static uint64_t butterfly_node_count(int dim)
{
    /* (dim+1) * 2^dim */
    return (uint64_t)(dim + 1) << dim;
}

static uint64_t standalone_node_count(int level);
static uint64_t connected_node_count(int level);

static uint64_t connected_node_count(int level)
{
    if (level == 0) return 1;
    return 2 * connected_node_count(level - 1) + 2 * butterfly_node_count(level - 1);
}

static uint64_t standalone_node_count(int level)
{
    if (level == 0) return 1;
    return standalone_node_count(level - 1)
         + butterfly_node_count(level - 1)
         + connected_node_count(level - 1);
}



/* ── emitter state ────────────────────────────────────────────────────────── */

/*
 * Per-emitter arena factor.  Arena size = POSE_ARENA_FACTOR × half_w uint64_t.
 * Theoretical peak A_s(n+1) ≈ 6.5 × half_w; factor 16 gives a 2.5× margin.
 * For m == POSE_CHUNK_BLOCKS: half_w == 2048, arena == 32768 uint64_t (256 KB).
 */
#define POSE_ARENA_FACTOR 16u

/* Resolve chunk_blocks: 0 means use the compile-time default. */
static uint32_t resolve_chunk_blocks(uint32_t chunk_blocks)
{
    return chunk_blocks != 0 ? chunk_blocks : POSE_CHUNK_BLOCKS;
}

typedef struct {
    uint64_t        next_id;       /* next node index to assign */
    uint8_t        *labels;        /* labels[node_id * POSE_HASH_BYTES]; NULL in edge-only mode */
    pose_node_t    *nodes;         /* edge recorder; NULL in labeling mode */
    const uint8_t  *seed;          /* always POSE_HASH_BYTES bytes */
    uint8_t         desc[POSE_HASH_BYTES]; /* graph descriptor digest */
    uint64_t       *arena;         /* bump-pointer arena for uint64_t node-ID arrays */
    size_t          arena_cap;     /* capacity in uint64_t elements */
    size_t          arena_off;     /* current bump offset in uint64_t elements */
    pose_hash_algo_t algo;         /* hash algorithm for this session */
} emitter_t;

static inline uint8_t *label_of(emitter_t *e, uint64_t node_id)
{
    return e->labels + node_id * POSE_HASH_BYTES;
}

/* Allocate n uint64_t elements from the emitter's arena (bump pointer). */
static inline uint64_t *arena_alloc(emitter_t *e, size_t n)
{
    uint64_t *p = e->arena + e->arena_off;
    e->arena_off += n;
    return p;
}

/* Emit a source node (no predecessors) and return its id. */
static uint64_t emit_source(emitter_t *e)
{
    uint64_t id = e->next_id++;
    if (e->labels)
        pose_label_node(label_of(e, id), 1, id, NULL, NULL, e->seed, e->desc, e->algo);
    if (e->nodes) {
        e->nodes[id].num_preds      = 0;
        e->nodes[id].challenge_rank = -1;
    }
    return id;
}

/* Emit a node with one predecessor. */
static uint64_t emit1(emitter_t *e, uint64_t pred0)
{
    uint64_t id = e->next_id++;
    if (e->labels)
        pose_label_node(label_of(e, id), 0, id,
                        label_of(e, pred0), NULL,
                        e->seed, e->desc, e->algo);
    if (e->nodes) {
        e->nodes[id].num_preds      = 1;
        e->nodes[id].pred[0]        = pred0;
        e->nodes[id].challenge_rank = -1;
    }
    return id;
}

/* Emit a node with two predecessors (lower-index pred first). */
static uint64_t emit2(emitter_t *e, uint64_t pred0, uint64_t pred1)
{
    uint64_t id = e->next_id++;
    /* Branchless canonical sort: pred0 ≤ pred1 after this swap. */
    if (pred0 > pred1) { uint64_t t = pred0; pred0 = pred1; pred1 = t; }
    if (e->labels)
        pose_label_node(label_of(e, id), 0, id,
                        label_of(e, pred0), label_of(e, pred1),
                        e->seed, e->desc, e->algo);
    if (e->nodes) {
        e->nodes[id].num_preds      = 2;
        e->nodes[id].pred[0]        = pred0;
        e->nodes[id].pred[1]        = pred1;
        e->nodes[id].challenge_rank = -1;
    }
    return id;
}

/* ── butterfly connector ──────────────────────────────────────────────────── */

/*
 * Emit a butterfly connector of dimension `dim`.
 * inputs[] has 2^dim elements (node ids of predecessor layer).
 * outputs[] receives 2^dim element node ids of the last layer.
 *
 * Structure: dim+1 layers of width 2^dim.
 * Layer 0:   emit1(inputs[i])         for each i
 * Layer k:   emit2(prev[i], prev[i ^ (1 << (dim-1-k))]) for each i  (k=1..dim-1)
 * Layer dim: emit2(prev[i], prev[i ^ (1 << 0)])         for i (k=dim-1 means bit=0)
 *
 * Label mode: batches of up to POSE_LABEL_BATCH nodes via pose_label_node_many
 *             → widest BLAKE3 SIMD the CPU has (AVX-512 16-wide / NEON 4-wide).
 * Edge mode:  sequential via emit1/emit2 as before.
 */
static void emit_connector(emitter_t *e, int dim,
                           const uint64_t *inputs, uint64_t *outputs)
{
    int width = 1 << dim;
    size_t saved = e->arena_off;
    uint64_t *prev = arena_alloc(e, (size_t)width);
    uint64_t *curr = arena_alloc(e, (size_t)width);

    if (e->labels) {
        /* ── batched label path ─────────────────────────── */

        /* Layer 0: each node has one predecessor from inputs */
        for (int i = 0; i < width; ) {
            int batch = (width - i < POSE_LABEL_BATCH) ? (width - i) : POSE_LABEL_BATCH;
            /* Prefetch predecessor labels for the next batch. */
            int ni = i + batch;
            for (int k = 0; k < POSE_LABEL_BATCH && ni + k < width; k++)
                __builtin_prefetch(label_of(e, inputs[ni + k]), 0, 0);
            uint64_t ids[POSE_LABEL_BATCH];
            const uint8_t *pred0s[POSE_LABEL_BATCH];
            uint64_t first_id = e->next_id;
            for (int j = 0; j < batch; j++) {
                ids[j] = e->next_id++;
                prev[i + j] = ids[j];
                pred0s[j] = label_of(e, inputs[i + j]);
            }
            pose_label_node_many((size_t)batch, 0, ids, pred0s, NULL,
                                 e->seed, e->desc, label_of(e, first_id), e->algo);
            i += batch;
        }

        /* Layers 1..dim: each node has two predecessors from prev */
        for (int layer = 0; layer < dim; layer++) {
            int bit = 1 << (dim - 1 - layer);
            for (int i = 0; i < width; ) {
                int batch = (width - i < POSE_LABEL_BATCH) ? (width - i) : POSE_LABEL_BATCH;
                /* Prefetch both predecessor labels for the next batch. */
                int ni = i + batch;
                for (int k = 0; k < POSE_LABEL_BATCH && ni + k < width; k++) {
                    __builtin_prefetch(label_of(e, prev[ni + k]), 0, 0);
                    __builtin_prefetch(label_of(e, prev[(ni + k) ^ bit]), 0, 0);
                }
                uint64_t ids[POSE_LABEL_BATCH];
                const uint8_t *pred0s[POSE_LABEL_BATCH];
                const uint8_t *pred1s[POSE_LABEL_BATCH];
                uint64_t first_id = e->next_id;
                for (int j = 0; j < batch; j++) {
                    int idx = i + j;
                    ids[j] = e->next_id++;
                    curr[idx] = ids[j];
                    uint64_t p0 = prev[idx], p1 = prev[idx ^ bit];
                    if (p0 > p1) { uint64_t t = p0; p0 = p1; p1 = t; }
                    pred0s[j] = label_of(e, p0);
                    pred1s[j] = label_of(e, p1);
                }
                pose_label_node_many((size_t)batch, 0, ids, pred0s, pred1s,
                                     e->seed, e->desc, label_of(e, first_id), e->algo);
                i += batch;
            }
            uint64_t *tmp = prev; prev = curr; curr = tmp;
        }
    } else {
        /* ── sequential edge-recording path ────────────────── */

        for (int i = 0; i < width; i++)
            prev[i] = emit1(e, inputs[i]);

        for (int layer = 0; layer < dim; layer++) {
            int bit = 1 << (dim - 1 - layer);
            for (int i = 0; i < width; i++)
                curr[i] = emit2(e, prev[i], prev[i ^ bit]);
            uint64_t *tmp = prev; prev = curr; curr = tmp;
        }
    }

    memcpy(outputs, prev, (size_t)width * sizeof(uint64_t));
    e->arena_off = saved;
}

/* ── merged center ingress (two interleaved butterfly connectors) ──────────── */

/*
 * Merged center ingress.
 *
 * Emits two butterfly connectors of dimension `dim` that are interleaved:
 * - the "ingress" copy processes ingress_inputs top-down
 * - the "center" copy mixes primary_inputs with the ingress outputs bottom-up
 *
 * Produces 2^dim output node ids in outputs[].
 *
 * Node-emission ordering uses a priority-queue-based ready-list (topological)
 * that processes the two butterfly layers in the correct order.
 */
static void emit_merged_center_ingress(emitter_t *e, int dim,
                                       const uint64_t *primary_inputs,
                                       const uint64_t *ingress_inputs,
                                       uint64_t *outputs)
{
    if (dim == 0) {
        /*
         * dim=0: width=1.
         * ingress: emit1(ingress_inputs[0])        → ingress_out[0]
         * center:  emit2(primary_inputs[0], ingress_out[0]) → outputs[0]
         */
        uint64_t ig = emit1(e, ingress_inputs[0]);
        outputs[0] = emit2(e, primary_inputs[0], ig);
        return;
    }

    int width = 1 << dim;

    if (e->labels) {
        /*
         * ── batched label path ─────────────────────────────────────────────
         *
         * The ingress butterfly (dim+1 layers) and center butterfly (dim+1
         * layers) have no intra-layer dependencies: all nodes in layer k depend
         * only on layer k-1.  We process each layer sequentially and batch up
         * to POSE_LABEL_BATCH nodes per pose_label_node_many call so the widest
         * BLAKE3 hash_many backend fires (AVX-512 16-wide on x86, NEON 4-wide
         * on ARM64).  width = 2^dim, so full batches when 2^dim >= batch and a
         * single short tail batch otherwise — both correct (lanes are pure
         * parallelism, output is batch-width-independent).
         *
         * Ingress runs first (top-down, layer 0 → dim); center runs after
         * (layer 0 → dim), with center layer 0 depending on primary_inputs and
         * the last ingress layer.
         *
         * Rolling prev/curr pointer swaps track the previous layer's node IDs
         * without any extra allocation.
         */
        size_t saved   = e->arena_off;
        uint64_t *ing_prev = arena_alloc(e, (size_t)width);
        uint64_t *ing_curr = arena_alloc(e, (size_t)width);
        uint64_t *ctr_prev = arena_alloc(e, (size_t)width);
        uint64_t *ctr_curr = arena_alloc(e, (size_t)width);

        /* ── Ingress butterfly ── */

        /* Layer 0: one predecessor from ingress_inputs[i]. */
        for (int i = 0; i < width; ) {
            int batch = (width - i < POSE_LABEL_BATCH) ? (width - i) : POSE_LABEL_BATCH;
            int ni = i + batch;
            for (int k = 0; k < POSE_LABEL_BATCH && ni + k < width; k++)
                __builtin_prefetch(label_of(e, ingress_inputs[ni + k]), 0, 0);
            uint64_t ids[POSE_LABEL_BATCH];
            const uint8_t *pred0s[POSE_LABEL_BATCH];
            uint64_t first_id = e->next_id;
            for (int j = 0; j < batch; j++) {
                ids[j] = e->next_id++;
                ing_prev[i + j] = ids[j];
                pred0s[j] = label_of(e, ingress_inputs[i + j]);
            }
            pose_label_node_many((size_t)batch, 0, ids, pred0s, NULL,
                                 e->seed, e->desc, label_of(e, first_id), e->algo);
            i += batch;
        }

        /* Layers 1..dim: two predecessors from previous ingress layer. */
        for (int layer = 1; layer <= dim; layer++) {
            int bit = 1 << (dim - layer);
            for (int i = 0; i < width; ) {
                int batch = (width - i < POSE_LABEL_BATCH) ? (width - i) : POSE_LABEL_BATCH;
                int ni = i + batch;
                for (int k = 0; k < POSE_LABEL_BATCH && ni + k < width; k++) {
                    __builtin_prefetch(label_of(e, ing_prev[ni + k]), 0, 0);
                    __builtin_prefetch(label_of(e, ing_prev[(ni + k) ^ bit]), 0, 0);
                }
                uint64_t ids[POSE_LABEL_BATCH];
                const uint8_t *pred0s[POSE_LABEL_BATCH], *pred1s[POSE_LABEL_BATCH];
                uint64_t first_id = e->next_id;
                for (int j = 0; j < batch; j++) {
                    int idx = i + j;
                    ids[j] = e->next_id++;
                    ing_curr[idx] = ids[j];
                    uint64_t p0 = ing_prev[idx], p1 = ing_prev[idx ^ bit];
                    if (p0 > p1) { uint64_t t = p0; p0 = p1; p1 = t; }
                    pred0s[j] = label_of(e, p0);
                    pred1s[j] = label_of(e, p1);
                }
                pose_label_node_many((size_t)batch, 0, ids, pred0s, pred1s,
                                     e->seed, e->desc, label_of(e, first_id), e->algo);
                i += batch;
            }
            uint64_t *tmp = ing_prev; ing_prev = ing_curr; ing_curr = tmp;
        }
        /* ing_prev now holds ingress layer dim node IDs. */

        /* ── Center butterfly ── */

        /* Layer 0: pred0 = primary_inputs[i], pred1 = ing_prev[i] (ingress layer dim). */
        for (int i = 0; i < width; ) {
            int batch = (width - i < POSE_LABEL_BATCH) ? (width - i) : POSE_LABEL_BATCH;
            int ni = i + batch;
            for (int k = 0; k < POSE_LABEL_BATCH && ni + k < width; k++) {
                __builtin_prefetch(label_of(e, primary_inputs[ni + k]), 0, 0);
                __builtin_prefetch(label_of(e, ing_prev[ni + k]), 0, 0);
            }
            uint64_t ids[POSE_LABEL_BATCH];
            const uint8_t *pred0s[POSE_LABEL_BATCH], *pred1s[POSE_LABEL_BATCH];
            uint64_t first_id = e->next_id;
            for (int j = 0; j < batch; j++) {
                int idx = i + j;
                ids[j] = e->next_id++;
                ctr_prev[idx] = ids[j];
                uint64_t p0 = primary_inputs[idx], p1 = ing_prev[idx];
                if (p0 > p1) { uint64_t t = p0; p0 = p1; p1 = t; }
                pred0s[j] = label_of(e, p0);
                pred1s[j] = label_of(e, p1);
            }
            pose_label_node_many((size_t)batch, 0, ids, pred0s, pred1s,
                                 e->seed, e->desc, label_of(e, first_id), e->algo);
            i += batch;
        }

        /* Layers 1..dim: two predecessors from previous center layer.
         * The last layer writes node IDs directly into outputs[]. */
        for (int layer = 1; layer <= dim; layer++) {
            int bit = 1 << (dim - layer);
            uint64_t *dst = (layer == dim) ? outputs : ctr_curr;
            for (int i = 0; i < width; ) {
                int batch = (width - i < POSE_LABEL_BATCH) ? (width - i) : POSE_LABEL_BATCH;
                int ni = i + batch;
                for (int k = 0; k < POSE_LABEL_BATCH && ni + k < width; k++) {
                    __builtin_prefetch(label_of(e, ctr_prev[ni + k]), 0, 0);
                    __builtin_prefetch(label_of(e, ctr_prev[(ni + k) ^ bit]), 0, 0);
                }
                uint64_t ids[POSE_LABEL_BATCH];
                const uint8_t *pred0s[POSE_LABEL_BATCH], *pred1s[POSE_LABEL_BATCH];
                uint64_t first_id = e->next_id;
                for (int j = 0; j < batch; j++) {
                    int idx = i + j;
                    ids[j] = e->next_id++;
                    dst[idx] = ids[j];
                    uint64_t p0 = ctr_prev[idx], p1 = ctr_prev[idx ^ bit];
                    if (p0 > p1) { uint64_t t = p0; p0 = p1; p1 = t; }
                    pred0s[j] = label_of(e, p0);
                    pred1s[j] = label_of(e, p1);
                }
                pose_label_node_many((size_t)batch, 0, ids, pred0s, pred1s,
                                     e->seed, e->desc, label_of(e, first_id), e->algo);
                i += batch;
            }
            if (layer < dim) {
                uint64_t *tmp = ctr_prev; ctr_prev = ctr_curr; ctr_curr = tmp;
            }
        }

        e->arena_off = saved;
        return;
    }

    /* ── edge/structure path ───────────────────────────────────────────────────
     *
     * Emits node IDs in the SAME strict layer order as the batched label path
     * above (all ingress layers 0→dim, then all center layers 0→dim).  This is
     * what makes pose_graph_edges (the GPU's topology source) and
     * pose_graph_challenge_node_ids number every node identically to
     * do_label_inplace's label buffer — the prerequisite for byte-identical
     * GPU↔CPU labels.  Predecessor relationships are unchanged from the old
     * topological ready-queue; only the emission (ID-assignment) order is
     * aligned.  emit1/emit2 still guarantee pred-id < node-id because each layer
     * is fully emitted before the next, so the GPU's ascending-id level schedule
     * stays valid.
     */
    size_t saved   = e->arena_off;
    uint64_t *ing_prev = arena_alloc(e, (size_t)width);
    uint64_t *ing_curr = arena_alloc(e, (size_t)width);
    uint64_t *ctr_prev = arena_alloc(e, (size_t)width);
    uint64_t *ctr_curr = arena_alloc(e, (size_t)width);

    /* ── Ingress butterfly ── */

    /* Layer 0: one predecessor from ingress_inputs[i]. */
    for (int i = 0; i < width; i++)
        ing_prev[i] = emit1(e, ingress_inputs[i]);

    /* Layers 1..dim: two predecessors from the previous ingress layer. */
    for (int layer = 1; layer <= dim; layer++) {
        int bit = 1 << (dim - layer);
        for (int i = 0; i < width; i++)
            ing_curr[i] = emit2(e, ing_prev[i], ing_prev[i ^ bit]);
        uint64_t *tmp = ing_prev; ing_prev = ing_curr; ing_curr = tmp;
    }
    /* ing_prev now holds ingress layer dim node IDs. */

    /* ── Center butterfly ── */

    /* Layer 0: pred0 = primary_inputs[i], pred1 = ing_prev[i] (ingress layer dim). */
    for (int i = 0; i < width; i++)
        ctr_prev[i] = emit2(e, primary_inputs[i], ing_prev[i]);

    /* Layers 1..dim: two predecessors from the previous center layer.
     * The last layer writes node IDs directly into outputs[]. */
    for (int layer = 1; layer <= dim; layer++) {
        int bit = 1 << (dim - layer);
        uint64_t *dst = (layer == dim) ? outputs : ctr_curr;
        for (int i = 0; i < width; i++)
            dst[i] = emit2(e, ctr_prev[i], ctr_prev[i ^ bit]);
        if (layer < dim) {
            uint64_t *tmp = ctr_prev; ctr_prev = ctr_curr; ctr_curr = tmp;
        }
    }

    e->arena_off = saved;
}

/* ── recursive graph components ───────────────────────────────────────────── */

static void emit_connected(emitter_t *e, int level,
                           int width, const uint64_t *inputs,
                           uint64_t *base_out);

static void emit_standalone(emitter_t *e, int level, uint64_t *base_out)
{
    if (level == 0) {
        base_out[0] = emit_source(e);
        return;
    }
    int half = 1 << (level - 1);
    size_t saved         = e->arena_off;
    uint64_t *left_base  = arena_alloc(e, (size_t)half);
    uint64_t *center_out = arena_alloc(e, (size_t)half);
    uint64_t *right_base = arena_alloc(e, (size_t)half);

    emit_standalone(e, level - 1, left_base);
    emit_connector(e, level - 1, left_base, center_out);
    emit_connected(e, level - 1, half, center_out, right_base);

    memcpy(base_out,        left_base,  (size_t)half * sizeof(uint64_t));
    memcpy(base_out + half, right_base, (size_t)half * sizeof(uint64_t));

    e->arena_off = saved;
}

static void emit_connected(emitter_t *e, int level,
                           int width, const uint64_t *inputs,
                           uint64_t *base_out)
{
    if (level == 0) {
        /* single node with 1 predecessor */
        base_out[0] = emit1(e, inputs[0]);
        return;
    }
    int half = width / 2;
    size_t saved         = e->arena_off;
    uint64_t *left_base  = arena_alloc(e, (size_t)half);
    uint64_t *center_out = arena_alloc(e, (size_t)half);
    uint64_t *right_base = arena_alloc(e, (size_t)half);

    emit_connected(e, level - 1, half, inputs,        left_base);
    emit_merged_center_ingress(e, level - 1, left_base, inputs + half, center_out);
    emit_connected(e, level - 1, half, center_out,    right_base);

    memcpy(base_out,        left_base,  (size_t)half * sizeof(uint64_t));
    memcpy(base_out + half, right_base, (size_t)half * sizeof(uint64_t));

    e->arena_off = saved;
}

/* ── chunk labeling ───────────────────────────────────────────────────────── */

static int do_label(const uint8_t *seed, uint64_t m,
                    uint8_t *out, uint8_t *labels_buf, pose_hash_algo_t algo);
static int do_label_inplace(const uint8_t *seed, uint64_t m,
                             uint8_t *buf, uint8_t *scratch, pose_hash_algo_t algo);
static uint64_t scratch_node_count(uint64_t m);

typedef struct {
    uint8_t          cseed[POSE_HASH_BYTES];
    uint64_t         m;
    uint8_t         *out;
    uint8_t         *scratch;
    int              rc;
    pose_hash_algo_t algo;
    int              inplace; /* 1 = do_label_inplace; 0 = do_label */

    /* Optional write-through: when wt_sink != NULL, the worker streams its
     * labeled output to the sink immediately after labeling, so the caller need
     * not retain the whole batch in RAM.  Must be NULL on every other path. */
    pose_label_sink_fn wt_sink;
    void              *wt_ctx;
    uint64_t           wt_off;
    size_t             wt_len;
} chunk_arg_t;

static void *chunk_thread_fn(void *arg)
{
    chunk_arg_t *a = (chunk_arg_t *)arg;
    if (a->inplace)
        a->rc = do_label_inplace(a->cseed, a->m, a->out, a->scratch, a->algo);
    else
        a->rc = do_label(a->cseed, a->m, a->out, a->scratch, a->algo);
    if (a->rc == 0 && a->wt_sink)
        a->rc = a->wt_sink(a->wt_ctx, a->out, a->wt_len, a->wt_off);
    return NULL;
}

/* ── public interface ─────────────────────────────────────────────────────── */

int pose_graph_parameter_n(uint64_t m)
{
    if (m <= 1) return 0;
    int bits = 0;
    uint64_t v = m - 1;
    while (v > 0) { bits++; v >>= 1; }
    int n = bits - 1;
    return n < 0 ? 0 : n;
}

/*
 * Shared labeling core.  seed must be exactly POSE_HASH_BYTES bytes.
 */
static int do_label(const uint8_t *seed, uint64_t m,
                    uint8_t *out, uint8_t *labels_buf, pose_hash_algo_t algo)
{
    if (m == 0 || m > POSE_GRAPH_MAX_M) return -1;

    int n     = pose_graph_parameter_n(m);
    int level = n + 1;
    uint64_t half_w   = (uint64_t)1 << n;
    uint64_t retained = m - half_w;
    uint64_t left_total = standalone_node_count(level);

    /*
     * Carve base-output arrays and per-emitter arenas from the tail of
     * labels_buf (which must be at least pose_graph_scratch_bytes(m) in size).
     * Arena per emitter: POSE_ARENA_FACTOR × half_w uint64_t — covers peak
     * A_s(level) ≈ 6.5 × half_w with a factor-of-POSE_ARENA_FACTOR margin.
     * The two emitters run concurrently and use separate arenas.
     */
    size_t arena_elems   = (size_t)POSE_ARENA_FACTOR * (size_t)half_w;
    size_t label_bytes   = scratch_node_count(m) * POSE_HASH_BYTES;
    uint64_t *left_base  = (uint64_t *)(labels_buf + label_bytes);
    uint64_t *right_base = left_base  + (size_t)(2 * half_w);
    uint64_t *arena_left  = right_base + (size_t)(2 * half_w);
    uint64_t *arena_right = arena_left + arena_elems;

    emitter_t e;
    e.next_id   = 0;
    e.labels    = labels_buf;
    e.nodes     = NULL;
    e.seed      = seed;
    e.arena     = arena_left;
    e.arena_cap = arena_elems;
    e.arena_off = 0;
    e.algo      = algo;
    pose_graph_descriptor(e.desc, m, (uint64_t)n);

    /*
     * The two standalones produce independent sub-graphs: left occupies node IDs
     * [0, left_total) and right occupies [left_total, 2*left_total).  Both halves
     * run sequentially in the calling thread.
     *
     * Previously the right half ran in a pthread_create'd thread alongside the
     * left half.  Per-chunk threads are unsafe when the labeled region is live
     * physical memory (the kernel may place the new thread's stack inside it),
     * and sequential execution is also preferable for throughput: with
     * POSE_REGION_THREADS chunks already running in parallel, a second thread
     * per chunk only doubles cache pressure and adds scheduling overhead.
     */
    emit_standalone(&e, level, left_base);

    e.next_id   = left_total;
    e.arena     = arena_right;
    e.arena_cap = arena_elems;
    e.arena_off = 0;
    emit_standalone(&e, level, right_base);

    /* challenge set: second half of left_base, then first `retained` of right */
    uint64_t *challenge = left_base + half_w;
    for (uint64_t i = 0; i < half_w; i++)
        memcpy(out + i * POSE_HASH_BYTES,
               labels_buf + challenge[i] * POSE_HASH_BYTES,
               POSE_HASH_BYTES);

    challenge = right_base + half_w;
    for (uint64_t i = 0; i < retained; i++)
        memcpy(out + (half_w + i) * POSE_HASH_BYTES,
               labels_buf + challenge[i] * POSE_HASH_BYTES,
               POSE_HASH_BYTES);

    return 0;
}

static uint64_t scratch_node_count(uint64_t m)
{
    int n     = pose_graph_parameter_n(m);
    int level = n + 1;
    return 2 * standalone_node_count(level);
}

/* ── In-place labeling (paper-faithful: persist only the output set O(G)) ──── */

/*
 * Label-carrying emitter.
 *
 * The original emitter stored every node's label in an id-indexed buffer
 * (label_of(e, id)) and let the bump arena hold only node ids.  The faithful
 * in-place prover must NOT keep all scratch_node_count labels live — only the
 * m output-set labels are persisted, the rest are transient.  So here the
 * arena holds the LABELS themselves (32-byte blocks), addressed by array
 * position rather than global node id.  A label lives exactly as long as the
 * arena slot that holds it; the existing save/restore bump lifetimes already
 * bound the simultaneously-live set (structural high-water ≈ 6.5·half_w).
 *
 * Predecessor ordering: the original emit2 sorted preds by node id (lower id
 * first) so the hash is canonical.  Here ids are assigned in ascending array
 * order, so for a butterfly pair {idx, idx^bit} the lower-id label is at the
 * index with `bit` cleared (idx & ~bit) and the higher-id at (idx | bit).  For
 * the center-butterfly layer-0 merge the "primary" inputs are always emitted
 * before the freshly-built "ingress" outputs, so primary is pred0.  The order
 * is thus structurally determined — no id comparison is needed and the bytes
 * are identical to do_label's output set.
 */
typedef struct {
    uint64_t        next_id;   /* next node index (hash domain separation only) */
    const uint8_t  *seed;      /* POSE_HASH_BYTES bytes */
    uint8_t         desc[POSE_HASH_BYTES];
    uint8_t        *arena;     /* bump arena of 32-byte label blocks */
    size_t          arena_cap; /* capacity in label slots */
    size_t          arena_off; /* current bump offset in label slots */
    size_t          arena_hi;  /* high-water mark in slots (debug/proof) */
    pose_hash_algo_t algo;
} lemitter_t;

/* Address label block i within a label array `base`. */
#define LBLK(base, i) ((base) + (size_t)(i) * POSE_HASH_BYTES)

/* Allocate n label slots (n × POSE_HASH_BYTES bytes) from the arena. */
static inline uint8_t *lc_arena_alloc(lemitter_t *e, size_t n)
{
    uint8_t *p = e->arena + e->arena_off * POSE_HASH_BYTES;
    e->arena_off += n;
    if (e->arena_off > e->arena_hi) e->arena_hi = e->arena_off;
    /* Bounded scratch invariant: the transient label set never exceeds the
     * arena.  If this trips, POSE_ARENA_FACTOR is too small for some m. */
    assert(e->arena_off <= e->arena_cap);
    return p;
}

static void lc_source(lemitter_t *e, uint8_t *dst)
{
    uint64_t id = e->next_id++;
    pose_label_node(dst, 1, id, NULL, NULL, e->seed, e->desc, e->algo);
}

static void lc1(lemitter_t *e, uint8_t *dst, const uint8_t *p0)
{
    uint64_t id = e->next_id++;
    pose_label_node(dst, 0, id, p0, NULL, e->seed, e->desc, e->algo);
}

/* Two predecessors, already in canonical (lower-id, higher-id) order. */
static void lc2(lemitter_t *e, uint8_t *dst, const uint8_t *p0, const uint8_t *p1)
{
    uint64_t id = e->next_id++;
    pose_label_node(dst, 0, id, p0, p1, e->seed, e->desc, e->algo);
}

static void lc_connector(lemitter_t *e, int dim,
                         const uint8_t *inputs, uint8_t *outputs)
{
    int width = 1 << dim;
    size_t saved = e->arena_off;
    uint8_t *prev = lc_arena_alloc(e, (size_t)width);
    uint8_t *curr = lc_arena_alloc(e, (size_t)width);

    /* Layer 0: one predecessor from inputs[i]. */
    for (int i = 0; i < width; ) {
        int batch = (width - i < POSE_LABEL_BATCH) ? (width - i) : POSE_LABEL_BATCH;
        int ni = i + batch;
        for (int k = 0; k < POSE_LABEL_BATCH && ni + k < width; k++)
            __builtin_prefetch(LBLK(inputs, ni + k), 0, 0);
        uint64_t ids[POSE_LABEL_BATCH];
        const uint8_t *pred0s[POSE_LABEL_BATCH];
        for (int j = 0; j < batch; j++) {
            ids[j]    = e->next_id++;
            pred0s[j] = LBLK(inputs, i + j);
        }
        pose_label_node_many((size_t)batch, 0, ids, pred0s, NULL,
                             e->seed, e->desc, LBLK(prev, i), e->algo);
        i += batch;
    }

    /* Layers 1..dim: two predecessors from the previous layer. */
    for (int layer = 0; layer < dim; layer++) {
        int bit = 1 << (dim - 1 - layer);
        for (int i = 0; i < width; ) {
            int batch = (width - i < POSE_LABEL_BATCH) ? (width - i) : POSE_LABEL_BATCH;
            int ni = i + batch;
            for (int k = 0; k < POSE_LABEL_BATCH && ni + k < width; k++) {
                __builtin_prefetch(LBLK(prev, (ni + k) & ~bit), 0, 0);
                __builtin_prefetch(LBLK(prev, (ni + k) | bit), 0, 0);
            }
            uint64_t ids[POSE_LABEL_BATCH];
            const uint8_t *pred0s[POSE_LABEL_BATCH], *pred1s[POSE_LABEL_BATCH];
            for (int j = 0; j < batch; j++) {
                int idx   = i + j;
                ids[j]    = e->next_id++;
                pred0s[j] = LBLK(prev, idx & ~bit);
                pred1s[j] = LBLK(prev, idx | bit);
            }
            pose_label_node_many((size_t)batch, 0, ids, pred0s, pred1s,
                                 e->seed, e->desc, LBLK(curr, i), e->algo);
            i += batch;
        }
        uint8_t *tmp = prev; prev = curr; curr = tmp;
    }

    memcpy(outputs, prev, (size_t)width * POSE_HASH_BYTES);
    e->arena_off = saved;
}

static void lc_merged_center_ingress(lemitter_t *e, int dim,
                                     const uint8_t *primary_inputs,
                                     const uint8_t *ingress_inputs,
                                     uint8_t *outputs)
{
    if (dim == 0) {
        uint8_t ig[POSE_HASH_BYTES];
        lc1(e, ig, LBLK(ingress_inputs, 0));
        lc2(e, LBLK(outputs, 0), LBLK(primary_inputs, 0), ig);
        return;
    }

    int width = 1 << dim;
    size_t saved = e->arena_off;
    uint8_t *ing_prev = lc_arena_alloc(e, (size_t)width);
    uint8_t *ing_curr = lc_arena_alloc(e, (size_t)width);
    uint8_t *ctr_prev = lc_arena_alloc(e, (size_t)width);
    uint8_t *ctr_curr = lc_arena_alloc(e, (size_t)width);

    /* ── Ingress butterfly ── */

    /* Layer 0: one predecessor from ingress_inputs[i]. */
    for (int i = 0; i < width; ) {
        int batch = (width - i < POSE_LABEL_BATCH) ? (width - i) : POSE_LABEL_BATCH;
        int ni = i + batch;
        for (int k = 0; k < POSE_LABEL_BATCH && ni + k < width; k++)
            __builtin_prefetch(LBLK(ingress_inputs, ni + k), 0, 0);
        uint64_t ids[POSE_LABEL_BATCH];
        const uint8_t *pred0s[POSE_LABEL_BATCH];
        for (int j = 0; j < batch; j++) {
            ids[j]    = e->next_id++;
            pred0s[j] = LBLK(ingress_inputs, i + j);
        }
        pose_label_node_many((size_t)batch, 0, ids, pred0s, NULL,
                             e->seed, e->desc, LBLK(ing_prev, i), e->algo);
        i += batch;
    }

    /* Layers 1..dim: two predecessors from the previous ingress layer. */
    for (int layer = 1; layer <= dim; layer++) {
        int bit = 1 << (dim - layer);
        for (int i = 0; i < width; ) {
            int batch = (width - i < POSE_LABEL_BATCH) ? (width - i) : POSE_LABEL_BATCH;
            int ni = i + batch;
            for (int k = 0; k < POSE_LABEL_BATCH && ni + k < width; k++) {
                __builtin_prefetch(LBLK(ing_prev, (ni + k) & ~bit), 0, 0);
                __builtin_prefetch(LBLK(ing_prev, (ni + k) | bit), 0, 0);
            }
            uint64_t ids[POSE_LABEL_BATCH];
            const uint8_t *pred0s[POSE_LABEL_BATCH], *pred1s[POSE_LABEL_BATCH];
            for (int j = 0; j < batch; j++) {
                int idx   = i + j;
                ids[j]    = e->next_id++;
                pred0s[j] = LBLK(ing_prev, idx & ~bit);
                pred1s[j] = LBLK(ing_prev, idx | bit);
            }
            pose_label_node_many((size_t)batch, 0, ids, pred0s, pred1s,
                                 e->seed, e->desc, LBLK(ing_curr, i), e->algo);
            i += batch;
        }
        uint8_t *tmp = ing_prev; ing_prev = ing_curr; ing_curr = tmp;
    }
    /* ing_prev now holds ingress layer dim labels. */

    /* ── Center butterfly ── */

    /* Layer 0: pred0 = primary_inputs[i] (lower id), pred1 = ing_prev[i]. */
    for (int i = 0; i < width; ) {
        int batch = (width - i < POSE_LABEL_BATCH) ? (width - i) : POSE_LABEL_BATCH;
        int ni = i + batch;
        for (int k = 0; k < POSE_LABEL_BATCH && ni + k < width; k++) {
            __builtin_prefetch(LBLK(primary_inputs, ni + k), 0, 0);
            __builtin_prefetch(LBLK(ing_prev, ni + k), 0, 0);
        }
        uint64_t ids[POSE_LABEL_BATCH];
        const uint8_t *pred0s[POSE_LABEL_BATCH], *pred1s[POSE_LABEL_BATCH];
        for (int j = 0; j < batch; j++) {
            int idx   = i + j;
            ids[j]    = e->next_id++;
            pred0s[j] = LBLK(primary_inputs, idx);
            pred1s[j] = LBLK(ing_prev, idx);
        }
        pose_label_node_many((size_t)batch, 0, ids, pred0s, pred1s,
                             e->seed, e->desc, LBLK(ctr_prev, i), e->algo);
        i += batch;
    }

    /* Layers 1..dim: two predecessors from the previous center layer.
     * The last layer writes directly into outputs[]. */
    for (int layer = 1; layer <= dim; layer++) {
        int bit = 1 << (dim - layer);
        uint8_t *dst = (layer == dim) ? outputs : ctr_curr;
        for (int i = 0; i < width; ) {
            int batch = (width - i < POSE_LABEL_BATCH) ? (width - i) : POSE_LABEL_BATCH;
            int ni = i + batch;
            for (int k = 0; k < POSE_LABEL_BATCH && ni + k < width; k++) {
                __builtin_prefetch(LBLK(ctr_prev, (ni + k) & ~bit), 0, 0);
                __builtin_prefetch(LBLK(ctr_prev, (ni + k) | bit), 0, 0);
            }
            uint64_t ids[POSE_LABEL_BATCH];
            const uint8_t *pred0s[POSE_LABEL_BATCH], *pred1s[POSE_LABEL_BATCH];
            for (int j = 0; j < batch; j++) {
                int idx   = i + j;
                ids[j]    = e->next_id++;
                pred0s[j] = LBLK(ctr_prev, idx & ~bit);
                pred1s[j] = LBLK(ctr_prev, idx | bit);
            }
            pose_label_node_many((size_t)batch, 0, ids, pred0s, pred1s,
                                 e->seed, e->desc, LBLK(dst, i), e->algo);
            i += batch;
        }
        if (layer < dim) {
            uint8_t *tmp = ctr_prev; ctr_prev = ctr_curr; ctr_curr = tmp;
        }
    }

    e->arena_off = saved;
}

static void lc_connected(lemitter_t *e, int level,
                         int width, const uint8_t *inputs, uint8_t *base_out);

static void lc_standalone(lemitter_t *e, int level, uint8_t *base_out)
{
    if (level == 0) {
        lc_source(e, LBLK(base_out, 0));
        return;
    }
    int half = 1 << (level - 1);
    size_t saved = e->arena_off;
    uint8_t *left_base  = lc_arena_alloc(e, (size_t)half);
    uint8_t *center_out = lc_arena_alloc(e, (size_t)half);
    uint8_t *right_base = lc_arena_alloc(e, (size_t)half);

    lc_standalone(e, level - 1, left_base);
    lc_connector(e, level - 1, left_base, center_out);
    lc_connected(e, level - 1, half, center_out, right_base);

    memcpy(base_out,                          left_base,  (size_t)half * POSE_HASH_BYTES);
    memcpy(LBLK(base_out, half),              right_base, (size_t)half * POSE_HASH_BYTES);

    e->arena_off = saved;
}

static void lc_connected(lemitter_t *e, int level,
                         int width, const uint8_t *inputs, uint8_t *base_out)
{
    if (level == 0) {
        lc1(e, LBLK(base_out, 0), LBLK(inputs, 0));
        return;
    }
    int half = width / 2;
    size_t saved = e->arena_off;
    uint8_t *left_base  = lc_arena_alloc(e, (size_t)half);
    uint8_t *center_out = lc_arena_alloc(e, (size_t)half);
    uint8_t *right_base = lc_arena_alloc(e, (size_t)half);

    lc_connected(e, level - 1, half, inputs, left_base);
    lc_merged_center_ingress(e, level - 1, left_base,
                             LBLK(inputs, half), center_out);
    lc_connected(e, level - 1, half, center_out, right_base);

    memcpy(base_out,             left_base,  (size_t)half * POSE_HASH_BYTES);
    memcpy(LBLK(base_out, half), right_base, (size_t)half * POSE_HASH_BYTES);

    e->arena_off = saved;
}

/*
 * Faithful in-place chunk labeler.  Produces, in `buf`, exactly the m output-set
 * labels of the graph (challenge rank r at buf[r·POSE_HASH_BYTES]) — byte-for-byte
 * identical to do_label's `out`.  The full scaffold is computed transiently in
 * the bounded label arena and is NOT persisted, so 100% of `buf`'s bytes are
 * challengeable output-set labels.
 *
 * buf must be chunk_blocks·POSE_HASH_BYTES bytes (the persisted super-chunk).
 * scratch must be pose_graph_scratch_bytes_inplace(m) bytes:
 *   left_base (2·half_w slots) + right_base (2·half_w slots)
 *   + label arena (POSE_ARENA_FACTOR·half_w slots, reused by both halves).
 *
 * With the identity challenge mapping, persisted slot r is output rank r.
 */
static int do_label_inplace(const uint8_t *seed, uint64_t m,
                             uint8_t *buf, uint8_t *scratch,
                             pose_hash_algo_t algo)
{
    if (m == 0 || m > POSE_GRAPH_MAX_M) return -1;

    int n        = pose_graph_parameter_n(m);
    int level    = n + 1;
    uint64_t half_w     = (uint64_t)1 << n;
    uint64_t retained   = m - half_w;
    uint64_t left_total = standalone_node_count(level);

    size_t arena_slots = (size_t)POSE_ARENA_FACTOR * (size_t)half_w;
    uint8_t *left_base  = scratch;
    uint8_t *right_base = left_base  + (size_t)(2 * half_w) * POSE_HASH_BYTES;
    uint8_t *arena      = right_base + (size_t)(2 * half_w) * POSE_HASH_BYTES;

    lemitter_t e;
    e.next_id   = 0;
    e.seed      = seed;
    e.arena     = arena;
    e.arena_cap = arena_slots;
    e.arena_off = 0;
    e.arena_hi  = 0;
    e.algo      = algo;
    pose_graph_descriptor(e.desc, m, (uint64_t)n);

    /* Left half occupies node IDs [0, left_total); the right half continues at
     * left_total — the exact id assignment do_label uses, so the labels match. */
    lc_standalone(&e, level, left_base);

    e.next_id   = left_total;
    e.arena_off = 0;   /* left half done; reuse the arena for the right half */
    lc_standalone(&e, level, right_base);

    /* Output set: ranks [0, half_w) from left_base[half_w + i],
     * ranks [half_w, m) from right_base[half_w + i]. */
    for (uint64_t i = 0; i < half_w; i++)
        memcpy(LBLK(buf, i), LBLK(left_base, half_w + i), POSE_HASH_BYTES);
    for (uint64_t i = 0; i < retained; i++)
        memcpy(LBLK(buf, half_w + i), LBLK(right_base, half_w + i), POSE_HASH_BYTES);

    return 0;
}

int pose_graph_label(const uint8_t *seed, size_t seed_len,
                     uint64_t m, uint8_t *out, pose_hash_algo_t algo)
{
    (void)seed_len;
    int n_m = pose_graph_parameter_n(m);
    size_t half_w_m = (size_t)1 << n_m;
    size_t total = scratch_node_count(m) * POSE_HASH_BYTES
                 + (size_t)4 * half_w_m * sizeof(uint64_t)
                 + (size_t)2 * POSE_ARENA_FACTOR * half_w_m * sizeof(uint64_t);
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -1;
    int rc = do_label(seed, m, out, buf, algo);
    free(buf);
    return rc;
}

int pose_graph_challenge(const uint8_t *seed, size_t seed_len,
                         uint64_t m, uint64_t challenge_idx,
                         uint8_t out[POSE_HASH_BYTES], pose_hash_algo_t algo)
{
    (void)seed_len;
    if (challenge_idx >= m) return -1;

    int n_m = pose_graph_parameter_n(m);
    size_t half_w_m = (size_t)1 << n_m;
    size_t scratch_total = scratch_node_count(m) * POSE_HASH_BYTES
                         + (size_t)4 * half_w_m * sizeof(uint64_t)
                         + (size_t)2 * POSE_ARENA_FACTOR * half_w_m * sizeof(uint64_t);
    uint8_t *all = (uint8_t *)malloc(m * POSE_HASH_BYTES);
    uint8_t *buf = (uint8_t *)malloc(scratch_total);
    if (!all || !buf) { free(all); free(buf); return -1; }

    int rc = do_label(seed, m, all, buf, algo);
    if (rc == 0)
        memcpy(out, all + challenge_idx * POSE_HASH_BYTES, POSE_HASH_BYTES);

    free(all);
    free(buf);
    return rc;
}

int pose_graph_edges(uint64_t m, pose_node_t **nodes_out, uint64_t *count_out)
{
    if (m == 0 || m > POSE_GRAPH_MAX_M) return -1;

    int n        = pose_graph_parameter_n(m);
    int level    = n + 1;
    uint64_t half_w   = (uint64_t)1 << n;
    uint64_t retained = m - half_w;
    uint64_t total    = 2 * standalone_node_count(level);

    pose_node_t *nodes = (pose_node_t *)malloc(total * sizeof(pose_node_t));
    if (!nodes) return -1;

    for (uint64_t i = 0; i < total; i++) {
        nodes[i].num_preds      = 0;
        nodes[i].pred[0]        = 0;
        nodes[i].pred[1]        = 0;
        nodes[i].challenge_rank = -1;
    }

    uint64_t *left_base  = (uint64_t *)malloc((size_t)(2 * half_w) * sizeof(uint64_t));
    uint64_t *right_base = (uint64_t *)malloc((size_t)(2 * half_w) * sizeof(uint64_t));
    /* Arena for two sequential emit_standalone calls (reused between calls). */
    size_t edge_arena_elems = (size_t)POSE_ARENA_FACTOR * (size_t)half_w;
    uint64_t *edges_arena   = (uint64_t *)malloc(edge_arena_elems * sizeof(uint64_t));
    if (!left_base || !right_base || !edges_arena) {
        free(nodes); free(left_base); free(right_base); free(edges_arena); return -1;
    }

    emitter_t e;
    e.next_id   = 0;
    e.labels    = NULL;
    e.nodes     = nodes;
    e.seed      = NULL;
    e.arena     = edges_arena;
    e.arena_cap = edge_arena_elems;
    e.arena_off = 0;
    e.algo      = POSE_HASH_BLAKE3;
    memset(e.desc, 0, sizeof(e.desc));

    emit_standalone(&e, level, left_base);   /* arena_off restored to 0 on return */
    emit_standalone(&e, level, right_base);  /* reuses arena; e.next_id continues */

    /* Mark challenge nodes: second half of left_base, first `retained` of right */
    for (uint64_t i = 0; i < half_w; i++)
        nodes[left_base[half_w + i]].challenge_rank = (int)i;
    for (uint64_t i = 0; i < retained; i++)
        nodes[right_base[half_w + i]].challenge_rank = (int)(half_w + i);

    *nodes_out = nodes;
    *count_out = e.next_id;

    free(edges_arena);
    free(left_base);
    free(right_base);
    return 0;
}

int pose_graph_slot_map(uint32_t chunk_blocks_in,
                        uint32_t *slot_out,
                        uint32_t *pool_size_out)
{
    if (!slot_out || !pool_size_out) return -1;
    uint32_t cb = resolve_chunk_blocks(chunk_blocks_in);
    if (cb == 0 || cb > POSE_GRAPH_MAX_M) return -1;

    pose_node_t *nodes = NULL;
    uint64_t count = 0;
    if (pose_graph_edges(cb, &nodes, &count) != 0) return -1;
    if (count == 0 || count > 0xFFFFFFFFull) { free(nodes); return -1; }

    /* Dependency level per node (every predecessor has a lower id, so a single
     * forward pass is a valid schedule) — identical to the host pass in
     * label_gpu_common.cu.
     * last_use[id] starts at the node's own level (a sink dies the level it is
     * born) and is raised to the level of its highest-level reader below. */
    int *level    = (int *)malloc(count * sizeof(int));
    int *last_use = (int *)malloc(count * sizeof(int));
    if (!level || !last_use) { free(nodes); free(level); free(last_use); return -1; }

    int maxlevel = 0;
    for (uint64_t id = 0; id < count; id++) {
        int np = nodes[id].num_preds, lv;
        if (np == 0) {
            lv = 0;
        } else if (np == 1) {
            lv = level[nodes[id].pred[0]] + 1;
        } else {
            int a = level[nodes[id].pred[0]];
            int b = level[nodes[id].pred[1]];
            lv = (a > b ? a : b) + 1;
        }
        level[id]    = lv;
        last_use[id] = lv;
        if (lv > maxlevel) maxlevel = lv;
    }
    for (uint64_t id = 0; id < count; id++) {
        int np = nodes[id].num_preds;
        int lv = level[id];
        for (int k = 0; k < np; k++) {
            uint64_t p = nodes[id].pred[k];
            if (last_use[p] < lv) last_use[p] = lv;
        }
    }

    /* Counting-sort node ids into per-level buckets twice: lvl_order groups by
     * birth level (allocation order); fre_order groups by last_use level (the
     * level after which the slot is reclaimed). */
    int Ln = maxlevel + 1;
    uint32_t *lvl_start = (uint32_t *)calloc((size_t)Ln + 1, sizeof(uint32_t));
    uint32_t *lvl_order = (uint32_t *)malloc(count * sizeof(uint32_t));
    uint32_t *fre_start = (uint32_t *)calloc((size_t)Ln + 1, sizeof(uint32_t));
    uint32_t *fre_order = (uint32_t *)malloc(count * sizeof(uint32_t));
    uint32_t *cur       = (uint32_t *)malloc((size_t)Ln * sizeof(uint32_t));
    uint32_t *free_stack = (uint32_t *)malloc(count * sizeof(uint32_t));
    if (!lvl_start || !lvl_order || !fre_start || !fre_order || !cur || !free_stack) {
        free(nodes); free(level); free(last_use);
        free(lvl_start); free(lvl_order); free(fre_start); free(fre_order);
        free(cur); free(free_stack);
        return -1;
    }

    for (uint64_t id = 0; id < count; id++) lvl_start[level[id] + 1]++;
    for (int i = 0; i < Ln; i++) lvl_start[i + 1] += lvl_start[i];
    for (int i = 0; i < Ln; i++) cur[i] = lvl_start[i];
    for (uint64_t id = 0; id < count; id++) lvl_order[cur[level[id]]++] = (uint32_t)id;

    for (uint64_t id = 0; id < count; id++) fre_start[last_use[id] + 1]++;
    for (int i = 0; i < Ln; i++) fre_start[i + 1] += fre_start[i];
    for (int i = 0; i < Ln; i++) cur[i] = fre_start[i];
    for (uint64_t id = 0; id < count; id++) fre_order[cur[last_use[id]]++] = (uint32_t)id;

    /* Free-slot stack: the pool grows (next_new) only when no slot is free, so
     * next_new equals the peak simultaneously-live slot count.  Allocation for a
     * level runs BEFORE the level's frees, so a node never reuses a predecessor
     * slot that is still being read this level.
     *
     * EVERY node gets a real pool slot — including output-set nodes.  Output
     * nodes are NOT pure sinks (they are read by later scaffold nodes), so they
     * must remain readable from scratch by their successors; the per-chunk
     * kernels additionally copy each output node to the persisted region at the
     * moment it is computed (before its slot can be recycled). */
    uint32_t next_new = 0, free_top = 0;
    for (int lv = 0; lv < Ln; lv++) {
        for (uint32_t i = lvl_start[lv]; i < lvl_start[lv + 1]; i++) {
            uint32_t id = lvl_order[i];
            slot_out[id] = (free_top > 0) ? free_stack[--free_top] : next_new++;
        }
        for (uint32_t i = fre_start[lv]; i < fre_start[lv + 1]; i++) {
            uint32_t id = fre_order[i];
            free_stack[free_top++] = slot_out[id];
        }
    }

    *pool_size_out = next_new;

#ifndef NDEBUG
    {
        uint64_t half_w = (uint64_t)1 << pose_graph_parameter_n(cb);
        assert((uint64_t)next_new <= 8u * half_w);
    }
#endif

    free(nodes); free(level); free(last_use);
    free(lvl_start); free(lvl_order); free(fre_start); free(fre_order);
    free(cur); free(free_stack);
    return 0;
}

/* Spawn a worker thread with a 1 MiB stack.  Callers that run under
 * mlockall(MCL_FUTURE) would otherwise fault the default 8 MiB stack straight
 * into physical RAM for every worker of every pool. */
static int create_worker_thread(pthread_t *t, void *(*fn)(void *), void *arg)
{
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return -1;
    pthread_attr_setstacksize(&attr, 1u << 20); /* 1 MiB */
    int rc = pthread_create(t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc;
}

int pose_graph_label_region(const uint8_t *session_seed, size_t seed_len,
                             void *region, size_t region_len,
                             uint64_t region_block_offset,
                             pose_hash_algo_t algo,
                             uint32_t chunk_blocks)
{
    chunk_blocks = resolve_chunk_blocks(chunk_blocks);
    if (!region || region_len == 0 || (region_len % POSE_HASH_BYTES) != 0)
        return -1;

    /*
     * One scratch buffer per thread.  Chunks are processed POSE_REGION_THREADS
     * at a time: threads 0..n-2 run in parallel, the calling thread runs
     * chunk n-1, then all are joined before the next batch starts.
     */
    size_t scratch_bytes = pose_graph_scratch_bytes(chunk_blocks);
    uint8_t *scratch[POSE_REGION_THREADS];
    for (int i = 0; i < POSE_REGION_THREADS; i++) {
        scratch[i] = (uint8_t *)malloc(scratch_bytes);
        if (!scratch[i]) {
            for (int j = 0; j < i; j++) free(scratch[j]);
            return -1;
        }
    }

    uint8_t *p = (uint8_t *)region;
    const uint64_t total_blocks = region_len / POSE_HASH_BYTES;
    uint64_t block = 0;
    int rc = 0;

    chunk_arg_t args[POSE_REGION_THREADS];
    pthread_t   threads[POSE_REGION_THREADS];

    while (block < total_blocks && rc == 0) {
        int n = 0;

        while (n < POSE_REGION_THREADS && block < total_blocks) {
            uint64_t this_blocks = total_blocks - block;
            if (this_blocks > chunk_blocks)
                this_blocks = chunk_blocks;

            uint64_t chunk_index = (region_block_offset + block) / chunk_blocks;
            pose_chunk_seed(args[n].cseed, session_seed, seed_len, chunk_index);
            args[n].m       = this_blocks;
            args[n].out     = p + block * POSE_HASH_BYTES;
            args[n].scratch = scratch[n];
            args[n].rc      = 0;
            args[n].algo    = algo;
            args[n].inplace = 0;
            args[n].wt_sink = NULL;

            block += this_blocks;
            n++;
        }

        /* Launch threads for chunks 0..n-2; run chunk n-1 on the calling thread. */
        int launched = 0;
        for (int i = 0; i < n - 1; i++) {
            if (create_worker_thread(&threads[i], chunk_thread_fn, &args[i]) != 0) {
                rc = -1;
                break;
            }
            launched++;
        }

        if (rc == 0)
            chunk_thread_fn(&args[n - 1]);

        for (int i = 0; i < launched; i++)
            pthread_join(threads[i], NULL);

        if (rc == 0) {
            for (int i = 0; i < n; i++) {
                if (args[i].rc != 0) { rc = args[i].rc; break; }
            }
        }
    }

    for (int i = 0; i < POSE_REGION_THREADS; i++) free(scratch[i]);
    return rc;
}

size_t pose_graph_scratch_bytes(uint32_t chunk_blocks)
{
    /* label buffer + two base-output arrays (2 × 2 × half_w uint64_t)
     * + two per-emitter arenas (POSE_ARENA_FACTOR × half_w each) */
    chunk_blocks = resolve_chunk_blocks(chunk_blocks);
    int n_max      = pose_graph_parameter_n(chunk_blocks);
    uint64_t half_w = (uint64_t)1 << n_max;
    size_t labels  = scratch_node_count(chunk_blocks) * POSE_HASH_BYTES;
    size_t bases   = (size_t)4 * (size_t)half_w * sizeof(uint64_t);
    size_t arenas  = 2 * (size_t)POSE_ARENA_FACTOR * (size_t)half_w * sizeof(uint64_t);
    return labels + bases + arenas;
}

/* ── Thread pool for chunk-parallel labeling ──────────────────────────────── */

typedef struct {
    pose_graph_pool_t *pool;
    int                slot;
} pool_worker_ctx_t;

struct pose_graph_pool {
    pthread_t         threads[POSE_REGION_THREADS - 1];
    pool_worker_ctx_t ctx[POSE_REGION_THREADS - 1]; /* stable storage for thread args */
    pthread_mutex_t   mutex;
    pthread_cond_t    work_cond;   /* broadcast when jobs[] updated or shutdown set */
    pthread_cond_t    done_cond;   /* signaled when n_done reaches n_expected */
    chunk_arg_t      *jobs[POSE_REGION_THREADS - 1]; /* NULL = idle */
    int               n_done;
    int               n_expected;
    int               shutdown;
};

static void *pool_worker_fn(void *arg)
{
    pool_worker_ctx_t *ctx  = (pool_worker_ctx_t *)arg;
    pose_graph_pool_t *pool = ctx->pool;
    int slot = ctx->slot;

    pthread_mutex_lock(&pool->mutex);
    for (;;) {
        while (!pool->shutdown && pool->jobs[slot] == NULL)
            pthread_cond_wait(&pool->work_cond, &pool->mutex);
        if (pool->shutdown)
            break;
        chunk_arg_t *job = pool->jobs[slot];
        pool->jobs[slot] = NULL;
        pthread_mutex_unlock(&pool->mutex);

        chunk_thread_fn(job);

        pthread_mutex_lock(&pool->mutex);
        if (++pool->n_done >= pool->n_expected)
            pthread_cond_signal(&pool->done_cond);
    }
    pthread_mutex_unlock(&pool->mutex);
    return NULL;
}

pose_graph_pool_t *pose_graph_pool_create(void)
{
    pose_graph_pool_t *pool = (pose_graph_pool_t *)calloc(1, sizeof(*pool));
    if (!pool) return NULL;

    if (pthread_mutex_init(&pool->mutex, NULL) != 0
     || pthread_cond_init(&pool->work_cond, NULL) != 0
     || pthread_cond_init(&pool->done_cond,  NULL) != 0) {
        free(pool);
        return NULL;
    }

    for (int i = 0; i < POSE_REGION_THREADS - 1; i++) {
        pool->ctx[i].pool = pool;
        pool->ctx[i].slot = i;
        pool->jobs[i]     = NULL;
        if (create_worker_thread(&pool->threads[i], pool_worker_fn, &pool->ctx[i]) != 0) {
            pthread_mutex_lock(&pool->mutex);
            pool->shutdown = 1;
            pthread_cond_broadcast(&pool->work_cond);
            pthread_mutex_unlock(&pool->mutex);
            for (int j = 0; j < i; j++)
                pthread_join(pool->threads[j], NULL);
            pthread_mutex_destroy(&pool->mutex);
            pthread_cond_destroy(&pool->work_cond);
            pthread_cond_destroy(&pool->done_cond);
            free(pool);
            return NULL;
        }
    }
    return pool;
}

void pose_graph_pool_destroy(pose_graph_pool_t *pool)
{
    if (!pool) return;
    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->work_cond);
    pthread_mutex_unlock(&pool->mutex);
    for (int i = 0; i < POSE_REGION_THREADS - 1; i++)
        pthread_join(pool->threads[i], NULL);
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->work_cond);
    pthread_cond_destroy(&pool->done_cond);
    free(pool);
}

int pose_graph_label_region_pooled(const uint8_t *session_seed, size_t seed_len,
                                    void *region, size_t region_len,
                                    uint64_t region_block_offset,
                                    uint8_t *scratch[POSE_REGION_THREADS],
                                    pose_graph_pool_t *pool,
                                    pose_hash_algo_t algo,
                                    uint32_t chunk_blocks)
{
    chunk_blocks = resolve_chunk_blocks(chunk_blocks);
    if (!region || region_len == 0 || (region_len % POSE_HASH_BYTES) != 0)
        return -1;

    uint8_t *p = (uint8_t *)region;
    const uint64_t total_blocks = region_len / POSE_HASH_BYTES;
    uint64_t block = 0;
    int rc = 0;

    chunk_arg_t args[POSE_REGION_THREADS];

    while (block < total_blocks && rc == 0) {
        int n = 0;

        while (n < POSE_REGION_THREADS && block < total_blocks) {
            uint64_t this_blocks = total_blocks - block;
            if (this_blocks > chunk_blocks)
                this_blocks = chunk_blocks;

            uint64_t chunk_index = (region_block_offset + block) / chunk_blocks;
            pose_chunk_seed(args[n].cseed, session_seed, seed_len, chunk_index);
            args[n].m       = this_blocks;
            args[n].out     = p + block * POSE_HASH_BYTES;
            args[n].scratch = scratch[n];
            args[n].rc      = 0;
            args[n].algo    = algo;
            args[n].inplace = 0;
            args[n].wt_sink = NULL;

            block += this_blocks;
            n++;
        }

        /* Submit chunks 0..n-2 to pool workers; run chunk n-1 on the calling thread. */
        if (n > 1) {
            pthread_mutex_lock(&pool->mutex);
            pool->n_done     = 0;
            pool->n_expected = n - 1;
            for (int i = 0; i < n - 1; i++)
                pool->jobs[i] = &args[i];
            pthread_cond_broadcast(&pool->work_cond);
            pthread_mutex_unlock(&pool->mutex);
        }

        chunk_thread_fn(&args[n - 1]);

        if (n > 1) {
            pthread_mutex_lock(&pool->mutex);
            while (pool->n_done < pool->n_expected)
                pthread_cond_wait(&pool->done_cond, &pool->mutex);
            pthread_mutex_unlock(&pool->mutex);
        }

        for (int i = 0; i < n; i++) {
            if (args[i].rc != 0) { rc = args[i].rc; break; }
        }
    }

    return rc;
}

int pose_graph_label_region_ex(const uint8_t *session_seed, size_t seed_len,
                                void *region, size_t region_len,
                                uint64_t region_block_offset,
                                uint8_t *scratch[POSE_REGION_THREADS],
                                pose_hash_algo_t algo,
                                uint32_t chunk_blocks)
{
    chunk_blocks = resolve_chunk_blocks(chunk_blocks);
    if (!region || region_len == 0 || (region_len % POSE_HASH_BYTES) != 0)
        return -1;

    uint8_t *p = (uint8_t *)region;
    const uint64_t total_blocks = region_len / POSE_HASH_BYTES;
    uint64_t block = 0;
    int rc = 0;

    chunk_arg_t args[POSE_REGION_THREADS];
    pthread_t   threads[POSE_REGION_THREADS];

    while (block < total_blocks && rc == 0) {
        int n = 0;

        while (n < POSE_REGION_THREADS && block < total_blocks) {
            uint64_t this_blocks = total_blocks - block;
            if (this_blocks > chunk_blocks)
                this_blocks = chunk_blocks;

            uint64_t chunk_index = (region_block_offset + block) / chunk_blocks;
            pose_chunk_seed(args[n].cseed, session_seed, seed_len, chunk_index);
            args[n].m       = this_blocks;
            args[n].out     = p + block * POSE_HASH_BYTES;
            args[n].scratch = scratch[n];
            args[n].rc      = 0;
            args[n].algo    = algo;
            args[n].inplace = 0;
            args[n].wt_sink = NULL;

            block += this_blocks;
            n++;
        }

        int launched = 0;
        for (int i = 0; i < n - 1; i++) {
            if (create_worker_thread(&threads[i], chunk_thread_fn, &args[i]) != 0) {
                rc = -1;
                break;
            }
            launched++;
        }

        if (rc == 0)
            chunk_thread_fn(&args[n - 1]);

        for (int i = 0; i < launched; i++)
            pthread_join(threads[i], NULL);

        if (rc == 0) {
            for (int i = 0; i < n; i++) {
                if (args[i].rc != 0) { rc = args[i].rc; break; }
            }
        }
    }

    return rc;
}

/* ── In-place public helpers ─────────────────────────────────────────────── */

size_t pose_graph_scratch_bytes_inplace(uint32_t chunk_blocks)
{
    /* left_base (2·half_w) + right_base (2·half_w) + transient label arena
     * (POSE_ARENA_FACTOR·half_w), all in 32-byte label slots. */
    chunk_blocks = resolve_chunk_blocks(chunk_blocks);
    int n = pose_graph_parameter_n(chunk_blocks);
    uint64_t half_w = (uint64_t)1 << n;
    size_t bases = (size_t)4 * half_w * POSE_HASH_BYTES;
    size_t arena = (size_t)POSE_ARENA_FACTOR * half_w * POSE_HASH_BYTES;
    return bases + arena;
}

uint64_t pose_graph_super_chunk_blocks(uint32_t chunk_blocks)
{
    /* Paper-faithful: a super-chunk persists exactly the m = chunk_blocks
     * output-set labels.  The scaffold is transient, never persisted. */
    return resolve_chunk_blocks(chunk_blocks);
}

uint64_t pose_graph_scaffold_node_count(uint32_t chunk_blocks)
{
    /* Full transient scaffold size (every emitted node) for one chunk.  Used by
     * the GPU labeler to size its transient HBM work buffer. */
    return scratch_node_count(resolve_chunk_blocks(chunk_blocks));
}

/*
 * Fill node_ids_out[0..chunk_blocks-1] with the physical block index of each
 * challenge rank.  With the faithful in-place prover the persisted super-chunk
 * holds the output set in rank order, so the mapping is the identity:
 * challenge rank r lives at physical block r.  (scratch is unused, kept for
 * ABI compatibility.)
 */
void pose_graph_challenge_node_ids(uint32_t chunk_blocks,
                                    uint64_t *node_ids_out, uint8_t *scratch)
{
    (void)scratch;
    chunk_blocks = resolve_chunk_blocks(chunk_blocks);
    if (chunk_blocks == 0 || chunk_blocks > POSE_GRAPH_MAX_M) return;

    for (uint64_t i = 0; i < chunk_blocks; i++)
        node_ids_out[i] = i;
}

/*
 * In-place region labeler.  Each physical super-chunk of
 * pose_graph_super_chunk_blocks(chunk_blocks)×POSE_HASH_BYTES bytes
 * is labeled directly in `region` (all scratch_node_count nodes written to
 * physical memory).  scratch[i] must be pose_graph_scratch_bytes_inplace(chunk_blocks)
 * bytes (base arrays + arenas only; no labels buffer needed on heap).
 *
 * region_len must be a multiple of super_chunk_bytes.
 */
int pose_graph_label_region_pooled_inplace(
        const uint8_t *session_seed, size_t seed_len,
        void *region, size_t region_len,
        uint64_t region_block_offset,
        uint8_t *scratch[POSE_REGION_THREADS],
        pose_graph_pool_t *pool,
        pose_hash_algo_t algo,
        uint32_t chunk_blocks)
{
    chunk_blocks = resolve_chunk_blocks(chunk_blocks);
    if (!region || region_len == 0 || (region_len % POSE_HASH_BYTES) != 0)
        return -1;

    /* A super-chunk now persists exactly chunk_blocks output labels. */
    uint64_t super_blocks = (uint64_t)chunk_blocks;
    size_t   super_bytes  = (size_t)super_blocks * POSE_HASH_BYTES;

    if (region_len < super_bytes)
        return 0; /* nothing to label — region smaller than one super-chunk */

    uint8_t *p            = (uint8_t *)region;
    const uint64_t total_physical = region_len / POSE_HASH_BYTES;
    uint64_t block = 0;
    int rc = 0;

    chunk_arg_t args[POSE_REGION_THREADS];

    while (block + super_blocks <= total_physical && rc == 0) {
        int n = 0;

        while (n < POSE_REGION_THREADS && block + super_blocks <= total_physical) {
            /*
             * chunk_index must match the formula pose_graph_label_region uses
             * so that the verifier (which calls pose_graph_label_region to
             * precompute expected labels) derives the same chunk seed.
             *
             * The verifier maps challenge block j*chunk_blocks to chunk_index:
             *   (region_block_offset + j * chunk_blocks) / chunk_blocks
             *
             * block is the physical-block offset within this region, measured
             * in super_blocks steps.  j = block / super_blocks.
             */
            uint64_t j           = block / super_blocks;
            uint64_t chunk_index = (region_block_offset + j * (uint64_t)chunk_blocks)
                                   / (uint64_t)chunk_blocks;
            pose_chunk_seed(args[n].cseed, session_seed, seed_len, chunk_index);
            args[n].m       = (uint64_t)chunk_blocks;
            args[n].out     = p + block * POSE_HASH_BYTES; /* super_bytes in phys mem */
            args[n].scratch = scratch[n];
            args[n].rc      = 0;
            args[n].algo    = algo;
            args[n].inplace = 1;
            args[n].wt_sink = NULL;

            block += super_blocks;
            n++;
        }

        if (n == 0) break;

        if (n > 1) {
            pthread_mutex_lock(&pool->mutex);
            pool->n_done     = 0;
            pool->n_expected = n - 1;
            for (int i = 0; i < n - 1; i++)
                pool->jobs[i] = &args[i];
            pthread_cond_broadcast(&pool->work_cond);
            pthread_mutex_unlock(&pool->mutex);
        }

        chunk_thread_fn(&args[n - 1]);

        if (n > 1) {
            pthread_mutex_lock(&pool->mutex);
            while (pool->n_done < pool->n_expected)
                pthread_cond_wait(&pool->done_cond, &pool->mutex);
            pthread_mutex_unlock(&pool->mutex);
        }

        for (int i = 0; i < n; i++) {
            if (args[i].rc != 0) { rc = args[i].rc; break; }
        }
    }

    return rc;
}

int pose_graph_label_disk_wave(
        const uint8_t *session_seed, size_t seed_len,
        uint8_t *const slots[POSE_REGION_THREADS],
        uint8_t *const scratch[POSE_REGION_THREADS],
        pose_graph_pool_t *pool,
        pose_hash_algo_t algo,
        uint32_t chunk_blocks,
        int n_this,
        uint64_t super_index_base,
        uint64_t region_block_offset,
        uint64_t byte_start,
        size_t write_len,
        pose_label_sink_fn sink,
        void *sink_ctx)
{
    chunk_blocks = resolve_chunk_blocks(chunk_blocks);
    if (n_this < 1 || n_this > POSE_REGION_THREADS || !sink)
        return -1;

    chunk_arg_t args[POSE_REGION_THREADS];

    for (int i = 0; i < n_this; i++) {
        uint64_t j = super_index_base + (uint64_t)i;
        /* Match the chunk-seed derivation pose_graph_label_region uses so the
         * verifier recomputes identical labels. */
        uint64_t chunk_index = (region_block_offset + j * (uint64_t)chunk_blocks)
                               / (uint64_t)chunk_blocks;
        pose_chunk_seed(args[i].cseed, session_seed, seed_len, chunk_index);
        args[i].m       = (uint64_t)chunk_blocks;
        args[i].out     = slots[i];
        args[i].scratch = scratch[i];
        args[i].rc      = 0;
        args[i].algo    = algo;
        args[i].inplace = 1;
        args[i].wt_sink = sink;
        args[i].wt_ctx  = sink_ctx;
        args[i].wt_off  = byte_start + j * (uint64_t)write_len;
        args[i].wt_len  = write_len;
    }

    /* Dispatch jobs 0..n_this-2 to pool workers; run the last on this thread.
     * Each worker labels its slot then streams it to the sink, so the whole wave
     * is committed by the time the barrier below releases and the slots can be
     * reused by the next wave. */
    if (n_this > 1) {
        pthread_mutex_lock(&pool->mutex);
        pool->n_done     = 0;
        pool->n_expected = n_this - 1;
        for (int i = 0; i < n_this - 1; i++)
            pool->jobs[i] = &args[i];
        pthread_cond_broadcast(&pool->work_cond);
        pthread_mutex_unlock(&pool->mutex);
    }

    chunk_thread_fn(&args[n_this - 1]);

    if (n_this > 1) {
        pthread_mutex_lock(&pool->mutex);
        while (pool->n_done < pool->n_expected)
            pthread_cond_wait(&pool->done_cond, &pool->mutex);
        pthread_mutex_unlock(&pool->mutex);
    }

    for (int i = 0; i < n_this; i++)
        if (args[i].rc != 0) return args[i].rc;
    return 0;
}
