#ifndef _LINUX_TRAPS_H
#define _LINUX_TRAPS_H

#include <linux/types.h>

/* Стековый кадр прерывания (все сохраненные регистры) */
struct trap_frame {
    /* Сохраняются ассемблерной заглушкой вручную */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, error_code;

    /* Сохраняются процессором автоматически */
    uint64_t rip, cs, rflags, rsp, ss;
};

/* 16-байтовый дескриптор прерывания x86_64 */
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

/* Указатель для инструкции lidt */
struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void trap_init(void);

#endif
