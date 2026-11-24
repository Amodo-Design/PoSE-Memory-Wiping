/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#include "sink_net.h"
#include "sink_proto.h"

#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* IPv4 only: the UKI kernels are built without IPv6.  No getaddrinfo either:
 * it drags NSS into a static binary and needs the glibc shared objects at
 * runtime, which a PID 1 initramfs does not have; numeric addresses and ports
 * need none of that. */
static int parse_port(const char *port, uint16_t *out)
{
    char *endp;
    unsigned long pn = strtoul(port, &endp, 10);
    if (*port == '\0' || *endp != '\0' || pn == 0 || pn > 65535) {
        fprintf(stderr, "bad port '%s'\n", port);
        return -1;
    }
    *out = (uint16_t)pn;
    return 0;
}

/* Challenges and labels are tiny frames; Nagle + delayed ACK would add ~40 ms
 * per round to the RTT the verifier times.  Non-fatal if it fails. */
static void set_nodelay(int fd)
{
    int one = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0)
        perror("setsockopt(TCP_NODELAY)");
}

/* Keepalive replaces an idle timeout (see sink_net.h).  Non-fatal if a knob
 * is missing: the connection still works, a dead peer is just noticed later. */
static void set_keepalive(int fd)
{
    int on = 1, idle = SINK_KEEPALIVE_IDLE_S, intvl = SINK_KEEPALIVE_INTVL_S,
        cnt = SINK_KEEPALIVE_CNT;
    unsigned int user_ms = (unsigned int)(SINK_KEEPALIVE_IDLE_S +
                                          SINK_KEEPALIVE_INTVL_S * SINK_KEEPALIVE_CNT) * 1000u;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) != 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) != 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl)) != 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt)) != 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_ms, sizeof(user_ms)) != 0)
        perror("setsockopt(keepalive)");
}

uint8_t *sink_net_alloc_rx(void)
{
    /* 64-aligned block, body starting SINK_LABEL_DATA_HDR bytes before the
     * next 64-byte boundary. */
    uint8_t *block = aligned_alloc(64, SINK_MAX_LABEL_BODY + 64);
    if (!block)
        return NULL;
    return block + 64 - SINK_LABEL_DATA_HDR;
}

