/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * sink-daemon — a PoSE-DB target that stores labels it did not
 * compute.  Discovers its own RAM regions and block devices, reports them to
 * a label source, receives the label stream, answers challenges, and wipes
 * itself when told to or when the source goes away.  Protocol: sink_proto.h.
 *
 * Runs as PID 1 in a UKI (pose-init execs it) or as an ordinary process on a
 * BMC.  No graph code, no hash code, no TLS: the binary is meant to stay
 * small enough to read.
 *
 * Status: inventory exchange, RAM and disk sessions (SessionInit, LabelData,
 * StreamEnd, Challenge) and Finish are implemented.
 */

#include "sink_proto.h"
#include "sink_net.h"
#include "sink_inventory.h"
#include "sink_session.h"
#include "self_wipe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *g_port = SINK_LISTEN_PORT;

static sink_inventory_t g_inventory;
static uint8_t        *g_rx;                 /* one receive buffer, from sink_net_alloc_rx */
static uint8_t         g_tx[SINK_MAX_CTRL_BODY];
static int             g_listen_fd = -1;
static int             g_fd = -1;            /* the label source */
static int             g_sessions_started;   /* no more reconnects */

#define SERVE_DISCONNECTED 2                 /* serve(): source left before any session */

static void usage(void)
{
    fprintf(stderr,
            "usage: sink-daemon [-p PORT]\n"
            "  -p PORT   TCP port to listen on (default %s)\n", SINK_LISTEN_PORT);
}

/* Send an Error frame (best effort), close, and leave through daemon_exit:
 * as PID 1 that pauses, self-wipes and reboots or halts; otherwise it just
 * returns the code. */
static int fail(const char *reason)
{
    fprintf(stderr, "sink: %s\n", reason);
    int n = sink_enc_error(g_tx, sizeof(g_tx), reason);
    if (n > 0 && g_fd >= 0)
        sink_net_send(g_fd, SINK_MSG_ERROR, g_tx, (uint32_t)n);
    sink_net_close(&g_fd);
    sink_net_close(&g_listen_fd);
    sink_session_close_all();
    return daemon_exit(1, reason);
}

/* ── frame handlers ──────────────────────────────────────────────────────── */

static int on_inventory_request(const uint8_t *p, uint32_t len)
{
    uint16_t ver;
    if (sink_dec_inventory_request(p, len, &ver) != 0)
        return fail("malformed InventoryRequest");
    if (ver != SINK_PROTO_VERSION) {
        char why[SINK_REASON_MAX];
        snprintf(why, sizeof(why), "protocol version %u not supported (sink speaks %u)",
                 ver, SINK_PROTO_VERSION);
        return fail(why);
    }
    /* Afresh each time: a disk can appear after startup. */
    if (sink_inventory_local(&g_inventory) < 0)
        return fail("inventory has more entries than the protocol allows");
    sink_inventory_print(&g_inventory);
    if (sink_inventory_targets(&g_inventory) == 0)
        return fail("nothing to label: empty inventory");

    int n = sink_enc_inventory(g_tx, sizeof(g_tx), &g_inventory);
    if (n < 0 || sink_net_send(g_fd, SINK_MSG_INVENTORY, g_tx, (uint32_t)n) != 0)
        return fail("cannot send Inventory");
    return 0;
}

/* The first SessionInit commits the sink to this source: stop listening, so
 * no other source can ever connect.  There is no idle timeout:
 * keepalive on the connection notices a peer that is gone (sink_net.h). */
static void start_sessions(void)
{
    g_sessions_started = 1;
    sink_net_close(&g_listen_fd);
}

/* SessionInit -> InitAck.  Only an accepted init commits the sink to this
 * source (stop listening): a rejected one holds nothing,
 * so the source may try another range or go away. */
static int on_session_init(const uint8_t *p, uint32_t len)
{
    sink_session_init_t init;
    if (sink_dec_session_init(p, len, &init) != 0)
        return fail("malformed SessionInit");
    sink_init_ack_t ack;
    int accepted = sink_session_init(&g_inventory, &init, &ack);
    int n = sink_enc_init_ack(g_tx, sizeof(g_tx), ack.status, ack.error);
    if (n < 0 || sink_net_send(g_fd, SINK_MSG_INIT_ACK, g_tx, (uint32_t)n) != 0)
        return fail("cannot send InitAck");
    if (accepted)
        start_sessions();
    return 0;
}


/* LabelData: stored in place, no reply.  Any violation ends the connection. */
static int on_label_data(const uint8_t *p, uint32_t len)
{
    sink_label_data_t hdr;
    const uint8_t    *data;
    size_t            data_len;
    if (sink_dec_label_data(p, len, &hdr, &data, &data_len) != 0)
        return fail("malformed LabelData");
    char why[SINK_REASON_MAX];
    if (sink_session_label_data(&hdr, data, data_len, why, sizeof(why)) != 0)
        return fail(why);
    return 0;
}

