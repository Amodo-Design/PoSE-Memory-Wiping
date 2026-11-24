/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#ifndef POSE_SINK_PROTO_H
#define POSE_SINK_PROTO_H

/*
 * PoSE-DB tray wire protocol: verifier <-> source-daemon <-> sink-daemon.
 *
 * One message set serves both links.  Every party that stores labels is a
 * *sink*: it reports an inventory, accepts SessionInit for a range inside it,
 * reports Coverage once that range holds labels, answers Challenges and acts
 * on Finish.  A sink-daemon (DPU, BMC) has its labels fed to it with LabelData
 * frames; a source-daemon (the tray host) computes them itself from the seed
 * in SessionInit, for its own targets and for every downstream sink it is
 * configured with.  So the verifier speaks to the source exactly as the source
 * speaks to a sink, minus LabelData and StreamEnd, and the source forwards a
 * downstream target's frames rather than translating them.
 *
 * A sink listens and its peer connects.  Until the first accepted SessionInit
 * it holds nothing, so a peer may disconnect and another connect (an
 * inventory check costs no reboot); from then on that connection is the only
 * one it will ever serve.  Plain TCP on SINK_LISTEN_PORT (the verifier link
 * gains TLS later; the framing does not change): PoSE-DB soundness rests on
 * labels being *stored* in the proven memory and being too deep to
 * *recompute* within delta_ms, not on where they were computed or whether the
 * stream was secret, so a corrupted or observed stream only makes that
 * sink's proof fail.
 *
 * Wire encoding:
 *   Framing:  [uint8 type][uint32 payload_len][payload...]   (5-byte header)
 *   Integers: big-endian
 *   Strings:  [uint16 len][bytes...]  (no NUL)
 *   Byte arrays: inline, length implied by the frame
 * Type bytes start at 0x10 so no frame can be mistaken for one of the earlier
 * verifier protocol (0x01..0x06), which this replaces.  The same sink_proto.c
 * encodes and decodes on every end, so the sides cannot drift.
 *
 * Messages (V = verifier, S = source, K = sink; "peer" = V->S or S->K):
 *   InventoryRequest peer->sink  [u16 version]
 *   Inventory        sink->peer  [u32 count] { [u8 kind][str node][str name][u64 base][u64 size][u32 sector] }...
 *   SessionInit      peer->sink  [u32 session][str node][str target][u64 base][u64 size][u32 chunk_blocks][seed:32][u8 algo]
 *   InitAck          sink->peer  [u8 status][str error]
 *   LabelData        S->K        [u32 session][u64 offset][bytes...]
 *   StreamEnd        S->K        [u32 session]
 *   Coverage         sink->peer  [u32 region_count] { [u64 phys_base][u64 block_count] }...
 *   Progress         S->V        [u32 session][u64 bytes_done]
 *   Challenge        peer->sink  [u32 session][u64 block_index]
 *   Label            sink->peer  [label:SINK_LABEL_BYTES]
 *   Finish           peer->sink  [u8 action][str reason]
 *   Error            sink->peer  [str reason]
 *
 * Flow:
 *   1. InventoryRequest / Inventory.  The sink discovers what it can label
 *      and reports each target raw: physical base and size for RAM, 0 and
 *      device size for a disk, plus the logical sector size.  The peer
 *      chooses chunk_blocks and how much of each target to use; the sink does
 *      not.  A version mismatch or an empty inventory is an Error.
 *
 *      Besides the targets (SINK_TARGET_RAM, SINK_TARGET_DISK) the inventory
 *      maps where the rest of the machine's DRAM went, so the verifier can
 *      show what could not be labelled and why.  These entries are never a
 *      session's target (SessionInit naming one is a rejected InitAck):
 *        SINK_RAM_DECLARED  a DRAM range the firmware declares (DTB memory@,
 *                           SMBIOS Type 19); name "dram".  Every other RAM
 *                           entry, the carve-out included, lies inside one.
 *        SINK_RAM_KERNEL    System RAM the kernel manages; name "System RAM".
 *        SINK_RAM_RESERVED  any other top-level /proc/iomem claim, named as
 *                           there ("Reserved", "ACPI Tables", ...).
 *        SINK_RAM_UNUSABLE  declared and unclaimed but not carved out; name
 *                           "too small" (under the 64 MiB minimum or the
 *                           page-alignment trim) or "not mappable" (/dev/mem
 *                           refused it).
 *      Kernel, reserved and unusable entries are clipped to the declared
 *      ranges and adjacent entries of the same kind and name are merged, so
 *      they tile the declared DRAM together with the RAM targets.  sector is
 *      0 for all of them.
 *
 *      A source-daemon with NVIDIA GPUs reports each one's HBM the same way
 *      (protocol version 3): target SINK_TARGET_GPU, name "gpuN" (CUDA device
 *      ordinal), base 0, size = free HBM minus the driver headroom, sector 0;
 *      and map entries SINK_GPU_DECLARED "gpuN" (the device's whole HBM) and
 *      SINK_GPU_RESERVED "driver" (held by the driver and context when
 *      inventoried) and "headroom" (512 MiB left for them), which with the
 *      target tile the declared HBM.  GPU labels are only ever BLAKE3, so a
 *      SessionInit on a GPU target with another algo is a rejected InitAck.
 *      Block i of a GPU session is at HBM offset i * 32 and the chunk seeds
 *      run from base 0, exactly as a RAM target at physical address 0 would.
 *      The labeler's transient scratch is not in the inventory: the verifier
 *      sizes the session below the target (pose_gpu_scratch_bytes) and
 *      records that, the headroom and the driver's share as unverifiable.
 *
 *      node says which machine a target is on.  A sink-daemon reports only
 *      its own targets, with node "".  A source-daemon reports its own
 *      targets with node "" plus every target of each downstream sink, with
 *      node set to that sink's configured name ("dpu0") or, for a target that
 *      already had a node, "<name>/<node>".  SessionInit and Challenge carry
 *      the node back: "" is the receiver's own target, anything else is
 *      forwarded to the sink named by the first component, minus that
 *      component.  A configured downstream sink that cannot be reached is an
 *      Error, never a shorter inventory.
 *   2. Per session, in the order the peer wants them labelled: SessionInit,
 *      answered at once with InitAck (accepted or why not), then Coverage
 *      when the range holds labels.  The seed and algo are what the labels
 *      are computed from; a sink-daemon ignores them.  Session numbers are
 *      the verifier's, global across the tray, so a forwarded SessionInit
 *      keeps its number.  How the labels get there:
 *        - a sink-daemon is fed LabelData frames in strictly increasing
 *          offset order (each frame's offset must equal the bytes received
 *          so far for that session; the sink never seeks), each carrying a
 *          whole number of labels, then StreamEnd; it flushes caches (RAM)
 *          or fsyncs (disk) and then sends Coverage;
 *        - a source-daemon generates the labels itself, into its own target
 *          or as a LabelData stream to the downstream sink, and sends
 *          Coverage when that is done (a downstream sink's Coverage is
 *          forwarded unchanged).  It never receives LabelData or StreamEnd.
 *      Coverage is the (phys_base, block_count) runs the verifier
 *      recomputes against.  Sessions are labelled one after another; all
 *      stay mapped until Finish.
 *
 *      While it generates a session's labels, a source-daemon sends its peer
 *      Progress frames, at most one per SINK_PROGRESS_INTERVAL_MS, carrying
 *      the bytes of the session generated and delivered so far (stored in
 *      its own target, or sent down as LabelData).  The total is the
 *      SessionInit's size; Coverage, not a Progress frame, marks completion.
 *      A sink-daemon never sends Progress, and a source-daemon does not
 *      relay one (its downstream sinks are sink-daemons).
 *   3. Challenges in any order across sessions; each answered with the 32
 *      bytes at base + block_index * 32 of that session's target (identity
 *      map: persisted block r is output rank r, as in the faithful labeler).
 *      The source answers for its own sessions and relays the others.  A
 *      Challenge naming an unknown session, a session whose labels are not
 *      stored yet, or a block past its end is not a protocol error: it is
 *      logged and answered with 32 zero bytes so the verifier sees a
 *      mismatch and the run carries on.
 *   4. Finish, with the action (halt, exit or reboot; SINK_FINISH_*) and a
 *      free-text reason for the log ("proof accepted").  The source passes it
 *      to every downstream sink before acting on it itself.  Labels are left
 *      in place on every target.  Any protocol error or loss of the connection
 *      takes the same path as Finish, so a sink never sits holding labels
 *      after its peer is gone.  Silence is not an error: a source generating
 *      labels for hours sends nothing to the sinks it has already filled.
 *      (Its peer does hear from it, through Progress, and a failed Progress
 *      send is how a source notices mid-generation that its peer is gone.)
 *      Instead every connection runs TCP keepalive (sink_net.h), so a peer
 *      that is gone (reset, powered off, unreachable) is noticed within about
 *      two minutes and counts as a lost connection, while a busy one is not.
 *      A peer that is alive but hung is not detected; the verifier notices.
 *
 * Label placement contract (what makes the verifier's recomputation match):
 *   SessionInit's [base, base+size) must lie inside one inventory entry, and
 *   size must be a whole multiple of chunk_blocks * SINK_LABEL_BYTES; the
 *   session has m = size / SINK_LABEL_BYTES blocks.  Labels are stored at
 *   base + idx*32 for idx in [0, m).  The verifier keys chunk k of a region
 *   at phys_base by pose_chunk_seed(seed, phys_base/32/chunk_blocks + k), so
 *   the source must generate with region_block_offset = base/32 and the sink
 *   must report that same base in Coverage.  Block devices use the byte
 *   offset into the device as "base".
 */

