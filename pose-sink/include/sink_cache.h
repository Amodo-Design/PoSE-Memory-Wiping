/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#ifndef POSE_SINK_CACHE_H
#define POSE_SINK_CACHE_H
/*
 * Push a labelled RAM range out of the CPU caches, so every label is in DRAM
 * before the sink reports Coverage and nothing a challenge reads comes back
 * from a cache line that was never written back.
 *
 * Whether this does any work depends on how the range is mapped (the /dev/mem
 * flags are chosen in sink_inventory.c, open_devmem).  An x86 carve-out is
 * mapped write-back (no O_SYNC) and this flush is what commits the labels.  An
 * arm64 carve-out is Device memory, so no label ever sits in a cache and the
 * flush is a cheap no-op, but a cacheable range would be committed here too.
 * The armv7 BMC cannot flush by address from user space, so it maps with
 * O_SYNC (non-cacheable, measured) instead.  The sink cannot tell which at
 * runtime, so it always flushes.
 */

#include <stddef.h>

/* Write back and invalidate every cache line of [p, p+len), then a barrier.
 * aarch64: DC CIVAC by VA + DSB SY.  x86-64: CLFLUSHOPT (or CLFLUSH where the
 * CPU lacks it) + SFENCE.  armv7: no cache maintenance by VA is available at
 * PL0, so a DSB only, which is sufficient for the non-cacheable mapping the
 * BMC gets and is all the old daemon did there. */
void        sink_cache_flush(const void *p, size_t len);

/* What sink_cache_flush does on this build, for the log. */
const char *sink_cache_flush_name(void);

#endif /* POSE_SINK_CACHE_H */
