#include <linux/syscall.h>
#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/keyboard.h>

#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084

extern volatile uint64_t jiffies;
extern int64_t sys_fork(struct trap_frame *tf);
extern void syscall_entry(void);

static inline void wrmsr(uint32_t msr, uint64_t val)
{
    uint32_t low = val & 0xFFFFFFFF;
    uint32_t high = val >> 32;
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

void syscall_init(void)
{
    /*
     * STAR MSR:
     * Биты 47:32 = 0x0008 (Kernel CS = 0x08, Kernel SS = 0x10)
     * Биты 63:48 = 0x0010 (User SS = 0x18 | 3 = 0x1B, User CS = 0x20 | 3 = 0x23)
     */
    uint64_t star = ((uint64_t)0x0010 << 48) | ((uint64_t)0x0008 << 32);
    wrmsr(MSR_STAR, star);

    /* LSTAR MSR: адрес функции-обработчика syscall_entry */
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* SFMASK MSR: маскируем IF (бит 9, 0x200), запрещая прерывания при входе */
    wrmsr(MSR_SFMASK, 0x200);

    printk("[OK] Hardware 'syscall/sysret' MSRs Initialized\n");
}

static int64_t sys_read(int fd, char *buf, uint64_t count)
{
    if (fd != 0 || count == 0) {
        return -1;
    }

    uint64_t bytes_read = 0;

    while (bytes_read < count) {
        __asm__ volatile ("sti");

        char c = keyboard_getchar();
        if (c == 0) {
            __asm__ volatile ("hlt");
            continue;
        }

        buf[bytes_read++] = c;
        if (c == '\n' || c == '\b') {
            break;
        }
    }

    return bytes_read;
}

static int64_t sys_write(int fd, const char *buf, uint64_t count)
{
    (void)fd;
    for (uint64_t i = 0; i < count; i++) {
        console_putc(buf[i]);
    }
    return count;
}

static int64_t sys_getpid(void)
{
    return current->pid;
}

static int64_t sys_time(void)
{
    return (int64_t)jiffies;
}

static void sys_ps(void)
{
    printk("\nPID   STATE       PRIORITY  COUNTER\n");
    for (int i = 0; i < NR_TASKS; i++) {
        if (task[i]) {
            const char *st = "UNKNOWN";
            if (task[i]->state == TASK_RUNNING) st = "RUNNING";
            else if (task[i]->state == TASK_ZOMBIE) st = "ZOMBIE ";
            printk("%d     %s     %d        %d\n",
                   task[i]->pid, st, task[i]->priority, task[i]->counter);
        }
    }
    printk("\n");
}

static int64_t sys_exit(int status)
{
    printk("\n[Process %d exited with status %d]\n", current->pid, status);
    current->state = TASK_ZOMBIE;
    schedule();
    for (;;);
    return 0;
}

int64_t syscall_dispatcher(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3, struct trap_frame *tf)
{
    switch (nr) {
        case __NR_fork:
            if (!tf) {
                printk("[FORK] sys_fork requires trap_frame (called via int 0x80)\n");
                return -1;
            }
            return sys_fork(tf);
        case __NR_read:
            return sys_read((int)arg1, (char *)arg2, arg3);
        case __NR_write:
            return sys_write((int)arg1, (const char *)arg2, arg3);
        case __NR_getpid:
            return sys_getpid();
        case __NR_time:
            return sys_time();
        case __NR_ps:
            sys_ps();
            return 0;
        case __NR_exit:
            return sys_exit((int)arg1);
        default:
            printk("[SYSCALL] Unknown syscall: %d\n", nr);
            return -1;
    }
}