#include <stdint.h>
#include <stddef.h>

#define SINK_PROTO_VERSION     3u          /* 3: GPU target and map kinds */
#define SINK_LISTEN_PORT       "7776"

/* Message type tags. */
#define SINK_MSG_INVENTORY_REQUEST 0x10u   /* source -> sink */
#define SINK_MSG_INVENTORY         0x11u   /* sink -> source */
#define SINK_MSG_SESSION_INIT      0x12u   /* source -> sink */
#define SINK_MSG_INIT_ACK          0x13u   /* sink -> source */
#define SINK_MSG_LABEL_DATA        0x14u   /* source -> sink */
#define SINK_MSG_STREAM_END        0x15u   /* source -> sink */
#define SINK_MSG_COVERAGE          0x16u   /* sink -> source */
#define SINK_MSG_CHALLENGE         0x17u   /* source -> sink */
#define SINK_MSG_LABEL             0x18u   /* sink -> source */
#define SINK_MSG_FINISH            0x19u   /* source -> sink */
#define SINK_MSG_ERROR             0x1Au   /* sink -> source */
#define SINK_MSG_PROGRESS          0x1Bu   /* source -> verifier */

/* Protocol limits. */
#define SINK_LABEL_BYTES       32          /* one label; a sink-daemon never computes one */
#define SINK_SEED_BYTES        32          /* the verifier's session seed */
#define SINK_MAX_SESSIONS      32          /* the verifier plans at most this many */
#define SINK_MAX_TARGETS       1024        /* inventory entries (targets + RAM map entries) */
#define SINK_MAX_REGIONS       4096        /* Coverage entries */
#define SINK_MAX_CTRL_BODY     262144      /* 256 KB: every frame except LabelData */
#define SINK_MAX_LABEL_BODY    (12 + 1048576) /* LabelData: 12-byte header + 1 MiB of labels */
#define SINK_LABEL_DATA_MAX    1048576     /* label bytes per LabelData frame; a multiple of SINK_LABEL_BYTES */
#define SINK_LABEL_DATA_HDR    12          /* [u32 session][u64 offset] in front of the label bytes */
#define SINK_NAME_MAX          64          /* target name incl NUL ("ram", "/dev/nvme0n1") */
#define SINK_NODE_MAX          64          /* node incl NUL ("" local, "dpu0", "dpu0/bmc0") */
#define SINK_REASON_MAX        256         /* error / reason string incl NUL */

