/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#include "sink_session.h"
#include "sink_cache.h"
#include "sink_inventory.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Disk sessions stage LabelData here and write it with O_DIRECT in blocks of
 * this size: frames can be smaller than a sector (a super-chunk is
 * chunk_blocks * 32 bytes), O_DIRECT needs sector-sized, sector-aligned
 * writes from an aligned buffer, and large writes keep the device streaming.
 * A multiple of every logical sector size (512, 4096). */
#define DISK_STAGE_BYTES (4u << 20)

typedef enum {
    SESSION_NONE = 0,
    SESSION_STREAMING,         /* accepted; LabelData expected from offset 0 */
    SESSION_STORED,            /* StreamEnd seen; challenges may be answered */
} session_state_t;

typedef struct {
    session_state_t state;
    sink_target_t   target;    /* the inventory entry the range lies in */
    uint64_t        base;      /* RAM: physical address; disk: byte offset */
    uint64_t        size;
    uint32_t        chunk_blocks;
    uint64_t        received;  /* bytes written so far: the only write cursor */
    void           *map;       /* RAM: the /dev/mem mapping of [base, base+size) */
    int             fd;        /* disk: O_DIRECT|O_EXCL descriptor, else -1 */
    uint8_t        *stage;     /* disk: DISK_STAGE_BYTES, 4096-aligned; reused for challenge reads */
    size_t          staged;    /* disk: bytes in stage not yet written */
    uint64_t        written;   /* disk: bytes written to the device so far */
    /* Stream timing, logged at StreamEnd: where the stream's time went. */
    double          t_first;   /* when the first LabelData arrived; 0 = none yet */
    double          copy_s;    /* time spent storing LabelData into the target */
    uint64_t        frames;
} session_t;

static session_t g_sessions[SINK_MAX_SESSIONS];
static int       g_count;

static int reject(sink_init_ack_t *ack, const char *fmt, const char *arg)
{
    ack->status = SINK_STATUS_ERROR;
    snprintf(ack->error, sizeof(ack->error), fmt, arg);
    fprintf(stderr, "session: rejected: %s\n", ack->error);
    return 0;
}

/* The target that init names and whose extent contains its range.  RAM
 * regions all share the name "ram", so the range decides between them; RAM
 * map entries are never targets. */
static const sink_target_t *find_target(const sink_inventory_t *inv,
                                        const sink_session_init_t *init, int *name_seen)
{
    *name_seen = 0;
    for (uint32_t i = 0; i < inv->count; i++) {
        const sink_target_t *t = &inv->targets[i];
        if (t->node[0] != '\0' || !sink_kind_is_target(t->kind) ||
            strcmp(t->name, init->target) != 0)
            continue;
        *name_seen = 1;
        if (init->base >= t->base && init->size <= t->size &&
            init->base - t->base <= t->size - init->size)
            return t;
    }
    return NULL;
}

/* Two sessions may not share a byte: the second stream would overwrite the
 * first's labels.  RAM ranges are compared as physical addresses whatever
 * the entry was called; disk ranges only within the same device. */
static int overlaps_existing(const sink_target_t *t, uint64_t base, uint64_t size)
{
    for (int i = 0; i < SINK_MAX_SESSIONS; i++) {
        const session_t *s = &g_sessions[i];
        if (s->state == SESSION_NONE || s->target.kind != t->kind)
            continue;
        if (t->kind == SINK_TARGET_DISK && strcmp(s->target.name, t->name) != 0)
            continue;
        if (base < s->base + s->size && s->base < base + size)
            return i;
    }
    return -1;
}

/* Open a disk for a session: O_DIRECT so labels go to the device, not the
 * page cache, and O_EXCL so the kernel refuses a disk that is mounted, has a
 * mounted partition or is claimed by something else (LVM, md); under the UKI
 * nothing is, but on a development machine this keeps the sink off its root
 * disk.  The exclusive claim is per open file, so a second session on the same
 * disk shares the first one's descriptor (dup) rather than opening it again.
 * Returns 0 with *fd set, or -1 with ack filled in as a rejection. */
static int open_disk(const char *name, int *fd, sink_init_ack_t *ack)
{
    for (int i = 0; i < SINK_MAX_SESSIONS; i++) {
        const session_t *o = &g_sessions[i];
        if (o->state != SESSION_NONE && o->fd >= 0 && strcmp(o->target.name, name) == 0) {
            *fd = fcntl(o->fd, F_DUPFD_CLOEXEC, 0);
            if (*fd < 0) {
                reject(ack, "cannot share the disk descriptor: %s", strerror(errno));
                return -1;
            }
            return 0;
        }
    }
    *fd = open(name, O_RDWR | O_DIRECT | O_EXCL | O_CLOEXEC);
    if (*fd >= 0)
        return 0;
    if (errno == EBUSY)
        reject(ack, "disk %s is in use (mounted, or claimed by another driver or process)", name);
    else
        reject(ack, "cannot open the disk O_DIRECT: %s", strerror(errno));
    return -1;
}

