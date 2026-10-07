/*
 * boot/boot.S -- Точка входа x86_64.
 * Переводит процессор из 32-битного режима Multiboot в 64-битный Long Mode.
 */

.set MULTIBOOT_MAGIC,    0x1BADB002
.set MULTIBOOT_FLAGS,    0x00000003   /* Выравнивание модулей + инфо о памяти */
.set MULTIBOOT_CHECKSUM, -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)

.section .multiboot, "a"
.align 4
.long MULTIBOOT_MAGIC
.long MULTIBOOT_FLAGS
.long MULTIBOOT_CHECKSUM

.section .bss
.align 4096
/* 4-уровневые таблицы страниц для инициализации (покрывают первые 1 ГБ памяти) */
pml4_table:
    .skip 4096
pdpt_table:
    .skip 4096
pd_table:
    .skip 4096
stack_bottom:
    .skip 16384 /* 16 КБ стека для ядра */
stack_top:

.section .rodata
/* 64-битная глобальная таблица дескрипторов (GDT) */
gdt64:
    .quad 0x0000000000000000          /* Нулевой дескриптор */
    .quad 0x00209A0000000000          /* 64-bit Code Segment (Ring 0): L-bit=1, D-bit=0 */
    .quad 0x0000920000000000          /* 64-bit Data Segment (Ring 0) */
gdt64_desc:
    .word . - gdt64 - 1
    .quad gdt64

.section .text
.code32
.globl _start
_start:
    cli
    cld
    movl $stack_top, %esp

    /* 1. Настраиваем начальные таблицы страниц (Identity Paging для первых 1 ГБ) */
    /* PML4[0] -> PDPT */
    movl $pdpt_table, %eax
    orl  $0x3, %eax                   /* Present | Writable */
    movl %eax, pml4_table

    /* PDPT[0] -> PD */
    movl $pd_table, %eax
    orl  $0x3, %eax                   /* Present | Writable */
    movl %eax, pdpt_table

    /* Заполняем PD (512 записей по 2 МБ Huge Pages = 1 ГБ памяти) */
    movl $0, %ecx                     /* Счетчик цикла: 0..511 */
set_pd_entries:
    movl $0x200000, %eax              /* 2 МБ */
    mull %ecx
    orl  $0x83, %eax                  /* Present | Writable | Page Size (2MB Huge) */
    movl %eax, pd_table(, %ecx, 8)
    incl %ecx
    cmpl $512, %ecx
    jne  set_pd_entries

    /* 2. Загружаем адрес PML4 в регистр CR3 */
    movl $pml4_table, %eax
    movl %eax, %cr3

    /* 3. Включаем бит PAE (Physical Address Extension) в CR4 */
    movl %cr4, %eax
    orl  $(1 << 5), %eax
    movl %eax, %cr4

    /* 4. Включаем Long Mode (бит LME) в EFER MSR (0xC0000080) */
    movl $0xC0000080, %ecx
    rdmsr
    orl  $(1 << 8), %eax              /* Бит 8: LME (Long Mode Enable) */
    wrmsr

    /* 5. Включаем пейджинг в CR0 (активирует переход в Long Mode) */
    movl %cr0, %eax
    orl  $(1 << 31) | (1 << 0), %eax  /* PG (Paging) и PE (Protection Enable) */
    movl %eax, %cr0

    /* 6. Загружаем новую 64-битную GDT */
    lgdt gdt64_desc

    /* 7. Дальний переход для смены CS на 64-битный селектор (0x08) */
    ljmp $0x08, $long_mode_entry

.code64
long_mode_entry:
    /* Сбрасываем сегментные регистры данных (в 64 битах они зануляются) */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    movq $stack_top, %rsp

    /* Переход в код на Си! */
    call main

1:  hlt
    jmp 1b
