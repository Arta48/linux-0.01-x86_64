#include <linux/gdt.h>
#include <linux/tty.h>

/* GDT: 5 дескрипторов по 8 байт + 1 дескриптор TSS на 16 байт = 7 квадрослов */
static uint64_t gdt[7];

static struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdtr;

static struct tss_entry tss;

void set_tss_stack(uint64_t kstack)
{
    tss.rsp0 = kstack;
}

void gdt_init(void)
{
    /* 0x00: Нулевой дескриптор */
    gdt[0] = 0x0000000000000000ULL;

    /* 0x08: Kernel Code (Ring 0, 64-bit, Exec/Read) */
    gdt[1] = 0x00209A0000000000ULL;

    /* 0x10: Kernel Data (Ring 0, Read/Write) */
    gdt[2] = 0x0000920000000000ULL;

    /* 0x18: User Data (Ring 3, Read/Write: DPL=3, Present) */
    gdt[3] = 0x0000F20000000000ULL;

    /* 0x20: User Code (Ring 3, 64-bit, Exec/Read: DPL=3, L=1, Present) */
    gdt[4] = 0x0020FA0000000000ULL;

    /* 0x28: 64-битный дескриптор TSS (16 байт / 2 слота) */
    uint64_t tss_base = (uint64_t)&tss;
    uint64_t tss_limit = sizeof(struct tss_entry) - 1;

    /* Инициализируем поля TSS нулями */
    uint8_t *p = (uint8_t *)&tss;
    for (uint32_t i = 0; i < sizeof(struct tss_entry); i++) p[i] = 0;
    tss.iomap_base = sizeof(struct tss_entry);

    /* Младшие 8 байт дескриптора TSS */
    gdt[5] = (tss_limit & 0xFFFFULL) |
    ((tss_base & 0xFFFFULL) << 16) |
    (((tss_base >> 16) & 0xFFULL) << 32) |
    (0x89ULL << 40) | /* Type: 64-bit TSS (Available), Present */
    (((tss_limit >> 16) & 0xFULL) << 48) |
    (((tss_base >> 24) & 0xFFULL) << 56);

    /* Старшие 8 байт дескриптора TSS (содержат верхние 32 бита адреса базы) */
    gdt[6] = (tss_base >> 32) & 0xFFFFFFFFULL;

    /* Загружаем GDT */
    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (uint64_t)&gdt;
    __asm__ volatile ("lgdt %0" : : "m"(gdtr));

    /* Загружаем селектор TSS в регистр TR (Task Register) */
    __asm__ volatile ("ltr %%ax" : : "a"(TSS_SEL));

    printk("[OK] GDT & 64-bit TSS Initialized (User Segments 0x1B/0x23 Active)\n");
}
