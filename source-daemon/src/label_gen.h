/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#ifndef POSE_SOURCE_LABEL_GEN_H
#define POSE_SOURCE_LABEL_GEN_H
/*
 * Label generation for a session: the source-daemon computes the faithful
 * in-place labels (only the output set O(G), rank r stored at block r) for
 * [base, base + size) from the session's seed, algo and chunk_blocks, and
 * hands them to a delivery callback in strictly increasing offset order.
 *
 * Delivery is the only thing that differs between a session on this
 * machine's own target (store into the mapped range through
 * sink_session_label_data) and one on a downstream sink (send LabelData
 * frames), so the generator does not know which it is feeding.
 *
 * Generation runs in waves of up to pose_region_threads() super-chunks, one
 * per pool thread (pose_graph_label_disk_wave).  Workers finish out of order;
 * a gate holds each finished super-chunk until every lower offset has been
 * delivered.  Each pool worker owns one fixed job slot, so the super-chunk at
 * the lowest pending offset is always running and never waits: the gate
 * cannot deadlock.  A delivery failure wakes every waiter and fails the wave.
 *
 * Labels match the verifier's recomputation (and pose-label) because the
 * wave labeler is called with region_block_offset = base / 32, the placement
 * contract in sink_proto.h.
 */

#include "sink_proto.h"

#include <stddef.h>
#include <stdint.h>

/* Hand `len` bytes of labels, the session's bytes [offset, offset + len), to
 * wherever they are stored.  Called in strictly increasing offset order, one
 * call at a time, each a whole super-chunk (chunk_blocks * SINK_LABEL_BYTES).
 * buf is 64-byte aligned.  Return 0, or -1 with the reason in why. */
typedef int (*label_deliver_fn)(void *ctx, uint32_t session, uint64_t offset,
                                const uint8_t *buf, size_t len, char *why, size_t why_cap);

/* Check what only a label generator cares about (algo, chunk_blocks a power
 * of two within the graph's limit, size a whole number of super-chunks),
 * choose the generator for this session — a GPU when built with TARGET_GPU,
 * one is present and the algo is BLAKE3, unless POSE_LABEL_GEN says
 * otherwise; the CPU thread pool else — and make sure its buffers for this
 * chunk_blocks are allocated.  Call before accepting the session, so a bad
 * parameter or a shortage of memory is a rejected InitAck, not a failure
 * after the session is accepted.  Returns 0, or -1 with the reason in why. */
int label_gen_prepare(const sink_session_init_t *init, char *why, size_t why_cap);

/* Generate every label of a session already passed by label_gen_prepare and
 * deliver them in order.  Blocks until done; prints progress.  Returns 0, or
 * -1 with the reason in why (labeling or delivery failed). */
int label_gen_run(const sink_session_init_t *init, label_deliver_fn deliver, void *ctx,
                  char *why, size_t why_cap);

/* Free the per-thread buffers and stop the thread pool.  Not called on the
 * way out (Finish or failure): the buffers held seed-derived scaffold labels,
 * and while they stay mapped the PID 1 self-wipe zeroes them; freed, large
 * allocations would go back to the kernel unwiped. */
void label_gen_release(void);

#endif /* POSE_SOURCE_LABEL_GEN_H */
