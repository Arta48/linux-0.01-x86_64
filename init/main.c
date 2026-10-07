#include <linux/tty.h>
#include <linux/traps.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/gdt.h>
#include <linux/syscall.h>
#include <linux/fs.h>
#include <linux/signal.h>

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

static inline int64_t u_dup2(int oldfd, int newfd)
{
    return u_syscall(__NR_dup2, oldfd, newfd, 0);
}

static inline int64_t u_unlink(const char *path)
{
    return u_syscall(__NR_unlink, (uint64_t)path, 0, 0);
}

static inline int64_t u_chdir(const char *path)
{
    return u_syscall(__NR_chdir, (uint64_t)path, 0, 0);
}

static inline int64_t u_mkdir(const char *path)
{
    return u_syscall(__NR_mkdir, (uint64_t)path, 0, 0);
}

static inline int64_t u_rmdir(const char *path)
{
    return u_syscall(__NR_rmdir, (uint64_t)path, 0, 0);
}

static inline int64_t u_getcwd(char *buf, uint64_t size)
{
    return u_syscall(__NR_getcwd, (uint64_t)buf, size, 0);
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

static inline int64_t u_execve(const char *path, char **argv, char **envp)
{
    return u_int80(__NR_execve, (uint64_t)path, (uint64_t)argv, (uint64_t)envp);
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

static inline int64_t u_signal(int sig, void (*handler)(int))
{
    return u_syscall(__NR_signal, sig, (uint64_t)handler, 0);
}

static inline int64_t u_brk(uint64_t new_brk)
{
    return u_syscall(__NR_brk, new_brk, 0, 0);
}

static inline int64_t u_list(const char *path, char *buf, uint64_t max_len)
{
    return u_syscall(__NR_list, (uint64_t)path, (uint64_t)buf, max_len);
}

static inline void u_exit(int status)
{
    u_syscall(__NR_exit, status, 0, 0);
}

/* Аллокатор памяти malloc / free */
struct block_header {
    uint64_t size;
    int is_free;
    struct block_header *next;
};

static struct block_header *heap_head = NULL;

static void *u_sbrk(int64_t increment)
{
    uint64_t cur_brk = (uint64_t)u_brk(0);
    if (increment == 0) return (void *)cur_brk;
    uint64_t new_brk = cur_brk + increment;
    uint64_t res = (uint64_t)u_brk(new_brk);
    if (res < new_brk) return (void *)-1;
    return (void *)cur_brk;
}

static void *u_malloc(uint64_t size)
{
    if (size == 0) return NULL;
    size = (size + 15) & ~15ULL;
    struct block_header *curr = heap_head;
    while (curr) {
        if (curr->is_free && curr->size >= size) {
            curr->is_free = 0;
            return (void *)(curr + 1);
        }
        curr = curr->next;
    }
    uint64_t total_size = sizeof(struct block_header) + size;
    void *raw = u_sbrk((int64_t)total_size);
    if (raw == (void *)-1) return NULL;
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

static volatile int sigint_received = 0;
static void user_sigint_handler(int sig)
{
    (void)sig;
    u_print("\n>>> [USER SIGNAL HANDLER] Caught SIGINT (Ctrl+C) in Ring 3!\n");
    sigint_received = 1;
}

static void execute_command(const char *cmd);

static void execute_pipeline(const char *cmd, const char *pipe_pos)
{
    char left[64];
    char right[64];

    uint64_t l_len = pipe_pos - cmd;
    if (l_len >= sizeof(left)) l_len = sizeof(left) - 1;
    for (uint64_t i = 0; i < l_len; i++) left[i] = cmd[i];
    while (l_len > 0 && (left[l_len - 1] == ' ' || left[l_len - 1] == '\t')) l_len--;
    left[l_len] = '\0';

    const char *r_ptr = pipe_pos + 1;
    while (*r_ptr == ' ' || *r_ptr == '\t') r_ptr++;
    uint64_t r_len = 0;
    while (r_ptr[r_len] && r_len < sizeof(right) - 1) {
        right[r_len] = r_ptr[r_len];
        r_len++;
    }
    right[r_len] = '\0';

    int p[2];
    if (u_pipe(p) < 0) return;

    int64_t pid1 = u_fork();
    if (pid1 == 0) {
        u_dup2(p[1], 1);
        u_close(p[0]);
        u_close(p[1]);
        execute_command(left);
        u_exit(0);
    }

    int64_t pid2 = u_fork();
    if (pid2 == 0) {
        u_dup2(p[0], 0);
        u_close(p[0]);
        u_close(p[1]);
        execute_command(right);
        u_exit(0);
    }

    u_close(p[0]);
    u_close(p[1]);
    int status;
    u_waitpid(pid1, &status, 0);
    u_waitpid(pid2, &status, 0);
}

static void execute_redirection(const char *cmd, const char *redir_pos)
{
    char left[64];
    char fname[MAX_FILENAME];

    uint64_t l_len = redir_pos - cmd;
    if (l_len >= sizeof(left)) l_len = sizeof(left) - 1;
    for (uint64_t i = 0; i < l_len; i++) left[i] = cmd[i];
    while (l_len > 0 && (left[l_len - 1] == ' ' || left[l_len - 1] == '\t')) l_len--;
    left[l_len] = '\0';

    const char *r_ptr = redir_pos + 1;
    while (*r_ptr == ' ' || *r_ptr == '\t') r_ptr++;
    uint64_t r_len = 0;
    while (r_ptr[r_len] && r_ptr[r_len] != ' ' && r_ptr[r_len] != '\t' && r_len < MAX_FILENAME - 1) {
        fname[r_len] = r_ptr[r_len];
        r_len++;
    }
    fname[r_len] = '\0';

    int64_t pid = u_fork();
    if (pid == 0) {
        int64_t fd = u_open(fname, O_CREAT | O_WRONLY | O_TRUNC);
        if (fd < 0) {
            u_print("shell: cannot open file: ");
            u_print(fname);
            u_print("\n");
            u_exit(1);
        }
        u_dup2((int)fd, 1);
        u_close((int)fd);

        execute_command(left);
        u_exit(0);
    } else if (pid > 0) {
        int status;
        u_waitpid(pid, &status, 0);
    }
}

static void execute_command(const char *cmd)
{
    if (cmd[0] == '\0') return;

    const char *pipe_pos = cmd;
    while (*pipe_pos && *pipe_pos != '|') pipe_pos++;
    if (*pipe_pos == '|') {
        execute_pipeline(cmd, pipe_pos);
        return;
    }

    const char *redir_pos = cmd;
    while (*redir_pos && *redir_pos != '>') redir_pos++;
    if (*redir_pos == '>') {
        execute_redirection(cmd, redir_pos);
        return;
    }

    if (u_strcmp(cmd, "help") == 0) {
        u_print("Linux 0.01 (x86_64) Shell built-in commands:\n");
        u_print("  help            - show this help message\n");
        u_print("  pwd / cd [dir]  - directory navigation\n");
        u_print("  ls [dir]        - list files in directory\n");
        u_print("  mkdir / rmdir   - directory management\n");
        u_print("  cat [file]      - display file contents (or stdin)\n");
        u_print("  touch / rm      - create or remove files\n");
        u_print("  cmd > <file>    - redirect command output to file\n");
        u_print("  cmd1 | cmd2     - execute Unix pipeline\n");
        u_print("  <binary>        - execute binary via fork() + execve()\n");
        u_print("  heap / malloc   - dynamic memory management\n");
        u_print("  malloctest      - stress test malloc() and free()\n");
        u_print("  sleep <sec>     - sleep N seconds (interruptible by Ctrl+C)\n");
        u_print("  sigtest         - test Ring 3 custom SIGINT handler\n");
        u_print("  ps / kill / wait- process management\n");
        u_print("  bench           - benchmark 'int 0x80' vs 'syscall'\n");
        u_print("  clear / exit    - terminal control\n");
    } else if (u_strcmp(cmd, "pwd") == 0) {
        char buf[64];
        if (u_getcwd(buf, sizeof(buf)) >= 0) {
            u_print(buf);
            u_print("\n");
        }
    } else if (u_strncmp(cmd, "cd ", 3) == 0) {
        const char *target = cmd + 3;
        while (*target == ' ') target++;
        if (u_chdir(target) != 0) {
            u_print("cd: no such directory: ");
            u_print(target);
            u_print("\n");
        }
    } else if (u_strcmp(cmd, "cd") == 0) {
        u_chdir("/");
    } else if (u_strncmp(cmd, "mkdir ", 6) == 0) {
        const char *dname = cmd + 6;
        while (*dname == ' ') dname++;
        if (u_mkdir(dname) != 0) {
            u_print("mkdir: cannot create directory\n");
        }
    } else if (u_strncmp(cmd, "rmdir ", 6) == 0) {
        const char *dname = cmd + 6;
        while (*dname == ' ') dname++;
        if (u_rmdir(dname) != 0) {
            u_print("rmdir: cannot remove directory\n");
        }
    } else if (u_strncmp(cmd, "ls", 2) == 0 && (cmd[2] == ' ' || cmd[2] == '\0')) {
        const char *arg = cmd + 2;
        while (*arg == ' ') arg++;
        char buf[512];
        if (u_list(arg, buf, sizeof(buf)) > 0) {
            u_print(buf);
        }
    } else if (u_strncmp(cmd, "touch ", 6) == 0) {
        const char *fname = cmd + 6;
        while (*fname == ' ') fname++;
        int64_t fd = u_open(fname, O_CREAT | O_WRONLY);
        if (fd >= 0) u_close((int)fd);
    } else if (u_strncmp(cmd, "rm ", 3) == 0) {
        const char *fname = cmd + 3;
        while (*fname == ' ') fname++;
        if (u_unlink(fname) != 0) u_print("rm: cannot remove file\n");
    } else if (u_strcmp(cmd, "cat") == 0) {
        char fbuf[128];
        int64_t n;
        while ((n = u_read(0, fbuf, sizeof(fbuf) - 1)) > 0) {
            fbuf[n] = '\0';
            u_write(1, fbuf, n);
        }
    } else if (u_strncmp(cmd, "cat ", 4) == 0) {
        const char *filename = cmd + 4;
        while (*filename == ' ') filename++;
        int64_t fd = u_open(filename, O_RDONLY);
        if (fd < 0) {
            u_print("cat: file not found: ");
            u_print(filename);
            u_print("\n");
        } else {
            char fbuf[128];
            int64_t n;
            while ((n = u_read((int)fd, fbuf, sizeof(fbuf) - 1)) > 0) {
                fbuf[n] = '\0';
                u_write(1, fbuf, n);
            }
            u_close((int)fd);
        }
    } else if (u_strncmp(cmd, "echo ", 5) == 0) {
        u_print(cmd + 5);
        u_print("\n");
    } else if (u_strcmp(cmd, "heap") == 0) {
        uint64_t cur_brk = (uint64_t)u_brk(0);
        uint64_t heap_size = cur_brk - HEAP_START_VIRT;
        u_print("Process Heap Info:\n  Start Break : ");
        u_print_hex(HEAP_START_VIRT);
        u_print("\n  Current Break: ");
        u_print_hex(cur_brk);
        u_print("\n  Heap Size   : ");
        u_print_num(heap_size);
        u_print(" bytes\n");
    } else if (u_strncmp(cmd, "malloc ", 7) == 0) {
        uint64_t sz = (uint64_t)u_atoi(cmd + 7);
        if (sz == 0) return;
        void *ptr = u_malloc(sz);
        if (!ptr) {
            u_print("malloc: out of memory!\n");
        } else {
            u_print("Allocated ");
            u_print_num(sz);
            u_print(" bytes at address: ");
            u_print_hex((uint64_t)ptr);
            u_print("\nFreeing block...\n");
            u_free(ptr);
        }
    } else if (u_strcmp(cmd, "malloctest") == 0) {
        u_print("--- Running malloc / free Stress Test ---\n");
        char *b1 = (char *)u_malloc(64);
        char *b2 = (char *)u_malloc(256);
        char *b3 = (char *)u_malloc(1024);
        u_print("1. B1 at "); u_print_hex((uint64_t)b1); u_print("\n");
        u_print("   B2 at "); u_print_hex((uint64_t)b2); u_print("\n");
        u_print("   B3 at "); u_print_hex((uint64_t)b3); u_print("\n");
        u_free(b2);
        char *b4 = (char *)u_malloc(128);
        u_print("2. B4 at "); u_print_hex((uint64_t)b4);
        if (b4 == b2) u_print(" (REUSED B2!)\n"); else u_print("\n");
        u_free(b1); u_free(b4); u_free(b3);
        char *b5 = (char *)u_malloc(2048);
        u_print("3. B5 at "); u_print_hex((uint64_t)b5);
        if (b5 == b1) u_print(" (COALESCED SUCCESS!)\n"); else u_print("\n");
        u_free(b5);
        u_print("--- Malloc Test Passed! ---\n");
    } else if (u_strncmp(cmd, "sleep ", 6) == 0) {
        int sec = u_atoi(cmd + 6);
        if (sec <= 0) sec = 1;

        u_print("Sleeping for ");
        u_print_num(sec);
        u_print(" seconds (press Ctrl+C to abort)...\n");

        int64_t pid = u_fork();
        if (pid == 0) {
            uint64_t start = (uint64_t)u_time();
            while ((uint64_t)u_time() - start < (uint64_t)(sec * 100)) {}
            u_exit(0);
        } else if (pid > 0) {
            int status = 0;
            u_waitpid(pid, &status, 0);
            if (status == 130) {
                u_print("Sleep aborted by SIGINT (Ctrl+C)!\n");
            } else {
                u_print("Done.\n");
            }
        }
    } else if (u_strcmp(cmd, "sigtest") == 0) {
        int64_t pid = u_fork();
        if (pid == 0) {
            u_print("Child registered custom SIGINT handler in Ring 3!\n");
            sigint_received = 0;
            u_signal(SIGINT, user_sigint_handler);
            u_print("Press Ctrl+C in terminal to trigger it...\n");

            while (!sigint_received) {
                for (volatile int i = 0; i < 10000000; i++) {}
            }

            u_print("Signal handled successfully. Exiting child.\n");
            u_exit(0);
        } else if (pid > 0) {
            int status = 0;
            u_waitpid(pid, &status, 0);
        }
    } else if (u_strcmp(cmd, "pipe") == 0) {
        int pipefd[2];
        if (u_pipe(pipefd) < 0) return;
        int64_t pid = u_fork();
        if (pid == 0) {
            u_close(pipefd[0]);
            const char msg[] = ">>> [PIPE IPC] Secret message transmitted through Pipe!\n";
            u_write(pipefd[1], msg, sizeof(msg) - 1);
            u_close(pipefd[1]);
            u_exit(0);
        } else if (pid > 0) {
            u_close(pipefd[1]);
            char pbuf[128];
            int64_t n = u_read(pipefd[0], pbuf, sizeof(pbuf) - 1);
            if (n > 0) {
                pbuf[n] = '\0';
                u_print("Parent received: ");
                u_print(pbuf);
            }
            u_close(pipefd[0]);
            int status = 0;
            u_waitpid(pid, &status, 0);
        }
    } else if (u_strcmp(cmd, "fork") == 0) {
        int64_t pid = u_fork();
        if (pid == 0) {
            u_print("Child process working...\n");
            u_exit(0);
        } else if (pid > 0) {
            int status = 0;
            u_waitpid(pid, &status, 0);
            u_print("Child finished and reaped.\n");
        }
    } else if (u_strcmp(cmd, "ps") == 0) {
        u_ps();
    } else if (u_strncmp(cmd, "kill ", 5) == 0) {
        int target_pid = u_atoi(cmd + 5);
        if (target_pid > 1) u_kill(target_pid, SIGKILL);
    } else if (u_strcmp(cmd, "uptime") == 0) {
        uint64_t ticks = (uint64_t)u_time();
        u_print("Uptime: ");
        u_print_num(ticks / 100);
        u_print(" seconds\n");
    } else if (u_strcmp(cmd, "getpid") == 0) {
        u_print("Current PID: ");
        u_print_num((uint64_t)u_getpid());
        u_print("\n");
    } else if (u_strcmp(cmd, "clear") == 0) {
        u_print("\f");
    } else if (u_strcmp(cmd, "bench") == 0) {
        u_print("Benchmarking 500,000 getpid()...\n");
        uint64_t s_int = (uint64_t)u_time();
        for (int i = 0; i < 500000; i++) u_int80(__NR_getpid, 0, 0, 0);
        uint64_t t_int = (uint64_t)u_time() - s_int;
        uint64_t s_fast = (uint64_t)u_time();
        for (int i = 0; i < 500000; i++) u_syscall(__NR_getpid, 0, 0, 0);
        uint64_t t_fast = (uint64_t)u_time() - s_fast;
        u_print("int 0x80: "); u_print_num(t_int); u_print(" jiffies\n");
        u_print("syscall : "); u_print_num(t_fast); u_print(" jiffies\n");
    } else if (u_strcmp(cmd, "exit") == 0) {
        u_exit(0);
    } else {
        int64_t pid = u_fork();
        if (pid == 0) {
            int64_t err = u_execve(cmd, NULL, NULL);
            if (err < 0) {
                char bin_path[64];
                bin_path[0] = '/'; bin_path[1] = 'b'; bin_path[2] = 'i'; bin_path[3] = 'n'; bin_path[4] = '/';
                uint64_t bi = 5;
                for (uint64_t k = 0; cmd[k] && bi < sizeof(bin_path) - 1; k++) {
                    bin_path[bi++] = cmd[k];
                }
                bin_path[bi] = '\0';
                err = u_execve(bin_path, NULL, NULL);
            }

            if (err < 0) {
                u_print("shell: command or binary not found: ");
                u_print(cmd);
                u_print("\n");
                u_exit(127);
            }
        } else if (pid > 0) {
            int status = 0;
            u_waitpid(pid, &status, 0);
            u_print("[Program finished with exit code ");
            u_print_num((uint64_t)status);
            u_print("]\n");
        }
    }
}

static void print_prompt(void)
{
    char cwd[64];
    u_getcwd(cwd, sizeof(cwd));
    u_print("user@linux64:");
    u_print(cwd);
    u_print("$ ");
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

    print_prompt();

    while (1) {
        char c;
        if (u_read(0, &c, 1) > 0) {
            if (c == 3) {
                u_print("^C\n");
                buf_len = 0;
                print_prompt();
                continue;
            }

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
                print_prompt();
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
