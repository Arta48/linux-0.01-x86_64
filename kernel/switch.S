/*
 * kernel/switch.S -- Программное переключение контекста для x86_64
 */

.code64
.globl switch_to

switch_to:
    /* 1. Сохраняем регистры текущего процесса в его стек */
    pushfq
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* 2. Сохраняем текущий RSP в prev->rsp (смещение 0) */
    movq %rsp, 0(%rdi)

    /* 3. Загружаем новый RSP из next->rsp (смещение 0) */
    movq 0(%rsi), %rsp

    /* 4. При необходимости переключаем таблицы страниц (смещение 8 - cr3) */
    movq 8(%rsi), %rax
    testq %rax, %rax
    jz 1f
    movq %cr3, %rdx
    cmpq %rax, %rdx
    je 1f
    movq %rax, %cr3
1:
    /* 5. Восстанавливаем регистры нового процесса из его стека */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    popfq

    /* 6. Прыжок в новый процесс! */
    ret