/* Inventory entry kinds. */
#define SINK_TARGET_RAM        0u          /* base = physical address, sector = 0 */
#define SINK_TARGET_DISK       1u          /* base = 0 (device byte offset), sector = logical sector size */
/* RAM map entries: not targets, see Flow 1.  base = physical address, sector = 0. */
#define SINK_RAM_DECLARED      2u          /* firmware-declared DRAM, "dram" */
#define SINK_RAM_KERNEL        3u          /* kernel-managed, "System RAM" */
#define SINK_RAM_RESERVED      4u          /* other /proc/iomem claim, named as there */
#define SINK_RAM_UNUSABLE      5u          /* unclaimed but not carved out: "too small" / "not mappable" */
#define SINK_TARGET_GPU        6u          /* GPU HBM: name "gpuN", base = 0, size = labelable bytes, sector = 0 */
#define SINK_GPU_DECLARED      7u          /* a GPU's whole HBM, name "gpuN" */
#define SINK_GPU_RESERVED      8u          /* HBM not labelable: "driver" (held when inventoried), "headroom" */
#define SINK_KIND_MAX          5u

/* Progress frames from a source generating a session, at most this often. */
#define SINK_PROGRESS_INTERVAL_MS 1000

/* Hash algorithms (the values of pose_hash_algo_t in pose-common). */
#define SINK_ALGO_BLAKE3       0u
#define SINK_ALGO_SHA256       1u
#define SINK_ALGO_AES_PRF      2u

