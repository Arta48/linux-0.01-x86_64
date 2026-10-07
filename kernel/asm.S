/*
 * kernel/asm.S -- Низкоуровневые обработчики прерываний для x86_64
 */

.code64

.macro ISR_NOERR index
.globl isr\index
isr\index:
    pushq $0                    /* Фиктивный код ошибки */
    pushq $\index               /* Номер прерывания */
    jmp isr_common_stub
.endm

.macro ISR_ERR index
.globl isr\index
isr\index:
    /* Код ошибки уже в стеке от процессора */
    pushq $\index               /* Номер прерывания */
    jmp isr_common_stub
.endm

/* Исключения процессора (0..31) */
ISR_NOERR 0   /* Divide Error */
ISR_NOERR 1   /* Debug */
ISR_NOERR 2   /* NMI */
ISR_NOERR 3   /* Breakpoint */
ISR_NOERR 4   /* Overflow */
ISR_NOERR 5   /* Bound Range Exceeded */
ISR_NOERR 6   /* Invalid Opcode */
ISR_NOERR 7   /* Device Not Available */
ISR_ERR   8   /* Double Fault */
ISR_NOERR 9   /* Coprocessor Segment Overrun */
ISR_ERR   10  /* Invalid TSS */
ISR_ERR   11  /* Segment Not Present */
ISR_ERR   12  /* Stack-Segment Fault */
ISR_ERR   13  /* General Protection Fault */
ISR_ERR   14  /* Page Fault */
ISR_NOERR 15  /* Reserved */
ISR_NOERR 16  /* x87 FPU Error */
ISR_ERR   17  /* Alignment Check */
ISR_NOERR 18  /* Machine Check */
ISR_NOERR 19  /* SIMD Floating-Point Exception */

/* Аппаратные прерывания IRQ (32..47) */
ISR_NOERR 32  /* IRQ0: Timer PIT */
ISR_NOERR 33  /* IRQ1: Keyboard PS/2 */

isr_common_stub:
    /* Сохраняем все регистры общего назначения */
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

    /* Первый аргумент в C (rdi) = указатель на кадр стека (rsp) */
    movq %rsp, %rdi
    call isr_handler

    /* Восстанавливаем регистры */
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

    /* Снимаем номер прерывания и код ошибки */
    addq $16, %rsp
    iretq
