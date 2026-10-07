#include <linux/tty.h>
#include <linux/traps.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/gdt.h>
#include <linux/syscall.h>

extern volatile uint64_t jiffies;
extern void enter_user_mode(uint64_t entry_point, uint64_t user_stack);

/* Обертки системных вызовов для Ring 3 */
static inline int64_t u_write(int fd, const char *buf, uint64_t count)
{
    int64_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(__NR_write), "b"(fd), "c"((uint64_t)buf), "d"(count)
        : "memory"
    );
    return ret;
}

static inline int64_t u_getpid(void)
{
    int64_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(__NR_getpid)
        : "memory"
    );
    return ret;
}

/*
 * ЭТА ФУНКЦИЯ ВЫПОЛНЯЕТСЯ В USER SPACE (RING 3)!
 */
void user_init_process(void)
{
    const char msg[] = "[USER SPACE] Hello from Ring 3 (User Mode) via int 0x80!\n";
    u_write(1, msg, sizeof(msg) - 1);

    int64_t my_pid = u_getpid();
    if (my_pid == 1) {
        const char pid_ok[] = "[USER SPACE] sys_getpid() returned PID = 1. Verified!\n";
        u_write(1, pid_ok, sizeof(pid_ok) - 1);
    }

    while (1) {
        const char tick_msg[] = "[USER SPACE] Working in Ring 3... (PID = 1)\n";
        u_write(1, tick_msg, sizeof(tick_msg) - 1);

        for (volatile int i = 0; i < 50000000; i++);
    }
}

/* Трамплин для перехода задачи 1 в Ring 3 */
void user_trampoline(void)
{
    uint64_t user_stack = get_free_page() + PAGE_SIZE - 16;
    printk("[OK] Transitioning Task 1 to Ring 3 (User Space)...\n");
    enter_user_mode((uint64_t)user_init_process, user_stack);
}

/* Фоновая задача ядра в Ring 0 */
void kernel_task(void)
{
    while (1) {
        printk("    [KERNEL TASK] Running in Ring 0 (PID = %d)\n", current->pid);
        for (volatile int i = 0; i < 50000000; i++);
    }
}

void main(void)
{
    console_init();

    printk("==============================================\n");
    printk("   Linux 0.01 (x86_64 Edition) Booting...    \n");
    printk("==============================================\n\n");

    gdt_init();
    trap_init();
    mem_init();
    sched_init();

    /* Задача 1 перейдет в Ring 3 */
    task_create(user_trampoline, 10);

    /* Задача 2 останется в Ring 0 */
    task_create(kernel_task, 10);

    __asm__ volatile ("sti");
    printk("\n[OK] System Ready. Starting Task Switching!\n\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
