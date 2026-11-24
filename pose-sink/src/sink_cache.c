/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#include "sink_cache.h"

#include <stdint.h>

#if defined(__aarch64__)

/* Smallest data cache line from CTR_EL0.DminLine (log2 of words): 64 bytes on
 * Cortex-A78 (BlueField-3) and most others; read rather than assumed. */
static size_t line_bytes(void)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    return (size_t)4 << ((ctr >> 16) & 0xf);
}

void sink_cache_flush(const void *p, size_t len)
{
    size_t line = line_bytes();
    const char *a   = (const char *)((uintptr_t)p & ~(uintptr_t)(line - 1));
    const char *end = (const char *)p + len;
    /* DC CIVAC: clean (write back) and invalidate by VA to the point of
     * coherency.  DSB SY afterwards waits for all of them to complete. */
    for (; a < end; a += line)
        __asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb sy" : : : "memory");
}

const char *sink_cache_flush_name(void) { return "DC CIVAC"; }

#elif defined(__x86_64__)

#include <cpuid.h>

static int have_clflushopt(void)
{
    unsigned a, b, c, d;
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d))
        return 0;
    return (b >> 23) & 1;
}

void sink_cache_flush(const void *p, size_t len)
{
    /* 64-byte lines on every x86-64 made this century.  CLFLUSHOPT is the
     * fast, weakly ordered form; CLFLUSH is in the baseline ISA.  Either
     * writes back and invalidates the line; SFENCE orders them before any
     * later access. */
    static int opt = -1;
    if (opt < 0)
        opt = have_clflushopt();
    const char *a   = (const char *)((uintptr_t)p & ~(uintptr_t)63);
    const char *end = (const char *)p + len;
    if (opt) {
        for (; a < end; a += 64)
            __asm__ volatile("clflushopt %0" : : "m"(*a) : "memory");
    } else {
        for (; a < end; a += 64)
            __asm__ volatile("clflush %0" : : "m"(*a) : "memory");
    }
    __asm__ volatile("sfence" : : : "memory");
}

const char *sink_cache_flush_name(void) { return have_clflushopt() ? "CLFLUSHOPT" : "CLFLUSH"; }

#elif defined(__arm__)

void sink_cache_flush(const void *p, size_t len)
{
    /* DCCIMVAC traps at PL0 on ARMv7, so there is no clean-by-VA from user
     * space.  DSB drains the write buffer, which is all a non-cacheable
     * mapping needs; a cacheable one would not be covered (see the header). */
    (void)p; (void)len;
    __asm__ volatile("dsb" : : : "memory");
}

const char *sink_cache_flush_name(void) { return "DSB only (no clean-by-VA at PL0)"; }

#else
#error "sink_cache_flush: unsupported architecture (need aarch64, x86_64 or arm)"
#endif
