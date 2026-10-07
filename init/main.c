#include <linux/tty.h>
#include <linux/traps.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/gdt.h>
#include <linux/syscall.h>

extern void enter_user_mode(uint64_t entry_point, uint64_t user_stack);

/* Медленный системный вызов через прерывание int 0x80 */
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

/* Быстрый аппаратный 64-битный системный вызов через инструкцию syscall */
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

/* Обертки для шелла: используем быстрый syscall */
static inline int64_t u_read(int fd, char *buf, uint64_t count)
{
    return u_syscall(__NR_read, fd, (uint64_t)buf, count);
}

static inline int64_t u_write(int fd, const char *buf, uint64_t count)
{
    return u_syscall(__NR_write, fd, (uint64_t)buf, count);
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
    /* fork требует полный кадр trap_frame, поэтому вызывается через int 0x80 */
    return u_int80(__NR_fork, 0, 0, 0);
}

static inline void u_ps(void)
{
    u_syscall(__NR_ps, 0, 0, 0);
}

static inline void u_exit(int status)
{
    u_syscall(__NR_exit, status, 0, 0);
}

/* Строковые функции */
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

static void execute_command(const char *cmd)
{
    if (cmd[0] == '\0') {
        return;
    }

    if (u_strcmp(cmd, "help") == 0) {
        u_print("Linux 0.01 (x86_64) Shell built-in commands:\n");
        u_print("  help    - show this help message\n");
        u_print("  bench   - benchmark 'int 0x80' vs 'syscall'\n");
        u_print("  fork    - test sys_fork() child creation\n");
        u_print("  ps      - show running processes\n");
        u_print("  uptime  - show system uptime\n");
        u_print("  getpid  - show current process ID\n");
        u_print("  clear   - clear the console screen\n");
        u_print("  echo .. - print arguments to console\n");
        u_print("  exit    - terminate this shell process\n");
    } else if (u_strcmp(cmd, "bench") == 0) {
        u_print("Running benchmark: 500,000 getpid() syscalls...\n");

        /* 1. Замер int 0x80 */
        uint64_t start_int = (uint64_t)u_time();
        for (int i = 0; i < 500000; i++) {
            u_int80(__NR_getpid, 0, 0, 0);
        }
        uint64_t time_int = (uint64_t)u_time() - start_int;

        /* 2. Замер аппаратного syscall */
        uint64_t start_fast = (uint64_t)u_time();
        for (int i = 0; i < 50000; i++) {
            u_syscall(__NR_getpid, 0, 0, 0);
        }
        uint64_t time_fast = (uint64_t)u_time() - start_fast;

        u_print("Results:\n  Legacy 'int 0x80' : ");
        u_print_num(time_int);
        u_print(" jiffies\n  Fast   'syscall'  : ");
        u_print_num(time_fast);
        u_print(" jiffies\n");
    } else if (u_strcmp(cmd, "fork") == 0) {
        int64_t pid = u_fork();

        if (pid < 0) {
            u_print("fork: failed to clone process!\n");
        } else if (pid == 0) {
            u_print("\n>>> [CHILD] Process successfully spawned!\n");
            u_print(">>> [CHILD] My PID is: ");
            u_print_num(u_getpid());
            u_print("\n>>> [CHILD] Simulating work for 2 seconds...\n");

            uint64_t start = (uint64_t)u_time();
            while ((uint64_t)u_time() - start < 200) {
                /* Ждем 2 сек */
            }

            u_print(">>> [CHILD] Work finished. Calling sys_exit(0)...\n");
            u_exit(0);
        } else {
            u_print("Parent spawned child with PID = ");
            u_print_num(pid);
            u_print("\n");
        }
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

    task_create(user_trampoline, 10);

    __asm__ volatile ("sti");
    printk("[OK] System Initialized. Launching User Shell...\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
