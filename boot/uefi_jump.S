.code64
.globl uefi_kernel_jump

uefi_kernel_jump:
    /* Вход по System V ABI: rdi = magic, rsi = mbi_ptr, rdx = entry_point */
    cli
    cld
    movq %rdx, %rax    /* entry_point -> rax */
    jmp *%rax
