#include <linux/smp.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/string.h>
#include <linux/nic.h>

extern void load_idt(void);
extern char trampoline_start[];
extern char trampoline_end[];
extern char trampoline_cr3[];
extern char trampoline_stack[];
extern char trampoline_entry[];
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

#include <linux/acpi.h>
#include <linux/gdt.h>
#include <linux/multiboot.h>
#include <linux/nic.h>

#ifndef SMP_ENABLE
#define SMP_ENABLE 1
#endif

extern char trampoline_gdt[], trampoline_gdt_desc[], trampoline_jmp32[], trampoline_jmp64[];
extern char trampoline_32[], trampoline_64[];
extern struct mb_info mbi;

static volatile uint8_t ap_flag[MAX_CPUS];      /* AP с данным индексом дошёл до C-кода */

void ap_startup(void)
{
    gdt_load_ap();
    load_idt();
    lapic_init();

    uint32_t id = smp_get_cpu_id();
    int idx = -1;
    for (int i = 0; i < MAX_CPUS; i++) if (cpus[i].id == id && cpus[i].kstack) { idx = i; break; }
    if (idx >= 0) { cpus[idx].online = 1; ap_flag[idx] = 1; }
    __sync_fetch_and_add(&smp_num_cpus, 1);

    /* Планировщик пока однопроцессорный: AP припаркован с выключенными
     * прерываниями (на них не заведены ни IRQ, ни таймер). */
    for (;;) __asm__ volatile ("cli; hlt");
}

static int icr_wait(void)
{
    for (int t = 0; t < 1000; t++) {
        if (!(lapic_read(LAPIC_ICR_LOW) & (1 << 12))) return 0;
        udelay(10);
    }
    return -1;
}

static void send_ipi(uint32_t apic_id, uint32_t low)
{
    icr_wait();
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, low);
    icr_wait();
}

/* Запуск одного AP: INIT, пауза 10 мс, два SIPI. Ждём до 200 мс. */
static int start_ap(uint64_t tramp, int idx, uint32_t apic_id)
{
    uint64_t stack = get_free_pages(4);
    if (!stack) return -1;
    uint64_t top = stack + 4 * PAGE_SIZE - 16;
    cpus[idx].id = apic_id;
    cpus[idx].kstack = top;
    cpus[idx].online = 0;
    ap_flag[idx] = 0;

    /* Блок параметров в странице трамплина (абсолютные адреса) */
    uint64_t off_gdt  = (uint64_t)(trampoline_gdt - trampoline_start);
    uint64_t off_desc = (uint64_t)(trampoline_gdt_desc - trampoline_start);
    uint64_t off_j32  = (uint64_t)(trampoline_jmp32 - trampoline_start);
    uint64_t off_j64  = (uint64_t)(trampoline_jmp64 - trampoline_start);
    uint64_t off_cr3  = (uint64_t)(trampoline_cr3 - trampoline_start);
    uint64_t off_stk  = (uint64_t)(trampoline_stack - trampoline_start);
    uint64_t off_ent  = (uint64_t)(trampoline_entry - trampoline_start);
    uint64_t off_32   = (uint64_t)(trampoline_32 - trampoline_start);
    uint64_t off_64   = (uint64_t)(trampoline_64 - trampoline_start);

    *(uint32_t *)(tramp + off_desc + 2) = (uint32_t)(tramp + off_gdt);
    *(uint32_t *)(tramp + off_j32) = (uint32_t)(tramp + off_32);
    *(uint32_t *)(tramp + off_j64) = (uint32_t)(tramp + off_64);
    *(uint64_t *)(tramp + off_cr3) = (uint64_t)(uint32_t)(uint64_t)pml4_table;
    *(uint64_t *)(tramp + off_stk) = top;
    *(uint64_t *)(tramp + off_ent) = (uint64_t)ap_startup;
    __asm__ volatile ("mfence" ::: "memory");

    uint32_t vec = (uint32_t)(tramp >> 12);
    send_ipi(apic_id, 0x00004500);                      /* INIT, assert */
    udelay(10000);
    send_ipi(apic_id, 0x00004600 | vec);                /* SIPI */
    udelay(300);
    if (!ap_flag[idx]) {
        send_ipi(apic_id, 0x00004600 | vec);            /* второй SIPI */
    }
    for (int t = 0; t < 2000 && !ap_flag[idx]; t++) udelay(100);
    return ap_flag[idx] ? 0 : -1;
}

