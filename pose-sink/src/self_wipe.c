/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#include "self_wipe.h"
#include "sink_proto.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <unistd.h>

/* linux/reboot.h command values (sys/reboot.h names them RB_*). */
#ifndef RB_HALT_SYSTEM
#define RB_HALT_SYSTEM 0xcdef0123u
#endif
#ifndef RB_AUTOBOOT
#define RB_AUTOBOOT 0x01234567u
#endif

/* ── action selection ─────────────────────────────────────────────────────── */

/* Map "halt" / "reboot" to a reboot(2) command; unknown → dflt. */
static unsigned action_from_env(const char *name, unsigned dflt)
{
    const char *v = getenv(name);
    if (!v) return dflt;
    if (strcmp(v, "halt") == 0)   return RB_HALT_SYSTEM;
    if (strcmp(v, "reboot") == 0) return RB_AUTOBOOT;
    fprintf(stderr, "%s=%s: unknown action, using default\n", name, v);
    return dflt;
}

static const char *action_name(unsigned cmd)
{
    return cmd == RB_AUTOBOOT ? "reboot" : "halt";
}

/* Final fallback when the trampoline cannot be used: finish unwiped. */
static __attribute__((noreturn)) void finish_unwiped(unsigned cmd)
{
    sync();
    reboot((int)cmd);
    for (;;) pause();   /* reboot(2) only returns on error; PID 1 must not exit */
}

/* ── trampoline (AArch64 + x86-64) ────────────────────────────────────────── */

#if defined(__aarch64__) || defined(__x86_64__)
#define POSE_HAVE_TRAMPOLINE 1

/*
 * wipe_range_t must match the load layout in self_wipe_aarch64.S / self_wipe_x86_64.S:
 *   offset 0: addr (8 bytes)
 *   offset 8: len  (8 bytes)
 */
typedef struct {
    uintptr_t addr;
    size_t    len;
} wipe_range_t;

#define MAX_WIPE_RANGES 128

/* Provided by self_wipe_aarch64.S (AArch64) or self_wipe_x86_64.S (x86-64). */
extern const uint8_t _pose_trampoline_start[];
extern const uint8_t _pose_trampoline_end[];

/*
 * Parse /proc/self/maps and collect every writable virtual address range,
 * excluding:
 *  - the trampoline page itself (we're running from it)
 *  - /dev/mem and /dev/pose_probe mappings (labeled DRAM — no sensitive
 *    daemon state there, and zeroing it would undo the proof's evidence)
 *
 * Returns number of ranges collected.
 */
static int collect_wipe_ranges(wipe_range_t *out, int max,
                                uintptr_t excl_start, size_t excl_len)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;

    int n = 0;
    char line[256];
    while (fgets(line, sizeof(line), f) && n < max) {
        unsigned long start, end;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3)
            continue;
        if (perms[1] != 'w')
            continue;
        if ((uintptr_t)start >= excl_start &&
            (uintptr_t)start <  excl_start + excl_len)
            continue;
        if (strstr(line, "/dev/mem") || strstr(line, "/dev/pose_probe"))
            continue;

        out[n].addr = (uintptr_t)start;
        out[n].len  = (size_t)(end - start);
        n++;
    }
    fclose(f);
    return n;
}

/* Zero every writable daemon page then reboot(cmd).  Never returns. */
static __attribute__((noreturn)) void wipe_and_finish(unsigned cmd)
{
    size_t tramp_len = (size_t)(_pose_trampoline_end - _pose_trampoline_start);
    long   page_sz   = sysconf(_SC_PAGE_SIZE);

    /*
     * Allocate one anonymous page to hold both the trampoline code and the
     * wipe-range table.  The trampoline executes from here so it can safely
     * zero all original daemon pages including .text and stack.
     */
    uint8_t *page = mmap(NULL, (size_t)page_sz,
                          PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED)
        finish_unwiped(cmd);

    memcpy(page, _pose_trampoline_start, tramp_len);

    /*
     * Place the wipe_range_t array immediately after the trampoline code,
     * 16-byte aligned so the trampoline's paired load can read it.
     */
    size_t table_offset = (tramp_len + 15u) & ~15u;
    int    max_entries  = (int)(((size_t)page_sz - table_offset)
                                / sizeof(wipe_range_t));
    if (max_entries > MAX_WIPE_RANGES) max_entries = MAX_WIPE_RANGES;

    wipe_range_t *ranges = (wipe_range_t *)(page + table_offset);
    int n = collect_wipe_ranges(ranges, max_entries,
                                 (uintptr_t)page, (size_t)page_sz);

    /* Flush the I-cache so the CPU sees the copied trampoline instructions. */
    __builtin___clear_cache((char *)page, (char *)page + tramp_len);

    /*
     * Hand off to the trampoline.  After this call the original stack,
     * heap, and .text are zeroed; execution never returns.
     *
     * Cast through uintptr_t to suppress the function-pointer-to-object-
     * pointer conversion warning; PROT_EXEC was set above.
     */
    typedef __attribute__((noreturn)) void (*tramp_fn)(wipe_range_t *, int, unsigned);
    tramp_fn fn = (tramp_fn)(uintptr_t)page;
    fn(ranges, n, cmd);
}

#else /* armv7 BMC and anything else: no trampoline */
#define POSE_HAVE_TRAMPOLINE 0

static __attribute__((noreturn)) void wipe_and_finish(unsigned cmd)
{
    finish_unwiped(cmd);
}

#endif

/* ── public entry points ──────────────────────────────────────────────────── */

__attribute__((noreturn)) void self_wipe_finish(uint8_t finish_action)
{
    if (getpid() != 1) {
        /*
         * Ordinary process (a BMC, or a development run): we must NOT touch
         * the machine's power state, whatever Finish asked.  The connection is
         * closed and the process ends, so the daemon is no longer accessible.
         */
        printf("Not PID 1: exiting (power state left alone)\n");
        fflush(stdout);
        sync();
        _exit(0);
    }

    unsigned    cmd;
    const char *why;
    if (finish_action == SINK_FINISH_HALT) {
        cmd = RB_HALT_SYSTEM;
        why = "as Finish asked";
    } else if (finish_action == SINK_FINISH_REBOOT) {
        cmd = RB_AUTOBOOT;
        why = "as Finish asked";
    } else {
        /* SINK_FINISH_EXIT (or unknown): PID 1 cannot exit, so the image's
         * own choice decides. */
        cmd = action_from_env("POSE_FINAL_ACTION", RB_HALT_SYSTEM);
        why = "POSE_FINAL_ACTION, since PID 1 cannot exit";
    }
    printf("PID 1: wiping daemon pages, then %s (%s)\n", action_name(cmd), why);
    fflush(stdout);
    wipe_and_finish(cmd);
}

int daemon_exit(int code, const char *why)
{
    if (getpid() != 1) {
        if (why)
            fprintf(stderr, "exiting: %s\n", why);
        return code;
    }

    unsigned cmd = action_from_env("POSE_FAIL_ACTION", RB_AUTOBOOT);
    fprintf(stderr, "PID 1: %s (exit %d) — %s in %d s\n",
            why ? why : "error", code, action_name(cmd), POSE_FAIL_PAUSE_SEC);
    fflush(stdout);
    fflush(stderr);
    sleep(POSE_FAIL_PAUSE_SEC);
    wipe_and_finish(cmd);
}