int sink_net_listen(const char *port)
{
    uint16_t pn;
    if (parse_port(port, &pn) != 0)
        return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in a4;
    memset(&a4, 0, sizeof(a4));
    a4.sin_family      = AF_INET;
    a4.sin_addr.s_addr = htonl(INADDR_ANY);
    a4.sin_port        = htons(pn);
    if (bind(fd, (struct sockaddr *)&a4, sizeof(a4)) != 0 || listen(fd, 1) != 0) {
        fprintf(stderr, "cannot listen on port %s: %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }
    printf("Listening on port %s (plain TCP) ...\n", port);
    fflush(stdout);
    return fd;
}

int sink_net_accept(int listen_fd)
{
    printf("Waiting for a connection ...\n");
    fflush(stdout);

    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int fd;
    do {
        fd = accept(listen_fd, (struct sockaddr *)&peer, &plen);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        perror("accept");
        return -1;
    }
    set_nodelay(fd);
    set_keepalive(fd);

    char host[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &peer.sin_addr, host, sizeof(host));
    printf("Connection from %s:%u\n", host, ntohs(peer.sin_port));
    fflush(stdout);
    return fd;
}

int sink_net_connect(const char *host, const char *port, int retry_s)
{
    struct sockaddr_in a4;
    memset(&a4, 0, sizeof(a4));
    a4.sin_family = AF_INET;
    uint16_t pn;
    if (parse_port(port, &pn) != 0)
        return -1;
    a4.sin_port = htons(pn);
    if (inet_pton(AF_INET, host, &a4.sin_addr) != 1) {
        fprintf(stderr, "bad IPv4 address '%s'\n", host);
        return -1;
    }

    for (int attempt = 0; ; attempt++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            perror("socket");
            return -1;
        }
        if (connect(fd, (struct sockaddr *)&a4, sizeof(a4)) == 0) {
            set_nodelay(fd);
            set_keepalive(fd);
            return fd;
        }
        int err = errno;
        close(fd);
        /* Not listening yet, or not reachable yet: worth waiting for a sink
         * that is still booting.  Anything else is a configuration error. */
        int transient = err == ECONNREFUSED || err == ETIMEDOUT || err == EHOSTUNREACH ||
                        err == ENETUNREACH || err == EINTR;
        if (!transient || attempt >= retry_s) {
            fprintf(stderr, "cannot connect to %s:%s: %s\n", host, port, strerror(err));
            return -1;
        }
        if (attempt == 0) {
            printf("Waiting for %s:%s to accept (up to %d s) ...\n", host, port, retry_s);
            fflush(stdout);
        }
        sleep(1);
    }
}

/* more: further parts of the same frame follow (MSG_MORE), so TCP_NODELAY
 * does not push the header out as a segment of its own. */
static int send_all(int fd, const uint8_t *buf, size_t len, int more)
{
    while (len > 0) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL | (more ? MSG_MORE : 0));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("send");
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Receive exactly len bytes; -1 on error or peer close, -2 on timeout. */
static int recv_all(int fd, uint8_t *buf, size_t len, int timeout_s)
{
    while (len > 0) {
        if (timeout_s > 0) {
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            int pr = poll(&pfd, 1, timeout_s * 1000);
            if (pr == 0)
                return -2;
            if (pr < 0) {
                if (errno == EINTR)
                    continue;
                perror("poll");
                return -1;
            }
        }
        ssize_t n = recv(fd, buf, len, 0);
        if (n == 0)
            return -1;                      /* peer closed */
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("recv");
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

int sink_net_send2(int fd, uint8_t type, const uint8_t *a, uint32_t a_len,
                   const uint8_t *b, uint32_t b_len)
{
    uint32_t len = a_len + b_len;
    uint8_t hdr[5] = { type, (uint8_t)(len >> 24), (uint8_t)(len >> 16),
                       (uint8_t)(len >> 8), (uint8_t)len };
    if (send_all(fd, hdr, sizeof(hdr), len > 0) != 0)
        return -1;
    if (a_len > 0 && send_all(fd, a, a_len, b_len > 0) != 0)
        return -1;
    if (b_len > 0 && send_all(fd, b, b_len, 0) != 0)
        return -1;
    return 0;
}

int sink_net_send(int fd, uint8_t type, const uint8_t *payload, uint32_t len)
{
    return sink_net_send2(fd, type, payload, len, NULL, 0);
}

int sink_net_recv(int fd, uint8_t *type_out, uint8_t *buf, size_t cap,
                  uint32_t *len_out, int timeout_s)
{
    uint8_t hdr[5];
    int rc = recv_all(fd, hdr, sizeof(hdr), timeout_s);
    if (rc != 0)
        return rc;
    uint8_t  type = hdr[0];
    uint32_t len  = ((uint32_t)hdr[1] << 24) | ((uint32_t)hdr[2] << 16) |
                    ((uint32_t)hdr[3] << 8) | hdr[4];
    uint32_t limit = (type == SINK_MSG_LABEL_DATA) ? SINK_MAX_LABEL_BODY : SINK_MAX_CTRL_BODY;
    if (len > limit || len > cap) {
        fprintf(stderr, "frame type 0x%02x of %u bytes exceeds the limit (%u)\n",
                type, len, limit < cap ? limit : (uint32_t)cap);
        return -1;
    }
    rc = recv_all(fd, buf, len, timeout_s);
    if (rc != 0)
        return rc;
    *type_out = type;
    *len_out  = len;
    return 0;
}

void sink_net_close(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}
