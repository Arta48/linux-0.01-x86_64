#include <linux/syscall.h>
#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/keyboard.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/time.h>
#include <linux/utsname.h>

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
    if (fd < 0 || fd >= NR_OPEN) return -1;

    if (current->filp[fd].in_use) {
        if (current->filp[fd].type == FILE_TYPE_PIPE) {
            return pipe_read(&current->filp[fd], buf, count);
        } else {
            return sys_file_read(fd, buf, count);
        }
    }

    if (fd == 0) {
        uint64_t bytes_read = 0;
        while (bytes_read < count) {
            __asm__ volatile ("sti");

            /* Если поступил сигнал (например Ctrl+C), немедленно прерываем чтение */
            if (current->signal) {
                return -1;
            }

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

    return -1;
}

static int64_t sys_write(int fd, const char *buf, uint64_t count)
{
    if (fd < 0 || fd >= NR_OPEN) return -1;

    if (current->filp[fd].in_use) {
        if (current->filp[fd].type == FILE_TYPE_PIPE) {
            return pipe_write(&current->filp[fd], buf, count);
        } else {
            return sys_file_write(fd, buf, count);
        }
    }

    if (fd == 1 || fd == 2) {
        for (uint64_t i = 0; i < count; i++) {
            console_putc(buf[i]);
        }
        return count;
    }

    return -1;
}

static int64_t sys_dup2(int oldfd, int newfd)
{
    if (oldfd < 0 || oldfd >= NR_OPEN) return -1;
    if (newfd < 0 || newfd >= NR_OPEN) return -1;

    if (oldfd == newfd) return newfd;

    if (current->filp[newfd].in_use) {
        sys_close(newfd);
    }

    if (!current->filp[oldfd].in_use) {
        current->filp[newfd].in_use = 0;
        return newfd;
    }

    current->filp[newfd] = current->filp[oldfd];

    if (current->filp[newfd].type == FILE_TYPE_PIPE && current->filp[newfd].pipe) {
        current->filp[newfd].pipe->ref_count++;
        if (current->filp[newfd].mode == 1) current->filp[newfd].pipe->readers++;
        if (current->filp[newfd].mode == 2) current->filp[newfd].pipe->writers++;
    }

    return newfd;
}

static int64_t sys_getpid(void)
{
    return current->pid;
}

static int64_t sys_time(void)
{
    return (int64_t)get_current_time();
}

static void sys_ps(void)
{
    printk("\nPID   PPID  STATE       PRIORITY  COUNTER\n");
    for (int i = 0; i < NR_TASKS; i++) {
        if (task[i]) {
            const char *st = "UNKNOWN";
            if (task[i]->state == TASK_RUNNING) st = "RUNNING";
            else if (task[i]->state == TASK_INTERRUPTIBLE) st = "SLEEP  ";
            else if (task[i]->state == TASK_ZOMBIE) st = "ZOMBIE ";
            printk("%d     %d     %s     %d        %d\n",
                   task[i]->pid, task[i]->father, st, task[i]->priority, task[i]->counter);
        }
    }
    printk("\n");
}

static int64_t sys_waitpid(int64_t pid, int *stat_addr, int options)
{
    repeat:
    for (int i = 1; i < NR_TASKS; i++) {
        if (task[i] && task[i]->father == current->pid) {
            if (pid == -1 || task[i]->pid == pid) {
                if (task[i]->state == TASK_ZOMBIE) {
                    int64_t child_pid = task[i]->pid;
                    if (stat_addr) {
                        *stat_addr = task[i]->exit_code;
                    }

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
        /* WNOHANG: не ждем, если процесс еще работает */
        if (options & 1) {
            return 0;
        }
        __asm__ volatile ("sti");
        schedule();
        goto repeat;
    }

    return -1;
}

static int64_t sys_kill(int64_t pid, int sig)
{
    if (pid <= 1 || pid >= NR_TASKS || !task[pid]) {
        return -1;
    }

    if (task[pid]->state == TASK_ZOMBIE) {
        return -1;
    }

    send_signal(task[pid], sig);
    return 0;
}

/* sys_signal: регистрация пользовательского обработчика сигнала */
static int64_t sys_signal(int sig, uint64_t handler)
{
    if (sig <= 0 || sig >= 32 || sig == SIGKILL) {
        return -1;
    }
    uint64_t old = current->sig_fn[sig];
    current->sig_fn[sig] = handler;
    return (int64_t)old;
}

static int64_t sys_pause(void)
{
    current->state = TASK_INTERRUPTIBLE;
    schedule();
    return 0;
}

static int64_t sys_brk(uint64_t new_brk)
{
    if (new_brk == 0 || new_brk < current->start_brk) {
        return (int64_t)current->brk;
    }

    if (new_brk > current->brk) {
        uint64_t cur_page = PAGE_ALIGN(current->brk);
        uint64_t end_page = PAGE_ALIGN(new_brk);

        for (uint64_t addr = cur_page; addr < end_page; addr += PAGE_SIZE) {
            uint64_t phys = get_free_page();
            if (!phys) {
                return (int64_t)current->brk;
            }
            map_page(NULL, addr, phys, PTE_WRITABLE | PTE_USER);
        }
    }

    current->brk = new_brk;
    return (int64_t)current->brk;
}

static int64_t sys_execve(const char *filename, char **argv, char **envp, struct trap_frame *tf)
{
    (void)argv;
    (void)envp;

    if (!tf) return -1;

    uint64_t file_size = 0;
    const char *data = fs_get_file_data(filename, &file_size);
    if (!data || file_size < sizeof(struct exec_header)) {
        return -1;
    }

    const struct exec_header *hdr = (const struct exec_header *)data;
    if (hdr->magic != EXEC_MAGIC) {
        return -1;
    }

    uint64_t text_phys = get_free_page();
    if (!text_phys) return -1;
    map_page(NULL, USER_TEXT_BASE, text_phys, PTE_WRITABLE | PTE_USER);

    memcpy((void *)USER_TEXT_BASE, data, file_size);

    uint64_t new_stack = get_free_page();
    if (!new_stack) return -1;

    if (current->user_stack_page) {
        free_page(current->user_stack_page);
    }
    current->user_stack_page = new_stack;
    uint64_t user_rsp = new_stack + PAGE_SIZE - 16;

    current->start_brk = HEAP_START_VIRT;
    current->brk = HEAP_START_VIRT;

    for (int i = 3; i < NR_OPEN; i++) {
        if (current->filp[i].in_use) {
            sys_close(i);
        }
    }

    tf->rip = hdr->entry;
    tf->rsp = user_rsp;
    tf->rbp = user_rsp;
    tf->rax = 0;
    tf->rflags = 0x202;

    return 0;
}

int64_t sys_exit(int status)
{
    printk("\n[Process %d exited with status %d]\n", (int)current->pid, status);

    for (int i = 0; i < NR_OPEN; i++) {
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

static int64_t sys_uname(struct utsname *name)
{
    if (!name) return -1;

    const char *s_sys     = "Linux";
    const char *s_node    = "linux64";
    const char *s_release = "0.01-x86_64";
    const char *s_version = "#1 PREEMPT 2026";
    const char *s_machine = "x86_64";

    memcpy(name->sysname,  s_sys,     strlen(s_sys) + 1);
    memcpy(name->nodename, s_node,    strlen(s_node) + 1);
    memcpy(name->release,  s_release, strlen(s_release) + 1);
    memcpy(name->version,  s_version, strlen(s_version) + 1);
    memcpy(name->machine,  s_machine, strlen(s_machine) + 1);

    return 0;
}

int64_t syscall_dispatcher(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, struct trap_frame *tf)
{
    int64_t ret = -1;

    switch (nr) {
        case __NR_fork:
            if (!tf) return -1;
            ret = sys_fork(tf);
        break;
        case __NR_execve:
            if (!tf) return -1;
            ret = sys_execve((const char *)arg1, (char **)arg2, (char **)arg3, tf);
        break;
        case __NR_pipe:
            ret = sys_pipe((int *)arg1);
            break;
        case __NR_open:
            ret = sys_open((const char *)arg1, (int)arg2);
            break;
        case __NR_close:
            ret = sys_close((int)arg1);
            break;
        case __NR_unlink:
            ret = sys_unlink((const char *)arg1);
            break;
        case __NR_dup2:
            ret = sys_dup2((int)arg1, (int)arg2);
            break;
        case __NR_chdir:
            ret = sys_chdir((const char *)arg1);
            break;
        case __NR_mkdir:
            ret = sys_mkdir((const char *)arg1);
            break;
        case __NR_rmdir:
            ret = sys_rmdir((const char *)arg1);
            break;
        case __NR_getcwd:
            ret = sys_getcwd((char *)arg1, arg2);
            break;
        case __NR_read:
            ret = sys_read((int)arg1, (char *)arg2, arg3);
            break;
        case __NR_write:
            ret = sys_write((int)arg1, (const char *)arg2, arg3);
            break;
        case __NR_getpid:
            ret = sys_getpid();
            break;
        case __NR_time:
            ret = sys_time();
            break;
        case __NR_ps:
            sys_ps();
            ret = 0;
            break;
        case __NR_list:
            /* ЧЕСТНЫЙ 4-й АРГУМЕНТ arg4 (is_long: 0 или 1) */
            ret = sys_list((const char *)arg1, (char *)arg2, arg3, (int)arg4);
            break;
        case __NR_waitpid:
            ret = sys_waitpid((int64_t)arg1, (int *)arg2, (int)arg3);
            break;
        case __NR_kill:
            ret = sys_kill((int64_t)arg1, (int)arg2);
            break;
        case __NR_signal:
            ret = sys_signal((int)arg1, arg2);
            break;
        case __NR_pause:
            ret = sys_pause();
            break;
        case __NR_brk:
            ret = sys_brk(arg1);
            break;
        case __NR_uname:
            ret = sys_uname((struct utsname *)arg1);
            break;
        case __NR_exit:
            ret = sys_exit((int)arg1);
            break;
        default:
            printk("[SYSCALL] Unknown syscall: %d\n", nr);
            ret = -1;
            break;
    }

    /* Проверка и доставка сигналов перед возвратом в Ring 3 */
    if (current->signal && current->pid > 0) {
        for (int sig = 1; sig < 32; sig++) {
            if (current->signal & (1U << sig)) {
                current->signal &= ~(1U << sig);

                if (current->sig_fn[sig] == (uint64_t)SIG_IGN) continue;

                if (current->sig_fn[sig] == (uint64_t)SIG_DFL) {
                    sys_exit(128 + sig);
                } else {
                    if (tf) {
                        tf->rsp -= 8;
                        *(uint64_t *)tf->rsp = tf->rip;
                        tf->rip = current->sig_fn[sig];
                    }
                }
            }
        }
    }

    return ret;
}
