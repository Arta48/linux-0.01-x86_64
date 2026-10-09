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
#include <linux/tcp.h>
#include <linux/elf.h>

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
                        free_pages(task[i]->user_stack_page, 8);
                    }
                    if (task[i]->cr3) {
                        free_process_pml4(task[i]->cr3);
                        task[i]->cr3 = 0;
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
            map_page((uint64_t *)current->cr3, addr, phys, PTE_WRITABLE | PTE_USER);
        }
    }

    current->brk = new_brk;
    return (int64_t)current->brk;
}

static int64_t sys_execve(const char *filename, char **argv, char **envp, struct trap_frame *tf)
{
    (void)envp;

    if (!tf) return -1;

    uint64_t file_size = 0;
    const char *data = fs_get_file_data(filename, &file_size);
    if (!data || file_size < 16) {
        return -1;
    }

    const struct exec_header *hdr = (const struct exec_header *)data;
    int is_linus001 = (file_size >= sizeof(struct exec_header) && hdr->magic == EXEC_MAGIC);
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;
    int is_elf64 = 0;
    if (file_size >= sizeof(Elf64_Ehdr) &&
        ehdr->e_ident[0] == ELFMAG0 && ehdr->e_ident[1] == ELFMAG1 &&
        ehdr->e_ident[2] == ELFMAG2 && ehdr->e_ident[3] == ELFMAG3 &&
        ehdr->e_ident[4] == ELFCLASS64 && ehdr->e_machine == EM_X86_64) {
        is_elf64 = 1;
    }

    if (!is_linus001 && !is_elf64) {
        return -1;
    }

    int argc = 0;
    char k_argv_buf[16][64];
    while (argv && argv[argc] && argc < 15) {
        uint64_t len = strlen(argv[argc]);
        if (len >= 63) len = 63;
        memcpy(k_argv_buf[argc], argv[argc], len);
        k_argv_buf[argc][len] = '\0';
        argc++;
    }
    if (argc == 0) {
        uint64_t len = strlen(filename);
        if (len >= 63) len = 63;
        memcpy(k_argv_buf[0], filename, len);
        k_argv_buf[0][len] = '\0';
        argc = 1;
    }

    uint64_t old_pml4 = current->cr3;
    uint64_t new_pml4 = create_process_pml4();
    if (!new_pml4) return -1;

    current->cr3 = new_pml4;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(new_pml4) : "memory");

    if (old_pml4) {
        free_process_pml4(old_pml4);
    }

uint64_t entry_point = 0;
    uint64_t max_vaddr = HEAP_START_VIRT;

    if (is_linus001) {
        uint64_t total_mem_size = hdr->text_size;
        if (total_mem_size < file_size) total_mem_size = file_size;
        if (total_mem_size > 16 * 1024 * 1024) total_mem_size = 16 * 1024 * 1024;

        uint64_t total_pages = (total_mem_size + PAGE_SIZE - 1) / PAGE_SIZE;
        for (uint64_t p = 0; p < total_pages; p++) {
            uint64_t text_phys = get_free_page();
            if (!text_phys) return -1;
            map_page((uint64_t *)new_pml4, USER_TEXT_BASE + (p * PAGE_SIZE), text_phys, PTE_WRITABLE | PTE_USER);
        }
        memcpy((void *)USER_TEXT_BASE, data, file_size);
        if (total_pages * PAGE_SIZE > file_size) {
            memset((void *)(USER_TEXT_BASE + file_size), 0, (total_pages * PAGE_SIZE) - file_size);
        }
        entry_point = hdr->entry;
        max_vaddr = USER_TEXT_BASE + total_pages * PAGE_SIZE;
    } else if (is_elf64) {
        entry_point = ehdr->e_entry;
        for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
            const Elf64_Phdr *ph = (const Elf64_Phdr *)(data + ehdr->e_phoff + i * ehdr->e_phentsize);
            if (ph->p_type != PT_LOAD) continue;

            uint64_t seg_start = ph->p_vaddr;
            uint64_t seg_end   = ph->p_vaddr + ph->p_memsz;
            if (seg_end > max_vaddr) max_vaddr = seg_end;

            uint64_t page_start = seg_start & ~0xFFFULL;
            uint64_t page_end   = PAGE_ALIGN(seg_end);

            for (uint64_t va = page_start; va < page_end; va += PAGE_SIZE) {
                uint64_t phys = get_free_page();
                if (!phys) return -1;
                map_page((uint64_t *)new_pml4, va, phys, PTE_WRITABLE | PTE_USER);
                memset((void *)va, 0, PAGE_SIZE);
            }

            if (ph->p_filesz > 0 && ph->p_offset + ph->p_filesz <= file_size) {
                memcpy((void *)ph->p_vaddr, data + ph->p_offset, ph->p_filesz);
            }
        }
    }

    uint64_t new_stack = get_free_pages(8);
    if (!new_stack) return -1;

    if (current->user_stack_page) {
        free_pages(current->user_stack_page, 8);
    }
    current->user_stack_page = new_stack;
    uint64_t user_rsp = new_stack + (8 * PAGE_SIZE) - 16;

    uint64_t u_argv_ptrs[18];
    for (int i = 0; i < argc; i++) {
        uint64_t slen = strlen(k_argv_buf[i]) + 1;
        user_rsp -= slen;
        memcpy((void *)user_rsp, k_argv_buf[i], slen);
        u_argv_ptrs[i] = user_rsp;
    }
    u_argv_ptrs[argc] = 0;

    user_rsp &= ~15ULL;

    uint64_t argv_table_size = (argc + 1) * sizeof(uint64_t);
    user_rsp -= argv_table_size;
    memcpy((void *)user_rsp, u_argv_ptrs, argv_table_size);
    uint64_t argv_ptr = user_rsp;

    user_rsp -= sizeof(uint64_t);
    *(uint64_t *)user_rsp = (uint64_t)argc;

    if ((user_rsp % 16) == 0) user_rsp -= 8;

    current->start_brk = (max_vaddr > HEAP_START_VIRT) ? PAGE_ALIGN(max_vaddr) : HEAP_START_VIRT;
    current->brk = current->start_brk;

    for (int i = 3; i < NR_OPEN; i++) {
        if (current->filp[i].in_use) {
            sys_close(i);
        }
    }

    tf->rip = entry_point;
    tf->rsp = user_rsp;
    tf->rbp = user_rsp;
    tf->rdi = (uint64_t)argc;
    tf->rsi = argv_ptr;
    tf->rax = 0;
    tf->rflags = 0x202;

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

