/*
 * user/crt0.S -- Точка входа C Runtime пользовательского пространства (Ring 3)
 */

.code64
.section .text.entry
.globl _start

_start:
    /* Согласно System V AMD64 ABI:
       %rdi = argc
       %rsi = argv
    */

    /* Выравниваем стек по границе 16 байт */
    pushq %rbp
    movq  %rsp, %rbp
    andq  $-16, %rsp

    /* Запуск функции main(argc, argv) */
    call main

    /* Возвращаемое значение main (%rax) передаем в exit(%rdi) */
    movq %rax, %rdi
    call exit

1:  hlt
    jmp 1b