/* Write everything staged at the session's write cursor: pwrite with O_DIRECT,
 * retrying short writes.  The staged length is a whole number of sectors
 * (whole stage blocks, or the session's tail, which is sector-aligned because
 * base and size are). */
static int disk_write_staged(session_t *s, char *why, size_t why_cap)
{
    size_t done = 0;
    while (done < s->staged) {
        ssize_t n = pwrite(s->fd, s->stage + done, s->staged - done,
                           (off_t)(s->base + s->written + done));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            snprintf(why, why_cap, "writing %s at byte %llu: %s", s->target.name,
                     (unsigned long long)(s->base + s->written + done),
                     n < 0 ? strerror(errno) : "no progress");
            return -1;
        }
        done += (size_t)n;
    }
    s->written += s->staged;
    s->staged = 0;
    return 0;
}

int sink_session_init(const sink_inventory_t *inv, const sink_session_init_t *init,
                      sink_init_ack_t *ack)
{
    memset(ack, 0, sizeof(*ack));

    if (init->node[0] != '\0')
        return reject(ack, "no such node '%s'", init->node);
    if (init->session >= SINK_MAX_SESSIONS) {
        char num[16];
        snprintf(num, sizeof(num), "%u", init->session);
        return reject(ack, "session number %s is out of range", num);
    }
    session_t *s = &g_sessions[init->session];
    if (s->state != SESSION_NONE) {
        char num[16];
        snprintf(num, sizeof(num), "%u", init->session);
        return reject(ack, "session %s is already initialised", num);
    }
    if (init->size == 0)
        return reject(ack, "size is zero%s", "");
    if (init->chunk_blocks == 0)
        return reject(ack, "chunk_blocks is zero%s", "");
    if (init->size % ((uint64_t)init->chunk_blocks * SINK_LABEL_BYTES) != 0)
        return reject(ack, "size is not a whole number of super-chunks (chunk_blocks * %s bytes)",
                      "32");

    int name_seen;
    const sink_target_t *t = find_target(inv, init, &name_seen);
    if (!t)
        return reject(ack, name_seen ? "range lies outside target '%s'"
                                     : "no such target '%s'", init->target);

    if (t->kind == SINK_TARGET_GPU)
        return reject(ack, "GPU target '%s' is labelled by the source-daemon, not a sink session",
                      init->target);

    int other = overlaps_existing(t, init->base, init->size);
    if (other >= 0) {
        char num[16];
        snprintf(num, sizeof(num), "%d", other);
        return reject(ack, "range overlaps session %s", num);
    }

    /* Alignment: a RAM range is mapped by page; a disk range is written and
     * read by sector (O_DIRECT). */
    uint64_t align = t->kind == SINK_TARGET_RAM ? (uint64_t)sysconf(_SC_PAGE_SIZE) : t->sector;
    if (align == 0) align = 4096;
    if (init->base % align != 0 || init->size % align != 0) {
        char num[32];
        snprintf(num, sizeof(num), "%llu", (unsigned long long)align);
        return reject(ack, "base and size must be multiples of %s bytes", num);
    }

    void    *map   = NULL;
    int      fd    = -1;
    uint8_t *stage = NULL;
    if (t->kind == SINK_TARGET_RAM) {
        map = sink_inventory_map_ram(init->base, init->size);
        if (!map)
            return reject(ack, "cannot map the RAM range through /dev/mem%s", "");
    } else {
        if (open_disk(t->name, &fd, ack) != 0)
            return 0;
        void *buf = NULL;
        if (posix_memalign(&buf, 4096, DISK_STAGE_BYTES) != 0) {
            close(fd);
            return reject(ack, "cannot allocate the disk staging buffer%s", "");
        }
        stage = buf;
    }

    memset(s, 0, sizeof(*s));
    s->state        = SESSION_STREAMING;
    s->target       = *t;
    s->base         = init->base;
    s->size         = init->size;
    s->chunk_blocks = init->chunk_blocks;
    s->map          = map;
    s->fd           = fd;
    s->stage        = stage;
    g_count++;

    ack->status = SINK_STATUS_OK;
    printf("Session %u: %s %s base=0x%llx size=%llu MiB chunk_blocks=%u (%llu super-chunks)\n",
           init->session, t->kind == SINK_TARGET_RAM ? "ram" : "disk", t->name,
           (unsigned long long)init->base, (unsigned long long)(init->size >> 20),
           init->chunk_blocks,
           (unsigned long long)(init->size / ((uint64_t)init->chunk_blocks * SINK_LABEL_BYTES)));
    fflush(stdout);
    return 1;
}

