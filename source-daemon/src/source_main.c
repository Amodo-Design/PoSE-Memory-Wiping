/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * source-daemon — the tray host's PoSE-DB daemon.  It is a label sink like
 * sink-daemon (it offers this machine's own RAM and disks over the same
 * protocol, sink_proto.h), and the label source for the downstream sinks it is
 * configured with (the DPU, the BMC): their targets appear in its inventory
 * under their node names, and later their sessions are generated here and
 * relayed.  Whoever connects to it sees one sink holding the whole tray.
 *
 * Downstream sinks come from -s NODE=IPV4:PORT, or from POSE_SINK_<NODE>
 * environment variables when it runs as PID 1 (pose.env on the UKI command
 * line); one that cannot be reached is a configuration error.
 *
 * A SessionInit for one of this machine's own targets is answered at once
 * with InitAck; the source then computes the labels itself from the seed,
 * algo and chunk_blocks (label_gen.c), stores them into the target and sends
 * Coverage.  It never receives LabelData or StreamEnd: either is a protocol
 * error.
 *
 * A SessionInit naming a downstream sink's node is forwarded to that sink
 * (session number unchanged) and its InitAck relayed; the source then
 * generates the labels the same way and streams them down as LabelData,
 * relays the sink's Coverage, and later relays Challenges for that session.
 * Either way the client gets Progress frames while the labels are generated.
 *
 * Status: the combined inventory, RAM and disk sessions on this machine and
 * on downstream sinks (generation, Coverage, Challenge) and Finish (passed on
 * to every downstream sink) are implemented.
 */

#include "sink_proto.h"
#include "sink_net.h"
#include "sink_inventory.h"
#include "sink_session.h"
#include "self_wipe.h"
#include "downstream.h"
#include "label_gen.h"
#include "gpu_targets.h"
#include "gpu_sessions.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *g_port = SINK_LISTEN_PORT;
static int         g_wait_s = DOWNSTREAM_CONNECT_WAIT;

static sink_inventory_t g_local;             /* this machine's own targets */
static sink_inventory_t g_combined;          /* g_local + every downstream sink's */
static uint8_t        *g_rx;                 /* one receive buffer, from sink_net_alloc_rx */
static uint8_t         g_tx[SINK_MAX_CTRL_BODY];
static int             g_listen_fd = -1;
static int             g_fd = -1;            /* the upstream client */
static int             g_sessions_started;   /* no more reconnects */

#define SERVE_DISCONNECTED 2                 /* serve(): client left before any session */

static void usage(void)
{
    fprintf(stderr,
            "usage: source-daemon [-p PORT] [-s NODE=IPV4:PORT]... [-w SECONDS]\n"
            "  -p PORT              TCP port to listen on (default %s)\n"
            "  -s NODE=IPV4:PORT    a downstream sink, repeatable; also read from\n"
            "                       POSE_SINK_<NODE>=IPV4:PORT environment variables\n"
            "  -w SECONDS           how long to wait for a downstream sink to accept\n"
            "                       (default %d)\n",
            SINK_LISTEN_PORT, DOWNSTREAM_CONNECT_WAIT);
}

/* Send an Error frame upstream (best effort), drop every connection, and leave
 * through daemon_exit: as PID 1 that pauses, self-wipes and reboots or halts;
 * otherwise it just returns the code.  Downstream sinks see the disconnect and
 * take their own fail-safe. */