/* StreamEnd -> Coverage.  Any violation ends the connection. */
static int on_stream_end(const uint8_t *p, uint32_t len)
{
    uint32_t session;
    if (sink_dec_stream_end(p, len, &session) != 0)
        return fail("malformed StreamEnd");
    static sink_coverage_t cov;         /* large: keep it off the stack */
    char why[SINK_REASON_MAX];
    if (sink_session_stream_end(session, &cov, why, sizeof(why)) != 0)
        return fail(why);
    int n = sink_enc_coverage(g_tx, sizeof(g_tx), &cov);
    if (n < 0 || sink_net_send(g_fd, SINK_MSG_COVERAGE, g_tx, (uint32_t)n) != 0)
        return fail("cannot send Coverage");
    return 0;
}

/* Challenge -> Label.  A bad session or index is answered with zeros (logged),
 * not treated as a protocol error; only a malformed frame ends the connection. */
static int on_challenge(const uint8_t *p, uint32_t len)
{
    sink_challenge_t ch;
    if (sink_dec_challenge(p, len, &ch) != 0)
        return fail("malformed Challenge");
    uint8_t label[SINK_LABEL_BYTES];
    char    why[SINK_REASON_MAX];
    if (sink_session_challenge(ch.session, ch.block_index, label, why, sizeof(why)) != 0) {
        fprintf(stderr, "sink: %s; answering zeros\n", why);
        fflush(stderr);
    }
    int n = sink_enc_label(g_tx, sizeof(g_tx), label);
    if (n < 0 || sink_net_send(g_fd, SINK_MSG_LABEL, g_tx, (uint32_t)n) != 0)
        return fail("cannot send Label");
    return 0;
}

static int on_finish(const uint8_t *p, uint32_t len)
{
    uint8_t action;
    char    reason[SINK_REASON_MAX];
    if (sink_dec_finish(p, len, &action, reason) != 0)
        return fail("malformed Finish");
    printf("Finish: %s (%s)\n", sink_finish_action_name(action), reason);
    fflush(stdout);
    sink_net_close(&g_fd);
    sink_net_close(&g_listen_fd);
    /* Labeled targets keep their labels (RAM and disk alike); only the
     * daemon's own pages are wiped.  The action is honoured as PID 1; an
     * ordinary process just exits (self_wipe.h). */
    sink_session_close_all();
    self_wipe_finish(action);
}

/* ── main loop ───────────────────────────────────────────────────────────── */

/* Serve one source connection.  Returns SERVE_DISCONNECTED if the source went
 * away before any session (nothing is held, so the sink may take another);
 * anything else ends the daemon. */
static int serve(void)
{
    for (;;) {
        uint8_t  type;
        uint32_t len;
        int rc = sink_net_recv(g_fd, &type, g_rx, SINK_MAX_LABEL_BODY, &len,
                               0);
        if (rc != 0) {
            if (!g_sessions_started)
                return SERVE_DISCONNECTED;
            return fail("label source connection lost");
        }

        switch (type) {
        case SINK_MSG_INVENTORY_REQUEST:
            rc = on_inventory_request(g_rx, len);
            break;
        case SINK_MSG_SESSION_INIT:
            rc = on_session_init(g_rx, len);
            break;
        case SINK_MSG_LABEL_DATA:
            rc = on_label_data(g_rx, len);
            break;
        case SINK_MSG_STREAM_END:
            rc = on_stream_end(g_rx, len);
            break;
        case SINK_MSG_CHALLENGE:
            rc = on_challenge(g_rx, len);
            break;
        case SINK_MSG_FINISH:
            rc = on_finish(g_rx, len);
            break;
        default: {
            char why[SINK_REASON_MAX];
            snprintf(why, sizeof(why), "unexpected frame type 0x%02x", type);
            rc = fail(why);
            break;
        }
        }
        if (rc != 0)
            return rc;
    }
}

int main(int argc, char **argv)
{
    int opt;
    while ((opt = getopt(argc, argv, "p:h")) != -1) {
        switch (opt) {
        case 'p': g_port = optarg; break;
        case 'h': usage(); return 0;
        default:  usage(); return 2;
        }
    }

    g_rx = sink_net_alloc_rx();
    if (!g_rx)
        return daemon_exit(1, "out of memory");

    sink_inventory_local(&g_inventory);
    sink_inventory_print(&g_inventory);

    g_listen_fd = sink_net_listen(g_port);
    if (g_listen_fd < 0)
        return daemon_exit(1, "cannot listen for the label source");

    for (;;) {
        g_fd = sink_net_accept(g_listen_fd);
        if (g_fd < 0)
            return daemon_exit(1, "cannot accept the label source");
        int rc = serve();
        if (rc != SERVE_DISCONNECTED)
            return rc;
        printf("Label source disconnected before any session; waiting for another\n");
        fflush(stdout);
        sink_net_close(&g_fd);
    }
}
