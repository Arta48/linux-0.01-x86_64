#include <linux/tty.h>
#include <linux/traps.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/gdt.h>
#include <linux/syscall.h>

extern volatile uint64_t jiffies;
extern void enter_user_mode(uint64_t entry_point, uint64_t user_stack);

/* Обертки системных вызовов Ring 3 */
static inline int64_t u_read(int fd, char *buf, uint64_t count)
{
    int64_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(__NR_read), "b"(fd), "c"((uint64_t)buf), "d"(count)
        : "memory"
    );
    return ret;
}

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

/*
 * ИНТЕРАКТИВНЫЙ ПОЛЬЗОВАТЕЛЬСКИЙ ПРОЦЕСС В RING 3
 */
void user_init_process(void)
{
    const char banner[] =
    "\n========================================\n"
    "  Interactive Ring 3 User Terminal v0.01 \n"
    "  Type characters to test keyboard input \n"
    "========================================\n\n"
    "user@linux64:~$ ";
        u_write(1, banner, sizeof(banner) - 1);

        char c;
        while (1) {
            /* Блокирующее чтение одного символа из stdin через системный вызов */
            if (u_read(0, &c, 1) > 0) {
                /* Эхо-вывод на экран */
                u_write(1, &c, 1);

                /* При нажатии Enter переходим на новую строку с приглашением */
                if (c == '\n') {
                    const char prompt[] = "user@linux64:~$ ";
                    u_write(1, prompt, sizeof(prompt) - 1);
                }
            }
        }
}

void user_trampoline(void)
{
    uint64_t user_stack = get_free_page() + PAGE_SIZE - 16;
    printk("[OK] Transitioning Task 1 to Ring 3 (User Shell)...\n");
    enter_user_mode((uint64_t)user_init_process, user_stack);
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

    /* Запускаем интерактивную задачу пользователя */
    task_create(user_trampoline, 10);

    __asm__ volatile ("sti");
    printk("[OK] Keyboard IRQ1 Active. System ready for input!\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