static int fail(const char *reason)
{
    fprintf(stderr, "source: %s\n", reason);
    int n = sink_enc_error(g_tx, sizeof(g_tx), reason);
    if (n > 0 && g_fd >= 0)
        sink_net_send(g_fd, SINK_MSG_ERROR, g_tx, (uint32_t)n);
    sink_net_close(&g_fd);
    sink_net_close(&g_listen_fd);
    downstream_close_all();
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
        snprintf(why, sizeof(why), "protocol version %u not supported (source speaks %u)",
                 ver, SINK_PROTO_VERSION);
        return fail(why);
    }

    /* Asked afresh each time, so a disk that appeared after startup is
     * included and a sink that has gone away is noticed. */
    char why[SINK_REASON_MAX];
    if (sink_inventory_local(&g_local) < 0)
        return fail("inventory has more entries than the protocol allows");
    if (gpu_targets_append(&g_local, why, sizeof(why)) != 0)
        return fail(why);
    g_combined = g_local;
    if (downstream_inventory(&g_combined, g_wait_s, why, sizeof(why)) != 0)
        return fail(why);
    if (sink_inventory_targets(&g_combined) == 0)
        return fail("nothing to label: empty inventory");
    sink_inventory_print(&g_combined);

    int n = sink_enc_inventory(g_tx, sizeof(g_tx), &g_combined);
    if (n < 0 || sink_net_send(g_fd, SINK_MSG_INVENTORY, g_tx, (uint32_t)n) != 0)
        return fail("cannot send Inventory");
    return 0;
}

/* The first SessionInit commits the source to this client: stop listening, so
 * no other client can ever connect.  There is no idle timeout:
 * keepalive on the connection notices a peer that is gone (sink_net.h). */
static void start_sessions(void)
{
    g_sessions_started = 1;
    sink_net_close(&g_listen_fd);
}

/* label_deliver_fn for a session on this machine's own target: store the
 * super-chunk through the same code a sink-daemon uses for a LabelData frame,
 * so the offset-order, bounds and alignment checks are shared. */
static int deliver_local(void *ctx, uint32_t session, uint64_t offset,
                         const uint8_t *buf, size_t len, char *why, size_t why_cap)
{
    (void)ctx;
    sink_label_data_t hdr = { .session = session, .offset = offset };
    return sink_session_label_data(&hdr, buf, len, why, why_cap);
}

/* label_deliver_fn that wraps the real one and, at most once per
 * SINK_PROGRESS_INTERVAL_MS, tells the client how far the session has got.
 * Runs on a labeling thread while the main thread waits in label_gen_run, so
 * nothing else writes to g_fd meanwhile.  A failed send fails the delivery:
 * that is how a client that has gone away is noticed mid-generation. */
typedef struct {
    label_deliver_fn inner;
    double           t_last;       /* when the last Progress was sent */
} progress_t;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int send_progress(progress_t *pr, uint32_t session, uint64_t done_bytes)
{
    double t = now_s();
    if (t - pr->t_last < SINK_PROGRESS_INTERVAL_MS / 1000.0)
        return 0;
    pr->t_last = t;
    uint8_t frame[16];
    int n = sink_enc_progress(frame, sizeof(frame), session, done_bytes);
    return (n < 0 || sink_net_send(g_fd, SINK_MSG_PROGRESS, frame, (uint32_t)n) != 0) ? -1 : 0;
}

static int deliver_with_progress(void *ctx, uint32_t session, uint64_t offset,
                                 const uint8_t *buf, size_t len, char *why, size_t why_cap)
{
    progress_t *pr = ctx;
    if (pr->inner(NULL, session, offset, buf, len, why, why_cap) != 0)
        return -1;
    if (send_progress(pr, session, offset + len) != 0) {
        snprintf(why, why_cap, "client connection lost sending Progress");
        return -1;
    }
    return 0;
}

/* gpu_sessions_progress_fn: the GPU labeler reports after every batch, on
 * its own thread, under the same "main waits meanwhile" rule. */
static int gpu_progress(void *ctx, uint32_t session, uint64_t done_bytes)
{
    return send_progress(ctx, session, done_bytes);
}

static int send_init_ack(uint8_t status, const char *error)
{
    int n = sink_enc_init_ack(g_tx, sizeof(g_tx), status, error);
    if (n < 0 || sink_net_send(g_fd, SINK_MSG_INIT_ACK, g_tx, (uint32_t)n) != 0)
        return fail("cannot send InitAck");
    return 0;
}

/* Session numbers are the verifier's and global across the tray: one number
 * names one session, on this machine or on a downstream sink. */
static int g_session_used[SINK_MAX_SESSIONS];

