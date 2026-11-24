/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#ifndef POSE_SINK_SESSION_H
#define POSE_SINK_SESSION_H
/*
 * Label sessions on this machine's own targets: the sink side of SessionInit,
 * LabelData, StreamEnd and Challenge from sink_proto.h.  One table of at most
 * SINK_MAX_SESSIONS, indexed by the session number the source chose; each
 * holds a mapped RAM range or an open disk until Finish.
 *
 * RAM sessions store into the /dev/mem mapping and flush the CPU caches at
 * StreamEnd.  Disk sessions open the device O_DIRECT|O_EXCL (a disk that is
 * mounted or claimed is a rejected init), stage LabelData into 4 MiB aligned
 * blocks written in order, write the tail and fsync at StreamEnd, and answer
 * a Challenge by reading the label's sector back with O_DIRECT.  Labels lie
 * contiguously at base + index * 32 in both.
 */

#include "sink_proto.h"

/* Validate a SessionInit against the inventory this machine reported and, if
 * it is acceptable, map the RAM range (or open the disk O_DIRECT) and record
 * the session.  ack is always filled in: SINK_STATUS_OK, or SINK_STATUS_ERROR
 * with the reason, which the caller sends as the InitAck.  A rejected init
 * leaves nothing mapped or open, so the connection may carry on.  Returns 1 if
 * the session was accepted, 0 if not. */
int  sink_session_init(const sink_inventory_t *inv, const sink_session_init_t *init,
                       sink_init_ack_t *ack);

/* Store one LabelData frame: the session must be streaming, the frame's
 * offset must equal the bytes received so far (the sink never seeks), and it
 * must fit in what is left of the session.  A violation is a protocol error:
 * returns -1 with the reason in why, and the caller ends the connection (the
 * stream has no per-frame reply to carry a soft rejection).  Returns 0 when
 * the bytes are stored. */
int  sink_session_label_data(const sink_label_data_t *hdr, const uint8_t *data, size_t len,
                             char *why, size_t why_cap);

/* End a session's stream: it must be streaming and complete (received ==
 * size), else a protocol error (-1, reason in why).  Flushes the range out of
 * the CPU caches, marks the session stored, and fills cov with its coverage
 * map, which the caller sends as the Coverage reply: one run,
 * (base, size / SINK_LABEL_BYTES), the form the verifier consumes.  Returns 0. */
int  sink_session_stream_end(uint32_t session, sink_coverage_t *cov, char *why, size_t why_cap);

/* Answer a Challenge: copy the label at base + block_index * SINK_LABEL_BYTES
 * of a stored session into label and return 0.  An unknown session, one whose
 * stream has not ended, an index past the end, or a disk read that fails is
 * not a protocol error: label is filled with zeros, the reason
 * goes to why, and -1 is returned so the caller can log it.  The caller sends
 * label either way, so the verifier sees a mismatch and the run carries on. */
int  sink_session_challenge(uint32_t session, uint64_t block_index,
                            uint8_t label[SINK_LABEL_BYTES], char *why, size_t why_cap);

/* Number of sessions accepted so far. */
int  sink_session_count(void);

/* Unmap and close every session, printing how much of each was received
 * (nothing is zeroed yet: that comes with the Finish work). */
void sink_session_close_all(void);

#endif /* POSE_SINK_SESSION_H */
