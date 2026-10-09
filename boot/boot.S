/*
 * boot/boot.S -- Сохранение параметров Multiboot и переход в 64-бит
 */

.set MULTIBOOT_MAGIC,    0x1BADB002
.set MULTIBOOT_FLAGS,    0x00000003
.set MULTIBOOT_CHECKSUM, -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)

.section .multiboot, "a"
.align 4
.long MULTIBOOT_MAGIC
.long MULTIBOOT_FLAGS
.long MULTIBOOT_CHECKSUM

.align 8
.globl uefi_entry_ptr
uefi_entry_ptr:
    .quad uefi_entry

.section .data
.align 8
mb_magic_val:
    .quad 0
mb_info_val:
    .quad 0

.section .bss
.align 4096
.globl pml4_table
pml4_table:
    .skip 4096
.globl pdpt_table
pdpt_table:
    .skip 4096
.globl pd_table
pd_table:
    .skip 4096
stack_bottom:
    .skip 16384
stack_top:

.section .rodata
gdt64:
    .quad 0x0000000000000000
    .quad 0x00209A0000000000
    .quad 0x0000920000000000
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

    /* 0. Сохраняем аргументы Multiboot из EAX и EBX */
    movl %eax, mb_magic_val
    movl $0,   mb_magic_val + 4
    movl %ebx, mb_info_val
    movl $0,   mb_info_val + 4

    /* 1. Настройка 4-уровневых таблиц страниц (1 ГБ identity mapped) */
    movl $pdpt_table, %eax
    orl  $0x7, %eax
    movl %eax, pml4_table

    movl $pd_table, %eax
    orl  $0x7, %eax
    movl %eax, pdpt_table

    movl $0, %ecx
set_pd_entries:
    movl $0x200000, %eax
    mull %ecx
    orl  $0x87, %eax
    movl %eax, pd_table(, %ecx, 8)
    incl %ecx
    cmpl $512, %ecx
    jne  set_pd_entries

    movl $pml4_table, %eax
    movl %eax, %cr3

    /* 2. Включаем PAE, OSFXSR, OSXMMEXCPT */
    movl %cr4, %eax
    orl  $((1 << 5) | (1 << 9) | (1 << 10)), %eax
    movl %eax, %cr4

    /* 3. Очищаем CR0.EM, включаем CR0.MP */
    movl %cr0, %eax
    andl $~(1 << 2), %eax
    orl  $(1 << 1), %eax
    movl %eax, %cr0

    /* 4. Включаем Long Mode (бит 8 LME) и SCE (бит 0) */
    movl $0xC0000080, %ecx
    rdmsr
    orl  $((1 << 8) | (1 << 0)), %eax
    wrmsr

    /* 5. Включаем пейджинг в CR0 */
    movl %cr0, %eax
    orl  $(1 << 31) | (1 << 0), %eax
    movl %eax, %cr0

    /* 6. Загружаем временную GDT и прыгаем в 64-битный режим */
    lgdt gdt64_desc
    ljmp $0x08, $long_mode_entry

.code64
.globl uefi_entry
uefi_entry:
    cli
    cld
    /* 1. Инициализируем ядерный стек */
    leaq stack_top(%rip), %rsp

    /* 2. Сохраняем переданные параметры magic (%rdi) и mbi (%rsi) */
    movq %rdi, mb_magic_val(%rip)
    movq %rsi, mb_info_val(%rip)

    /* Включаем EFER.SCE (бит 0) и EFER.LME (бит 8) для аппаратного syscall */
    movl $0xC0000080, %ecx
    rdmsr
    orl  $((1 << 8) | (1 << 0)), %eax
    wrmsr

    /* 3. Настройка таблиц страниц ядра (1 ГБ identity map) на чистом x86_64 */
    leaq pdpt_table(%rip), %rax
    orq  $0x7, %rax
    movq %rax, pml4_table(%rip)

    leaq pd_table(%rip), %rax
    orq  $0x7, %rax
    movq %rax, pdpt_table(%rip)

    xorq %rcx, %rcx
1:
    movq %rcx, %rax
    shlq $21, %rax              /* 2MB huge pages */
    orq  $0x87, %rax            /* Present | Writable | PageSize(2MB) */
    leaq pd_table(%rip), %rdx
    movq %rax, (%rdx, %rcx, 8)
    incq %rcx
    cmpq $512, %rcx
    jne  1b

    /* 4. Загружаем CR3 ядра */
    leaq pml4_table(%rip), %rax
    movq %rax, %cr3

    /* 5. Загружаем 64-битную GDT ядра */
    lgdt gdt64_desc(%rip)

    /* 6. Переключаем CS на ядерный селектор 0x08 через 64-битный far return */
    pushq $0x08
    leaq 2f(%rip), %rax
    pushq %rax
    lretq

2:
    /* 7. Загружаем сегменты данных ядра (0x10) */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* 8. Передаем аргументы и вызываем функцию main ядра */
    movq mb_magic_val(%rip), %rdi
    movq mb_info_val(%rip),  %rsi
    call main
3:  hlt
    jmp 3b

.code64
long_mode_entry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    movq $stack_top, %rsp

    /* Передаем Multiboot magic (%rdi) и Multiboot info ptr (%rsi) в main */
    movq mb_magic_val(%rip), %rdi
    movq mb_info_val(%rip),  %rsi

    call main

1:  hlt
    jmp 1b