int64_t sys_sync(void)
{
    minix_sync();
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

    for (int i = 1; i < NR_TASKS; i++) {
        if (task[i] && task[i]->father == current->pid) {
            task[i]->father = 1;
        }
    }

    current->exit_code = status;
    current->state = TASK_ZOMBIE;
    schedule();
    for (;;);
    return 0;
}

static int64_t sys_socket(int domain, int type, int protocol)
{
    (void)domain; (void)type; (void)protocol;
    int sock_id = tcp_socket_create();
    if (sock_id < 0) return -1;

    for (int fd = 3; fd < NR_OPEN; fd++) {
        if (!current->filp[fd].in_use) {
            current->filp[fd].type = FILE_TYPE_SOCKET;
            current->filp[fd].sock_id = sock_id;
            current->filp[fd].in_use = 1;
            current->filp[fd].mode = 3; /* Read/Write */
            return fd;
        }
    }
    tcp_socket_close(sock_id);
    return -1;
}

static int64_t sys_bind(int fd, uint16_t port)
{
    if (fd < 0 || fd >= NR_OPEN || !current->filp[fd].in_use) return -1;
    if (current->filp[fd].type != FILE_TYPE_SOCKET) return -1;
    return tcp_socket_bind(current->filp[fd].sock_id, port);
}

static int64_t sys_listen(int fd, int backlog)
{
    (void)backlog;
    if (fd < 0 || fd >= NR_OPEN || !current->filp[fd].in_use) return -1;
    if (current->filp[fd].type != FILE_TYPE_SOCKET) return -1;
    return tcp_socket_listen(current->filp[fd].sock_id);
}

static int64_t sys_accept(int fd)
{
    if (fd < 0 || fd >= NR_OPEN || !current->filp[fd].in_use) return -1;
    if (current->filp[fd].type != FILE_TYPE_SOCKET) return -1;

    int client_sock = tcp_socket_accept(current->filp[fd].sock_id);
    if (client_sock < 0) return -1;

    /* Ищем свободный дескриптор, исключая дескриптор самого сервера */
    for (int nfd = fd + 1; nfd < NR_OPEN; nfd++) {
        if (!current->filp[nfd].in_use) {
            current->filp[nfd].type = FILE_TYPE_SOCKET;
            current->filp[nfd].sock_id = client_sock;
            current->filp[nfd].in_use = 1;
            current->filp[nfd].mode = 3;
            return nfd;
        }
    }
    tcp_socket_close(client_sock);
    return -1;
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
        case __NR_uname:
            ret = sys_uname((struct utsname *)arg1);
            break;
        case __NR_exit:
            ret = sys_exit((int)arg1);
            break;
        case __NR_socket:
            ret = sys_socket((int)arg1, (int)arg2, (int)arg3);
            break;
        case __NR_bind:
            ret = sys_bind((int)arg1, (uint16_t)arg2);
            break;
        case __NR_listen:
            ret = sys_listen((int)arg1, (int)arg2);
            break;
        case __NR_accept:
            ret = sys_accept((int)arg1);
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
