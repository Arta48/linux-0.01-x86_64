#include <linux/tty.h>

void main(void)
{
    console_init();

    printk("==============================================\n");
    printk("   Linux 0.01 (x86_64 Edition) Booting...    \n");
    printk("==============================================\n\n");

    printk("[OK] CPU Mode: 64-bit Long Mode Enabled\n");
    printk("[OK] Memory: 4-Level Paging Initialized (1 GB Identity Mapped)\n");
    printk("[OK] Console: VGA Text Mode and Serial COM1 Active\n\n");

    printk("System ready for Stage 2 (IDT & Interrupts)...\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
