/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * Encoders and decoders for the sink protocol (sink_proto.h).  Compiled into
 * both the sink and the label source, so the two ends share one definition.
 * Decoders are strict: every read is bounds-checked, strings are length-
 * limited, and trailing bytes are an error.
 */

#include "sink_proto.h"

#include <stdio.h>
#include <string.h>

/* ── primitives ──────────────────────────────────────────────────────────── */

typedef struct { uint8_t *p; uint8_t *end; } wr_t;
typedef struct { const uint8_t *p; const uint8_t *end; } rd_t;

static int put_u8(wr_t *w, uint8_t v)
{
    if (w->end - w->p < 1) return -1;
    *w->p++ = v;
    return 0;
}
static int put_u16(wr_t *w, uint16_t v)
{
    if (w->end - w->p < 2) return -1;
    *w->p++ = (uint8_t)(v >> 8); *w->p++ = (uint8_t)v;
    return 0;
}
static int put_u32(wr_t *w, uint32_t v)
{
    if (w->end - w->p < 4) return -1;
    for (int i = 3; i >= 0; i--) *w->p++ = (uint8_t)(v >> (8 * i));
    return 0;
}
static int put_u64(wr_t *w, uint64_t v)
{
    if (w->end - w->p < 8) return -1;
    for (int i = 7; i >= 0; i--) *w->p++ = (uint8_t)(v >> (8 * i));
    return 0;
}
static int put_str(wr_t *w, const char *s, size_t max)
{
    size_t n = strnlen(s, max);
    if (n >= max || n > 0xFFFF) return -1;     /* must be NUL-terminated within max */
    if (put_u16(w, (uint16_t)n) != 0) return -1;
    if ((size_t)(w->end - w->p) < n) return -1;
    memcpy(w->p, s, n);
    w->p += n;
    return 0;
}
static int put_bytes(wr_t *w, const uint8_t *b, size_t n)
{
    if ((size_t)(w->end - w->p) < n) return -1;
    memcpy(w->p, b, n);
    w->p += n;
    return 0;
}

static int get_u8(rd_t *r, uint8_t *v)
{
    if (r->end - r->p < 1) return -1;
    *v = *r->p++;
    return 0;
}
static int get_u16(rd_t *r, uint16_t *v)
{
    if (r->end - r->p < 2) return -1;
    *v = (uint16_t)((r->p[0] << 8) | r->p[1]);
    r->p += 2;
    return 0;
}
static int get_u32(rd_t *r, uint32_t *v)
{
    if (r->end - r->p < 4) return -1;
    *v = 0;
    for (int i = 0; i < 4; i++) *v = (*v << 8) | r->p[i];
    r->p += 4;
    return 0;
}
static int get_u64(rd_t *r, uint64_t *v)
{
    if (r->end - r->p < 8) return -1;
    *v = 0;
    for (int i = 0; i < 8; i++) *v = (*v << 8) | r->p[i];
    r->p += 8;
    return 0;
}
static int get_bytes(rd_t *r, uint8_t *out, size_t n)
{
    if ((size_t)(r->end - r->p) < n) return -1;
    memcpy(out, r->p, n);
    r->p += n;
    return 0;
}
static int get_str(rd_t *r, char *out, size_t max)
{
    uint16_t n;
    if (get_u16(r, &n) != 0) return -1;
    if (n >= max || (size_t)(r->end - r->p) < n) return -1;
    memcpy(out, r->p, n);
    out[n] = '\0';
    r->p += n;
    return 0;
}

static wr_t wr(uint8_t *buf, size_t cap) { wr_t w = { buf, buf + cap }; return w; }
static rd_t rd(const uint8_t *p, size_t len) { rd_t r = { p, p + len }; return r; }
static int  wr_len(const wr_t *w, const uint8_t *buf) { return (int)(w->p - buf); }
static int  rd_done(const rd_t *r) { return r->p == r->end ? 0 : -1; }

