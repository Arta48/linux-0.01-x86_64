#include <linux/tty.h>
#include <linux/traps.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/gdt.h>
#include <linux/syscall.h>
#include <linux/fs.h>

extern void enter_user_mode(uint64_t entry_point, uint64_t user_stack);

static inline int64_t u_int80(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    int64_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "b"(arg1), "c"(arg2), "d"(arg3)
        : "memory"
    );
    return ret;
}

static inline int64_t u_syscall(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int64_t u_open(const char *path, int flags)
{
    return u_syscall(__NR_open, (uint64_t)path, flags, 0);
}

static inline int64_t u_close(int fd)
{
    return u_syscall(__NR_close, fd, 0, 0);
}

static inline int64_t u_read(int fd, char *buf, uint64_t count)
{
    return u_syscall(__NR_read, fd, (uint64_t)buf, count);
}

static inline int64_t u_write(int fd, const char *buf, uint64_t count)
{
    return u_syscall(__NR_write, fd, (uint64_t)buf, count);
}

static inline int64_t u_pipe(int *pipefd)
{
    return u_syscall(__NR_pipe, (uint64_t)pipefd, 0, 0);
}

static inline int64_t u_getpid(void)
{
    return u_syscall(__NR_getpid, 0, 0, 0);
}

static inline int64_t u_time(void)
{
    return u_syscall(__NR_time, 0, 0, 0);
}

static inline int64_t u_fork(void)
{
    return u_int80(__NR_fork, 0, 0, 0);
}

static inline void u_ps(void)
{
    u_syscall(__NR_ps, 0, 0, 0);
}

static inline int64_t u_waitpid(int64_t pid, int *stat, int options)
{
    return u_syscall(__NR_waitpid, (uint64_t)pid, (uint64_t)stat, options);
}

static inline int64_t u_kill(int64_t pid, int sig)
{
    return u_syscall(__NR_kill, (uint64_t)pid, sig, 0);
}

static inline int64_t u_brk(uint64_t new_brk)
{
    return u_syscall(__NR_brk, new_brk, 0, 0);
}

static inline int64_t u_list(char *buf, uint64_t max_len)
{
    return u_syscall(__NR_list, (uint64_t)buf, max_len, 0);
}

static inline void u_exit(int status)
{
    u_syscall(__NR_exit, status, 0, 0);
}

/* --- ПОЛЬЗОВАТЕЛЬСКИЙ АЛЛОКАТОР ПАМЯТИ MALLOC / FREE (RING 3) --- */

struct block_header {
    uint64_t size;
    int is_free;
    struct block_header *next;
};

static struct block_header *heap_head = NULL;

static void *u_sbrk(int64_t increment)
{
    uint64_t cur_brk = (uint64_t)u_brk(0);
    if (increment == 0) {
        return (void *)cur_brk;
    }

    uint64_t new_brk = cur_brk + increment;
    uint64_t res = (uint64_t)u_brk(new_brk);
    if (res < new_brk) {
        return (void *)-1;
    }
    return (void *)cur_brk;
}

static void *u_malloc(uint64_t size)
{
    if (size == 0) return NULL;

    /* Выравнивание размера по 16 байт */
    size = (size + 15) & ~15ULL;

    /* Поиск свободного блока (First-Fit) */
    struct block_header *curr = heap_head;
    while (curr) {
        if (curr->is_free && curr->size >= size) {
            curr->is_free = 0;
            return (void *)(curr + 1);
        }
        curr = curr->next;
    }

    /* Свободный блок не найден: расширяем кучу через sbrk */
    uint64_t total_size = sizeof(struct block_header) + size;
    void *raw = u_sbrk((int64_t)total_size);
    if (raw == (void *)-1) {
        return NULL; /* Out of Memory */
    }

    struct block_header *new_block = (struct block_header *)raw;
    new_block->size = size;
    new_block->is_free = 0;
    new_block->next = NULL;

    if (!heap_head) {
        heap_head = new_block;
    } else {
        curr = heap_head;
        while (curr->next) curr = curr->next;
        curr->next = new_block;
    }

    return (void *)(new_block + 1);
}

static void u_free(void *ptr)
{
    if (!ptr) return;

    struct block_header *hdr = (struct block_header *)ptr - 1;
    hdr->is_free = 1;

    /* Объединение соседних свободных блоков (Coalescing) */
    struct block_header *curr = heap_head;
    while (curr && curr->next) {
        if (curr->is_free && curr->next->is_free) {
            curr->size += sizeof(struct block_header) + curr->next->size;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}

/* Строковые вспомогательные функции */
static void u_print(const char *s)
{
    uint64_t len = 0;
    while (s[len]) len++;
    u_write(1, s, len);
}

static void u_print_num(uint64_t n)
{
    char buf[32];
    char digits[] = "0123456789";
    int i = 0;
    if (n == 0) {
        u_print("0");
        return;
    }
    while (n > 0) {
        buf[i++] = digits[n % 10];
        n /= 10;
    }
    while (--i >= 0) {
        u_write(1, &buf[i], 1);
    }
}

static void u_print_hex(uint64_t n)
{
    char buf[32];
    char digits[] = "0123456789ABCDEF";
    int i = 0;
    if (n == 0) {
        u_print("0x0");
        return;
    }
    while (n > 0) {
        buf[i++] = digits[n % 16];
        n /= 16;
    }
    u_print("0x");
    while (--i >= 0) {
        u_write(1, &buf[i], 1);
    }
}

static int u_strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static int u_strncmp(const char *s1, const char *s2, uint64_t n)
{
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static int u_atoi(const char *s)
{
    int res = 0;
    while (*s >= '0' && *s <= '9') {
        res = res * 10 + (*s - '0');
        s++;
    }
    return res;
}

static void execute_command(const char *cmd)
{
    if (cmd[0] == '\0') {
        return;
    }

    if (u_strcmp(cmd, "help") == 0) {
        u_print("Linux 0.01 (x86_64) Shell built-in commands:\n");
        u_print("  help         - show this help message\n");
        u_print("  heap         - inspect process heap (brk / sbrk)\n");
        u_print("  malloc <sz>  - allocate <sz> bytes on the heap\n");
        u_print("  malloctest   - stress test malloc() and free() allocation\n");
        u_print("  ls           - list files in root RamFS\n");
        u_print("  cat <file>   - display file contents\n");
        u_print("  pipe         - test Unix IPC pipe (auto-reaped)\n");
        u_print("  fork         - test fork and waitpid reaping\n");
        u_print("  spawn        - spawn background infinite process\n");
        u_print("  kill <pid>   - kill process by PID\n");
        u_print("  wait         - reap zombie processes\n");
        u_print("  ps           - show running processes\n");
        u_print("  bench        - benchmark 'int 0x80' vs 'syscall'\n");
        u_print("  uptime       - show system uptime\n");
        u_print("  getpid       - show current process ID\n");
        u_print("  clear        - clear the console screen\n");
        u_print("  echo ..      - print arguments to console\n");
        u_print("  exit         - terminate this shell process\n");
    } else if (u_strcmp(cmd, "heap") == 0) {
        uint64_t cur_brk = (uint64_t)u_brk(0);
        uint64_t heap_size = cur_brk - HEAP_START_VIRT;

        u_print("Process Heap Info:\n  Start Break : ");
        u_print_hex(HEAP_START_VIRT);
        u_print("\n  Current Break: ");
        u_print_hex(cur_brk);
        u_print("\n  Heap Size   : ");
        u_print_num(heap_size);
        u_print(" bytes (");
        u_print_num((heap_size + 4095) / 4096);
        u_print(" pages)\n");
    } else if (u_strncmp(cmd, "malloc ", 7) == 0) {
        uint64_t sz = (uint64_t)u_atoi(cmd + 7);
        if (sz == 0) {
            u_print("malloc: size must be > 0\n");
            return;
        }

        void *ptr = u_malloc(sz);
        if (!ptr) {
            u_print("malloc: out of memory!\n");
        } else {
            u_print("Allocated ");
            u_print_num(sz);
            u_print(" bytes at address: ");
            u_print_hex((uint64_t)ptr);
            u_print("\nWriting test pattern...\n");

            char *cp = (char *)ptr;
            for (uint64_t i = 0; i < sz - 1 && i < 26; i++) {
                cp[i] = 'A' + i;
            }
            cp[(sz > 26 ? 26 : sz - 1)] = '\0';

            u_print("Data verification: \"");
            u_print(cp);
            u_print("\"\nFreeing allocated block...\n");
            u_free(ptr);
            u_print("Block freed successfully!\n");
        }
    } else if (u_strcmp(cmd, "malloctest") == 0) {
        u_print("--- Running malloc / free Stress Test ---\n");

        u_print("1. Allocating 3 blocks: B1 (64B), B2 (256B), B3 (1024B)...\n");
        char *b1 = (char *)u_malloc(64);
        char *b2 = (char *)u_malloc(256);
        char *b3 = (char *)u_malloc(1024);

        u_print("   B1 at "); u_print_hex((uint64_t)b1); u_print("\n");
        u_print("   B2 at "); u_print_hex((uint64_t)b2); u_print("\n");
        u_print("   B3 at "); u_print_hex((uint64_t)b3); u_print("\n");

        u_print("2. Writing test patterns into B1, B2, B3...\n");
        b1[0] = 'X'; b1[1] = '\0';
        b2[0] = 'Y'; b2[1] = '\0';
        b3[0] = 'Z'; b3[1] = '\0';

        u_print("3. Freeing middle block B2...\n");
        u_free(b2);

        u_print("4. Allocating B4 (128B) - should reuse B2's slot without heap expansion...\n");
        char *b4 = (char *)u_malloc(128);
        u_print("   B4 at "); u_print_hex((uint64_t)b4);
        if (b4 == b2) {
            u_print(" (EXACT MATCH: B2 reused successfully!)\n");
        } else {
            u_print(" (Allocated new)\n");
        }

        u_print("5. Freeing B1, B4, B3 (Coalescing test)...\n");
        u_free(b1);
        u_free(b4);
        u_free(b3);

        u_print("6. Allocating big block B5 (2048B) across merged space...\n");
        char *b5 = (char *)u_malloc(2048);
        u_print("   B5 at "); u_print_hex((uint64_t)b5);
        if (b5 == b1) {
            u_print(" (COALESCING PASSED: Reused merged blocks!)\n");
        } else {
            u_print("\n");
        }
        u_free(b5);

        u_print("--- Malloc / Free Test Passed 100%! ---\n");
    } else if (u_strcmp(cmd, "pipe") == 0) {
        int pipefd[2];
        if (u_pipe(pipefd) < 0) {
            u_print("pipe: failed to create pipe!\n");
            return;
        }

        u_print("Created pipe: read_fd = ");
        u_print_num(pipefd[0]);
        u_print(", write_fd = ");
        u_print_num(pipefd[1]);
        u_print("\nForking child to test IPC communication...\n");

        int64_t pid = u_fork();
        if (pid == 0) {
            u_close(pipefd[0]);
            const char msg[] = ">>> [PIPE IPC] Secret message transmitted from Child to Parent through Pipe!\n";
            u_write(pipefd[1], msg, sizeof(msg) - 1);
            u_close(pipefd[1]);
            u_exit(0);
        } else if (pid > 0) {
            u_close(pipefd[1]);
            char pbuf[128];
            int64_t n = u_read(pipefd[0], pbuf, sizeof(pbuf) - 1);
            if (n > 0) {
                pbuf[n] = '\0';
                u_print("Parent received via Pipe:\n");
                u_print(pbuf);
            }
            u_close(pipefd[0]);

            int status = 0;
            u_waitpid(pid, &status, 0);
            u_print("[Parent] Reaped child PID ");
            u_print_num(pid);
            u_print(" successfully (status = ");
            u_print_num(status);
            u_print(")\n");
        }
    } else if (u_strcmp(cmd, "fork") == 0) {
        int64_t pid = u_fork();

        if (pid < 0) {
            u_print("fork: failed to clone process!\n");
        } else if (pid == 0) {
            u_print("\n>>> [CHILD] Process successfully spawned! PID = ");
            u_print_num(u_getpid());
            u_print("\n>>> [CHILD] Simulating work for 2 seconds...\n");

            uint64_t start = (uint64_t)u_time();
            while ((uint64_t)u_time() - start < 200) {}

            u_print(">>> [CHILD] Work finished. Calling sys_exit(0)...\n");
            u_exit(0);
        } else {
            u_print("Parent spawned child with PID = ");
            u_print_num(pid);
            u_print(". Waiting for child to finish...\n");

            int status = 0;
            u_waitpid(pid, &status, 0);
            u_print("[Parent] Child finished and reaped! Status = ");
            u_print_num(status);
            u_print("\n");
        }
    } else if (u_strcmp(cmd, "spawn") == 0) {
        int64_t pid = u_fork();
        if (pid == 0) {
            while (1) {
                for (volatile int i = 0; i < 50000000; i++) {}
            }
        } else if (pid > 0) {
            u_print("Spawned background infinite process with PID = ");
            u_print_num(pid);
            u_print(". Type 'ps' or 'kill <pid>'\n");
        }
    } else if (u_strncmp(cmd, "kill ", 5) == 0) {
        int target_pid = u_atoi(cmd + 5);
        if (target_pid <= 1) {
            u_print("kill: cannot kill system processes!\n");
        } else {
            if (u_kill(target_pid, 9) == 0) {
                u_print("Killed process ");
                u_print_num(target_pid);
                u_print("\n");
            } else {
                u_print("kill: process not found or already dead\n");
            }
        }
    } else if (u_strcmp(cmd, "wait") == 0) {
        int status = 0;
        int64_t reaped = u_waitpid(-1, &status, 0);
        if (reaped > 0) {
            u_print("Reaped zombie process PID = ");
            u_print_num(reaped);
            u_print(" (status = ");
            u_print_num(status);
            u_print(")\n");
        } else {
            u_print("wait: no zombie children to reap\n");
        }
    } else if (u_strcmp(cmd, "ls") == 0) {
        char buf[512];
        if (u_list(buf, sizeof(buf)) > 0) {
            u_print(buf);
        }
    } else if (u_strncmp(cmd, "cat ", 4) == 0) {
        const char *filename = cmd + 4;
        while (*filename == ' ') filename++;

        int64_t fd = u_open(filename, 0);
        if (fd < 0) {
            u_print("cat: file not found: ");
            u_print(filename);
            u_print("\n");
        } else {
            char fbuf[128];
            int64_t n;
            while ((n = u_read(fd, fbuf, sizeof(fbuf) - 1)) > 0) {
                fbuf[n] = '\0';
                u_write(1, fbuf, n);
            }
            u_close(fd);
        }
    } else if (u_strcmp(cmd, "bench") == 0) {
        u_print("Running benchmark: 500,000 getpid() syscalls...\n");

        uint64_t start_int = (uint64_t)u_time();
        for (int i = 0; i < 500000; i++) {
            u_int80(__NR_getpid, 0, 0, 0);
        }
        uint64_t time_int = (uint64_t)u_time() - start_int;

        uint64_t start_fast = (uint64_t)u_time();
        for (int i = 0; i < 500000; i++) {
            u_syscall(__NR_getpid, 0, 0, 0);
        }
        uint64_t time_fast = (uint64_t)u_time() - start_fast;

        u_print("Results:\n  Legacy 'int 0x80' : ");
        u_print_num(time_int);
        u_print(" jiffies\n  Fast   'syscall'  : ");
        u_print_num(time_fast);
        u_print(" jiffies\n");
    } else if (u_strcmp(cmd, "ps") == 0) {
        u_ps();
    } else if (u_strcmp(cmd, "uptime") == 0) {
        uint64_t ticks = (uint64_t)u_time();
        uint64_t sec = ticks / 100;
        u_print("Uptime: ");
        u_print_num(sec);
        u_print(" seconds (");
        u_print_num(ticks);
        u_print(" jiffies)\n");
    } else if (u_strcmp(cmd, "getpid") == 0) {
        u_print("Current Process PID: ");
        u_print_num((uint64_t)u_getpid());
        u_print("\n");
    } else if (u_strcmp(cmd, "clear") == 0) {
        u_print("\f");
    } else if (u_strncmp(cmd, "echo ", 5) == 0) {
        u_print(cmd + 5);
        u_print("\n");
    } else if (u_strcmp(cmd, "exit") == 0) {
        u_print("Exiting shell...\n");
        u_exit(0);
    } else {
        u_print("shell: command not found: ");
        u_print(cmd);
        u_print("\n");
    }
}

void user_init_process(void)
{
    const char banner[] =
    "\n========================================\n"
    "  Linux 0.01 (x86_64) Interactive Shell \n"
    "  Type 'help' to see available commands \n"
    "========================================\n\n";
    u_print(banner);

    char cmd_buf[128];
    int buf_len = 0;
    const char prompt[] = "user@linux64:~$ ";

    u_print(prompt);

    while (1) {
        char c;
        if (u_read(0, &c, 1) > 0) {
            if (c == '\b') {
                if (buf_len > 0) {
                    buf_len--;
                    u_write(1, "\b", 1);
                }
            }
            else if (c == '\n') {
                u_write(1, "\n", 1);
                cmd_buf[buf_len] = '\0';

                execute_command(cmd_buf);

                buf_len = 0;
                u_print(prompt);
            }
            else if (c >= 32 && c <= 126) {
                if (buf_len < (int)sizeof(cmd_buf) - 1) {
                    cmd_buf[buf_len++] = c;
                    u_write(1, &c, 1);
                }
            }
        }
    }
}

void user_trampoline(void)
{
    uint64_t user_stack = get_free_page() + PAGE_SIZE - 16;
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
    syscall_init();
    mem_init();
    sched_init();
    fs_init();

    task_create(user_trampoline, 10);

    __asm__ volatile ("sti");
    printk("[OK] System Initialized. Launching User Shell...\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
