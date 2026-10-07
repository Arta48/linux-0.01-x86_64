#ifndef _LINUX_GDT_H
#define _LINUX_GDT_H

#include <linux/types.h>

#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_DS   0x1B   /* 0x18 | 3 */
#define USER_CS   0x23   /* 0x20 | 3 */
#define TSS_SEL   0x28

struct tss_entry {
    uint32_t reserved0;
    uint64_t rsp0;       /* Стек ядра для возврата из Ring 3 */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

void gdt_init(void);
void set_tss_stack(uint64_t kstack);

#endif