/* ── InventoryRequest ────────────────────────────────────────────────────── */

int sink_enc_inventory_request(uint8_t *buf, size_t cap, uint16_t version)
{
    wr_t w = wr(buf, cap);
    if (put_u16(&w, version) != 0) return -1;
    return wr_len(&w, buf);
}
int sink_dec_inventory_request(const uint8_t *p, size_t len, uint16_t *version_out)
{
    rd_t r = rd(p, len);
    if (get_u16(&r, version_out) != 0) return -1;
    return rd_done(&r);
}

/* ── Inventory ───────────────────────────────────────────────────────────── */

int sink_enc_inventory(uint8_t *buf, size_t cap, const sink_inventory_t *inv)
{
    wr_t w = wr(buf, cap);
    if (inv->count > SINK_MAX_TARGETS) return -1;
    if (put_u32(&w, inv->count) != 0) return -1;
    for (uint32_t i = 0; i < inv->count; i++) {
        const sink_target_t *t = &inv->targets[i];
        if (put_u8(&w, t->kind) != 0 || put_str(&w, t->node, SINK_NODE_MAX) != 0 ||
            put_str(&w, t->name, SINK_NAME_MAX) != 0 ||
            put_u64(&w, t->base) != 0 || put_u64(&w, t->size) != 0 ||
            put_u32(&w, t->sector) != 0)
            return -1;
    }
    return wr_len(&w, buf);
}
int sink_dec_inventory(const uint8_t *p, size_t len, sink_inventory_t *inv_out)
{
    rd_t r = rd(p, len);
    memset(inv_out, 0, sizeof(*inv_out));
    if (get_u32(&r, &inv_out->count) != 0) return -1;
    if (inv_out->count > SINK_MAX_TARGETS) return -1;
    for (uint32_t i = 0; i < inv_out->count; i++) {
        sink_target_t *t = &inv_out->targets[i];
        if (get_u8(&r, &t->kind) != 0 || get_str(&r, t->node, SINK_NODE_MAX) != 0 ||
            get_str(&r, t->name, SINK_NAME_MAX) != 0 ||
            get_u64(&r, &t->base) != 0 || get_u64(&r, &t->size) != 0 ||
            get_u32(&r, &t->sector) != 0)
            return -1;
        if (t->kind > SINK_KIND_MAX) return -1;
    }
    return rd_done(&r);
}

/* ── SessionInit / InitAck ───────────────────────────────────────────────── */

int sink_enc_session_init(uint8_t *buf, size_t cap, const sink_session_init_t *init)
{
    wr_t w = wr(buf, cap);
    if (put_u32(&w, init->session) != 0 || put_str(&w, init->node, SINK_NODE_MAX) != 0 ||
        put_str(&w, init->target, SINK_NAME_MAX) != 0 ||
        put_u64(&w, init->base) != 0 || put_u64(&w, init->size) != 0 ||
        put_u32(&w, init->chunk_blocks) != 0 ||
        put_bytes(&w, init->seed, SINK_SEED_BYTES) != 0 || put_u8(&w, init->algo) != 0)
        return -1;
    return wr_len(&w, buf);
}
int sink_dec_session_init(const uint8_t *p, size_t len, sink_session_init_t *init_out)
{
    rd_t r = rd(p, len);
    memset(init_out, 0, sizeof(*init_out));
    if (get_u32(&r, &init_out->session) != 0 || get_str(&r, init_out->node, SINK_NODE_MAX) != 0 ||
        get_str(&r, init_out->target, SINK_NAME_MAX) != 0 ||
        get_u64(&r, &init_out->base) != 0 || get_u64(&r, &init_out->size) != 0 ||
        get_u32(&r, &init_out->chunk_blocks) != 0 ||
        get_bytes(&r, init_out->seed, SINK_SEED_BYTES) != 0 || get_u8(&r, &init_out->algo) != 0)
        return -1;
    return rd_done(&r);
}

