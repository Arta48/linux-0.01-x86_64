/*
 * kernel/switch.S -- Переключение задач и первый запуск ребенка (ret_from_fork)
 */

.code64
.globl switch_to
.globl ret_from_fork

switch_to:
    pushfq
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, 0(%rdi)
    movq 0(%rsi), %rsp

    movq 8(%rsi), %rax
    testq %rax, %rax
    jz 1f
    movq %cr3, %rdx
    cmpq %rax, %rdx
    je 1f
    movq %rax, %cr3
1:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    popfq
    ret

/*
 * Первый запуск клонированного процесса:
 * RSP указывает на скопированный struct trap_frame ребенка
 */
ret_from_fork:
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
    addq $16, %rsp      /* Пропускаем int_no и error_code */
    iretq               /* Возврат в Ring 3 с RAX = 0 */