int sink_session_label_data(const sink_label_data_t *hdr, const uint8_t *data, size_t len,
                            char *why, size_t why_cap)
{
    if (hdr->session >= SINK_MAX_SESSIONS || g_sessions[hdr->session].state == SESSION_NONE) {
        snprintf(why, why_cap, "LabelData for session %u, which is not initialised", hdr->session);
        return -1;
    }
    session_t *s = &g_sessions[hdr->session];
    if (s->state != SESSION_STREAMING) {
        snprintf(why, why_cap, "LabelData for session %u after its StreamEnd", hdr->session);
        return -1;
    }
    if (hdr->offset != s->received) {
        snprintf(why, why_cap, "LabelData for session %u at offset %llu, expected %llu",
                 hdr->session, (unsigned long long)hdr->offset, (unsigned long long)s->received);
        return -1;
    }
    if (len > s->size - s->received) {
        snprintf(why, why_cap, "LabelData for session %u runs %llu bytes past its end",
                 hdr->session, (unsigned long long)(len - (s->size - s->received)));
        return -1;
    }
    if (len % SINK_LABEL_BYTES != 0) {
        snprintf(why, why_cap, "LabelData of %llu bytes is not a whole number of labels",
                 (unsigned long long)len);
        return -1;
    }
    double t0 = now_s();
    if (s->t_first == 0.0)
        s->t_first = t0;

    if (s->target.kind == SINK_TARGET_DISK) {
        /* Stage, and write each full stage block.  Any frame size works: the
         * device only ever sees whole, aligned DISK_STAGE_BYTES writes until
         * the tail at StreamEnd. */
        for (size_t k = 0; k < len; ) {
            size_t n = len - k < DISK_STAGE_BYTES - s->staged ? len - k
                                                              : DISK_STAGE_BYTES - s->staged;
            memcpy(s->stage + s->staged, data + k, n);
            s->staged += n;
            k += n;
            if (s->staged == DISK_STAGE_BYTES && disk_write_staged(s, why, why_cap) != 0)
                return -1;
        }
        s->copy_s += now_s() - t0;
        s->frames++;
        s->received += len;
        return 0;
    }

    /* A RAM mapping is Device memory on arm64 and takes aligned stores only.
     * Both ends are aligned here: the destination because base is page-aligned
     * and every earlier frame was a whole number of labels, the source because
     * the receive buffer comes from sink_net_alloc_rx.  A SIGBUS as PID 1 is a
     * silent reboot, so the source is checked rather than assumed. */
    if (((uintptr_t)data & 15) != 0) {
        snprintf(why, why_cap, "internal: LabelData payload is not aligned (receive buffer not from sink_net_alloc_rx)");
        return -1;
    }
    memcpy((uint8_t *)s->map + s->received, data, len);
    s->copy_s += now_s() - t0;
    s->frames++;
    s->received += len;
    return 0;
}

int sink_session_stream_end(uint32_t session, sink_coverage_t *cov, char *why, size_t why_cap)
{
    if (session >= SINK_MAX_SESSIONS || g_sessions[session].state == SESSION_NONE) {
        snprintf(why, why_cap, "StreamEnd for session %u, which is not initialised", session);
        return -1;
    }
    session_t *s = &g_sessions[session];
    if (s->state != SESSION_STREAMING) {
        snprintf(why, why_cap, "StreamEnd for session %u, which already ended", session);
        return -1;
    }
    if (s->received != s->size) {
        snprintf(why, why_cap, "StreamEnd for session %u after %llu of %llu bytes",
                 session, (unsigned long long)s->received, (unsigned long long)s->size);
        return -1;
    }
    /* A disk session writes its staged tail now, so its time counts as copy. */
    if (s->target.kind == SINK_TARGET_DISK && s->staged > 0) {
        double t0 = now_s();
        if (disk_write_staged(s, why, why_cap) != 0)
            return -1;
        s->copy_s += now_s() - t0;
    }

    /* Where the stream's time went, from the first LabelData to StreamEnd:
     * storing the labels into the target ("copy"), and everything else
     * ("other": on a sink-daemon, waiting on and reading the socket; on a
     * source-daemon's own session, waiting for the generator).  A copy share
     * near 100% means the target memory is the limit; a small one means the
     * stream (network or generator) is. */
    double t_end    = now_s();
    double elapsed  = s->t_first > 0.0 ? t_end - s->t_first : 0.0;
    double mib      = (double)s->size / (1 << 20);
    double other_s  = elapsed > s->copy_s ? elapsed - s->copy_s : 0.0;
    printf("Session %u: stream %.0f MiB in %.2f s (%.1f MiB/s, %llu frames): "
           "copy %.2f s (%.0f%%, %.1f MiB/s), other %.2f s\n",
           session, mib, elapsed, elapsed > 0.0 ? mib / elapsed : 0.0,
           (unsigned long long)s->frames, s->copy_s,
           elapsed > 0.0 ? 100.0 * s->copy_s / elapsed : 0.0,
           s->copy_s > 0.0 ? mib / s->copy_s : 0.0, other_s);
    fflush(stdout);

    /* Commit the labels: RAM out of the CPU caches, a disk out of the page
     * cache (none: O_DIRECT) and the device's volatile write cache (fsync,
     * which also issues the device flush). */
    const char *how;
    if (s->target.kind == SINK_TARGET_DISK) {
        if (fsync(s->fd) != 0) {
            snprintf(why, why_cap, "fsync %s: %s", s->target.name, strerror(errno));
            return -1;
        }
        how = "fsync";
    } else {
        sink_cache_flush(s->map, (size_t)s->size);
        how = sink_cache_flush_name();
    }
    s->state = SESSION_STORED;

    /* No pages are ever skipped, so the whole range is one run; this is what
     * the old daemon's encoder produced for a contiguous region. */
    memset(cov, 0, sizeof(*cov));
    cov->count = 1;
    cov->regions[0].phys_base   = s->base;
    cov->regions[0].block_count = s->size / SINK_LABEL_BYTES;

    printf("Session %u: stored, %s flushed, coverage base=0x%llx blocks=%llu\n", session,
           how, (unsigned long long)s->base,
           (unsigned long long)cov->regions[0].block_count);
    fflush(stdout);
    return 0;
}