/* InitAck status. */
#define SINK_STATUS_OK         1u
#define SINK_STATUS_ERROR      0u

/* Finish actions. */
/* Finish actions.  Labeled targets keep their labels in every case (RAM and
 * disk alike: seed-derived noise, the old contents are gone).  As PID 1 the
 * daemon wipes its own pages and then does what was asked; EXIT, which PID 1
 * cannot do, falls back to the image's POSE_FINAL_ACTION.  A daemon that is
 * not PID 1 (a BMC, a development run) just exits whatever was asked: it
 * must never change the machine's power state.  See self_wipe.h. */
#define SINK_FINISH_HALT       0u          /* PID 1: self-wipe, halt */
#define SINK_FINISH_EXIT       1u          /* exit; PID 1: self-wipe, POSE_FINAL_ACTION */
#define SINK_FINISH_REBOOT     2u          /* PID 1: self-wipe, reboot */

/* One inventory entry as the sink reports it: a labelable target or a RAM map entry. */
typedef struct {
    uint8_t  kind;                 /* SINK_TARGET_*, or a SINK_RAM_* / SINK_GPU_* map entry */
    char     node[SINK_NODE_MAX];  /* "" for the reporting machine, else a downstream sink */
    char     name[SINK_NAME_MAX];  /* "ram" (RAM regions share it; base disambiguates) or device path */
    uint64_t base;                 /* RAM: physical address; disk: 0 */
    uint64_t size;                 /* bytes */
    uint32_t sector;               /* disk: logical sector size; RAM: 0 */
} sink_target_t;

typedef struct {
    uint32_t      count;
    sink_target_t targets[SINK_MAX_TARGETS];
} sink_inventory_t;

/* SessionInit as decoded by the sink. */
typedef struct {
    uint32_t session;              /* index the source uses in later frames */
    char     node[SINK_NODE_MAX];  /* the inventory entry's node: "" = this machine */
    char     target[SINK_NAME_MAX];/* must name an inventory entry */
    uint64_t base;                 /* first byte to label, inside that entry */
    uint64_t size;                 /* bytes; whole multiple of chunk_blocks * SINK_LABEL_BYTES */
    uint32_t chunk_blocks;         /* labels per super-chunk (the verifier's chunk_blocks) */
    uint8_t  seed[SINK_SEED_BYTES];/* session seed the labels are computed from */
    uint8_t  algo;                 /* SINK_ALGO_*; a sink-daemon ignores seed and algo */
} sink_session_init_t;

/* InitAck. */
typedef struct {
    uint8_t status;                /* SINK_STATUS_OK / SINK_STATUS_ERROR */
    char    error[SINK_REASON_MAX];
} sink_init_ack_t;

/* LabelData header; the label bytes follow in the frame. */
typedef struct {
    uint32_t session;
    uint64_t offset;               /* byte offset within the session's [0, size) */
} sink_label_data_t;

