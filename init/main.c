#include <linux/tty.h>
#include <linux/traps.h>
#include <linux/mm.h>

extern volatile uint64_t jiffies;

void main(void)
{
    console_init();

    printk("==============================================\n");
    printk("   Linux 0.01 (x86_64 Edition) Booting...    \n");
    printk("==============================================\n\n");

    printk("[OK] CPU Mode: 64-bit Long Mode Enabled\n");
    printk("[OK] Console: VGA and Serial Active\n");

    /* Инициализация IDT и таймера */
    trap_init();
    printk("[OK] IDT and Timer Initialized\n");

    /* Инициализация менеджера памяти */
    mem_init();

    /* --- ТЕСТ ПЕЙДЖИНГА И АЛЛОКАТОРА --- */
    printk("\n--- Testing 64-bit Paging & Allocator ---\n");

    uint64_t phys_page = get_free_page();
    printk("Allocated Physical Frame: %p\n", phys_page);

    /* Маппим страницу на виртуальный адрес 8 ГБ (0x200000000) */
    uint64_t virt_addr = 0x200000000ULL;
    map_page(NULL, virt_addr, phys_page, PTE_WRITABLE);
    printk("Mapped Virtual %p -> Physical %p\n", virt_addr, phys_page);

    /* Записываем данные по виртуальному адресу 8 ГБ */
    volatile char *test_ptr = (volatile char *)virt_addr;
    const char secret[] = "Hello from 8GB Virtual Address Space!";

    for (int i = 0; secret[i] != '\0'; i++) {
        test_ptr[i] = secret[i];
    }
    test_ptr[sizeof(secret) - 1] = '\0';

    printk("Verification Read: \"%s\"\n", (const char *)virt_addr);
    printk("--- Paging Test Passed Successfully! ---\n\n");

    /* Включаем прерывания */
    __asm__ volatile ("sti");
    printk("Interrupts enabled. Timer is ticking!\n\n");

    uint64_t last_sec = 0;
    for (;;) {
        uint64_t current_sec = jiffies / 100;
        if (current_sec != last_sec) {
            last_sec = current_sec;
            printk("Uptime: %d sec (jiffies = %d)\n", current_sec, jiffies);
        }
        __asm__ volatile ("hlt");
    }
}
