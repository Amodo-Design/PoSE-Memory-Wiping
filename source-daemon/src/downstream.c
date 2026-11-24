/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#include "downstream.h"
#include "sink_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;

#define ENV_PREFIX "POSE_SINK_"

typedef struct {
    char node[SINK_NODE_MAX];
    char host[16];                 /* dotted IPv4 */
    char port[6];
    int  fd;                       /* -1 until connected */
} downstream_t;

static downstream_t g_ds[DOWNSTREAM_MAX];
static int          g_nds;
static uint8_t      g_buf[SINK_MAX_CTRL_BODY];

/* Node names become inventory node strings ("dpu0", "dpu0/bmc0"), so keep
 * them to characters that need no quoting anywhere. */
static int valid_node(const char *s, size_t len)
{
    if (len == 0 || len >= SINK_NODE_MAX / 2)
        return 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

/* Add NODE (len bytes of node) at "IPV4:PORT". */
static int add(const char *node, size_t node_len, const char *addr, const char *origin)
{
    if (!valid_node(node, node_len)) {
        fprintf(stderr, "source: %s: bad node name '%.*s' (letters, digits, '-', '_')\n",
                origin, (int)node_len, node);
        return -1;
    }
    const char *colon = strrchr(addr, ':');
    size_t hlen = colon ? (size_t)(colon - addr) : 0;
    if (!colon || hlen == 0 || hlen >= sizeof(g_ds[0].host) ||
        strlen(colon + 1) == 0 || strlen(colon + 1) >= sizeof(g_ds[0].port)) {
        fprintf(stderr, "source: %s: '%s' is not IPV4:PORT\n", origin, addr);
        return -1;
    }
    for (int i = 0; i < g_nds; i++) {
        if (strlen(g_ds[i].node) == node_len && memcmp(g_ds[i].node, node, node_len) == 0) {
            fprintf(stderr, "source: %s: node '%.*s' configured twice\n",
                    origin, (int)node_len, node);
            return -1;
        }
    }
    if (g_nds >= DOWNSTREAM_MAX) {
        fprintf(stderr, "source: %s: more than %d downstream sinks\n", origin, DOWNSTREAM_MAX);
        return -1;
    }
    downstream_t *d = &g_ds[g_nds++];
    memcpy(d->node, node, node_len);
    d->node[node_len] = '\0';
    memcpy(d->host, addr, hlen);
    d->host[hlen] = '\0';
    strcpy(d->port, colon + 1);
    d->fd = -1;
    printf("Downstream sink %s at %s:%s (%s)\n", d->node, d->host, d->port, origin);
    return 0;
}

int downstream_add(const char *spec)
{
    const char *eq = strchr(spec, '=');
    if (!eq) {
        fprintf(stderr, "source: -s %s: expected NODE=IPV4:PORT\n", spec);
        return -1;
    }
    return add(spec, (size_t)(eq - spec), eq + 1, "-s");
}

int downstream_add_from_env(void)
{
    for (char **e = environ; *e; e++) {
        if (strncmp(*e, ENV_PREFIX, strlen(ENV_PREFIX)) != 0)
            continue;
        const char *node = *e + strlen(ENV_PREFIX);
        const char *eq = strchr(node, '=');
        if (!eq || add(node, (size_t)(eq - node), eq + 1, "environment") != 0)
            return -1;
    }
    return 0;
}

int downstream_count(void)
{
    return g_nds;
}

/* Request one sink's inventory over its open connection. */
static int fetch_inventory(downstream_t *d, sink_inventory_t *out, char *why, size_t why_cap)
{
    int n = sink_enc_inventory_request(g_buf, sizeof(g_buf), SINK_PROTO_VERSION);
    if (n < 0 || sink_net_send(d->fd, SINK_MSG_INVENTORY_REQUEST, g_buf, (uint32_t)n) != 0) {
        snprintf(why, why_cap, "downstream sink %s: cannot send InventoryRequest", d->node);
        return -1;
    }
    uint8_t  type;
    uint32_t len;
    int rc = sink_net_recv(d->fd, &type, g_buf, sizeof(g_buf), &len, DOWNSTREAM_CONNECT_WAIT);
    if (rc != 0) {
        snprintf(why, why_cap, "downstream sink %s: %s waiting for its Inventory", d->node,
                 rc == -2 ? "timed out" : "connection lost");
        return -1;
    }
    if (type == SINK_MSG_ERROR) {
        char reason[SINK_REASON_MAX] = "(malformed Error)";
        sink_dec_error(g_buf, len, reason);
        snprintf(why, why_cap, "downstream sink %s: %s", d->node, reason);
        return -1;
    }
    if (type != SINK_MSG_INVENTORY || sink_dec_inventory(g_buf, len, out) != 0) {
        snprintf(why, why_cap, "downstream sink %s: bad reply 0x%02x to InventoryRequest",
                 d->node, type);
        return -1;
    }
    return 0;
}

int downstream_inventory(sink_inventory_t *inv, int wait_s, char *why, size_t why_cap)
{
    static sink_inventory_t remote;     /* large: keep it off the stack */

    for (int i = 0; i < g_nds; i++) {
        downstream_t *d = &g_ds[i];
        if (d->fd < 0) {
            printf("Connecting to downstream sink %s at %s:%s ...\n", d->node, d->host, d->port);
            fflush(stdout);
            d->fd = sink_net_connect(d->host, d->port, wait_s);
            if (d->fd < 0) {
                snprintf(why, why_cap, "downstream sink %s at %s:%s is unreachable",
                         d->node, d->host, d->port);
                return -1;
            }
        }
        if (fetch_inventory(d, &remote, why, why_cap) != 0) {
            sink_net_close(&d->fd);
            return -1;
        }
        for (uint32_t k = 0; k < remote.count; k++) {
            if (inv->count >= SINK_MAX_TARGETS) {
                snprintf(why, why_cap, "combined inventory exceeds %d entries", SINK_MAX_TARGETS);
                return -1;
            }
            sink_target_t *t = &inv->targets[inv->count];
            *t = remote.targets[k];
            int w = remote.targets[k].node[0]
                  ? snprintf(t->node, sizeof(t->node), "%s/%s", d->node, remote.targets[k].node)
                  : snprintf(t->node, sizeof(t->node), "%s", d->node);
            if (w < 0 || (size_t)w >= sizeof(t->node)) {
                snprintf(why, why_cap, "downstream sink %s: node name too long for %s",
                         d->node, remote.targets[k].node);
                return -1;
            }
            inv->count++;
        }
    }
    return 0;
}

/* ── sessions on a downstream sink ───────────────────────────────────────── */

/* Which sink holds each session: index + 1 into g_ds, 0 = none. */
static int g_owner[SINK_MAX_SESSIONS];

static int connect_if_needed(downstream_t *d, int wait_s, char *why, size_t why_cap)
{
    if (d->fd >= 0)
        return 0;
    printf("Connecting to downstream sink %s at %s:%s ...\n", d->node, d->host, d->port);
    fflush(stdout);
    d->fd = sink_net_connect(d->host, d->port, wait_s);
    if (d->fd < 0) {
        snprintf(why, why_cap, "downstream sink %s at %s:%s is unreachable",
                 d->node, d->host, d->port);
        return -1;
    }
    return 0;
}

/* After a failed send: the sink usually explains itself with an Error frame
 * before closing, so look briefly for one to report instead of "lost". */
static void lost(downstream_t *d, const char *doing, char *why, size_t why_cap)
{
    uint8_t  type;
    uint32_t len;
    char     reason[SINK_REASON_MAX];
    if (d->fd >= 0 && sink_net_recv(d->fd, &type, g_buf, sizeof(g_buf), &len, 1) == 0 &&
        type == SINK_MSG_ERROR && sink_dec_error(g_buf, len, reason) == 0)
        snprintf(why, why_cap, "downstream sink %s: %s", d->node, reason);
    else
        snprintf(why, why_cap, "downstream sink %s: connection lost %s", d->node, doing);
}

/* Receive the reply to a request: an Error frame or the wrong type is a
 * failure.  Waits as long as it takes (keepalive notices a dead sink). */
static int recv_reply(downstream_t *d, uint8_t want, uint32_t *len_out, const char *doing,
                      char *why, size_t why_cap)
{
    uint8_t type;
    if (sink_net_recv(d->fd, &type, g_buf, sizeof(g_buf), len_out, 0) != 0) {
        snprintf(why, why_cap, "downstream sink %s: connection lost %s", d->node, doing);
        return -1;
    }
    if (type == SINK_MSG_ERROR) {
        char reason[SINK_REASON_MAX] = "(malformed Error)";
        sink_dec_error(g_buf, *len_out, reason);
        snprintf(why, why_cap, "downstream sink %s: %s", d->node, reason);
        return -1;
    }
    if (type != want) {
        snprintf(why, why_cap, "downstream sink %s: unexpected frame 0x%02x %s",
                 d->node, type, doing);
        return -1;
    }
    return 0;
}

static downstream_t *owner_of(uint32_t session)
{
    if (session >= SINK_MAX_SESSIONS || g_owner[session] == 0)
        return NULL;
    return &g_ds[g_owner[session] - 1];
}

int downstream_owns(uint32_t session)
{
    return owner_of(session) != NULL;
}

int downstream_session_init(const sink_session_init_t *init, sink_init_ack_t *ack,
                            int wait_s, char *why, size_t why_cap)
{
    const char *slash = strchr(init->node, '/');
    size_t      nlen  = slash ? (size_t)(slash - init->node) : strlen(init->node);
    downstream_t *d   = NULL;
    for (int i = 0; i < g_nds; i++)
        if (strlen(g_ds[i].node) == nlen && memcmp(g_ds[i].node, init->node, nlen) == 0)
            d = &g_ds[i];

    memset(ack, 0, sizeof(*ack));
    if (!d) {
        ack->status = SINK_STATUS_ERROR;
        snprintf(ack->error, sizeof(ack->error), "no such node '%s'", init->node);
        fprintf(stderr, "session: rejected: %s\n", ack->error);
        return 0;                       /* an InitAck rejection, not a failure */
    }
    if (connect_if_needed(d, wait_s, why, why_cap) != 0)
        return -1;

    sink_session_init_t fwd = *init;
    snprintf(fwd.node, sizeof(fwd.node), "%s", slash ? slash + 1 : "");
    int n = sink_enc_session_init(g_buf, sizeof(g_buf), &fwd);
    if (n < 0 || sink_net_send(d->fd, SINK_MSG_SESSION_INIT, g_buf, (uint32_t)n) != 0) {
        lost(d, "sending SessionInit", why, why_cap);
        return -1;
    }
    uint32_t len;
    if (recv_reply(d, SINK_MSG_INIT_ACK, &len, "waiting for InitAck", why, why_cap) != 0)
        return -1;
    if (sink_dec_init_ack(g_buf, len, ack) != 0) {
        snprintf(why, why_cap, "downstream sink %s: malformed InitAck", d->node);
        return -1;
    }
    if (ack->status == SINK_STATUS_OK && init->session < SINK_MAX_SESSIONS)
        g_owner[init->session] = (int)(d - g_ds) + 1;
    printf("Session %u: forwarded to %s: %s%s\n", init->session, d->node,
           ack->status == SINK_STATUS_OK ? "accepted" : "rejected: ",
           ack->status == SINK_STATUS_OK ? "" : ack->error);
    fflush(stdout);
    return 0;
}

int downstream_deliver(void *ctx, uint32_t session, uint64_t offset,
                       const uint8_t *buf, size_t len, char *why, size_t why_cap)
{
    (void)ctx;
    downstream_t *d = owner_of(session);
    if (!d || d->fd < 0) {
        snprintf(why, why_cap, "internal: no downstream sink holds session %u", session);
        return -1;
    }
    uint8_t hdr[SINK_LABEL_DATA_HDR];
    for (size_t k = 0; k < len; ) {
        size_t chunk = len - k < SINK_LABEL_DATA_MAX ? len - k : SINK_LABEL_DATA_MAX;
        int n = sink_enc_label_data_header(hdr, sizeof(hdr), session, offset + k);
        if (n < 0 || sink_net_send2(d->fd, SINK_MSG_LABEL_DATA, hdr, (uint32_t)n,
                                    buf + k, (uint32_t)chunk) != 0) {
            lost(d, "streaming LabelData", why, why_cap);
            return -1;
        }
        k += chunk;
    }
    return 0;
}

int downstream_stream_end(uint32_t session, sink_coverage_t *cov, char *why, size_t why_cap)
{
    downstream_t *d = owner_of(session);
    if (!d || d->fd < 0) {
        snprintf(why, why_cap, "internal: no downstream sink holds session %u", session);
        return -1;
    }
    int n = sink_enc_stream_end(g_buf, sizeof(g_buf), session);
    if (n < 0 || sink_net_send(d->fd, SINK_MSG_STREAM_END, g_buf, (uint32_t)n) != 0) {
        lost(d, "sending StreamEnd", why, why_cap);
        return -1;
    }
    uint32_t len;
    if (recv_reply(d, SINK_MSG_COVERAGE, &len, "waiting for Coverage", why, why_cap) != 0)
        return -1;
    if (sink_dec_coverage(g_buf, len, cov) != 0) {
        snprintf(why, why_cap, "downstream sink %s: malformed Coverage", d->node);
        return -1;
    }
    return 0;
}

int downstream_challenge(uint32_t session, uint64_t block_index,
                         uint8_t label[SINK_LABEL_BYTES], char *why, size_t why_cap)
{
    downstream_t *d = owner_of(session);
    if (!d || d->fd < 0) {
        snprintf(why, why_cap, "internal: no downstream sink holds session %u", session);
        return -1;
    }
    int n = sink_enc_challenge(g_buf, sizeof(g_buf), session, block_index);
    if (n < 0 || sink_net_send(d->fd, SINK_MSG_CHALLENGE, g_buf, (uint32_t)n) != 0) {
        lost(d, "sending Challenge", why, why_cap);
        return -1;
    }
    uint32_t len;
    if (recv_reply(d, SINK_MSG_LABEL, &len, "waiting for Label", why, why_cap) != 0)
        return -1;
    if (sink_dec_label(g_buf, len, label) != 0) {
        snprintf(why, why_cap, "downstream sink %s: malformed Label", d->node);
        return -1;
    }
    return 0;
}

void downstream_finish(uint8_t action, const char *reason)
{
    for (int i = 0; i < g_nds; i++) {
        downstream_t *d = &g_ds[i];
        if (d->fd < 0)
            continue;               /* never contacted: it holds nothing */
        int n = sink_enc_finish(g_buf, sizeof(g_buf), action, reason);
        if (n < 0 || sink_net_send(d->fd, SINK_MSG_FINISH, g_buf, (uint32_t)n) != 0)
            fprintf(stderr, "source: cannot send Finish to %s; it takes its fail-safe\n", d->node);
        else
            printf("Finish sent to downstream sink %s\n", d->node);
        sink_net_close(&d->fd);
    }
    fflush(stdout);
}

void downstream_close_all(void)
{
    for (int i = 0; i < g_nds; i++)
        sink_net_close(&g_ds[i].fd);
}
