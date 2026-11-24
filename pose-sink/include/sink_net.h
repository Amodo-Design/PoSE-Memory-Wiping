/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#ifndef POSE_SINK_NET_H
#define POSE_SINK_NET_H
/*
 * Plain-TCP transport for the sink protocol: the 5-byte frame header
 * ([u8 type][u32 len]) of sink_proto.h over IPv4 sockets.  A sink listens and
 * accepts its source; a source-daemon also connects to its downstream sinks,
 * so every call takes the socket it works on.  No TLS: see sink_proto.h.
 */

#include <stdint.h>
#include <stddef.h>

/* TCP keepalive on every connection (accepted and connected).  There is no
 * application-level idle timeout: a peer that is busy (a source generating
 * labels for hours) still answers keepalive probes from its kernel, while one
 * that is gone (host reset or powered off, link down) stops answering and the
 * connection is declared lost after roughly IDLE + INTVL * CNT seconds, which
 * the daemons treat like any disconnect (fail-safe).  TCP_USER_TIMEOUT gives
 * the same bound to data that is sent but never acknowledged. */
#define SINK_KEEPALIVE_IDLE_S  60
#define SINK_KEEPALIVE_INTVL_S 10
#define SINK_KEEPALIVE_CNT     6

/* The receive buffer for sink_net_recv: SINK_MAX_LABEL_BODY bytes, laid out
 * so that the label bytes of a LabelData frame, which follow its
 * SINK_LABEL_DATA_HDR-byte header, start on a 64-byte boundary.  A RAM target
 * is Device memory on arm64 (how the kernel maps a /dev/mem carve-out), which
 * refuses unaligned stores; with an aligned source and a whole number of
 * labels per frame, memcpy into the mapping uses aligned stores only.
 * Returns NULL if out of memory. */
uint8_t *sink_net_alloc_rx(void);

/* Listen on PORT (decimal) on every IPv4 address.  Returns the socket, or -1. */
int  sink_net_listen(const char *port);

/* Accept one connection on a listening socket, with TCP_NODELAY and keepalive
 * set.  Returns the connected socket, or -1. */
int  sink_net_accept(int listen_fd);

/* Connect to HOST:PORT (numeric IPv4 address and decimal port), retrying every
 * second for up to retry_s seconds while the peer is not yet listening (a sink
 * that is still booting).  TCP_NODELAY and keepalive are set.  Returns the
 * connected socket, or -1. */
int  sink_net_connect(const char *host, const char *port, int retry_s);

/* Send one frame.  0 on success, -1 on a socket error. */
int  sink_net_send(int fd, uint8_t type, const uint8_t *payload, uint32_t len);

/* Send one frame whose payload is a followed by b (a LabelData header and its
 * labels), without copying them together.  0 on success, -1 on a socket error. */
int  sink_net_send2(int fd, uint8_t type, const uint8_t *a, uint32_t a_len,
                    const uint8_t *b, uint32_t b_len);

/* Receive one frame into buf (capacity cap).  The body limit is checked
 * against the type (LabelData may be up to SINK_MAX_LABEL_BODY, anything else
 * SINK_MAX_CTRL_BODY).  timeout_s <= 0 waits forever.  Returns 0, or -1 on a
 * socket error, peer close or oversize frame, or -2 on timeout. */
int  sink_net_recv(int fd, uint8_t *type_out, uint8_t *buf, size_t cap,
                   uint32_t *len_out, int timeout_s);

/* Close a socket if open and mark it closed (*fd = -1). */
void sink_net_close(int *fd);

#endif /* POSE_SINK_NET_H */