int sink_session_challenge(uint32_t session, uint64_t block_index,
                           uint8_t label[SINK_LABEL_BYTES], char *why, size_t why_cap)
{
    memset(label, 0, SINK_LABEL_BYTES);

    if (session >= SINK_MAX_SESSIONS || g_sessions[session].state == SESSION_NONE) {
        snprintf(why, why_cap, "Challenge for session %u, which is not initialised", session);
        return -1;
    }
    const session_t *s = &g_sessions[session];
    if (s->state != SESSION_STORED) {
        snprintf(why, why_cap, "Challenge for session %u before its labels are stored", session);
        return -1;
    }
    uint64_t blocks = s->size / SINK_LABEL_BYTES;
    if (block_index >= blocks) {
        snprintf(why, why_cap, "Challenge for session %u block %llu, past its %llu blocks", session,
                 (unsigned long long)block_index, (unsigned long long)blocks);
        return -1;
    }
    if (s->target.kind == SINK_TARGET_DISK) {
        /* Read the one logical sector that holds the label, with O_DIRECT so
         * the answer comes from the device, not a cache.  The staging buffer is
         * free once the session is stored. */
        uint64_t sector = s->target.sector ? s->target.sector : 4096;
        uint64_t byte   = s->base + block_index * SINK_LABEL_BYTES;
        uint64_t at     = byte - byte % sector;
        ssize_t  n;
        do {
            n = pread(s->fd, s->stage, (size_t)sector, (off_t)at);
        } while (n < 0 && errno == EINTR);
        if (n != (ssize_t)sector) {
            snprintf(why, why_cap, "reading %s at byte %llu: %s", s->target.name,
                     (unsigned long long)at, n < 0 ? strerror(errno) : "short read");
            return -1;
        }
        memcpy(label, s->stage + (byte - at), SINK_LABEL_BYTES);
        return 0;
    }

    /* Identity map: persisted block r is output rank r.  The mapping is Device
     * memory on arm64, so read it with aligned 8-byte loads only (base is page
     * aligned, so every label is 32-byte aligned); volatile stops the compiler
     * from merging or widening them. */
    const volatile uint64_t *src =
        (const volatile uint64_t *)((const uint8_t *)s->map + block_index * SINK_LABEL_BYTES);
    for (int i = 0; i < SINK_LABEL_BYTES / 8; i++) {
        uint64_t w = src[i];
        memcpy(label + 8 * i, &w, 8);
    }
    return 0;
}

int sink_session_count(void)
{
    return g_count;
}

void sink_session_close_all(void)
{
    for (int i = 0; i < SINK_MAX_SESSIONS; i++) {
        session_t *s = &g_sessions[i];
        if (s->state == SESSION_NONE)
            continue;
        printf("Session %d: received %llu of %llu bytes\n", i,
               (unsigned long long)s->received, (unsigned long long)s->size);
        if (s->map)
            sink_inventory_unmap_ram(s->map, s->size);
        if (s->fd >= 0)
            close(s->fd);
        free(s->stage);
        memset(s, 0, sizeof(*s));
        s->fd = -1;
    }
    g_count = 0;
}
