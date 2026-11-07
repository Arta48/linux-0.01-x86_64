/*
 * kernel/syscall_entry.S -- шлюз инструкции syscall.
 *
 * Строит полный struct trap_frame (как isr_common_stub), чтобы fork/execve/
 * sigreturn могли менять регистры пользователя, и возвращается через iretq
 * (без ограничений sysret на неканонические RIP и различий Intel/AMD).
 */

.code64
.globl syscall_entry
.globl kernel_current_stack
.globl user_rsp_scratch
.globl syscall_return_to_user

.section .data
.align 8
user_rsp_scratch:
    .quad 0

.section .text
syscall_entry:
    movq %rsp, user_rsp_scratch(%rip)
    movq kernel_current_stack(%rip), %rsp

    pushq $0x1B                     /* SS  */
    pushq user_rsp_scratch(%rip)    /* RSP */
    pushq %r11                      /* RFLAGS (syscall положил его в r11) */
    pushq $0x23                     /* CS  */
    pushq %rcx                      /* RIP (syscall положил его в rcx) */
    pushq $0                        /* error_code */
    pushq $0x100                    /* int_no: признак syscall */

    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    cld
    movq %rsp, %rdi
    call syscall_entry_c

syscall_return_to_user:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    iretq