int sink_enc_init_ack(uint8_t *buf, size_t cap, uint8_t status, const char *error)
{
    wr_t w = wr(buf, cap);
    if (put_u8(&w, status) != 0 || put_str(&w, error ? error : "", SINK_REASON_MAX) != 0)
        return -1;
    return wr_len(&w, buf);
}
int sink_dec_init_ack(const uint8_t *p, size_t len, sink_init_ack_t *ack_out)
{
    rd_t r = rd(p, len);
    memset(ack_out, 0, sizeof(*ack_out));
    if (get_u8(&r, &ack_out->status) != 0 || get_str(&r, ack_out->error, SINK_REASON_MAX) != 0)
        return -1;
    return rd_done(&r);
}

/* ── LabelData ───────────────────────────────────────────────────────────── */

int sink_enc_label_data_header(uint8_t *buf, size_t cap, uint32_t session, uint64_t offset)
{
    wr_t w = wr(buf, cap);
    if (put_u32(&w, session) != 0 || put_u64(&w, offset) != 0) return -1;
    return wr_len(&w, buf);
}
int sink_dec_label_data(const uint8_t *p, size_t len, sink_label_data_t *hdr_out,
                        const uint8_t **data_out, size_t *data_len_out)
{
    rd_t r = rd(p, len);
    if (get_u32(&r, &hdr_out->session) != 0 || get_u64(&r, &hdr_out->offset) != 0) return -1;
    *data_out     = r.p;
    *data_len_out = (size_t)(r.end - r.p);
    if (*data_len_out == 0 || *data_len_out > SINK_LABEL_DATA_MAX) return -1;
    return 0;
}

/* ── StreamEnd / Coverage ────────────────────────────────────────────────── */

int sink_enc_stream_end(uint8_t *buf, size_t cap, uint32_t session)
{
    wr_t w = wr(buf, cap);
    if (put_u32(&w, session) != 0) return -1;
    return wr_len(&w, buf);
}
int sink_dec_stream_end(const uint8_t *p, size_t len, uint32_t *session_out)
{
    rd_t r = rd(p, len);
    if (get_u32(&r, session_out) != 0) return -1;
    return rd_done(&r);
}

int sink_enc_coverage(uint8_t *buf, size_t cap, const sink_coverage_t *cov)
{
    wr_t w = wr(buf, cap);
    if (cov->count > SINK_MAX_REGIONS) return -1;
    if (put_u32(&w, cov->count) != 0) return -1;
    for (uint32_t i = 0; i < cov->count; i++)
        if (put_u64(&w, cov->regions[i].phys_base) != 0 ||
            put_u64(&w, cov->regions[i].block_count) != 0)
            return -1;
    return wr_len(&w, buf);
}
int sink_dec_coverage(const uint8_t *p, size_t len, sink_coverage_t *cov_out)
{
    rd_t r = rd(p, len);
    memset(cov_out, 0, sizeof(*cov_out));
    if (get_u32(&r, &cov_out->count) != 0) return -1;
    if (cov_out->count > SINK_MAX_REGIONS) return -1;
    for (uint32_t i = 0; i < cov_out->count; i++)
        if (get_u64(&r, &cov_out->regions[i].phys_base) != 0 ||
            get_u64(&r, &cov_out->regions[i].block_count) != 0)
            return -1;
    return rd_done(&r);
}

/* ── Progress ────────────────────────────────────────────────────────────── */

int sink_enc_progress(uint8_t *buf, size_t cap, uint32_t session, uint64_t bytes_done)
{
    wr_t w = wr(buf, cap);
    if (put_u32(&w, session) != 0 || put_u64(&w, bytes_done) != 0) return -1;
    return wr_len(&w, buf);
}
int sink_dec_progress(const uint8_t *p, size_t len, uint32_t *session_out, uint64_t *bytes_done_out)
{
    rd_t r = rd(p, len);
    if (get_u32(&r, session_out) != 0 || get_u64(&r, bytes_done_out) != 0) return -1;
    return rd_done(&r);
}

