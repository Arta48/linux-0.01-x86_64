/*
 * kernel/syscall_entry.S -- Аппаратный шлюз syscall / sysretq с полной защитой регистров
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

    /* 2. Сохраняем служебные регистры для возврата через sysretq */
    pushq user_rsp_scratch(%rip)    /* User RSP */
    pushq %r11                      /* User RFLAGS (сохранен процессором) */
    pushq %rcx                      /* User RIP (сохранен процессором) */

    /* 3. Сохраняем ВСЕ регистры пользователя, чтобы Си-код их не повредил */
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %rbp
    pushq %rbx
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rdx
    pushq %rsi
    pushq %rdi

    /* 4. Передаем аргументы в C-функцию syscall_dispatcher:
          rdi = nr    (из rax)
          rsi = arg1  (из 0(%rsp) - rdi)
          rdx = arg2  (из 8(%rsp) - rsi)
          rcx = arg3  (из 16(%rsp) - rdx)
          r8  = tf    (NULL)
    */
    movq %rax, %rdi
    movq 0(%rsp),  %rsi
    movq 8(%rsp),  %rdx
    movq 16(%rsp), %rcx
    xorq %r8,      %r8

    call syscall_dispatcher
    /* Результат вызова в %rax */

    /* 5. Восстанавливаем сохраненные регистры пользователя */
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

    popq %rcx                       /* User RIP */
    popq %r11                       /* User RFLAGS */
    popq %rsp                       /* User RSP */

    /* 6. Возврат в Ring 3 */
    sysretq