static int reject_init(const char *why)
{
    fprintf(stderr, "session: rejected: %s\n", why);
    return send_init_ack(SINK_STATUS_ERROR, why);
}

/* A SessionInit on one of this machine's GPUs: the labels are computed in
 * HBM by the CUDA labeler, so there is no generator and nothing to deliver;
 * gpu_sessions.h does the checks and the allocation, then the labeling with
 * Progress at the usual cadence, then the one-region Coverage. */
static int on_gpu_session_init(const sink_session_init_t *init)
{
    char why[SINK_REASON_MAX];
    sink_init_ack_t ack;
    int accepted = gpu_sessions_init(init, &ack);
    if (send_init_ack(ack.status, ack.error) != 0)
        return -1;
    if (!accepted)
        return 0;
    g_session_used[init->session] = 1;
    start_sessions();

    progress_t pr = { .inner = NULL, .t_last = now_s() };
    if (gpu_sessions_label(init->session, init, gpu_progress, &pr, why, sizeof(why)) != 0)
        return fail(why);

    static sink_coverage_t cov;
    if (gpu_sessions_coverage(init->session, &cov, why, sizeof(why)) != 0)
        return fail(why);
    int n = sink_enc_coverage(g_tx, sizeof(g_tx), &cov);
    if (n < 0 || sink_net_send(g_fd, SINK_MSG_COVERAGE, g_tx, (uint32_t)n) != 0)
        return fail("cannot send Coverage");
    return 0;
}

/* SessionInit -> InitAck, then (if accepted) generate -> Coverage.  Node ""
 * is one of this machine's own targets; any other node names a downstream
 * sink, which gets the init forwarded and its labels streamed to it, and
 * whose InitAck and Coverage are relayed unchanged.  The generator's own
 * checks and buffer allocation come first, so a bad chunk_blocks or a
 * shortage of memory is a rejected InitAck and the connection carries on.
 * Only an accepted init commits the source to this client.  Generation blocks
 * this connection until Coverage is sent. */
static int on_session_init(const uint8_t *p, uint32_t len)
{
    sink_session_init_t init;
    if (sink_dec_session_init(p, len, &init) != 0)
        return fail("malformed SessionInit");

    char why[SINK_REASON_MAX];
    if (init.session >= SINK_MAX_SESSIONS) {
        snprintf(why, sizeof(why), "session number %u is out of range", init.session);
        return reject_init(why);
    }
    if (g_session_used[init.session]) {
        snprintf(why, sizeof(why), "session %u is already initialised", init.session);
        return reject_init(why);
    }
    const int remote = init.node[0] != '\0';
    const int gpu    = !remote && gpu_targets_parse(init.target) >= 0;
    if (gpu)
        return on_gpu_session_init(&init);
    if (label_gen_prepare(&init, why, sizeof(why)) != 0)
        return reject_init(why);

    sink_init_ack_t ack;
    int accepted;
    if (remote) {
        if (downstream_session_init(&init, &ack, g_wait_s, why, sizeof(why)) != 0)
            return fail(why);
        accepted = ack.status == SINK_STATUS_OK;
    } else {
        accepted = sink_session_init(&g_local, &init, &ack);
    }
    if (send_init_ack(ack.status, ack.error) != 0)
        return -1;
    if (!accepted)
        return 0;
    g_session_used[init.session] = 1;
    start_sessions();

    progress_t pr = { .inner = remote ? downstream_deliver : deliver_local, .t_last = now_s() };
    if (label_gen_run(&init, deliver_with_progress, &pr, why, sizeof(why)) != 0)
        return fail(why);

    static sink_coverage_t cov;         /* large: keep it off the stack */
    if ((remote ? downstream_stream_end(init.session, &cov, why, sizeof(why))
                : sink_session_stream_end(init.session, &cov, why, sizeof(why))) != 0)
        return fail(why);
    int n = sink_enc_coverage(g_tx, sizeof(g_tx), &cov);
    if (n < 0 || sink_net_send(g_fd, SINK_MSG_COVERAGE, g_tx, (uint32_t)n) != 0)
        return fail("cannot send Coverage");
    return 0;
}