/* ── Challenge / Label ───────────────────────────────────────────────────── */

int sink_enc_challenge(uint8_t *buf, size_t cap, uint32_t session, uint64_t block_index)
{
    wr_t w = wr(buf, cap);
    if (put_u32(&w, session) != 0 || put_u64(&w, block_index) != 0) return -1;
    return wr_len(&w, buf);
}
int sink_dec_challenge(const uint8_t *p, size_t len, sink_challenge_t *ch_out)
{
    rd_t r = rd(p, len);
    if (get_u32(&r, &ch_out->session) != 0 || get_u64(&r, &ch_out->block_index) != 0) return -1;
    return rd_done(&r);
}

int sink_enc_label(uint8_t *buf, size_t cap, const uint8_t label[SINK_LABEL_BYTES])
{
    wr_t w = wr(buf, cap);
    if (put_bytes(&w, label, SINK_LABEL_BYTES) != 0) return -1;
    return wr_len(&w, buf);
}
int sink_dec_label(const uint8_t *p, size_t len, uint8_t label_out[SINK_LABEL_BYTES])
{
    if (len != SINK_LABEL_BYTES) return -1;
    memcpy(label_out, p, SINK_LABEL_BYTES);
    return 0;
}

/* ── Finish / Error ──────────────────────────────────────────────────────── */

int sink_enc_finish(uint8_t *buf, size_t cap, uint8_t action, const char *reason)
{
    wr_t w = wr(buf, cap);
    if (put_u8(&w, action) != 0 || put_str(&w, reason ? reason : "", SINK_REASON_MAX) != 0)
        return -1;
    return wr_len(&w, buf);
}
int sink_dec_finish(const uint8_t *p, size_t len, uint8_t *action_out,
                    char reason_out[SINK_REASON_MAX])
{
    rd_t r = rd(p, len);
    if (get_u8(&r, action_out) != 0 || get_str(&r, reason_out, SINK_REASON_MAX) != 0)
        return -1;
    return rd_done(&r);
}

int sink_enc_error(uint8_t *buf, size_t cap, const char *reason)
{
    wr_t w = wr(buf, cap);
    if (put_str(&w, reason ? reason : "", SINK_REASON_MAX) != 0) return -1;
    return wr_len(&w, buf);
}
int sink_dec_error(const uint8_t *p, size_t len, char reason_out[SINK_REASON_MAX])
{
    rd_t r = rd(p, len);
    if (get_str(&r, reason_out, SINK_REASON_MAX) != 0) return -1;
    return rd_done(&r);
}

const char *sink_kind_name(uint8_t kind)
{
    switch (kind) {
    case SINK_TARGET_RAM:   return "ram";
    case SINK_TARGET_DISK:  return "disk";
    case SINK_RAM_DECLARED: return "declared";
    case SINK_RAM_KERNEL:   return "kernel";
    case SINK_RAM_RESERVED: return "reserved";
    case SINK_RAM_UNUSABLE: return "unusable";
    case SINK_TARGET_GPU:   return "gpu";
    case SINK_GPU_DECLARED: return "gpu-hbm";
    case SINK_GPU_RESERVED: return "gpu-reserved";
    default: {
        static char buf[16];
        snprintf(buf, sizeof(buf), "kind %u", kind);
        return buf;
    }
    }
}

const char *sink_finish_action_name(uint8_t action)
{
    switch (action) {
    case SINK_FINISH_HALT:   return "halt";
    case SINK_FINISH_EXIT:   return "exit";
    case SINK_FINISH_REBOOT: return "reboot";
    default: {
        static char buf[16];
        snprintf(buf, sizeof(buf), "action %u", action);
        return buf;
    }
    }
}
