#include <linux/syscall.h>
#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/keyboard.h>

extern volatile uint64_t jiffies;

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

static int64_t sys_exit(int status)
{
    printk("\n[Process %d exited with status %d]\n", current->pid, status);
    current->state = TASK_ZOMBIE;
    schedule();
    for (;;);
    return 0;
}

int64_t syscall_dispatcher(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    switch (nr) {
        case __NR_read:
            return sys_read((int)arg1, (char *)arg2, arg3);
        case __NR_write:
            return sys_write((int)arg1, (const char *)arg2, arg3);
        case __NR_getpid:
            return sys_getpid();
        case __NR_time:
            return sys_time();
        case __NR_exit:
            return sys_exit((int)arg1);
        default:
            printk("[SYSCALL] Unknown syscall: %d\n", nr);
            return -1;
    }
}