/* Challenge -> Label.  A session held by a downstream sink is relayed there
 * and its Label passed back; anything else is answered from this machine's
 * own sessions.  A bad session or index is answered with zeros (logged), not
 * treated as a protocol error; losing a downstream sink is. */
static int on_challenge(const uint8_t *p, uint32_t len)
{
    sink_challenge_t ch;
    if (sink_dec_challenge(p, len, &ch) != 0)
        return fail("malformed Challenge");
    uint8_t label[SINK_LABEL_BYTES];
    char    why[SINK_REASON_MAX];
    if (downstream_owns(ch.session)) {
        if (downstream_challenge(ch.session, ch.block_index, label, why, sizeof(why)) != 0)
            return fail(why);
    } else if (gpu_sessions_owns(ch.session)) {
        if (gpu_sessions_challenge(ch.session, ch.block_index, label, why, sizeof(why)) != 0) {
            fprintf(stderr, "source: %s; answering zeros\n", why);
            fflush(stderr);
        }
    } else if (sink_session_challenge(ch.session, ch.block_index, label, why, sizeof(why)) != 0) {
        fprintf(stderr, "source: %s; answering zeros\n", why);
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
    downstream_finish(action, reason);
    sink_net_close(&g_fd);
    sink_net_close(&g_listen_fd);
    /* Labeled targets keep their labels (RAM and disk alike); only the
     * daemon's own pages are wiped.  The action is honoured as PID 1; an
     * ordinary process just exits (self_wipe.h). */
    sink_session_close_all();
    self_wipe_finish(action);
}

/* ── main loop ───────────────────────────────────────────────────────────── */

/* Serve one upstream connection.  Returns SERVE_DISCONNECTED if the client
 * went away before any session (nothing is held, so the source may take
 * another; downstream connections stay open); anything else ends the daemon. */
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
            return fail("client connection lost");
        }

        switch (type) {
        case SINK_MSG_INVENTORY_REQUEST:
            rc = on_inventory_request(g_rx, len);
            break;
        case SINK_MSG_SESSION_INIT:
            rc = on_session_init(g_rx, len);
            break;
        case SINK_MSG_LABEL_DATA:
        case SINK_MSG_STREAM_END:
            rc = fail("LabelData and StreamEnd are not accepted by a source-daemon: "
                      "it computes the labels itself");
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
    while ((opt = getopt(argc, argv, "p:s:w:h")) != -1) {
        switch (opt) {
        case 'p': g_port = optarg; break;
        case 's':
            if (downstream_add(optarg) != 0)
                return daemon_exit(2, "bad -s option");
            break;
        case 'w': g_wait_s = atoi(optarg); break;
        case 'h': usage(); return 0;
        default:  usage(); return 2;
        }
    }
    if (downstream_add_from_env() != 0)
        return daemon_exit(2, "bad POSE_SINK_ environment variable");

    g_rx = sink_net_alloc_rx();
    if (!g_rx)
        return daemon_exit(1, "out of memory");

    {
        char why[SINK_REASON_MAX];
        if (gpu_targets_init(why, sizeof(why)) < 0)
            return daemon_exit(1, why);
        sink_inventory_local(&g_local);
        if (gpu_targets_append(&g_local, why, sizeof(why)) != 0)
            return daemon_exit(1, why);
    }
    sink_inventory_print(&g_local);
    printf("%d GPU(s), %d downstream sink(s) configured\n", gpu_targets_count(), downstream_count());
    fflush(stdout);

    g_listen_fd = sink_net_listen(g_port);
    if (g_listen_fd < 0)
        return daemon_exit(1, "cannot listen for a client");

    for (;;) {
        g_fd = sink_net_accept(g_listen_fd);
        if (g_fd < 0)
            return daemon_exit(1, "cannot accept a client");
        int rc = serve();
        if (rc != SERVE_DISCONNECTED)
            return rc;
        printf("Client disconnected before any session; waiting for another\n");
        fflush(stdout);
        sink_net_close(&g_fd);
    }
}
