/*
 * Position-independent AArch64 trampoline.  Zeroes a caller-supplied list of
 * virtual address ranges then halts via the reboot(2) syscall.
 *
 * Entry contract (set by self_wipe.c before branching here):
 *   x0 = pointer to wipe_range_t[] stored in the trampoline page itself
 *   x1 = number of entries
 *   x2 = reboot(2) command: LINUX_REBOOT_CMD_HALT or LINUX_REBOOT_CMD_RESTART
 *
 * No stack frame is used — all state lives in caller-saved registers so the
 * trampoline can zero the original stack without losing context.  No libc
 * calls; only direct svc syscalls.
 *
 * AArch64 Linux syscall numbers (asm-generic/unistd.h):
 *   sync   = 81
 *   reboot = 142
 */

.section .text.trampoline, "ax", %progbits
.balign 64
.global _pose_trampoline_start
.type   _pose_trampoline_start, %function

_pose_trampoline_start:
    /*
     * Register allocation (caller-saved, safe across the whole trampoline):
     *   x9  = ranges base pointer
     *   x10 = total entry count
     *   x11 = loop index i
     *   x12 = &ranges[i]
     *   x13 = current range addr (walks forward during zeroing)
     *   x14 = remaining bytes in current range
     *   x15 = reboot command (preserved across the zeroing loop)
     */
    mov     x9,  x0
    mov     x10, x1
    mov     x15, x2
    mov     x11, xzr            /* i = 0 */

.Lrange_loop:
    cmp     x11, x10
    b.hs    .Ldone_zeroing

    lsl     x12, x11, #4        /* offset = i * sizeof(wipe_range_t) = i * 16 */
    add     x12, x9,  x12       /* x12 = &ranges[i] */
    ldp     x13, x14, [x12]     /* x13 = addr, x14 = len */
    add     x11, x11, #1        /* i++ */

    /* Align start up to 16-byte boundary (all kernel-allocated pages are
     * page-aligned so this only adjusts sub-page-aligned edge regions). */
    add     x13, x13, #15
    and     x13, x13, #~0xf

    /* Align length down to 16 bytes. */
    and     x14, x14, #~0xf

.Lzero_loop:
    cbz     x14, .Lrange_loop
    stp     xzr, xzr, [x13], #16
    sub     x14, x14, #16
    b       .Lzero_loop

.Ldone_zeroing:
    /* sync() — flush any dirty kernel buffers */
    mov     x8, #81
    svc     #0

    /* reboot(MAGIC1, MAGIC2, cmd, NULL) */
    mov     w0, #0xdead
    movk    w0, #0xfee1, lsl #16    /* LINUX_REBOOT_MAGIC1 = 0xfee1dead */
    mov     w1, #0x1969
    movk    w1, #0x2812, lsl #16    /* LINUX_REBOOT_MAGIC2 = 0x28121969 */
    mov     w2, w15                 /* cmd from caller (HALT or RESTART) */
    mov     x3, xzr                 /* arg = NULL */
    mov     x8, #142                /* __NR_reboot */
    svc     #0

    /* Should not be reached — spin in low-power state. */
.Lspin:
    wfi
    b       .Lspin

.global _pose_trampoline_end
.type   _pose_trampoline_end, %function
_pose_trampoline_end:
    .size _pose_trampoline_start, _pose_trampoline_end - _pose_trampoline_start
