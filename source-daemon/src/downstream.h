/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#ifndef POSE_DOWNSTREAM_H
#define POSE_DOWNSTREAM_H
/*
 * The source-daemon's downstream sinks: the sink-daemons (or source-daemons)
 * it reaches over TCP, each under a node name chosen in its configuration.
 * A configured sink is an expected one: failing to reach it is an error that
 * stops the erasure, never a smaller inventory.
 */

#include "sink_proto.h"

#include <stddef.h>
#include <stdint.h>

#define DOWNSTREAM_MAX          8
#define DOWNSTREAM_CONNECT_WAIT 60   /* seconds to wait for a sink still booting */

/* Add one sink from "NODE=IPV4:PORT" (the -s option).  Returns 0, or -1 with a
 * message printed. */
int downstream_add(const char *spec);

/* Add every sink named by a POSE_SINK_<NODE>=IPV4:PORT environment variable,
 * which is how a PID 1 boot configures them (pose.env on the kernel command
 * line).  Returns 0, or -1 with a message printed. */
int downstream_add_from_env(void);

int downstream_count(void);

/* Ask every downstream sink for its inventory (connecting first, with up to
 * wait_s of retries, if not yet connected) and append each target to inv
 * under that sink's node name.  Returns 0, or -1 with the reason in why. */
int downstream_inventory(sink_inventory_t *inv, int wait_s, char *why, size_t why_cap);

/* ── sessions on a downstream sink (relayed) ──────────────────────────────
 *
 * The source generates the labels and streams them down; the sink stores
 * them and answers challenges.  Session numbers are the verifier's and pass
 * through unchanged, so each downstream session is recorded against the sink
 * that holds it.  Every call below returns -1 with the reason in why when the
 * sink is unreachable, the connection is lost, the sink sends an Error, or
 * the reply is not the expected frame: the caller fails the run. */

/* Forward a SessionInit whose node names a downstream sink.  The sink is the
 * one named by the node's first component, and the forwarded init carries the
 * node minus that component ("dpu0" -> "", "dpu0/bmc0" -> "bmc0").  ack gets
 * the sink's InitAck, accepted or not; an accepted session is recorded.
 * Returns 0 (ack filled in), or -1. */
int downstream_session_init(const sink_session_init_t *init, sink_init_ack_t *ack,
                            int wait_s, char *why, size_t why_cap);

/* 1 if session was accepted by a downstream sink, else 0. */
int downstream_owns(uint32_t session);

/* label_deliver_fn (label_gen.h): send the bytes to the sink that holds the
 * session as LabelData frames of at most SINK_LABEL_DATA_MAX bytes. */
int downstream_deliver(void *ctx, uint32_t session, uint64_t offset,
                       const uint8_t *buf, size_t len, char *why, size_t why_cap);

/* Send StreamEnd for a downstream session and fill cov with the sink's
 * Coverage reply (which the source relays unchanged). */
int downstream_stream_end(uint32_t session, sink_coverage_t *cov, char *why, size_t why_cap);

/* Relay one Challenge for a downstream session and return the sink's Label
 * (zeros if the sink considered the challenge bad: that is still a Label). */
int downstream_challenge(uint32_t session, uint64_t block_index,
                         uint8_t label[SINK_LABEL_BYTES], char *why, size_t why_cap);

/* Send Finish (same action and reason) to every connected downstream sink,
 * then close them all. */
void downstream_finish(uint8_t action, const char *reason);

/* Close every downstream connection without a Finish (error paths: each sink
 * takes its own fail-safe on the disconnect). */
void downstream_close_all(void);

#endif /* POSE_DOWNSTREAM_H */
