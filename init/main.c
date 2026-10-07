#include <linux/tty.h>
#include <linux/traps.h>

extern volatile uint64_t jiffies;

void main(void)
{
    console_init();

    printk("==============================================\n");
    printk("   Linux 0.01 (x86_64 Edition) Booting...    \n");
    printk("==============================================\n\n");

    printk("[OK] CPU Mode: 64-bit Long Mode Enabled\n");
    printk("[OK] Memory: 4-Level Paging Initialized\n");
    printk("[OK] Console: VGA and Serial Active\n");

    /* Инициализация IDT и таймера */
    trap_init();
    printk("[OK] IDT: 64-bit Interrupt Gates Installed\n");
    printk("[OK] PIC 8259: Remapped to 0x20..0x2F\n");
    printk("[OK] Timer PIT: Configured at 100 Hz\n\n");

    /* Включаем прерывания! */
    __asm__ volatile ("sti");
    printk("Interrupts enabled (sti). Timer is ticking!\n\n");

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
