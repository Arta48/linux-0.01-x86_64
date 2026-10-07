#include <linux/syscall.h>
#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/keyboard.h>
#include <linux/fs.h>
#include <linux/mm.h>

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
    uint64_t star = ((uint64_t)0x0010 << 48) | ((uint64_t)0x0008 << 32);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x200);

    printk("[OK] Hardware 'syscall/sysret' MSRs Initialized\n");
}

static int64_t sys_read(int fd, char *buf, uint64_t count)
{
    if (count == 0) return 0;

    if (fd >= 3) {
        return sys_file_read(fd, buf, count);
    }

    if (fd != 0) return -1;

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
    if (fd >= 3 && fd < NR_OPEN && current->filp[fd].in_use && current->filp[fd].type == FILE_TYPE_PIPE) {
        return pipe_write(&current->filp[fd], buf, count);
    }

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
    printk("\nPID   PPID  STATE       PRIORITY  COUNTER\n");
    for (int i = 0; i < NR_TASKS; i++) {
        if (task[i]) {
            const char *st = "UNKNOWN";
            if (task[i]->state == TASK_RUNNING) st = "RUNNING";
            else if (task[i]->state == TASK_ZOMBIE) st = "ZOMBIE ";
            printk("%d     %d     %s     %d        %d\n",
                   task[i]->pid, task[i]->father, st, task[i]->priority, task[i]->counter);
        }
    }
    printk("\n");
}

/* sys_waitpid: ожидает завершения потомка и освобождает его память */
static int64_t sys_waitpid(int64_t pid, int *stat_addr, int options)
{
    (void)options;

    repeat:
    for (int i = 1; i < NR_TASKS; i++) {
        if (task[i] && task[i]->father == current->pid) {
            if (pid == -1 || task[i]->pid == pid) {
                if (task[i]->state == TASK_ZOMBIE) {
                    int64_t child_pid = task[i]->pid;
                    if (stat_addr) {
                        *stat_addr = task[i]->exit_code;
                    }

                    /* Полная утилизация: освобождаем память стека и task_struct */
                    if (task[i]->user_stack_page) {
                        free_page(task[i]->user_stack_page);
                    }
                    free_page((uint64_t)task[i]);
                    task[i] = NULL;

                    return child_pid;
                }
            }
        }
    }

    /* Проверяем, есть ли вообще живые потомки */
    int has_children = 0;
    for (int i = 1; i < NR_TASKS; i++) {
        if (task[i] && task[i]->father == current->pid) {
            if (pid == -1 || task[i]->pid == pid) {
                has_children = 1;
                break;
            }
        }
    }

    if (has_children) {
        __asm__ volatile ("sti");
        schedule();
        goto repeat;
    }

    return -1; /* Нет таких потомков */
}

/* sys_kill: принудительное завершение процесса */
static int64_t sys_kill(int64_t pid, int sig)
{
    (void)sig;

    if (pid <= 1 || pid >= NR_TASKS || !task[pid]) {
        return -1; /* Запрещено завершать Idle (0) и Shell (1) */
    }

    if (task[pid]->state == TASK_ZOMBIE) {
        return -1;
    }

    /* Закрываем файлы процесса */
    for (int i = 3; i < NR_OPEN; i++) {
        if (task[pid]->filp[i].in_use) {
            struct file *f = &task[pid]->filp[i];
            if (f->type == FILE_TYPE_PIPE && f->pipe) {
                if (f->mode == 1) f->pipe->readers--;
                if (f->mode == 2) f->pipe->writers--;
                f->pipe->ref_count--;
                if (f->pipe->ref_count <= 0) {
                    free_page((uint64_t)f->pipe);
                }
            }
            f->in_use = 0;
            f->pipe = NULL;
        }
    }

    task[pid]->exit_code = 9; /* SIGKILL */
    task[pid]->state = TASK_ZOMBIE;
    printk("\n[Process %d killed]\n", (int)pid);

    return 0;
}

static int64_t sys_exit(int status)
{
    printk("\n[Process %d exited with status %d]\n", (int)current->pid, status);

    for (int i = 3; i < NR_OPEN; i++) {
        if (current->filp[i].in_use) {
            sys_close(i);
        }
    }

    current->exit_code = status;
    current->state = TASK_ZOMBIE;
    schedule();
    for (;;);
    return 0;
}

int64_t syscall_dispatcher(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3, struct trap_frame *tf)
{
    switch (nr) {
        case __NR_fork:
            if (!tf) return -1;
            return sys_fork(tf);
        case __NR_pipe:
            return sys_pipe((int *)arg1);
        case __NR_open:
            return sys_open((const char *)arg1, (int)arg2);
        case __NR_close:
            return sys_close((int)arg1);
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
        case __NR_list:
            return sys_list((char *)arg1, arg2);
        case __NR_waitpid:
            return sys_waitpid((int64_t)arg1, (int *)arg2, (int)arg3);
        case __NR_kill:
            return sys_kill((int64_t)arg1, (int)arg2);
        case __NR_exit:
            return sys_exit((int)arg1);
        default:
            printk("[SYSCALL] Unknown syscall: %d\n", nr);
            return -1;
    }
}
