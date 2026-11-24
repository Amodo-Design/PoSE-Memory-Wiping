/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#ifndef POSE_SELF_WIPE_H
#define POSE_SELF_WIPE_H

/*
 * How the daemon ends.
 *
 * Decided at runtime by whether the daemon is PID 1:
 *
 *  PID 1 (initramfs / UKI delivery: the DPU, the x86 host):
 *    there is no OS to preserve, and a PID 1 that simply exits panics the
 *    kernel ("Attempted to kill init").  Every end of the daemon therefore
 *    goes through reboot(2): the proof-accepted path zeroes the daemon's own
 *    pages first via the trampoline, error paths print, pause so the console
 *    can be read, and reboot.
 *
 *  Not PID 1 (stock Ubuntu x86/H200 experiment, development runs):
 *    the daemon is an ordinary process and must not touch the machine's power
 *    state.  The accepted path flushes and terminates the process; error
 *    paths return their exit code as before.
 *
 * Environment (set from the UKI cmdline through pose-init's pose.env=):
 *   POSE_FINAL_ACTION=halt|reboot   what to do as PID 1 when Finish asks for
 *                                   SINK_FINISH_EXIT, which PID 1 cannot do
 *                                   (default halt — the device must not
 *                                   remain accessible).  Finish's HALT and
 *                                   REBOOT override it.
 *   POSE_FAIL_ACTION=halt|reboot    what to do on any error as PID 1
 *                                   (default reboot, so a remote machine
 *                                   returns to its normal OS on its own)
 *
 * Labeled targets are left as they are at the end: RAM and disk keep their
 * labels (seed-derived noise; the old contents are gone already).  Only the
 * daemon's own pages are zeroed, by the trampoline, which skips the /dev/mem
 * mappings (and the sessions are closed before it runs anyway).
 */

#include <stdint.h>

/*
 * End the daemon as a Finish frame asked (finish_action is a SINK_FINISH_*
 * value from sink_proto.h).  As PID 1: zero all writable daemon pages (heap,
 * stack, .data/.bss) and then halt for SINK_FINISH_HALT, reboot for
 * SINK_FINISH_REBOOT, or do POSE_FINAL_ACTION for SINK_FINISH_EXIT (or any
 * unknown value), since PID 1 cannot exit; never returns.  As an ordinary
 * process (a BMC, a development run): sync and _exit(0) whatever was asked,
 * because such a daemon must never change the machine's power state.
 *
 * Execution is handed off to a position-independent trampoline copied to a
 * fresh anonymous page so the zeroing pass can overwrite daemon .text and
 * stack without pulling the rug out from the currently-executing code.
 * On any internal error (mmap failure, /proc/self/maps unreadable) it falls
 * back to an unwiped finish — ending unwiped is still better than staying up.
 */
__attribute__((noreturn)) void self_wipe_finish(uint8_t finish_action);

/*
 * Error exit.  As PID 1: print the reason, wait POSE_FAIL_PAUSE_SEC so the
 * console can be read, zero the daemon's pages (the seed may already be in
 * memory) and perform POSE_FAIL_ACTION; never returns.  Otherwise returns
 * `code` so the caller can `return daemon_exit(code);` from main(), after
 * printing the reason to stderr.
 */
int daemon_exit(int code, const char *why);

#define POSE_FAIL_PAUSE_SEC 30

#endif /* POSE_SELF_WIPE_H */
