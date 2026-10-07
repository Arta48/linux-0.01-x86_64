#include <linux/tty.h>
#include <linux/traps.h>
#include <linux/mm.h>
#include <linux/sched.h>

extern volatile uint64_t jiffies;

/* Задача 1: работает циклически */
void task_a(void)
{
    while (1) {
        printk("[Task 1] Running! PID = %d (jiffies = %d)\n", current->pid, jiffies);
        for (volatile int i = 0; i < 40000000; i++);
    }
}

/* Задача 2: работает циклически */
void task_b(void)
{
    while (1) {
        printk("    [Task 2] Running! PID = %d (jiffies = %d)\n", current->pid, jiffies);
        for (volatile int i = 0; i < 40000000; i++);
    }
}

void main(void)
{
    console_init();

    printk("==============================================\n");
    printk("   Linux 0.01 (x86_64 Edition) Booting...    \n");
    printk("==============================================\n\n");

    trap_init();
    mem_init();
    sched_init();

    /* Создаем две конкурирующие задачи */
    task_create(task_a, 10);
    task_create(task_b, 10);

    /* Включаем аппаратные прерывания */
    __asm__ volatile ("sti");
    printk("\n[OK] Preemptive Multitasking Started!\n\n");

    /* Задача 0 (Idle): спит на инструкции hlt, пока процессор занят другими задачами */
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