void smp_init(int bsp_only)
{
    tsc_calibrate();

    /* Страница Local APIC (адрес из MSR 0x1B, обычно 0xFEE00000) */
    uint64_t lbase = rdmsr(0x1B) & 0xFFFFF000ULL;
    if (!lbase) lbase = LAPIC_DEFAULT_BASE;
    lapic = (volatile uint32_t *)mmio_map(lbase, PAGE_SIZE, PTE_WRITABLE | (3ULL << 3));
    if (!lapic) { printk("[WARN] SMP: cannot map Local APIC\n"); return; }

    for (int i = 0; i < MAX_CPUS; i++) {
        cpus[i].id = 0xFFFFFFFF;
        cpus[i].online = 0;
        cpus[i].kstack = 0;
    }

    lapic_init();

    /* Виртуальный провод: прерывания 8259 (таймер, PS/2) должны доходить до BSP через LINT0 (ExtINT).
     * Часть прошивок (UEFI на ноутбуках) оставляет LINT0 замаскированным -- тогда IRQ0 не приходит
     * и ядро зависает после "Launching /bin/init". LINT1 -- NMI. */
    lapic_write(LAPIC_LINT0, 0x00000700);
    lapic_write(LAPIC_LINT1, 0x00000400);

    uint32_t bsp_id = smp_get_cpu_id();
    cpus[0].id = bsp_id;
    cpus[0].online = 1;
    smp_num_cpus = 1;

    printk("[OK] SMP: Bootstrap Processor (APIC ID %d) initialized\n", (int)bsp_id);

    if (bsp_only || !SMP_ENABLE) {
        printk("[OK] SMP: single-core mode (disabled at build time)\n");
        return;
    }

    /* Страница трамплина: UEFI выделяет её сам, на BIOS/multiboot -- 0x8000 */
    uint64_t tramp = (mbi.flags & MB_FLAG_UEFI) ? (uint64_t)mbi.smp_trampoline : 0x8000;
    if (!tramp || tramp >= 0xA0000 || (tramp & 0xFFF)) {
        printk("[WARN] SMP: no low-memory page for the AP trampoline, single-core mode\n");
        return;
    }

    uint32_t ids[MAX_CPUS * 4];
    int n = acpi_madt_cpus(ids, MAX_CPUS * 4);
    if (n <= 1) {
        printk("[OK] SMP: ACPI MADT lists %d CPU(s), nothing to start\n", n);
        return;
    }

    uint64_t tsize = (uint64_t)(trampoline_end - trampoline_start);
    memcpy((void *)tramp, trampoline_start, tsize);

    int idx = 1, started = 0, failed = 0;
    for (int i = 0; i < n && idx < MAX_CPUS; i++) {
        uint32_t id = ids[i];
        int dup = (id == bsp_id);
        for (int j = 1; j < idx; j++) if (cpus[j].id == id) dup = 1;
        if (dup) continue;
        if (start_ap(tramp, idx, id) == 0) { started++; idx++; }
        else { failed++; printk("[WARN] SMP: AP with APIC ID %d did not respond\n", (int)id); }
    }
    printk("[OK] SMP: %d CPU(s) online (%d AP started, %d failed); APs parked, scheduler runs on the BSP\n",
           smp_num_cpus, started, failed);
}

volatile int lapic_tick_mode = 0;
extern volatile uint64_t jiffies;

void lapic_eoi(void)
{
    if (lapic) lapic_write(LAPIC_EOI, 0);
}

/* Запасной источник тика: LAPIC timer (периодический, вектор 32 -- тот же обработчик, что у IRQ0) */
static int lapic_timer_start(int hz)
{
    if (!lapic) return -1;
    lapic_write(LAPIC_TDCR, 0x3);                       /* делитель 16 */
    lapic_write(LAPIC_TIMER, 0x10000 | 0xFF);           /* замаскирован, one-shot */
    lapic_write(LAPIC_TICR, 0xFFFFFFFFU);
    uint64_t t0 = time_us();
    while (time_us() - t0 < 10000) __asm__ volatile ("pause");
    uint32_t cnt = 0xFFFFFFFFU - lapic_read(LAPIC_TCCR);
    lapic_write(LAPIC_TIMER, 0x10000 | 0xFF);
    if (cnt < 1000) return -1;
    uint64_t per = (uint64_t)cnt * 100 / (uint64_t)hz;  /* cnt -- за 10 мс */
    if (per < 100) per = 100;
    if (per > 0xFFFFFFFFULL) per = 0xFFFFFFFFULL;
    lapic_tick_mode = 1;
    lapic_write(LAPIC_TIMER, 32 | (1 << 17));           /* вектор 32, periodic */
    lapic_write(LAPIC_TICR, (uint32_t)per);
    return 0;
}

static int wait_tick(uint32_t ms)
{
    uint64_t j0 = jiffies;
    uint64_t t0 = time_us();
    while (jiffies == j0 && time_us() - t0 < (uint64_t)ms * 1000) __asm__ volatile ("pause");
    return jiffies != j0;
}

/* Вызывается после sti: убеждаемся, что тик планировщика вообще идёт */
void timer_selftest(void)
{
    if (wait_tick(300)) {
        printk("[OK] Timer: PIT IRQ0 ticking\n");
        return;
    }
    printk("[WARN] Timer: no PIT IRQ0 within 300 ms, switching to LAPIC timer\n");
    if (lapic_timer_start(100) == 0 && wait_tick(300)) {
        printk("[OK] Timer: LAPIC timer ticking (IRQ0 unavailable)\n");
        return;
    }
    printk("[FAIL] Timer: no timer interrupts at all, scheduler cannot start\n");
}
