#include <linux/syscall.h>
#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/keyboard.h>
#include <linux/fs.h>
#include <linux/minix_fs.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/utsname.h>
#include <linux/time.h>
#include <linux/sock.h>
#include <linux/elf.h>
#include <linux/acpi.h>

#define MSR_EFER   0xC0000080
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

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

void syscall_init(void)
{
    /* Активируем бит SCE (System Call Extension, бит 0) в регистре EFER */
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= 1ULL;
    wrmsr(MSR_EFER, efer);

    /* STAR[63:48] = 0x13, а не 0x10: SYSRET берёт SS = STAR+8, CS = STAR+16.
     * Процессоры AMD (в отличие от QEMU) НЕ добавляют RPL=3 к SS сами, поэтому
     * RPL должен быть уже в значении STAR: SS = 0x1B, CS = 0x23. Иначе в кадре
     * лежит SS=0x18 (RPL 0) и последующий iretq даёт #GP(0x18). */
    uint64_t star = ((uint64_t)0x0013 << 48) | ((uint64_t)0x0008 << 32);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x200);

    printk("[OK] Hardware 'syscall/sysret' MSRs Initialized\n");
}

int64_t sys_read(int fd, char *buf, uint64_t count)
{
    if (count == 0) return 0;
    if (fd < 0 || fd >= NR_OPEN) return -1;

    if (current->files->f[fd].in_use && current->files->f[fd].type != FILE_TYPE_TTY) {
        if (current->files->f[fd].type == FILE_TYPE_PIPE) {
            return pipe_read(&current->files->f[fd], buf, count);
        } else {
            return sys_file_read(fd, buf, count);
        }
    }

    if (fd == 0 || (current->files->f[fd].in_use && current->files->f[fd].type == FILE_TYPE_TTY)) {
        uint64_t bytes_read = 0;
        while (bytes_read < count) {
            __asm__ volatile ("sti");

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

int64_t sys_write(int fd, const char *buf, uint64_t count)
{
    if (fd < 0 || fd >= NR_OPEN) return -1;

    if (current->files->f[fd].in_use && current->files->f[fd].type != FILE_TYPE_TTY) {
        if (current->files->f[fd].type == FILE_TYPE_PIPE) {
            return pipe_write(&current->files->f[fd], buf, count);
        } else {
            return sys_file_write(fd, buf, count);
        }
    }

    if (fd == 1 || fd == 2 || (current->files->f[fd].in_use && current->files->f[fd].type == FILE_TYPE_TTY)) {
        for (uint64_t i = 0; i < count; i++) {
            console_putc(buf[i]);
        }
        return count;
    }

    return -1;
}

int64_t sys_dup2(int oldfd, int newfd)
{
    if (oldfd < 0 || oldfd >= NR_OPEN) return -1;
    if (newfd < 0 || newfd >= NR_OPEN) return -1;

    if (oldfd == newfd) return newfd;

    if (current->files->f[newfd].in_use) {
        sys_close(newfd);
    }

    if (!current->files->f[oldfd].in_use) {
        current->files->f[newfd].in_use = 0;
        return newfd;
    }

    current->files->f[newfd] = current->files->f[oldfd];

    if (current->files->f[newfd].type == FILE_TYPE_PIPE && current->files->f[newfd].pipe) {
        current->files->f[newfd].pipe->ref_count++;
        if (current->files->f[newfd].mode == 1) current->files->f[newfd].pipe->readers++;
        if (current->files->f[newfd].mode == 2) current->files->f[newfd].pipe->writers++;
    }
    if (current->files->f[newfd].type == FILE_TYPE_SOCKET) sock_ref(current->files->f[newfd].sock_id);

    return newfd;
}

static int64_t sys_getpid(void)
{
    return current->pid;
}

static int64_t sys_getuid(void)
{
    return (int64_t)current->uid;
}

static int64_t sys_setuid(uint16_t uid, const char *password)
{
    if (current->euid == 0) {
        current->uid = uid;
        current->euid = uid;
        return 0;
    }

    if (uid == current->uid) {
        return 0;
    }

    if (!password) {
        return -1;
    }

    uint64_t fsz = 0;
    const char *data = fs_get_file_data("/etc/passwd", &fsz);
    if (!data || fsz == 0) return -1;

    const char *p = data;
    while (*p && (uint64_t)(p - data) < fsz) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;

        const char *c1 = line_start;
        while (c1 < p && *c1 != ':') c1++;

        const char *c2 = c1 + 1;
        while (c2 < p && *c2 != ':') c2++;

        const char *c3 = c2 + 1;
        while (c3 < p && *c3 != ':') c3++;

        if (c1 < p && c2 < p && c3 < p) {
            uint16_t entry_uid = 0;
            const char *uptr = c2 + 1;
            while (uptr < c3) {
                if (*uptr >= '0' && *uptr <= '9') {
                    entry_uid = entry_uid * 10 + (*uptr - '0');
                }
                uptr++;
            }

            if (entry_uid == uid) {
                uint64_t pass_len = c2 - (c1 + 1);
                if (strlen(password) == pass_len && memcmp(password, c1 + 1, pass_len) == 0) {
                    current->uid = uid;
                    current->euid = uid;
                    return 0;
                }
            }
        }

        if (*p == '\n') p++;
    }

    return -1;
}

static int64_t sys_time(void)
{
    return (int64_t)get_current_time();
}

static void sys_ps(void)
{
    printk("\nPID   PPID  UID   STATE       PRIORITY  COUNTER  NAME\n");
    for (int i = 0; i < NR_TASKS; i++) {
        if (task[i]) {
            const char *st = "UNKNOWN";
            if (task[i]->state == TASK_RUNNING) st = "RUNNING";
            else if (task[i]->state == TASK_INTERRUPTIBLE) st = "SLEEP  ";
            else if (task[i]->state == TASK_ZOMBIE) st = "ZOMBIE ";

            const char *pname = task[i]->name[0] ? task[i]->name : "user";
            printk("%d     %d     %d     %s     %d        %d        %s\n",
                   task[i]->pid, task[i]->father, task[i]->uid, st,
                   task[i]->priority, task[i]->counter, pname);
        }
    }
    printk("\n");
}

/* Есть ли у процесса tgid живые (не зомби) потоки, кроме лидера */
static int group_has_live_threads(long tgid)
{
    for (int i = 1; i < NR_TASKS; i++) {
        struct task_struct *t = task[i];
        if (t && t->is_thread && t->tgid == tgid && t->state != TASK_ZOMBIE) return 1;
    }
    return 0;
}

int64_t sys_waitpid(int64_t pid, int *stat_addr, int options)
{
    reap_zombie_threads();
    repeat:
    for (int i = 1; i < NR_TASKS; i++) {
        struct task_struct *c = task[i];
        if (c && !c->is_thread && c->father == current->tgid) {
            if (pid == -1 || c->pid == pid) {
                if (c->state == TASK_ZOMBIE && !group_has_live_threads(c->tgid)) {
                    int64_t child_pid = c->pid;
                    if (stat_addr) {
                        *stat_addr = c->exit_code;
                    }

                    /* потоки-зомби этого процесса: память принадлежит лидеру */
                    for (int k = 1; k < NR_TASKS; k++) {
                        struct task_struct *t = task[k];
                        if (t && t->is_thread && t->tgid == c->tgid) {
                            task[k] = NULL;
                            free_task(t);
                        }
                    }

                    if (c->cr3) {
                        free_process_pml4(c->cr3);
                        c->cr3 = 0;
                    }
                    vma_clear(c);
                    free_task(c);
                    task[i] = NULL;

                    return child_pid;
                }
            }
        }
    }

    int has_children = 0;
    for (int i = 1; i < NR_TASKS; i++) {
        if (task[i] && !task[i]->is_thread && task[i]->father == current->tgid) {
            if (pid == -1 || task[i]->pid == pid) {
                has_children = 1;
                break;
            }
        }
    }

    if (has_children) {
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

static struct vma *heap_vma(void)
{
    for (int i = 0; i < current->mm->nvma; i++) {
        if (current->mm->vmas[i].flags & VM_HEAP) return &current->mm->vmas[i];
    }
    return NULL;
}

/* brk: куча -- ленивая VMA [start_brk, brk). Страницы выделяются при первом обращении. */
int64_t sys_brk(uint64_t new_brk)
{
    if (new_brk == 0 || new_brk < current->mm->start_brk) {
        return (int64_t)current->mm->brk;
    }
    if (new_brk > USER_VA_MAX - PAGE_SIZE) return (int64_t)current->mm->brk;

    uint64_t old_end = PAGE_ALIGN(current->mm->brk);
    uint64_t new_end = PAGE_ALIGN(new_brk);

    if (new_end > old_end) {
        /* Диапазон не должен пересекаться с другими областями */
        for (int i = 0; i < current->mm->nvma; i++) {
            struct vma *v = &current->mm->vmas[i];
            if (v->flags & VM_HEAP) continue;
            if (v->start < new_end && v->end > old_end) return (int64_t)current->mm->brk;
        }
        struct vma *h = heap_vma();
        if (h) {
            h->end = new_end;
        } else if (vma_add(current, PAGE_ALIGN(current->mm->start_brk), new_end,
                           VM_READ | VM_WRITE, VM_ANON | VM_HEAP) < 0) {
            return (int64_t)current->mm->brk;
        }
    } else if (new_end < old_end) {
        uint64_t keep = PAGE_ALIGN(current->mm->start_brk);
        if (new_end < keep) new_end = keep;
        vma_unmap(current, new_end, old_end);          /* освобождает страницы и сжимает VMA */
    }

    current->mm->brk = new_brk;
    return (int64_t)current->mm->brk;
}

extern int64_t sys_execve(const char *filename, char **argv, char **envp, struct trap_frame *tf);

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

/* lseek: SEEK_SET=0, SEEK_CUR=1, SEEK_END=2 (END -- только для файлов ramfs и устройств /dev) */
int64_t sys_lseek(int fd, int64_t off, int whence)
{
    if (fd < 0 || fd >= NR_OPEN || !current->files->f[fd].in_use) return -1;
    struct file *f = &current->files->f[fd];
    if (f->type == FILE_TYPE_PIPE || f->type == FILE_TYPE_SOCKET) return -1;

    int64_t base;
    if (whence == 0) base = 0;
    else if (whence == 1) base = (int64_t)f->pos;
    else if (whence == 2 && f->rf) base = (int64_t)f->rf->size;
    else return -1;

    int64_t np = base + off;
    if (np < 0) return -1;
    f->pos = (uint64_t)np;
    return np;
}

int64_t sys_sync(void)
{
    minix_sync();
    return 0;
}

extern void thread_release(struct task_struct *t);

/* Закрыть все дескрипторы таблицы текущей задачи (последний пользователь таблицы) */
static void files_close_all(void)
{
    for (int i = 0; i < NR_OPEN; i++) {
        if (current->files->f[i].in_use) {
            sys_close(i);
        }
    }
}

/* Завершить остальные потоки процесса (exit_group, фатальный сигнал, execve) */
void kill_other_threads(int status)
{
    for (int i = 1; i < NR_TASKS; i++) {
        struct task_struct *t = task[i];
        if (!t || t == current || t->tgid != current->tgid || t->state == TASK_ZOMBIE) continue;
        thread_release(t);
        t->files->users--;
        t->exit_code = status;
        t->state = TASK_ZOMBIE;
    }
}

int64_t sys_exit(int status)
{

    thread_release(current);
    if (--current->files->users <= 0) files_close_all();

    if (!current->is_thread) {
        for (int i = 1; i < NR_TASKS; i++) {
            if (task[i] && task[i]->father == current->pid && !task[i]->is_thread) {
                task[i]->father = 1;
            }
        }

        /* Завершился корневой шелл (PID первого пользовательского процесса):
         * как и в настоящем init, дальше работать нечему -- выключаем компьютер. */
        if (root_task_pid >= 0 && (int)current->pid == root_task_pid) {
            printk("[INIT] Root shell exited. Powering off...\n");
            sys_reboot(0);
        }
    }

    current->exit_code = status;
    current->state = TASK_ZOMBIE;
    schedule();
    for (;;);
    return 0;
}

int64_t sys_exit_group(int status)
{
    kill_other_threads(status);
    return sys_exit(status);
}

#include <linux/ip.h>
static int64_t sys_netctl(int cmd, uint64_t a1)
{
    switch (cmd) {
    case NETCTL_INFO: {
        if (!user_access_ok(a1, sizeof(struct net_info), 1)) return -1;
        struct net_info ni;
        int r = net_get_info(&ni);
        memcpy((void *)a1, &ni, sizeof(ni));
        return r;
    }
    case NETCTL_DHCP:
        return net_dhcp_renew();
    }
    return -1;
}

extern int64_t linux_syscall(struct trap_frame *tf);

/* Вход из syscall_entry.S: tf -- полный кадр пользователя */
void syscall_entry_c(struct trap_frame *tf)
{
    if (current->linux_abi) {
        linux_syscall(tf);          /* сам записывает результат в tf->rax */
        return;
    }
    tf->rax = (uint64_t)syscall_dispatcher(tf->rax, tf->rdi, tf->rsi, tf->rdx, tf->r10, tf);
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
        case __NR_lseek:
            ret = sys_lseek((int)arg1, (int64_t)arg2, (int)arg3);
            break;
        case __NR_unlink:
            ret = sys_unlink((const char *)arg1);
            break;
        case __NR_chmod:
            ret = sys_chmod((const char *)arg1, (int)arg2);
            break;
        case __NR_dup2:
            ret = sys_dup2((int)arg1, (int)arg2);
            break;
        case __NR_stat:
            return sys_stat((const char *)arg1, (struct stat *)arg2);
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
        case __NR_getuid:
            ret = sys_getuid();
            break;
        case __NR_setuid:
            ret = sys_setuid((uint16_t)arg1, (const char *)arg2);
            break;
        case __NR_time:
            ret = sys_time();
            break;
        case __NR_ps:
            sys_ps();
            ret = 0;
            break;
        case __NR_list:
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
        case __NR_sync:
            ret = sys_sync();
            break;
        case __NR_reboot:
            ret = sys_reboot((int)arg1);
            break;
        case __NR_uname:
            ret = sys_uname((struct utsname *)arg1);
            break;
        case __NR_exit:
            ret = sys_exit((int)arg1);
            break;
        case __NR_netctl:
            ret = sys_netctl((int)arg1, arg2);
            break;
        default:
            printk("[SYSCALL] Unknown syscall: %d\n", nr);
            ret = -1;
            break;
    }

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
