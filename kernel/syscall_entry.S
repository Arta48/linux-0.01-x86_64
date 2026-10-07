/*
 * kernel/syscall_entry.S -- Аппаратный шлюз syscall с поддержкой 4 аргументов
 */

.code64
.globl syscall_entry
.globl kernel_current_stack
.globl user_rsp_scratch

.section .data
.align 8
user_rsp_scratch:
    .quad 0

.section .text
syscall_entry:
    /* 1. Сохраняем пользовательский RSP и переключаемся на стек ядра */
    movq %rsp, user_rsp_scratch(%rip)
    movq kernel_current_stack(%rip), %rsp

    /* 2. Сохраняем регистры пользователя для sysretq */
    pushq user_rsp_scratch(%rip)    /* +112: User RSP */
    pushq %r11                      /* +104: User RFLAGS */
    pushq %rcx                      /* +96:  User RIP */

    /* 3. Сохраняем регистры пользователя */
    pushq %r15                      /* +88 */
    pushq %r14                      /* +80 */
    pushq %r13                      /* +72 */
    pushq %r12                      /* +64 */
    pushq %rbp                      /* +56 */
    pushq %rbx                      /* +48 */
    pushq %r10                      /* +40: 4-й аргумент syscall (arg4) */
    pushq %r9                       /* +32 */
    pushq %r8                       /* +24 */
    pushq %rdx                      /* +16: 3-й аргумент (arg3) */
    pushq %rsi                      /* +8:  2-й аргумент (arg2) */
    pushq %rdi                      /* +0:  1-й аргумент (arg1) */

    /* 4. Передаем аргументы в C-функцию syscall_dispatcher:
          rdi = nr    (из rax)
          rsi = arg1  (из 0(%rsp))
          rdx = arg2  (из 8(%rsp))
          rcx = arg3  (из 16(%rsp))
          r8  = arg4  (из 40(%rsp) - был r10)
          r9  = tf    (NULL)
    */
    movq %rax, %rdi
    movq 0(%rsp),  %rsi
    movq 8(%rsp),  %rdx
    movq 16(%rsp), %rcx
    movq 40(%rsp), %r8
    xorq %r9,      %r9

    call syscall_dispatcher

    /* 5. Восстанавливаем регистры пользователя */
    popq %rdi
    popq %rsi
    popq %rdx
    popq %r8
    popq %r9
    popq %r10
    popq %rbx
    popq %rbp
    popq %r12
    popq %r13
    popq %r14
    popq %r15

    popq %rcx
    popq %r11
    popq %rsp

    sysretq