/* One Coverage entry: a run of labels, as the verifier's coverage map lists them. */
typedef struct {
    uint64_t phys_base;            /* RAM: physical address; disk: byte offset into the device */
    uint64_t block_count;          /* labels (32-byte blocks); whole multiple of chunk_blocks */
} sink_region_t;

typedef struct {
    uint32_t      count;
    sink_region_t regions[SINK_MAX_REGIONS];
} sink_coverage_t;

/* Challenge. */
typedef struct {
    uint32_t session;
    uint64_t block_index;          /* label index within the session: byte base + block_index*32 */
} sink_challenge_t;

/*
 * Encoders write a payload into buf (capacity cap) and return its length, or
 * -1 if it does not fit.  Decoders parse exactly len bytes and return 0, or -1
 * on any malformed input (short payload, trailing bytes, over-long string,
 * out-of-range count).  Neither side allocates.  Implemented in
 * pose-sink/src/sink_proto.c and compiled into both the sink and
 * the label source.
 */
int sink_enc_inventory_request(uint8_t *buf, size_t cap, uint16_t version);
int sink_dec_inventory_request(const uint8_t *p, size_t len, uint16_t *version_out);

int sink_enc_inventory(uint8_t *buf, size_t cap, const sink_inventory_t *inv);
int sink_dec_inventory(const uint8_t *p, size_t len, sink_inventory_t *inv_out);

int sink_enc_session_init(uint8_t *buf, size_t cap, const sink_session_init_t *init);
int sink_dec_session_init(const uint8_t *p, size_t len, sink_session_init_t *init_out);

int sink_enc_init_ack(uint8_t *buf, size_t cap, uint8_t status, const char *error);
int sink_dec_init_ack(const uint8_t *p, size_t len, sink_init_ack_t *ack_out);

/* LabelData: the header is encoded in front of the caller's label bytes. */
int sink_enc_label_data_header(uint8_t *buf, size_t cap, uint32_t session, uint64_t offset);
int sink_dec_label_data(const uint8_t *p, size_t len, sink_label_data_t *hdr_out,
                        const uint8_t **data_out, size_t *data_len_out);

int sink_enc_stream_end(uint8_t *buf, size_t cap, uint32_t session);
int sink_dec_stream_end(const uint8_t *p, size_t len, uint32_t *session_out);

int sink_enc_coverage(uint8_t *buf, size_t cap, const sink_coverage_t *cov);
int sink_dec_coverage(const uint8_t *p, size_t len, sink_coverage_t *cov_out);

int sink_enc_challenge(uint8_t *buf, size_t cap, uint32_t session, uint64_t block_index);
int sink_dec_challenge(const uint8_t *p, size_t len, sink_challenge_t *ch_out);

int sink_enc_progress(uint8_t *buf, size_t cap, uint32_t session, uint64_t bytes_done);
int sink_dec_progress(const uint8_t *p, size_t len, uint32_t *session_out, uint64_t *bytes_done_out);

int sink_enc_label(uint8_t *buf, size_t cap, const uint8_t label[SINK_LABEL_BYTES]);
int sink_dec_label(const uint8_t *p, size_t len, uint8_t label_out[SINK_LABEL_BYTES]);

int sink_enc_finish(uint8_t *buf, size_t cap, uint8_t action, const char *reason);
int sink_dec_finish(const uint8_t *p, size_t len, uint8_t *action_out,
                    char reason_out[SINK_REASON_MAX]);

int sink_enc_error(uint8_t *buf, size_t cap, const char *reason);

/* 1 if an inventory entry of this kind can be a session's target. */
static inline int sink_kind_is_target(uint8_t kind)
{
    return kind == SINK_TARGET_RAM || kind == SINK_TARGET_DISK || kind == SINK_TARGET_GPU;
}

/* "ram", "disk", "gpu", "declared", "kernel", "reserved", "unusable", "gpu-hbm",
 * "gpu-reserved" or "kind N" for the log. */
const char *sink_kind_name(uint8_t kind);

/* "halt", "exit", "reboot", or "action N" for the log. */
const char *sink_finish_action_name(uint8_t action);
int sink_dec_error(const uint8_t *p, size_t len, char reason_out[SINK_REASON_MAX]);

#endif /* POSE_SINK_PROTO_H */
