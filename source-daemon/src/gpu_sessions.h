/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#ifndef POSE_SOURCE_GPU_SESSIONS_H
#define POSE_SOURCE_GPU_SESSIONS_H
/*
 * The source-daemon's own sessions on its GPU targets (gpu_targets.h): the
 * HBM analogue of a sink's RAM session, except that nothing is streamed —
 * the labels are computed where they are stored, by pose-common's CUDA
 * labeler (gpu_session.h), byte-identical to the CPU path.  One session per
 * GPU; the session covers [0, size) of the device's labelable HBM as the
 * inventory advertised it (identity map, block i at hbm[i * 32]), so its
 * Coverage is the single region {0, size / 32}.
 *
 * Built without TARGET_GPU every init is rejected and nothing is owned.
 */

#include "sink_proto.h"

#include <stddef.h>
#include <stdint.h>

/* SessionInit on a GPU target -> InitAck.  Checks the target (one session per
 * device), algo (BLAKE3 only), base (0), chunk_blocks and size (whole
 * super-chunks) and that the HBM holds the session plus the labeler's own
 * scratch, then allocates the buffer.  Returns 1 if accepted (ack OK), 0 if
 * rejected (ack carries the reason). */
int gpu_sessions_init(const sink_session_init_t *init, sink_init_ack_t *ack);

/* Progress hook: bytes of the session labeled so far, from the labeling
 * thread, after every batch of super-chunks.  Return -1 to abort. */
typedef int (*gpu_sessions_progress_fn)(void *ctx, uint32_t session, uint64_t done_bytes);

/* Label the accepted session's whole buffer in place.  Blocks until done;
 * prints a timing line.  Returns 0, or -1 with the reason in why. */
int gpu_sessions_label(uint32_t session, const sink_session_init_t *init,
                       gpu_sessions_progress_fn progress, void *ctx, char *why, size_t why_cap);

/* The labeled session's coverage: one region, base 0.  Returns 0, or -1. */
int gpu_sessions_coverage(uint32_t session, sink_coverage_t *cov, char *why, size_t why_cap);

/* Whether `session` is one of this machine's GPU sessions. */
int gpu_sessions_owns(uint32_t session);

/* Challenge -> label: copies block_index's label out of HBM.  A bad session
 * or index fills label with zeros and returns -1 with the reason in why. */
int gpu_sessions_challenge(uint32_t session, uint64_t block_index,
                           uint8_t label[SINK_LABEL_BYTES], char *why, size_t why_cap);

#endif /* POSE_SOURCE_GPU_SESSIONS_H */
