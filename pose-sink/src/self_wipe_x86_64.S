/*
 * Position-independent x86-64 trampoline: zeroes a caller-supplied list of
 * virtual address ranges then finishes via the reboot(2) syscall.  Same
 * contract as the AArch64 version (self_wipe_aarch64.S):
 *
 *   rdi = pointer to wipe_range_t[] stored in the trampoline page itself
 *   rsi = number of entries
 *   rdx = reboot(2) command: LINUX_REBOOT_CMD_HALT or LINUX_REBOOT_CMD_RESTART
 *
 * No stack is used (the original stack is one of the ranges being zeroed).
 * No libc; only direct syscalls.  The syscall instruction clobbers rcx and
 * r11, so loop state lives in r8-r10 and the reboot command in r12 (callee-
 * saved, but we never return, so clobbering it is fine).
 *
 * x86-64 Linux syscall numbers:
 *   sync   = 162
 *   reboot = 169
 */

.section .text.trampoline, "ax", @progbits
.balign 64
.globl _pose_trampoline_start
.type  _pose_trampoline_start, @function

_pose_trampoline_start:
    mov     %rdi, %r9           /* ranges base */
    mov     %rsi, %r10          /* entry count */
    mov     %rdx, %r12          /* reboot command */
    xor     %r8d, %r8d          /* i = 0 */

.Lrange_loop:
    cmp     %r10, %r8
    jae     .Ldone_zeroing

    mov     %r8, %rax
    shl     $4, %rax            /* i * sizeof(wipe_range_t) */
    add     %r9, %rax           /* &ranges[i] */
    mov     (%rax), %rdi        /* addr */
    mov     8(%rax), %rcx       /* len  */
    inc     %r8

    add     $15, %rdi           /* align start up to 16 */
    and     $-16, %rdi
    and     $-16, %rcx          /* align length down to 16 */
    shr     $3, %rcx            /* qword count */
    xor     %eax, %eax
    cld
    rep stosq                   /* zero [rdi, rdi + 8*rcx) */
    jmp     .Lrange_loop

.Ldone_zeroing:
    mov     $162, %eax          /* sync() */
    syscall

    mov     $0xfee1dead, %edi   /* LINUX_REBOOT_MAGIC1 */
    mov     $0x28121969, %esi   /* LINUX_REBOOT_MAGIC2 */
    mov     %r12d, %edx         /* cmd from caller */
    xor     %r10d, %r10d        /* arg = NULL */
    mov     $169, %eax          /* reboot() */
    syscall

    /* Should not be reached. */
.Lspin:
    pause
    jmp     .Lspin

.globl _pose_trampoline_end
.type  _pose_trampoline_end, @function
_pose_trampoline_end:
    .size _pose_trampoline_start, _pose_trampoline_end - _pose_trampoline_start

.section .note.GNU-stack, "", @progbits
