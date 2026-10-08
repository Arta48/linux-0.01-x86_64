#include <linux/smp.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/string.h>

extern void load_idt(void);
extern char trampoline_start[];
extern char trampoline_end[];
extern uint32_t trampoline_cr3;
extern uint64_t trampoline_stack;
extern uint64_t trampoline_entry;
extern char pml4_table[];

struct cpu_info cpus[MAX_CPUS];
volatile int smp_num_cpus = 1;

static volatile uint32_t *lapic = (volatile uint32_t *)LAPIC_DEFAULT_BASE;

static inline void wrmsr(uint32_t msr, uint64_t val)
{
    uint32_t low = val & 0xFFFFFFFF;
    uint32_t high = val >> 32;
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline uint32_t lapic_read(uint32_t reg)
{
    return lapic[reg >> 2];
}

static inline void lapic_write(uint32_t reg, uint32_t val)
{
    lapic[reg >> 2] = val;
}

uint32_t smp_get_cpu_id(void)
{
    if (!lapic) return 0;
    return (lapic_read(LAPIC_ID) >> 24) & 0xFF;
}

void lapic_init(void)
{
    /* Включаем Local APIC через MSR 0x1B (бит 11) */
    uint64_t base_msr = rdmsr(0x1B);
    base_msr |= (1 << 11);
    wrmsr(0x1B, base_msr);

    /* Программируем Spurious Vector: вектор 0xFF + бит 8 (Software Enable) */
    lapic_write(LAPIC_SVR, 0x1FF);

    /* Сбрасываем Task Priority Register (разрешаем все прерывания) */
    lapic_write(LAPIC_TPR, 0x00);
}

static void pit_delay_ms(int ms)
{
    for (volatile int i = 0; i < ms * 200000; i++) {
        __asm__ volatile ("pause");
    }
}

void ap_startup(void)
{
    /* Загружаем IDT ядра на вторичном ядре */
    load_idt();
    lapic_init();

    uint32_t ap_id = smp_get_cpu_id();
    if (ap_id < MAX_CPUS) {
        cpus[ap_id].id = ap_id;
        cpus[ap_id].online = 1;
    }

    __sync_fetch_and_add(&smp_num_cpus, 1);

    printk("[OK] SMP: Application Processor (Core %d) online in 64-bit Long Mode!\n", (int)ap_id);

    __asm__ volatile ("sti");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void smp_init(void)
{
    /* Проецируем страницу Local APIC (0xFEE00000) */
    map_page(NULL, LAPIC_DEFAULT_BASE, LAPIC_DEFAULT_BASE, PTE_WRITABLE);

    for (int i = 0; i < MAX_CPUS; i++) {
        cpus[i].id = i;
        cpus[i].online = 0;
        cpus[i].kstack = 0;
    }

    lapic_init();

    uint32_t bsp_id = smp_get_cpu_id();
    cpus[bsp_id].id = bsp_id;
    cpus[bsp_id].online = 1;
    smp_num_cpus = 1;

    printk("[OK] SMP: Bootstrap Processor (Core %d) initialized\n", (int)bsp_id);

    /* Копируем трамплин в физическую память по адресу 0x8000 */
    uint64_t trampoline_size = (uint64_t)(trampoline_end - trampoline_start);
    memcpy((void *)0x8000, trampoline_start, trampoline_size);

    /* Выделяем непрерывный 16 КБ ядерный стек для вторичного ядра */
    uint64_t ap_stack = get_free_pages(4);
    uint64_t ap_stack_top = ap_stack + (4 * PAGE_SIZE) - 16;
    cpus[1].kstack = ap_stack_top;

    /* Записываем параметры в код трамплина на 0x8000 */
    uint32_t *p_cr3   = (uint32_t *)(0x8000 + ((char *)&trampoline_cr3 - trampoline_start));
    uint64_t *p_stack = (uint64_t *)(0x8000 + ((char *)&trampoline_stack - trampoline_start));
    uint64_t *p_entry = (uint64_t *)(0x8000 + ((char *)&trampoline_entry - trampoline_start));

    *p_cr3   = (uint32_t)(uint64_t)pml4_table;
    *p_stack = ap_stack_top;
    *p_entry = (uint64_t)ap_startup;

    /* 1. Рассылаем INIT IPI всем процессорам кроме текущего (All Excluding Self: 0x000C4500) */
    lapic_write(LAPIC_ICR_HIGH, 0x00000000);
    lapic_write(LAPIC_ICR_LOW,  0x000C4500);
    pit_delay_ms(10);

    /* 2. Рассылаем Startup IPI (SIPI) с вектором 0x08 (0x08 * 4096 = 0x8000) */
    lapic_write(LAPIC_ICR_HIGH, 0x00000000);
    lapic_write(LAPIC_ICR_LOW,  0x000C4608);
    pit_delay_ms(1);

    /* Повторный SIPI по спецификации Intel MP */
    lapic_write(LAPIC_ICR_HIGH, 0x00000000);
    lapic_write(LAPIC_ICR_LOW,  0x000C4608);
    pit_delay_ms(10);

    printk("[OK] SMP: Multiprocessor boot complete. Active cores: %d\n", smp_num_cpus);
}
