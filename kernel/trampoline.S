/*
 * kernel/trampoline.S -- трамплин пробуждения ядер AP.
 *
 * Код позиционно-независим: страница может лежать в любом месте ниже 1 МБ
 * (на UEFI её выделяет загрузчик через AllocatePages, на BIOS/multiboot это
 * 0x8000). Вектор SIPI = адрес >> 12, поэтому CS:0 указывает на начало
 * страницы, а в защищённом режиме база страницы хранится в %ebx.
 * Абсолютные адреса (база GDT, far-указатели, CR3, стек, точка входа)
 * ядро записывает в блок данных в конце трамплина перед запуском каждого AP.
 */

.code16
.globl trampoline_start
.globl trampoline_end
.globl trampoline_gdt
.globl trampoline_gdt_desc
.globl trampoline_jmp32
.globl trampoline_jmp64
.globl trampoline_cr3
.globl trampoline_stack
.globl trampoline_entry
.globl trampoline_32
.globl trampoline_64

trampoline_start:
    cli
    cld
    movw %cs, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x0FF0, %sp

    movzwl %ax, %ebx
    shll $4, %ebx                   /* %ebx = физическая база трамплина */

    lgdtl (trampoline_gdt_desc - trampoline_start)

    movl %cr0, %eax
    orl  $1, %eax
    movl %eax, %cr0                 /* Protected Mode */

    ljmpl *(trampoline_jmp32 - trampoline_start)

.code32
trampoline_32:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    movl %cr4, %eax
    orl  $(1 << 5), %eax            /* PAE */
    movl %eax, %cr4

    movl (trampoline_cr3 - trampoline_start)(%ebx), %eax
    movl %eax, %cr3

    movl $0xC0000080, %ecx          /* EFER.LME */
    rdmsr
    orl  $(1 << 8), %eax
    wrmsr

    movl %cr0, %eax
    orl  $(1 << 31), %eax           /* Paging -> Long Mode */
    movl %eax, %cr0

    ljmpl *(trampoline_jmp64 - trampoline_start)(%ebx)

.code64
trampoline_64:
    movl %ebx, %ebx                 /* старшие 32 бита после смены режима не определены */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movq (trampoline_stack - trampoline_start)(%rbx), %rsp
    movq (trampoline_entry - trampoline_start)(%rbx), %rax
    jmp *%rax

.align 8
trampoline_gdt:
    .quad 0x0000000000000000        /* 0x00: Null */
    .quad 0x00CF9A000000FFFF        /* 0x08: 32-bit code */
    .quad 0x00CF92000000FFFF        /* 0x10: data */
    .quad 0x00209A0000000000        /* 0x18: 64-bit code */
trampoline_gdt_desc:
    .word (4 * 8) - 1
    .long 0                         /* база GDT (абсолютная) -- пишет ядро */
trampoline_jmp32:
    .long 0                         /* смещение trampoline_32 (абсолютное) */
    .word 0x08
    .word 0
trampoline_jmp64:
    .long 0                         /* смещение trampoline_64 (абсолютное) */
    .word 0x18
    .word 0
.align 8
trampoline_cr3:
    .long 0
    .long 0
trampoline_stack:
    .quad 0
trampoline_entry:
    .quad 0
trampoline_end:
