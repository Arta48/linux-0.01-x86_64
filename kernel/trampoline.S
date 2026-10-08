/*
 * kernel/trampoline.S -- 16-битный трамплин пробуждения ядер AP (SIPI вектор 0x08 -> 0x8000)
 */

.code16
.globl trampoline_start
.globl trampoline_end
.globl trampoline_cr3
.globl trampoline_stack
.globl trampoline_entry

trampoline_start:
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x7C00, %sp

    /* Загружаем 32-битную GDTR (смещение 0x8000) */
    movw $(0x8000 + (trampoline_gdt_desc - trampoline_start)), %bx
    lgdt (%bx)

    /* Включаем Protected Mode (CR0.PE = 1) */
    movl %cr0, %eax
    orl  $1, %eax
    movl %eax, %cr0

    /* Прыжок в 32-битный сегмент Protected Mode (Селектор 0x08, D=1, L=0) */
    ljmpl $0x08, $(0x8000 + (trampoline_32 - trampoline_start))

.code32
trampoline_32:
    /* Загружаем 32-битные сегменты данных */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    /* Включаем PAE в CR4 */
    movl %cr4, %eax
    orl  $(1 << 5), %eax
    movl %eax, %cr4

    /* Загружаем CR3 (указатель на pml4_table ядра) */
    movl (0x8000 + (trampoline_cr3 - trampoline_start)), %eax
    movl %eax, %cr3

    /* Включаем Long Mode (EFER.LME = 1) */
    movl $0xC0000080, %ecx
    rdmsr
    orl  $(1 << 8), %eax
    wrmsr

    /* Включаем Paging в CR0 (активирует 64-битный режим) */
    movl %cr0, %eax
    orl  $(1 << 31), %eax
    movl %eax, %cr0

    /* Прыжок в 64-битный сегмент Long Mode (Селектор 0x18, L=1) */
    ljmpl $0x18, $(0x8000 + (trampoline_64 - trampoline_start))

.code64
trampoline_64:
    /* Загружаем ядерные сегменты */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    /* Устанавливаем стек ядра AP */
    movq (0x8000 + (trampoline_stack - trampoline_start)), %rsp

    /* Переход в ap_startup */
    movq (0x8000 + (trampoline_entry - trampoline_start)), %rax
    jmp *%rax

.align 8
trampoline_gdt:
    .quad 0x0000000000000000  /* 0x00: Null */
    .quad 0x00CF9A000000FFFF  /* 0x08: 32-bit Code (D=1, L=0) */
    .quad 0x00CF92000000FFFF  /* 0x10: 32/64-bit Data (Read/Write) */
    .quad 0x00209A0000000000  /* 0x18: 64-bit Code (L=1, D=0) */
trampoline_gdt_desc:
    .word (4 * 8) - 1
    .long (0x8000 + (trampoline_gdt - trampoline_start))

trampoline_cr3:
    .long 0
trampoline_stack:
    .quad 0
trampoline_entry:
    .quad 0
trampoline_end:
