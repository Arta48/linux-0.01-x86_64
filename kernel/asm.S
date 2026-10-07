/*
 * kernel/asm.S -- ISR и переход в Ring 3
 */

.code64

.macro ISR_NOERR index
.globl isr\index
isr\index:
    pushq $0
    pushq $\index
    jmp isr_common_stub
.endm

.macro ISR_ERR index
.globl isr\index
isr\index:
    pushq $\index
    jmp isr_common_stub
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19

ISR_NOERR 32  /* IRQ0 */
ISR_NOERR 33  /* IRQ1 */

ISR_NOERR 128 /* int 0x80 (System Call) */

isr_common_stub:
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

    movq %rsp, %rdi
    call isr_handler

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

/*
 * void enter_user_mode(uint64_t entry_point, uint64_t user_stack);
 * rdi = entry_point (RIP)
 * rsi = user_stack  (RSP)
 */
.globl enter_user_mode
enter_user_mode:
    movq %rdi, %rax
    movq %rsi, %rbx

    /* Загружаем пользовательский селектор данных в DS, ES, FS, GS */
    movw $0x1B, %cx     /* USER_DS = 0x18 | 3 */
    movw %cx, %ds
    movw %cx, %es
    movw %cx, %fs
    movw %cx, %gs

    /* Формируем кадр стека iretq: SS, RSP, RFLAGS, CS, RIP */
    pushq $0x1B         /* SS: User Data Segment */
    pushq %rbx         /* RSP: Вершина стека пользователя */

    pushfq              /* RFLAGS */
    popq  %rdx
    orq   $0x200, %rdx  /* Устанавливаем бит IF (прерывания разрешены) */
    pushq %rdx

    pushq $0x23         /* CS: User Code Segment (0x20 | 3) */
    pushq %rax         /* RIP: Адрес входа функции пользователя */

    iretq               /* Прыжок в Ring 3! */
