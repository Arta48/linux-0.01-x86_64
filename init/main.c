#include <linux/tty.h>
#include <linux/traps.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/gdt.h>
#include <linux/syscall.h>
#include <linux/fs.h>
#include <linux/signal.h>
#include <linux/time.h>
#include <linux/utsname.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/multiboot.h>

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

static inline int64_t u_open(const char *path, int flags) { return u_syscall(__NR_open, (uint64_t)path, flags, 0); }
static inline int64_t u_close(int fd) { return u_syscall(__NR_close, fd, 0, 0); }
static inline int64_t u_dup2(int oldfd, int newfd) { return u_syscall(__NR_dup2, oldfd, newfd, 0); }
static inline int64_t u_unlink(const char *path) { return u_syscall(__NR_unlink, (uint64_t)path, 0, 0); }
static inline int64_t u_chmod(const char *path, int mode) { return u_syscall(__NR_chmod, (uint64_t)path, mode, 0); }
static inline int64_t u_stat(const char *path, struct stat *buf) { return u_syscall(__NR_stat, (uint64_t)path, (uint64_t)buf, 0); }
static inline int64_t u_chdir(const char *path) { return u_syscall(__NR_chdir, (uint64_t)path, 0, 0); }
static inline int64_t u_mkdir(const char *path) { return u_syscall(__NR_mkdir, (uint64_t)path, 0, 0); }
static inline int64_t u_rmdir(const char *path) { return u_syscall(__NR_rmdir, (uint64_t)path, 0, 0); }
static inline int64_t u_getcwd(char *buf, uint64_t size) { return u_syscall(__NR_getcwd, (uint64_t)buf, size, 0); }
static inline int64_t u_read(int fd, char *buf, uint64_t count) { return u_syscall(__NR_read, fd, (uint64_t)buf, count); }
static inline int64_t u_write(int fd, const char *buf, uint64_t count) { return u_syscall(__NR_write, fd, (uint64_t)buf, count); }
static inline int64_t u_pipe(int *pipefd) { return u_syscall(__NR_pipe, (uint64_t)pipefd, 0, 0); }
static inline int64_t u_getpid(void) { return u_syscall(__NR_getpid, 0, 0, 0); }
static inline int64_t u_getuid(void) { return u_syscall(__NR_getuid, 0, 0, 0); }
static inline int64_t u_setuid(uint16_t uid, const char *password) { return u_syscall(__NR_setuid, (uint64_t)uid, (uint64_t)password, 0); }
static inline int64_t u_time(void) { return u_syscall(__NR_time, 0, 0, 0); }
static inline int64_t u_fork(void) { return u_int80(__NR_fork, 0, 0, 0); }
static inline int64_t u_execve(const char *path, char **argv, char **envp) { return u_int80(__NR_execve, (uint64_t)path, (uint64_t)argv, (uint64_t)envp); }
static inline void    u_ps(void) { u_syscall(__NR_ps, 0, 0, 0); }
static inline int64_t u_waitpid(int64_t pid, int *stat, int options) { return u_syscall(__NR_waitpid, (uint64_t)pid, (uint64_t)stat, options); }
static inline int64_t u_kill(int64_t pid, int sig) { return u_syscall(__NR_kill, (uint64_t)pid, sig, 0); }
static inline int64_t u_signal(int sig, void (*handler)(int)) { return u_syscall(__NR_signal, sig, (uint64_t)handler, 0); }
static inline int64_t u_uname(struct utsname *name) { return u_syscall(__NR_uname, (uint64_t)name, 0, 0); }
static inline void    u_exit(int status) { u_syscall(__NR_exit, status, 0, 0); }

static inline int64_t u_list(const char *path, char *buf, uint64_t max_len, int is_long)
{
    register uint64_t r10 __asm__("r10") = (uint64_t)is_long;
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(__NR_list), "D"((uint64_t)path), "S"((uint64_t)buf), "d"(max_len), "r"(r10)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static void u_print(const char *s)
{
    uint64_t len = 0;
    while (s[len]) len++;
    u_write(1, s, len);
}

static void u_print_num(uint64_t n)
{
    char buf[32];
    static const char digits[] = "0123456789";
    int i = 0;
    if (n == 0) { u_print("0"); return; }
    while (n > 0) { buf[i++] = digits[n % 10]; n /= 10; }
    while (--i >= 0) u_write(1, &buf[i], 1);
}

static void u_print_signed(int64_t n)
{
    if (n < 0) { u_print("-"); n = -n; }
    u_print_num((uint64_t)n);
}

static void u_print_octal(uint32_t n)
{
    char buf[16];
    static const char digits[] = "01234567";
    int i = 0;
    if (n == 0) { u_print("0"); return; }
    while (n > 0) { buf[i++] = digits[n % 8]; n /= 8; }
    u_print("0");
    while (--i >= 0) u_write(1, &buf[i], 1);
}

static int u_strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static int u_strncmp(const char *s1, const char *s2, uint64_t n)
{
    while (n && *s1 && (*s1 == *s2)) { s1++; s2++; n--; }
    if (n == 0) return 0;
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static const char *u_strstr(const char *haystack, const char *needle)
{
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        if (*haystack == *needle) {
            const char *h = haystack, *n = needle;
            while (*h && *n && *h == *n) { h++; n++; }
            if (!*n) return haystack;
        }
    }
    return NULL;
}

static int64_t u_atoi(const char *s)
{
    int64_t res = 0;
    int sign = 1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    while (*s >= '0' && *s <= '9') {
        res = res * 10 + (*s - '0');
        s++;
    }
    return res * sign;
}

static void strip_quotes(char *s)
{
    uint64_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
    while (*s == ' ' || *s == '\t') {
        for (uint64_t i = 0; i < len; i++) s[i] = s[i + 1];
        len--;
    }
    if (len >= 2 && ((s[0] == '"' && s[len - 1] == '"') || (s[0] == '\'' && s[len - 1] == '\''))) {
        for (uint64_t i = 0; i < len - 2; i++) s[i] = s[i + 1];
        s[len - 2] = '\0';
    }
}

static const char *day_names[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *mon_names[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const int days_in_month[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static void print_date(uint64_t epoch)
{
    uint64_t sec = epoch % 60;
    uint64_t min = (epoch / 60) % 60;
    uint64_t hour = (epoch / 3600) % 24;
    uint64_t days = epoch / 86400;

    int wday = (days + 4) % 7;
    int year = 1970;
    while (1) {
        int leap = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
        int ydays = leap ? 366 : 365;
        if (days >= (uint64_t)ydays) { days -= ydays; year++; }
        else break;
    }

    int leap = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
    int mon = 0;
    while (mon < 12) {
        int mdays = days_in_month[mon];
        if (mon == 1 && leap) mdays = 29;
        if (days >= (uint64_t)mdays) { days -= mdays; mon++; }
        else break;
    }
    int mday = days + 1;

    u_print(day_names[wday]); u_print(" ");
    u_print(mon_names[mon]); u_print(" ");
    if (mday < 10) u_print(" ");
    u_print_num(mday); u_print(" ");
    if (hour < 10) u_print("0");
    u_print_num(hour); u_print(":");
    if (min < 10) u_print("0");
    u_print_num(min); u_print(":");
    if (sec < 10) u_print("0");
    u_print_num(sec);
    u_print(" UTC ");
    u_print_num(year);
    u_print("\n");
}

#define MAX_ENV 32
struct env_var {
    char key[32];
    char val[96];
    int in_use;
};
static struct env_var env_vars[MAX_ENV];
static int last_exit_code = 0;

static void env_set(const char *key, const char *val)
{
    for (int i = 0; i < MAX_ENV; i++) {
        if (env_vars[i].in_use && u_strcmp(env_vars[i].key, key) == 0) {
            int j = 0;
            while (val[j] && j < 95) { env_vars[i].val[j] = val[j]; j++; }
            env_vars[i].val[j] = '\0';
            return;
        }
    }
    for (int i = 0; i < MAX_ENV; i++) {
        if (!env_vars[i].in_use) {
            int j = 0;
            while (key[j] && j < 31) { env_vars[i].key[j] = key[j]; j++; }
            env_vars[i].key[j] = '\0';
            j = 0;
            while (val[j] && j < 95) { env_vars[i].val[j] = val[j]; j++; }
            env_vars[i].val[j] = '\0';
            env_vars[i].in_use = 1;
            return;
        }
    }
}

static void env_init(void)
{
    for (int i = 0; i < MAX_ENV; i++) env_vars[i].in_use = 0;
    env_set("USER", "root");
    env_set("PATH", "/bin");
}

static const char *env_get(const char *key)
{
    for (int i = 0; i < MAX_ENV; i++) {
        if (env_vars[i].in_use && u_strcmp(env_vars[i].key, key) == 0) return env_vars[i].val;
    }
    return NULL;
}

static void env_unset(const char *key)
{
    for (int i = 0; i < MAX_ENV; i++) {
        if (env_vars[i].in_use && u_strcmp(env_vars[i].key, key) == 0) { env_vars[i].in_use = 0; return; }
    }
}

#define MAX_JOBS 8
struct bg_job {
    int64_t pid;
    char cmd[32];
    int in_use;
};
static struct bg_job bg_jobs[MAX_JOBS];

static void add_bg_job(int64_t pid, const char *cmd)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!bg_jobs[i].in_use) {
            bg_jobs[i].pid = pid;
            int j = 0;
            while (cmd[j] && j < 31) { bg_jobs[i].cmd[j] = cmd[j]; j++; }
            bg_jobs[i].cmd[j] = '\0';
            bg_jobs[i].in_use = 1;
            u_print("["); u_print_num(i + 1); u_print("] PID ");
            u_print_num((uint64_t)pid); u_print(" (background)\n");
            return;
        }
    }
}

static void check_bg_jobs(void)
{
    int status;
    int64_t reaped;
    while ((reaped = u_waitpid(-1, &status, 1)) > 0) {
        for (int i = 0; i < MAX_JOBS; i++) {
            if (bg_jobs[i].in_use && bg_jobs[i].pid == reaped) {
                u_print("\n["); u_print_num(i + 1); u_print("]+ Done: ");
                u_print(bg_jobs[i].cmd);
                u_print(" (exit code "); u_print_num((uint64_t)status); u_print(")\n");
                bg_jobs[i].in_use = 0;
                break;
            }
        }
    }
}

static int is_var_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c == '_');
}

static void expand_vars(const char *in, char *out, uint64_t max_len)
{
    uint64_t oi = 0;
    while (*in && oi < max_len - 1) {
        if (*in == '$') {
            in++;
            if (*in == '?') {
                char nb[16]; int ni = 0; int v = last_exit_code;
                if (v == 0) nb[ni++] = '0';
                while (v > 0) { nb[ni++] = '0' + (v % 10); v /= 10; }
                while (--ni >= 0 && oi < max_len - 1) out[oi++] = nb[ni];
                in++;
                continue;
            }
            if (*in == '#') {
                const char *val = env_get("#");
                if (val) {
                    while (*val && oi < max_len - 1) out[oi++] = *val++;
                } else {
                    out[oi++] = '0';
                }
                in++;
                continue;
            }

            int braced = 0;
            if (*in == '{') { braced = 1; in++; }

            char var_name[32]; int vi = 0;
            while (*in && vi < 31) {
                if (braced) {
                    if (*in == '}') { in++; break; }
                } else {
                    if (!is_var_char(*in)) break;
                }
                var_name[vi++] = *in++;
            }
            var_name[vi] = '\0';

            const char *val = NULL;
            char cwd_buf[64];
            if (u_strcmp(var_name, "PWD") == 0) {
                u_getcwd(cwd_buf, sizeof(cwd_buf));
                val = cwd_buf;
            } else if (u_strcmp(var_name, "PID") == 0) {
                val = "1";
            } else {
                val = env_get(var_name);
            }

            if (val) {
                while (*val && oi < max_len - 1) out[oi++] = *val++;
            }
        } else {
            out[oi++] = *in++;
        }
    }
    out[oi] = '\0';
}

#define HISTORY_MAX 8
static char history[HISTORY_MAX][128];
static int history_count = 0;
static int history_idx = 0;

static void history_add(const char *cmd)
{
    if (cmd[0] == '\0') return;
    int slot = history_count % HISTORY_MAX;
    uint64_t i = 0;
    while (cmd[i] && i < 127) { history[slot][i] = cmd[i]; i++; }
    history[slot][i] = '\0';
    history_count++;
    history_idx = history_count;
}

static volatile int sigint_received = 0;
static void user_sigint_handler(int sig)
{
    (void)sig;
    u_print("\n>>> [USER SIGNAL HANDLER] Caught SIGINT in Ring 3!\n");
    sigint_received = 1;
}

#define MAX_ED_LINES 64
#define MAX_ED_LEN   128
static char ed_lines[MAX_ED_LINES][MAX_ED_LEN];
static int ed_line_count = 0;

static void run_editor(const char *filename)
{
    char fname[64];
    uint64_t fni = 0;
    while (filename[fni] && fni < 63) { fname[fni] = filename[fni]; fni++; }
    fname[fni] = '\0';
    strip_quotes(fname);

    ed_line_count = 0;
    int64_t fd = u_open(fname, O_RDONLY);
    if (fd >= 0) {
        char c; int ci = 0;
        while (u_read((int)fd, &c, 1) > 0) {
            if (c == '\n' || c == '\r') {
                ed_lines[ed_line_count][ci] = '\0';
                ed_line_count++;
                ci = 0;
                if (ed_line_count >= MAX_ED_LINES) break;
            } else if (ci < MAX_ED_LEN - 1) {
                ed_lines[ed_line_count][ci++] = c;
            }
        }
        if (ci > 0 && ed_line_count < MAX_ED_LINES) {
            ed_lines[ed_line_count][ci] = '\0';
            ed_line_count++;
        }
        u_close((int)fd);
        u_print("--- Opened "); u_print(fname); u_print(" ("); u_print_num(ed_line_count); u_print(" lines) ---\n");
    } else {
        u_print("--- New file: "); u_print(fname); u_print(" ---\n");
    }

    u_print("Commands: 'p' (print), 'a <txt>' (append), 'i <num> <txt>' (insert), 'd <num>' (delete), 'w' (save), 'q' (quit)\n");

    char cmd[128];
    while (1) {
        u_print("edit> ");
        int ci = 0; char c;
        while (u_read(0, &c, 1) > 0) {
            if (c == '\b') {
                if (ci > 0) { ci--; u_write(1, "\b", 1); }
            } else if (c == '\n') {
                u_write(1, "\n", 1); cmd[ci] = '\0'; break;
            } else if (c >= 32 && c <= 126 && ci < 127) {
                cmd[ci++] = c; u_write(1, &c, 1);
            }
        }

        if (cmd[0] == 'q' && cmd[1] == '\0') {
            break;
        } else if (cmd[0] == 'p' && cmd[1] == '\0') {
            for (int i = 0; i < ed_line_count; i++) {
                u_print_num(i + 1); u_print(": "); u_print(ed_lines[i]); u_print("\n");
            }
        } else if (cmd[0] == 'a' && cmd[1] == ' ') {
            if (ed_line_count < MAX_ED_LINES) {
                const char *txt = cmd + 2; int k = 0;
                while (txt[k] && k < MAX_ED_LEN - 1) { ed_lines[ed_line_count][k] = txt[k]; k++; }
                ed_lines[ed_line_count][k] = '\0';
                ed_line_count++;
            } else {
                u_print("editor: buffer full!\n");
            }
        } else if (cmd[0] == 'i' && cmd[1] == ' ') {
            const char *p = cmd + 2;
            int line_num = (int)u_atoi(p);
            while (*p && *p != ' ') p++;
            while (*p == ' ') p++;
            if (line_num >= 1 && line_num <= ed_line_count + 1 && ed_line_count < MAX_ED_LINES) {
                int idx = line_num - 1;
                for (int k = ed_line_count; k > idx; k--) {
                    memcpy(ed_lines[k], ed_lines[k - 1], MAX_ED_LEN);
                }
                int k = 0;
                while (p[k] && k < MAX_ED_LEN - 1) { ed_lines[idx][k] = p[k]; k++; }
                ed_lines[idx][k] = '\0';
                ed_line_count++;
            } else {
                u_print("editor: invalid line number\n");
            }
        } else if (cmd[0] == 'd' && cmd[1] == ' ') {
            int line_num = (int)u_atoi(cmd + 2);
            if (line_num >= 1 && line_num <= ed_line_count) {
                int idx = line_num - 1;
                for (int k = idx; k < ed_line_count - 1; k++) {
                    memcpy(ed_lines[k], ed_lines[k + 1], MAX_ED_LEN);
                }
                ed_line_count--;
            } else {
                u_print("editor: invalid line number\n");
            }
        } else if (cmd[0] == 'w' && cmd[1] == '\0') {
            int64_t wfd = u_open(fname, O_CREAT | O_WRONLY | O_TRUNC);
            if (wfd < 0) {
                u_print("editor: cannot save file!\n");
            } else {
                uint64_t total = 0;
                for (int i = 0; i < ed_line_count; i++) {
                    uint64_t len = strlen(ed_lines[i]);
                    u_write((int)wfd, ed_lines[i], len);
                    u_write((int)wfd, "\n", 1);
                    total += len + 1;
                }
                u_close((int)wfd);
                u_print("Saved "); u_print_num(ed_line_count); u_print(" lines (");
                u_print_num(total); u_print(" bytes) to "); u_print(fname); u_print(".\n");
            }
        }
    }
}

static int do_test(int argc, char **argv)
{
    if (argc > 1 && u_strcmp(argv[argc - 1], "]") == 0) argc--;

    int t_argc = argc - 1;
    char **t_argv = argv + 1;

    if (t_argc <= 0) return 1;
    if (t_argc == 1) return (t_argv[0][0] != '\0') ? 0 : 1;

    if (t_argc == 2) {
        if (u_strcmp(t_argv[0], "-z") == 0) return (t_argv[1][0] == '\0') ? 0 : 1;
        if (u_strcmp(t_argv[0], "-n") == 0) return (t_argv[1][0] != '\0') ? 0 : 1;
        if (u_strcmp(t_argv[0], "-f") == 0) {
            struct stat st;
            return (u_stat(t_argv[1], &st) == 0 && S_ISREG(st.st_mode)) ? 0 : 1;
        }
        if (u_strcmp(t_argv[0], "-d") == 0) {
            struct stat st;
            return (u_stat(t_argv[1], &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : 1;
        }
        if (u_strcmp(t_argv[0], "-e") == 0) {
            struct stat st;
            return (u_stat(t_argv[1], &st) == 0) ? 0 : 1;
        }
        if (u_strcmp(t_argv[0], "-s") == 0) {
            struct stat st;
            return (u_stat(t_argv[1], &st) == 0 && st.st_size > 0) ? 0 : 1;
        }
        return 1;
    }

    if (t_argc == 3) {
        const char *a1 = t_argv[0];
        const char *op = t_argv[1];
        const char *a2 = t_argv[2];

        if (u_strcmp(op, "=") == 0 || u_strcmp(op, "==") == 0) return (u_strcmp(a1, a2) == 0) ? 0 : 1;
        if (u_strcmp(op, "!=") == 0) return (u_strcmp(a1, a2) != 0) ? 0 : 1;
        if (u_strcmp(op, "-eq") == 0) return (u_atoi(a1) == u_atoi(a2)) ? 0 : 1;
        if (u_strcmp(op, "-ne") == 0) return (u_atoi(a1) != u_atoi(a2)) ? 0 : 1;
        if (u_strcmp(op, "-lt") == 0) return (u_atoi(a1) < u_atoi(a2)) ? 0 : 1;
        if (u_strcmp(op, "-gt") == 0) return (u_atoi(a1) > u_atoi(a2)) ? 0 : 1;
        if (u_strcmp(op, "-le") == 0) return (u_atoi(a1) <= u_atoi(a2)) ? 0 : 1;
        if (u_strcmp(op, "-ge") == 0) return (u_atoi(a1) >= u_atoi(a2)) ? 0 : 1;
    }

    return 1;
}

static void execute_command(const char *cmd);

#define MAX_SCRIPT_DEPTH    4
#define MAX_SCRIPT_LINES    128
#define MAX_SCRIPT_LINE_LEN 160
static char script_lines[MAX_SCRIPT_DEPTH][MAX_SCRIPT_LINES][MAX_SCRIPT_LINE_LEN];
static int script_depth = 0;

#define MAX_LOOP_STACK 16
struct loop_entry {
    int type; /* 1 = WHILE, 2 = FOR */
    int cond_pc;
    int body_pc;
    int done_pc;
    char for_var[32];
    char for_words[16][64];
    int for_count;
    int for_idx;
};

/* Статический массив состояний циклов: 0 байт на стеке! */
static struct loop_entry script_loops[MAX_SCRIPT_DEPTH][MAX_LOOP_STACK];

/* Вспомогательная функция добавления строки с автоматическим разделением "do <cmd>" и "then <cmd>" */
static void add_script_line(int d_idx, int *line_count, const char *raw_line)
{
    if (*line_count >= MAX_SCRIPT_LINES) return;

    char buf[MAX_SCRIPT_LINE_LEN];
    uint64_t bi = 0;
    const char *p = raw_line;
    while (*p == ' ' || *p == '\t') p++;

    while (*p && bi < MAX_SCRIPT_LINE_LEN - 1) {
        buf[bi++] = *p++;
    }
    while (bi > 0 && (buf[bi - 1] == ' ' || buf[bi - 1] == '\t' || buf[bi - 1] == '\r')) bi--;
    buf[bi] = '\0';
    strip_quotes(buf);

    if (buf[0] == '\0') return;

    if (u_strncmp(buf, "do ", 3) == 0) {
        memcpy(script_lines[d_idx][*line_count], "do", 3);
        (*line_count)++;
        if (*line_count < MAX_SCRIPT_LINES) {
            const char *rest = buf + 3;
            while (*rest == ' ') rest++;
            if (*rest != '\0') {
                memcpy(script_lines[d_idx][*line_count], rest, strlen(rest) + 1);
                (*line_count)++;
            }
        }
        return;
    }

    if (u_strncmp(buf, "then ", 5) == 0) {
        memcpy(script_lines[d_idx][*line_count], "then", 5);
        (*line_count)++;
        if (*line_count < MAX_SCRIPT_LINES) {
            const char *rest = buf + 5;
            while (*rest == ' ') rest++;
            if (*rest != '\0') {
                memcpy(script_lines[d_idx][*line_count], rest, strlen(rest) + 1);
                (*line_count)++;
            }
        }
        return;
    }

    memcpy(script_lines[d_idx][*line_count], buf, strlen(buf) + 1);
    (*line_count)++;
}

static int find_matching_fi(int depth_idx, int total_lines, int start_pc, int *else_pc)
{
    if (else_pc) *else_pc = -1;
    int depth = 1;
    for (int i = start_pc + 1; i < total_lines; i++) {
        const char *l = script_lines[depth_idx][i];
        while (*l == ' ' || *l == '\t') l++;

        if (u_strncmp(l, "if ", 3) == 0 || u_strncmp(l, "if[", 3) == 0) {
            depth++;
        } else if (depth == 1 && else_pc && *else_pc == -1 && (u_strcmp(l, "else") == 0 || u_strncmp(l, "else ", 5) == 0)) {
            *else_pc = i;
        } else if (u_strcmp(l, "fi") == 0 || u_strncmp(l, "fi ", 3) == 0) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
}

static int find_matching_fi_from_else(int depth_idx, int total_lines, int else_pc)
{
    int depth = 1;
    for (int i = else_pc + 1; i < total_lines; i++) {
        const char *l = script_lines[depth_idx][i];
        while (*l == ' ' || *l == '\t') l++;

        if (u_strncmp(l, "if ", 3) == 0 || u_strncmp(l, "if[", 3) == 0) {
            depth++;
        } else if (u_strcmp(l, "fi") == 0 || u_strncmp(l, "fi ", 3) == 0) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return total_lines - 1;
}

static int find_matching_done(int depth_idx, int total_lines, int start_pc)
{
    int depth = 1;
    for (int i = start_pc + 1; i < total_lines; i++) {
        const char *l = script_lines[depth_idx][i];
        while (*l == ' ' || *l == '\t') l++;

        if (u_strncmp(l, "while ", 6) == 0 || u_strncmp(l, "for ", 4) == 0) {
            depth++;
        } else if (u_strcmp(l, "done") == 0 || u_strncmp(l, "done ", 5) == 0) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
}

static void run_script_engine(int depth_idx, int total_lines)
{
    struct loop_entry *loops = script_loops[depth_idx];
    int l_depth = 0;
    int pc = 0;

    while (pc < total_lines) {
        char clean[MAX_SCRIPT_LINE_LEN];
        uint64_t ci = 0;
        const char *src = script_lines[depth_idx][pc];
        while (*src == ' ' || *src == '\t') src++;

        while (*src && ci < sizeof(clean) - 1) clean[ci++] = *src++;
        while (ci > 0 && (clean[ci - 1] == ' ' || clean[ci - 1] == '\t' || clean[ci - 1] == '\r')) ci--;
        clean[ci] = '\0';

        if (clean[0] == '\0' || clean[0] == '#') {
            pc++;
            continue;
        }

        if (u_strncmp(clean, "if ", 3) == 0 || u_strncmp(clean, "if[", 3) == 0) {
            int else_idx = -1;
            int fi_idx = find_matching_fi(depth_idx, total_lines, pc, &else_idx);
            if (fi_idx == -1) {
                u_print("sh: syntax error: missing fi\n");
                last_exit_code = 1;
                return;
            }

            const char *cond_cmd = clean + (clean[2] == '[' ? 2 : 3);
            while (*cond_cmd == ' ') cond_cmd++;

            char cond_buf[MAX_SCRIPT_LINE_LEN];
            uint64_t cbi = 0;
            while (cond_cmd[cbi] && cbi < sizeof(cond_buf) - 1) { cond_buf[cbi] = cond_cmd[cbi]; cbi++; }
            cond_buf[cbi] = '\0';

            char *semi = cond_buf;
            while (*semi) {
                if (*semi == ';' && *(semi + 1) == ' ' && *(semi + 2) == 't' &&
                    *(semi + 3) == 'h' && *(semi + 4) == 'e' && *(semi + 5) == 'n') {
                    *semi = '\0';
                break;
                    }
                    semi++;
            }

            char exp_cond[MAX_SCRIPT_LINE_LEN];
            expand_vars(cond_buf, exp_cond, sizeof(exp_cond));
            execute_command(exp_cond);

            int cond_res = (last_exit_code == 0);
            int body_start = pc + 1;
            if (body_start < total_lines) {
                const char *next_l = script_lines[depth_idx][body_start];
                while (*next_l == ' ') next_l++;
                if (u_strcmp(next_l, "then") == 0) body_start++;
            }

            if (cond_res) {
                pc = body_start;
            } else {
                pc = (else_idx != -1) ? (else_idx + 1) : (fi_idx + 1);
            }
            continue;
        }

        if (u_strcmp(clean, "then") == 0) { pc++; continue; }
        if (u_strcmp(clean, "else") == 0 || u_strncmp(clean, "else ", 5) == 0) {
            pc = find_matching_fi_from_else(depth_idx, total_lines, pc) + 1;
            continue;
        }
        if (u_strcmp(clean, "fi") == 0) { pc++; continue; }

        if (u_strncmp(clean, "while ", 6) == 0) {
            int done_idx = find_matching_done(depth_idx, total_lines, pc);
            if (done_idx == -1) {
                u_print("sh: syntax error: missing done for while\n");
                last_exit_code = 1;
                return;
            }

            int body_start = pc + 1;
            if (body_start < total_lines) {
                const char *next_l = script_lines[depth_idx][body_start];
                while (*next_l == ' ') next_l++;
                if (u_strcmp(next_l, "do") == 0) body_start++;
            }

            const char *cond_cmd = clean + 6;
            while (*cond_cmd == ' ') cond_cmd++;

            char cond_buf[MAX_SCRIPT_LINE_LEN];
            uint64_t cbi = 0;
            while (cond_cmd[cbi] && cbi < sizeof(cond_buf) - 1) { cond_buf[cbi] = cond_cmd[cbi]; cbi++; }
            cond_buf[cbi] = '\0';

            char *semi = cond_buf;
            while (*semi) {
                if (*semi == ';' && *(semi + 1) == ' ' && *(semi + 2) == 'd' && *(semi + 3) == 'o') {
                    *semi = '\0';
                    break;
                }
                semi++;
            }

            char exp_cond[MAX_SCRIPT_LINE_LEN];
            expand_vars(cond_buf, exp_cond, sizeof(exp_cond));
            execute_command(exp_cond);

            if (last_exit_code == 0) {
                if (l_depth == 0 || loops[l_depth - 1].cond_pc != pc) {
                    if (l_depth < MAX_LOOP_STACK) {
                        loops[l_depth].type = 1;
                        loops[l_depth].cond_pc = pc;
                        loops[l_depth].body_pc = body_start;
                        loops[l_depth].done_pc = done_idx;
                        l_depth++;
                    }
                }
                pc = body_start;
            } else {
                if (l_depth > 0 && loops[l_depth - 1].cond_pc == pc) l_depth--;
                pc = done_idx + 1;
            }
            continue;
        }

        if (u_strncmp(clean, "for ", 4) == 0) {
            int done_idx = find_matching_done(depth_idx, total_lines, pc);
            if (done_idx == -1) {
                u_print("sh: syntax error: missing done for for\n");
                last_exit_code = 1;
                return;
            }

            int body_start = pc + 1;
            if (body_start < total_lines) {
                const char *next_l = script_lines[depth_idx][body_start];
                while (*next_l == ' ') next_l++;
                if (u_strcmp(next_l, "do") == 0) body_start++;
            }

            if (l_depth > 0 && loops[l_depth - 1].cond_pc == pc) {
                loops[l_depth - 1].for_idx++;
                if (loops[l_depth - 1].for_idx < loops[l_depth - 1].for_count) {
                    env_set(loops[l_depth - 1].for_var, loops[l_depth - 1].for_words[loops[l_depth - 1].for_idx]);
                    pc = body_start;
                } else {
                    l_depth--;
                    pc = done_idx + 1;
                }
                continue;
            }

            const char *p = clean + 4;
            while (*p == ' ') p++;
            char var_name[32]; int vi = 0;
            while (*p && *p != ' ' && vi < 31) var_name[vi++] = *p++;
            var_name[vi] = '\0';

            while (*p == ' ') p++;
            if (u_strncmp(p, "in", 2) == 0 && (p[2] == ' ' || p[2] == '\0')) p += 2;

            char exp_words[MAX_SCRIPT_LINE_LEN];
            expand_vars(p, exp_words, sizeof(exp_words));

            char *semi = exp_words;
            while (*semi) {
                if (*semi == ';' && *(semi + 1) == ' ' && *(semi + 2) == 'd' && *(semi + 3) == 'o') {
                    *semi = '\0';
                    break;
                }
                semi++;
            }

            if (l_depth < MAX_LOOP_STACK) {
                loops[l_depth].type = 2;
                loops[l_depth].cond_pc = pc;
                loops[l_depth].body_pc = body_start;
                loops[l_depth].done_pc = done_idx;
                memcpy(loops[l_depth].for_var, var_name, strlen(var_name) + 1);

                int wcount = 0;
                const char *wp = exp_words;
                while (*wp) {
                    while (*wp == ' ') wp++;
                    if (*wp == '\0') break;

                    int wi = 0;
                    while (*wp && *wp != ' ' && wi < 63) loops[l_depth].for_words[wcount][wi++] = *wp++;
                    loops[l_depth].for_words[wcount][wi] = '\0';
                    strip_quotes(loops[l_depth].for_words[wcount]);
                    wcount++;
                    if (wcount >= 16) break;
                }
                loops[l_depth].for_count = wcount;
                loops[l_depth].for_idx = 0;

                if (wcount > 0) {
                    env_set(var_name, loops[l_depth].for_words[0]);
                    l_depth++;
                    pc = body_start;
                } else {
                    pc = done_idx + 1;
                }
            } else {
                pc = done_idx + 1;
            }
            continue;
        }

        if (u_strcmp(clean, "do") == 0) { pc++; continue; }
        if (u_strcmp(clean, "done") == 0) {
            pc = (l_depth > 0) ? loops[l_depth - 1].cond_pc : (pc + 1);
            continue;
        }
        if (u_strcmp(clean, "break") == 0) {
            if (l_depth > 0) { int d_idx = loops[l_depth - 1].done_pc; l_depth--; pc = d_idx + 1; }
            else pc++;
            continue;
        }
        if (u_strcmp(clean, "continue") == 0) {
            pc = (l_depth > 0) ? loops[l_depth - 1].cond_pc : (pc + 1);
            continue;
        }

        char exp_line[256];
        expand_vars(clean, exp_line, sizeof(exp_line));
        execute_command(exp_line);
        pc++;
    }
}

static void execute_block(const char *block_str)
{
    if (script_depth >= MAX_SCRIPT_DEPTH) {
        u_print("sh: max script execution depth exceeded\n");
        return;
    }

    int d_idx = script_depth;
    script_depth++;
    int line_count = 0;
    const char *p = block_str;

    while (*p && line_count < MAX_SCRIPT_LINES) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ';') p++;
        if (*p == '\0') break;

        char buf[MAX_SCRIPT_LINE_LEN]; int bi = 0;
        while (*p && *p != '\n' && *p != ';' && bi < MAX_SCRIPT_LINE_LEN - 1) buf[bi++] = *p++;
        buf[bi] = '\0';
        strip_quotes(buf);

        if (buf[0] != '\0') {
            add_script_line(d_idx, &line_count, buf);
        }
    }

    run_script_engine(d_idx, line_count);
    script_depth--;
}

static void execute_pipeline(const char *cmd, const char *pipe_pos)
{
    char left[64]; char right[64];
    uint64_t l_len = pipe_pos - cmd;
    if (l_len >= sizeof(left)) l_len = sizeof(left) - 1;
    for (uint64_t i = 0; i < l_len; i++) left[i] = cmd[i];
    while (l_len > 0 && (left[l_len - 1] == ' ' || left[l_len - 1] == '\t')) l_len--;
    left[l_len] = '\0';

    const char *r_ptr = pipe_pos + 1;
    while (*r_ptr == ' ' || *r_ptr == '\t') r_ptr++;
    uint64_t r_len = 0;
    while (r_ptr[r_len] && r_len < sizeof(right) - 1) { right[r_len] = r_ptr[r_len]; r_len++; }
    right[r_len] = '\0';

    int p[2];
    if (u_pipe(p) < 0) return;

    int64_t pid1 = u_fork();
    if (pid1 == 0) {
        u_dup2(p[1], 1);
        u_close(p[0]); u_close(p[1]);
        execute_command(left);
        u_exit(0);
    }

    int64_t pid2 = u_fork();
    if (pid2 == 0) {
        u_dup2(p[0], 0);
        u_close(p[0]); u_close(p[1]);
        execute_command(right);
        u_exit(0);
    }

    u_close(p[0]); u_close(p[1]);
    int status;
    u_waitpid(pid1, &status, 0);
    u_waitpid(pid2, &status, 0);
    last_exit_code = status;
}

static void execute_redirection(const char *cmd, const char *redir_pos, int append)
{
    char left[64]; char fname[MAX_FILENAME];
    uint64_t l_len = redir_pos - cmd;
    if (l_len >= sizeof(left)) l_len = sizeof(left) - 1;
    for (uint64_t i = 0; i < l_len; i++) left[i] = cmd[i];
    while (l_len > 0 && (left[l_len - 1] == ' ' || left[l_len - 1] == '\t')) l_len--;
    left[l_len] = '\0';

    const char *r_ptr = redir_pos + (append ? 2 : 1);
    while (*r_ptr == ' ' || *r_ptr == '\t') r_ptr++;
    uint64_t r_len = 0;
    while (r_ptr[r_len] && r_ptr[r_len] != ' ' && r_ptr[r_len] != '\t' && r_len < MAX_FILENAME - 1) {
        fname[r_len] = r_ptr[r_len];
        r_len++;
    }
    fname[r_len] = '\0';
    strip_quotes(fname);

    int64_t pid = u_fork();
    if (pid == 0) {
        int flags = O_CREAT | O_WRONLY | (append ? O_APPEND : O_TRUNC);
        int64_t fd = u_open(fname, flags);
        if (fd < 0) { u_print("shell: cannot open file\n"); u_exit(1); }
        u_dup2((int)fd, 1);
        u_close((int)fd);
        execute_command(left);
        u_exit(0);
    } else if (pid > 0) {
        int status;
        u_waitpid(pid, &status, 0);
        last_exit_code = status;
    }
}

static void do_grep(const char *pattern, int fd)
{
    char line[128]; int li = 0; char c;
    while (u_read(fd, &c, 1) > 0) {
        if (c == '\n') {
            line[li] = '\0';
            if (u_strstr(line, pattern) != NULL) { u_print(line); u_print("\n"); }
            li = 0;
        } else {
            if (li < (int)sizeof(line) - 1) line[li++] = c;
        }
    }
    if (li > 0) {
        line[li] = '\0';
        if (u_strstr(line, pattern) != NULL) { u_print(line); u_print("\n"); }
    }
}

static void do_wc(int fd, int only_lines)
{
    uint64_t lines = 0, words = 0, bytes = 0; int in_word = 0; char c;
    while (u_read(fd, &c, 1) > 0) {
        bytes++;
        if (c == '\n') lines++;
        if (c == ' ' || c == '\t' || c == '\n') in_word = 0;
        else if (!in_word) { in_word = 1; words++; }
    }
    if (only_lines) {
        u_print_num(lines); u_print("\n");
    } else {
        u_print_num(lines); u_print(" ");
        u_print_num(words); u_print(" ");
        u_print_num(bytes); u_print("\n");
    }
}

static void read_password(char *out, int max_len)
{
    int pi = 0; char c;
    while (u_read(0, &c, 1) > 0) {
        if (c == '\n') break;
        if (c == '\b') { if (pi > 0) pi--; }
        else if (c >= 32 && c <= 126 && pi < max_len - 1) out[pi++] = c;
    }
    out[pi] = '\0';
    u_print("\n");
}

static void execute_script_args(const char *cmd_line)
{
    char fname[64]; int fi = 0;
    const char *p = cmd_line;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && fi < 63) fname[fi++] = *p++;
    fname[fi] = '\0';
    strip_quotes(fname);

    int argc = 0;
    char arg_keys[9][4] = { "1", "2", "3", "4", "5", "6", "7", "8", "9" };

    while (*p) {
        while (*p == ' ') p++;
        if (*p == '\0') break;

        char arg_val[64]; int ai = 0;
        while (*p && *p != ' ' && ai < 63) arg_val[ai++] = *p++;
        arg_val[ai] = '\0';
        strip_quotes(arg_val);

        if (argc < 9) { env_set(arg_keys[argc], arg_val); argc++; }
    }

    char argc_str[4];
    argc_str[0] = '0' + argc;
    argc_str[1] = '\0';
    env_set("#", argc_str);

    int64_t fd = u_open(fname, O_RDONLY);
    if (fd < 0) {
        u_print("sh: cannot open script: "); u_print(fname); u_print("\n");
        return;
    }

    if (script_depth >= MAX_SCRIPT_DEPTH) {
        u_print("sh: max script execution depth exceeded\n");
        u_close((int)fd);
        return;
    }

    int d_idx = script_depth;
    script_depth++;
    int line_count = 0;
    char line[MAX_SCRIPT_LINE_LEN]; int li = 0; char c;

    while (u_read((int)fd, &c, 1) > 0 && line_count < MAX_SCRIPT_LINES) {
        if (c == '\n' || c == '\r') {
            line[li] = '\0';
            strip_quotes(line);

            const char *lp = line;
            while (*lp == ' ' || *lp == '\t') lp++;

            if (*lp != '\0') {
                char clean_line[MAX_SCRIPT_LINE_LEN]; int ci = 0;
                while (*lp && ci < MAX_SCRIPT_LINE_LEN - 1) {
                    if (*lp == '#' && (ci == 0 || clean_line[ci - 1] == ' ' || clean_line[ci - 1] == '\t')) break;
                    clean_line[ci++] = *lp++;
                }
                while (ci > 0 && (clean_line[ci - 1] == ' ' || clean_line[ci - 1] == '\t')) ci--;
                clean_line[ci] = '\0';

                if (clean_line[0] != '\0') {
                    add_script_line(d_idx, &line_count, clean_line);
                }
            }
            li = 0;
        } else {
            if (li < MAX_SCRIPT_LINE_LEN - 1) line[li++] = c;
        }
    }
    u_close((int)fd);

    run_script_engine(d_idx, line_count);
    script_depth--;

    for (int k = 0; k < argc; k++) env_unset(arg_keys[k]);
    env_unset("#");
}

static void execute_command(const char *cmd)
{
    if (cmd[0] == '\0') return;

    int is_bg = 0;
    char clean_cmd[128];
    uint64_t clen = 0;
    while (cmd[clen] && clen < 127) { clean_cmd[clen] = cmd[clen]; clen++; }
    clean_cmd[clen] = '\0';

    while (clen > 0 && (clean_cmd[clen - 1] == ' ' || clean_cmd[clen - 1] == '\t')) clen--;
    if (clen > 0 && clean_cmd[clen - 1] == '&') {
        is_bg = 1;
        clen--;
        while (clen > 0 && (clean_cmd[clen - 1] == ' ' || clean_cmd[clen - 1] == '\t')) clen--;
        clean_cmd[clen] = '\0';
    }

    strip_quotes(clean_cmd);
    const char *cm = clean_cmd;
    while (*cm == ' ' || *cm == '\t') cm++;
    if (*cm == '#' || *cm == '\0') return;

    const char *exec_cmd = clean_cmd;

    /* 1. Прямое присваивание переменной: VAR=value */
    const char *eq = exec_cmd;
    if (is_var_char(*eq) && !(*eq >= '0' && *eq <= '9')) {
        while (is_var_char(*eq)) eq++;
        if (*eq == '=') {
            char var_name[32];
            int vlen = eq - exec_cmd;
            if (vlen >= 31) vlen = 31;
            for (int k = 0; k < vlen; k++) var_name[k] = exec_cmd[k];
            var_name[vlen] = '\0';

            const char *val_ptr = eq + 1;
            char var_val[96];
            uint64_t vi = 0;
            while (val_ptr[vi] && vi < sizeof(var_val) - 1) {
                var_val[vi] = val_ptr[vi];
                vi++;
            }
            var_val[vi] = '\0';
            strip_quotes(var_val);

            env_set(var_name, var_val);
            last_exit_code = 0;
            return;
        }
    }

    /* 2. Управляющие конструкции */
    if (u_strncmp(exec_cmd, "if ", 3) == 0 ||
        u_strncmp(exec_cmd, "for ", 4) == 0 ||
        u_strncmp(exec_cmd, "while ", 6) == 0) {
        execute_block(exec_cmd);
    return;
        }

        /* 3. Конвейеры */
        const char *pipe_pos = exec_cmd;
        while (*pipe_pos && *pipe_pos != '|') pipe_pos++;
        if (*pipe_pos == '|') { execute_pipeline(exec_cmd, pipe_pos); return; }

        /* 4. Перенаправление вывода */
        const char *redir_app = exec_cmd;
        while (*redir_app) {
            if (redir_app[0] == '>' && redir_app[1] == '>') { execute_redirection(exec_cmd, redir_app, 1); return; }
            redir_app++;
        }

        const char *redir_pos = exec_cmd;
        while (*redir_pos && *redir_pos != '>') redir_pos++;
        if (*redir_pos == '>') { execute_redirection(exec_cmd, redir_pos, 0); return; }

        /* 5. Встроенные команды */
        if (u_strcmp(exec_cmd, "help") == 0) {
            u_print("Linux 0.01 (x86_64) Shell built-in commands:\n");
            u_print("  help            - show this help message\n");
            u_print("  [ cond ] / test - evaluate condition (-eq, -lt, =, !=, -f, -d, -z...)\n");
            u_print("  let V = A + B   - calculate integer expression (+, -, *, /)\n");
            u_print("  inc / dec <VAR> - increment / decrement variable by 1\n");
            u_print("  expr A op B     - calculate and print expression result\n");
            u_print("  if / for / while- shell control flow structures\n");
            u_print("  stat <file>     - display file inode metadata\n");
            u_print("  edit <file>     - interactive text editor\n");
            u_print("  sh <f> [args..] - execute script with $1, $2, $# parameters\n");
            u_print("  whoami / id     - print current user / group info\n");
            u_print("  su [user]       - switch user (password check)\n");
            u_print("  chmod <mod> <f> - change file permissions\n");
            u_print("  VAR=val         - set environment variable directly\n");
            u_print("  echo $VAR       - variable expansion ($?, $PWD, $USER, $#)\n");
            u_print("  export K=V / env- environment variables management\n");
            u_print("  <cmd> & / jobs  - background process execution (&)\n");
            u_print("  grep <pat> [f]  - search pattern in file or stream\n");
            u_print("  wc [-l] [f]     - count lines/words/bytes\n");
            u_print("  cmd > / >> <f>  - overwrite (>) or append (>>) to file\n");
            u_print("  cmd1 | cmd2     - execute Unix pipeline\n");
            u_print("  pwd / cd [dir]  - directory navigation\n");
            u_print("  ls [-l] [dir]   - list files (compact or detailed -l)\n");
            u_print("  mkdir / rmdir   - directory management\n");
            u_print("  cat / touch / rm- file management\n");
            u_print("  <binary> [args] - execute binary via fork() + execve()\n");
            u_print("  sigtest         - test Ring 3 custom SIGINT handler\n");
            u_print("  date / sleep    - system time & sleeping\n");
            u_print("  ps / kill / wait- process management\n");
            u_print("  uname [-a]      - print system information\n");
            u_print("  clear / exit    - terminal control\n");
            last_exit_code = 0;
        } else if (u_strncmp(exec_cmd, "[ ", 2) == 0 || u_strcmp(exec_cmd, "[") == 0 ||
            u_strncmp(exec_cmd, "test ", 5) == 0 || u_strcmp(exec_cmd, "test") == 0) {
            static char t_buf[16][64];
        static char *t_ptrs[17];
        int t_cnt = 0;
        const char *tp = exec_cmd;
        while (*tp) {
            while (*tp == ' ') tp++;
            if (*tp == '\0') break;

            int ti = 0;
            while (*tp && *tp != ' ' && ti < 63) t_buf[t_cnt][ti++] = *tp++;
            t_buf[t_cnt][ti] = '\0';
            strip_quotes(t_buf[t_cnt]);
            t_ptrs[t_cnt] = t_buf[t_cnt];
            t_cnt++;
            if (t_cnt >= 16) break;
        }
        t_ptrs[t_cnt] = NULL;
        last_exit_code = do_test(t_cnt, t_ptrs);
            } else if (u_strncmp(exec_cmd, "inc ", 4) == 0) {
                const char *var = exec_cmd + 4;
                while (*var == ' ') var++;
                const char *cur = env_get(var);
                int64_t val = cur ? u_atoi(cur) : 0;
                val++;
                char nbuf[32]; int ni = 0; int64_t v = val; int is_neg = 0;
                if (v < 0) { is_neg = 1; v = -v; }
                if (v == 0) nbuf[ni++] = '0';
                while (v > 0) { nbuf[ni++] = '0' + (v % 10); v /= 10; }
                if (is_neg) nbuf[ni++] = '-';
                char rev[32]; int ri = 0;
                while (--ni >= 0) rev[ri++] = nbuf[ni];
                rev[ri] = '\0';
                env_set(var, rev);
                last_exit_code = 0;
            } else if (u_strncmp(exec_cmd, "dec ", 4) == 0) {
                const char *var = exec_cmd + 4;
                while (*var == ' ') var++;
                const char *cur = env_get(var);
                int64_t val = cur ? u_atoi(cur) : 0;
                val--;
                char nbuf[32]; int ni = 0; int64_t v = val; int is_neg = 0;
                if (v < 0) { is_neg = 1; v = -v; }
                if (v == 0) nbuf[ni++] = '0';
                while (v > 0) { nbuf[ni++] = '0' + (v % 10); v /= 10; }
                if (is_neg) nbuf[ni++] = '-';
                char rev[32]; int ri = 0;
                while (--ni >= 0) rev[ri++] = nbuf[ni];
                rev[ri] = '\0';
                env_set(var, rev);
                last_exit_code = 0;
            } else if (u_strncmp(exec_cmd, "let ", 4) == 0) {
                const char *p = exec_cmd + 4;
                while (*p == ' ') p++;
                char vname[32]; int vi = 0;
                while (*p && *p != ' ' && *p != '=' && vi < 31) vname[vi++] = *p++;
                vname[vi] = '\0';

                while (*p == ' ') p++;
                if (*p == '=') p++;
                while (*p == ' ') p++;

                char arg1[32]; int a1i = 0;
                while (*p && *p != ' ' && a1i < 31) arg1[a1i++] = *p++;
                arg1[a1i] = '\0';
                while (*p == ' ') p++;

                int64_t res = 0;
                if (*p == '+' || *p == '-' || *p == '*' || *p == '/') {
                    char op = *p++;
                    while (*p == ' ') p++;
                    char arg2[32]; int a2i = 0;
                    while (*p && *p != ' ' && a2i < 31) arg2[a2i++] = *p++;
                    arg2[a2i] = '\0';

                    int64_t num1 = u_atoi(arg1);
                    int64_t num2 = u_atoi(arg2);
                    if (op == '+') res = num1 + num2;
                    else if (op == '-') res = num1 - num2;
                    else if (op == '*') res = num1 * num2;
                    else if (op == '/') res = (num2 != 0) ? (num1 / num2) : 0;
                } else {
                    res = u_atoi(arg1);
                }

                char nbuf[32]; int ni = 0; int64_t v = res; int is_neg = 0;
                if (v < 0) { is_neg = 1; v = -v; }
                if (v == 0) nbuf[ni++] = '0';
                while (v > 0) { nbuf[ni++] = '0' + (v % 10); v /= 10; }
                if (is_neg) nbuf[ni++] = '-';
                char rev[32]; int ri = 0;
                while (--ni >= 0) rev[ri++] = nbuf[ni];
                rev[ri] = '\0';

                env_set(vname, rev);
                last_exit_code = 0;
            } else if (u_strncmp(exec_cmd, "expr ", 5) == 0) {
                const char *p = exec_cmd + 5;
                while (*p == ' ') p++;
                char arg1[32]; int a1i = 0;
                while (*p && *p != ' ' && a1i < 31) arg1[a1i++] = *p++;
                arg1[a1i] = '\0';
                while (*p == ' ') p++;

                char op = *p++;
                while (*p == ' ') p++;
                char arg2[32]; int a2i = 0;
                while (*p && *p != ' ' && a2i < 31) arg2[a2i++] = *p++;
                arg2[a2i] = '\0';

                int64_t n1 = u_atoi(arg1);
                int64_t n2 = u_atoi(arg2);
                int64_t res = 0;
                if (op == '+') res = n1 + n2;
                else if (op == '-') res = n1 - n2;
                else if (op == '*') res = n1 * n2;
                else if (op == '/') res = (n2 != 0) ? (n1 / n2) : 0;

                u_print_signed(res);
                u_print("\n");
                last_exit_code = 0;
            } else if (u_strncmp(exec_cmd, "stat ", 5) == 0) {
                const char *p = exec_cmd + 5;
                while (*p == ' ') p++;
                char fname[MAX_FILENAME]; int fi = 0;
                while (*p && fi < MAX_FILENAME - 1) fname[fi++] = *p++;
                fname[fi] = '\0';
                strip_quotes(fname);

                struct stat st;
                if (u_stat(fname, &st) != 0) {
                    u_print("stat: cannot stat: "); u_print(fname); u_print("\n");
                    last_exit_code = 1;
                } else {
                    u_print("  File: "); u_print(fname); u_print("\n");
                    u_print("  Size: "); u_print_num(st.st_size);
                    u_print("\t Blocks: 1\t IO Block: 4096   ");
                    if (S_ISDIR(st.st_mode)) u_print("directory\n");
                    else u_print("regular file\n");

                    u_print("Device: 1\t Inode: "); u_print_num(st.st_ino);
                    u_print("\t Links: "); u_print_num(st.st_nlink); u_print("\n");

                    u_print("Access: ("); u_print_octal(st.st_mode & 07777); u_print("/-");
                    if (S_ISDIR(st.st_mode)) u_print("d");
                    u_print((st.st_mode & 0400) ? "r" : "-");
                    u_print((st.st_mode & 0200) ? "w" : "-");
                    u_print((st.st_mode & 0100) ? "x" : "-");
                    u_print((st.st_mode & 0040) ? "r" : "-");
                    u_print((st.st_mode & 0020) ? "w" : "-");
                    u_print((st.st_mode & 0010) ? "x" : "-");
                    u_print((st.st_mode & 0004) ? "r" : "-");
                    u_print((st.st_mode & 0002) ? "w" : "-");
                    u_print((st.st_mode & 0001) ? "x" : "-");
                    u_print(")  Uid: ("); u_print_num(st.st_uid);
                    u_print(st.st_uid == 0 ? "/root)   Gid: (" : "/user)   Gid: (");
                    u_print_num(st.st_gid); u_print(st.st_gid == 0 ? "/root)\n" : "/user)\n");

                    u_print("Modify: ");
                    print_date(st.st_mtime);
                    last_exit_code = 0;
                }
            } else if (u_strncmp(exec_cmd, "edit ", 5) == 0) {
                const char *fname = exec_cmd + 5;
                while (*fname == ' ') fname++;
                run_editor(fname);
                last_exit_code = 0;
            } else if (u_strncmp(exec_cmd, "sh ", 3) == 0) {
                execute_script_args(exec_cmd + 3);
                last_exit_code = 0;
            } else if (u_strcmp(exec_cmd, "whoami") == 0) {
                uint16_t uid = (uint16_t)u_getuid();
                if (uid == 0) u_print("root\n");
                else if (uid == 1000) u_print("user\n");
                else if (uid == 1001) u_print("guest\n");
                else { u_print("uid_"); u_print_num(uid); u_print("\n"); }
                last_exit_code = 0;
            } else if (u_strcmp(exec_cmd, "id") == 0) {
                uint16_t uid = (uint16_t)u_getuid();
                u_print("uid="); u_print_num(uid);
                if (uid == 0) u_print("(root) gid=0(root)\n");
                else if (uid == 1000) u_print("(user) gid=1000(user)\n");
                else if (uid == 1001) u_print("(guest) gid=1001(guest)\n");
                else u_print(" gid=1000\n");
                last_exit_code = 0;
            } else if (u_strncmp(exec_cmd, "su", 2) == 0 && (exec_cmd[2] == ' ' || exec_cmd[2] == '\0')) {
                const char *user = exec_cmd + 2;
                while (*user == ' ') user++;
                uint16_t target_uid = 0;
                const char *uname = "root";

                if (user[0] == '\0' || u_strcmp(user, "root") == 0) { target_uid = 0; uname = "root"; }
                else if (u_strcmp(user, "user") == 0) { target_uid = 1000; uname = "user"; }
                else if (u_strcmp(user, "guest") == 0) { target_uid = 1001; uname = "guest"; }
                else { target_uid = (uint16_t)u_atoi(user); }

                uint16_t cur_uid = (uint16_t)u_getuid();
                if (cur_uid == 0) {
                    u_setuid(target_uid, NULL);
                    env_set("USER", uname);
                    last_exit_code = 0;
                    return;
                }

                u_print("Password: ");
                char pass_buf[32];
                read_password(pass_buf, sizeof(pass_buf));

                if (u_setuid(target_uid, pass_buf) == 0) {
                    env_set("USER", uname);
                    last_exit_code = 0;
                } else {
                    u_print("su: Authentication failure\n");
                    last_exit_code = 1;
                }
            } else if (u_strncmp(exec_cmd, "chmod ", 6) == 0) {
                const char *p = exec_cmd + 6;
                while (*p == ' ') p++;
                int mode = (int)u_atoi(p);
                while (*p && *p != ' ') p++;
                while (*p == ' ') p++;
                if (u_chmod(p, mode) != 0) {
                    u_print("chmod: permission denied or file not found\n");
                    last_exit_code = 1;
                } else last_exit_code = 0;
            } else if (u_strcmp(exec_cmd, "sigtest") == 0) {
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
                    last_exit_code = status;
                }
            } else if (u_strcmp(exec_cmd, "env") == 0) {
                for (int i = 0; i < MAX_ENV; i++) {
                    if (env_vars[i].in_use) {
                        u_print(env_vars[i].key); u_print("="); u_print(env_vars[i].val); u_print("\n");
                    }
                }
                last_exit_code = 0;
            } else if (u_strncmp(exec_cmd, "export ", 7) == 0) {
                const char *p = exec_cmd + 7;
                while (*p == ' ') p++;
                char k[32]; int ki = 0;
                while (*p && *p != '=' && *p != ' ' && ki < 31) k[ki++] = *p++;
                k[ki] = '\0';
                if (*p == '=') p++;
                env_set(k, p);
                last_exit_code = 0;
            } else if (u_strncmp(exec_cmd, "unset ", 6) == 0) {
                const char *p = exec_cmd + 6;
                while (*p == ' ') p++;
                env_unset(p);
                last_exit_code = 0;
            } else if (u_strcmp(exec_cmd, "jobs") == 0) {
                for (int i = 0; i < MAX_JOBS; i++) {
                    if (bg_jobs[i].in_use) {
                        u_print("["); u_print_num(i + 1); u_print("]  Running  PID ");
                        u_print_num((uint64_t)bg_jobs[i].pid); u_print("  ");
                        u_print(bg_jobs[i].cmd); u_print("\n");
                    }
                }
                last_exit_code = 0;
            } else if (u_strcmp(exec_cmd, "pwd") == 0) {
                char buf[64];
                if (u_getcwd(buf, sizeof(buf)) >= 0) { u_print(buf); u_print("\n"); }
                last_exit_code = 0;
            } else if (u_strncmp(exec_cmd, "cd ", 3) == 0) {
                const char *target = exec_cmd + 3;
                while (*target == ' ') target++;
                if (u_chdir(target) != 0) {
                    u_print("cd: no such directory: "); u_print(target); u_print("\n");
                    last_exit_code = 1;
                } else last_exit_code = 0;
            } else if (u_strcmp(exec_cmd, "cd") == 0) {
                u_chdir("/");
                last_exit_code = 0;
            } else if (u_strncmp(exec_cmd, "mkdir ", 6) == 0) {
                const char *dname = exec_cmd + 6;
                while (*dname == ' ') dname++;
                if (u_mkdir(dname) != 0) { u_print("mkdir: failed\n"); last_exit_code = 1; }
                else last_exit_code = 0;
            } else if (u_strncmp(exec_cmd, "rmdir ", 6) == 0) {
                const char *dname = exec_cmd + 6;
                while (*dname == ' ') dname++;
                if (u_rmdir(dname) != 0) { u_print("rmdir: failed\n"); last_exit_code = 1; }
                else last_exit_code = 0;
            } else if (u_strncmp(exec_cmd, "ls", 2) == 0 && (exec_cmd[2] == ' ' || exec_cmd[2] == '\0')) {
                const char *arg = exec_cmd + 2;
                while (*arg == ' ') arg++;
                int is_long = 0;
                if (u_strncmp(arg, "-l", 2) == 0) {
                    is_long = 1; arg += 2; while (*arg == ' ') arg++;
                }
                char buf[1024];
                if (u_list(arg, buf, sizeof(buf), is_long) > 0) u_print(buf);
                last_exit_code = 0;
            } else if (u_strncmp(exec_cmd, "grep ", 5) == 0) {
                const char *p = exec_cmd + 5;
                while (*p == ' ') p++;
                char pat[64]; int pi = 0;
                while (*p && *p != ' ' && pi < 63) pat[pi++] = *p++;
                pat[pi] = '\0';
                strip_quotes(pat);
                while (*p == ' ') p++;
                if (*p != '\0') {
                    char fname[MAX_FILENAME]; int fi = 0;
                    while (*p && fi < MAX_FILENAME - 1) fname[fi++] = *p++;
                    fname[fi] = '\0';
                    strip_quotes(fname);
                    int64_t fd = u_open(fname, O_RDONLY);
                    if (fd >= 0) { do_grep(pat, (int)fd); u_close((int)fd); last_exit_code = 0; }
                    else { u_print("grep: open failed\n"); last_exit_code = 1; }
                } else { do_grep(pat, 0); last_exit_code = 0; }
            } else if (u_strncmp(exec_cmd, "wc", 2) == 0 && (exec_cmd[2] == ' ' || exec_cmd[2] == '\0')) {
                const char *p = exec_cmd + 2;
                while (*p == ' ') p++;
                int only_l = 0;
                if (u_strncmp(p, "-l", 2) == 0) { only_l = 1; p += 2; while (*p == ' ') p++; }
                if (*p != '\0') {
                    char fname[MAX_FILENAME]; int fi = 0;
                    while (*p && fi < MAX_FILENAME - 1) fname[fi++] = *p++;
                    fname[fi] = '\0';
                    strip_quotes(fname);
                    int64_t fd = u_open(fname, O_RDONLY);
                    if (fd >= 0) { do_wc((int)fd, only_l); u_close((int)fd); last_exit_code = 0; }
                    else { u_print("wc: open failed\n"); last_exit_code = 1; }
                } else { do_wc(0, only_l); last_exit_code = 0; }
            } else if (u_strncmp(exec_cmd, "touch ", 6) == 0) {
                const char *fname = exec_cmd + 6;
                while (*fname == ' ') fname++;
                char cfname[MAX_FILENAME]; int ci = 0;
                while (*fname && ci < MAX_FILENAME - 1) cfname[ci++] = *fname++;
                cfname[ci] = '\0';
                strip_quotes(cfname);
                int64_t fd = u_open(cfname, O_CREAT | O_WRONLY);
                if (fd >= 0) { u_close((int)fd); last_exit_code = 0; }
                else last_exit_code = 1;
            } else if (u_strncmp(exec_cmd, "rm ", 3) == 0) {
                const char *fname = exec_cmd + 3;
                while (*fname == ' ') fname++;
                char cfname[MAX_FILENAME]; int ci = 0;
                while (*fname && ci < MAX_FILENAME - 1) cfname[ci++] = *fname++;
                cfname[ci] = '\0';
                strip_quotes(cfname);
                if (u_unlink(cfname) != 0) { u_print("rm: permission denied or file not found\n"); last_exit_code = 1; }
                else last_exit_code = 0;
            } else if (u_strcmp(exec_cmd, "cat") == 0) {
                char fbuf[128]; int64_t n;
                while ((n = u_read(0, fbuf, sizeof(fbuf) - 1)) > 0) {
                    fbuf[n] = '\0'; u_write(1, fbuf, n);
                }
                last_exit_code = 0;
            } else if (u_strncmp(exec_cmd, "cat ", 4) == 0) {
                const char *filename = exec_cmd + 4;
                while (*filename == ' ') filename++;
                char cfname[MAX_FILENAME]; int ci = 0;
                while (*filename && ci < MAX_FILENAME - 1) cfname[ci++] = *filename++;
                cfname[ci] = '\0';
                strip_quotes(cfname);
                int64_t fd = u_open(cfname, O_RDONLY);
                if (fd < 0) {
                    u_print("cat: not found\n"); last_exit_code = 1;
                } else {
                    char fbuf[128]; int64_t n;
                    while ((n = u_read((int)fd, fbuf, sizeof(fbuf) - 1)) > 0) {
                        fbuf[n] = '\0'; u_write(1, fbuf, n);
                    }
                    u_close((int)fd);
                    last_exit_code = 0;
                }
            } else if (u_strncmp(exec_cmd, "echo ", 5) == 0) {
                const char *p = exec_cmd + 5;
                while (*p == ' ') p++;
                char msg[128]; int mi = 0;
                while (p[mi] && mi < 127) { msg[mi] = p[mi]; mi++; }
                msg[mi] = '\0';
                strip_quotes(msg);
                u_print(msg); u_print("\n");
                last_exit_code = 0;
            } else if (u_strcmp(exec_cmd, "date") == 0) {
                uint64_t epoch = (uint64_t)u_time();
                print_date(epoch);
                last_exit_code = 0;
            } else if (u_strncmp(exec_cmd, "uname", 5) == 0 && (exec_cmd[5] == ' ' || exec_cmd[5] == '\0')) {
                static struct utsname un;
                if (u_uname(&un) == 0) {
                    if (exec_cmd[5] == ' ' && exec_cmd[6] == '-' && exec_cmd[7] == 'a') {
                        u_print(un.sysname); u_print(" ");
                        u_print(un.nodename); u_print(" ");
                        u_print(un.release); u_print(" ");
                        u_print(un.version); u_print(" ");
                        u_print(un.machine); u_print("\n");
                    } else {
                        u_print(un.sysname); u_print("\n");
                    }
                }
                last_exit_code = 0;
            } else if (u_strncmp(exec_cmd, "sleep ", 6) == 0) {
                int sec = (int)u_atoi(exec_cmd + 6);
                if (sec <= 0) sec = 1;

                int64_t pid = u_fork();
                if (pid == 0) {
                    uint64_t start = (uint64_t)u_time();
                    while ((uint64_t)u_time() - start < (uint64_t)sec) {}
                    u_exit(0);
                } else if (pid > 0) {
                    if (is_bg) {
                        add_bg_job(pid, exec_cmd);
                    } else {
                        u_print("Sleeping for "); u_print_num(sec); u_print(" seconds...\n");
                        int status = 0;
                        u_waitpid(pid, &status, 0);
                        last_exit_code = status;
                        if (status == 130) u_print("Sleep aborted by SIGINT!\n");
                    }
                }
            } else if (u_strcmp(exec_cmd, "ps") == 0) {
                u_ps();
                last_exit_code = 0;
            } else if (u_strncmp(exec_cmd, "kill ", 5) == 0) {
                int target_pid = (int)u_atoi(exec_cmd + 5);
                if (target_pid > 1) { u_kill(target_pid, SIGKILL); last_exit_code = 0; }
                else last_exit_code = 1;
            } else if (u_strcmp(exec_cmd, "exit") == 0) {
                u_exit(0);
            } else {
                uint64_t cl = 0;
                while (exec_cmd[cl]) cl++;
                if (cl > 3 && exec_cmd[cl - 3] == '.' && exec_cmd[cl - 2] == 's' && exec_cmd[cl - 1] == 'h') {
                    execute_script_args(exec_cmd);
                    return;
                }

                static char arg_buf[16][64];
                static char *argv_ptrs[17];
                int argc = 0;

                const char *ap = exec_cmd;
                while (*ap) {
                    while (*ap == ' ') ap++;
                    if (*ap == '\0') break;

                    int ai = 0;
                    while (*ap && *ap != ' ' && ai < 63) arg_buf[argc][ai++] = *ap++;
                    arg_buf[argc][ai] = '\0';
                    strip_quotes(arg_buf[argc]);

                    argv_ptrs[argc] = arg_buf[argc];
                    argc++;
                    if (argc >= 16) break;
                }
                argv_ptrs[argc] = NULL;

                int64_t pid = u_fork();
                if (pid == 0) {
                    int64_t err = u_execve(argv_ptrs[0], argv_ptrs, NULL);
                    if (err < 0) {
                        char bin_path[64];
                        bin_path[0] = '/'; bin_path[1] = 'b'; bin_path[2] = 'i'; bin_path[3] = 'n'; bin_path[4] = '/';
                        uint64_t bi = 5;
                        for (uint64_t k = 0; argv_ptrs[0][k] && bi < sizeof(bin_path) - 1; k++) bin_path[bi++] = argv_ptrs[0][k];
                        bin_path[bi] = '\0';
                        err = u_execve(bin_path, argv_ptrs, NULL);
                    }
                    if (err < 0) {
                        u_print("shell: command not found: "); u_print(argv_ptrs[0]); u_print("\n");
                        u_exit(127);
                    }
                } else if (pid > 0) {
                    if (is_bg) {
                        add_bg_job(pid, exec_cmd);
                    } else {
                        int status = 0;
                        u_waitpid(pid, &status, 0);
                        last_exit_code = status;
                        u_print("[Program finished with exit code "); u_print_num((uint64_t)status); u_print("]\n");
                    }
                }
            }
}

static void print_prompt(void)
{
    char cwd[64];
    u_getcwd(cwd, sizeof(cwd));
    uint16_t uid = (uint16_t)u_getuid();

    if (uid == 0) {
        u_print("root@linux64:"); u_print(cwd); u_print("# ");
    } else {
        const char *user = env_get("USER");
        if (user) u_print(user);
        else u_print("user");
        u_print("@linux64:"); u_print(cwd); u_print("$ ");
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

    env_init();
    execute_script_args("/etc/init.sh");

    char cmd_buf[128];
    int buf_len = 0;

    print_prompt();

    while (1) {
        char c;
        if (u_read(0, &c, 1) > 0) {
            if (c == 27) {
                char seq[2];
                if (u_read(0, &seq[0], 1) > 0 && seq[0] == '[') {
                    if (u_read(0, &seq[1], 1) > 0) {
                        if (seq[1] == 'A') {
                            if (history_count > 0 && history_idx > 0) {
                                history_idx--;
                                while (buf_len > 0) { u_write(1, "\b", 1); buf_len--; }
                                int slot = history_idx % HISTORY_MAX;
                                const char *hcmd = history[slot];
                                while (*hcmd && buf_len < 127) {
                                    cmd_buf[buf_len++] = *hcmd;
                                    u_write(1, hcmd, 1);
                                    hcmd++;
                                }
                            }
                        } else if (seq[1] == 'B') {
                            if (history_count > 0 && history_idx < history_count) {
                                history_idx++;
                                while (buf_len > 0) { u_write(1, "\b", 1); buf_len--; }
                                if (history_idx < history_count) {
                                    int slot = history_idx % HISTORY_MAX;
                                    const char *hcmd = history[slot];
                                    while (*hcmd && buf_len < 127) {
                                        cmd_buf[buf_len++] = *hcmd;
                                        u_write(1, hcmd, 1);
                                        hcmd++;
                                    }
                                }
                            }
                        }
                    }
                }
                continue;
            }

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
            } else if (c == '\n') {
                u_write(1, "\n", 1);
                cmd_buf[buf_len] = '\0';
                history_add(cmd_buf);

                const char *cb = cmd_buf;
                while (*cb == ' ' || *cb == '\t') cb++;

                /* Если это блок цикла/условия или цепочка со знаком ';',
                 *                   передаем строку нераскрытой для динамической подстановки на каждой итерации */
                if (u_strncmp(cb, "for ", 4) == 0 ||
                    u_strncmp(cb, "while ", 6) == 0 ||
                    u_strncmp(cb, "if ", 3) == 0 ||
                    u_strstr(cb, ";") != NULL) {
                    execute_command(cb);
                    } else {
                        char expanded[256];
                        expand_vars(cb, expanded, sizeof(expanded));
                        execute_command(expanded);
                    }

                    buf_len = 0;
                    check_bg_jobs();
                    print_prompt();
            } else if (c >= 32 && c <= 126) {
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
    /* Выделяем гарантированные непрерывные 32 КБ стека для пользователя */
    uint64_t s1 = get_free_pages(8);
    uint64_t user_stack = s1 + (8 * PAGE_SIZE) - 16;
    enter_user_mode((uint64_t)user_init_process, user_stack);
}

void main(uint64_t mb_magic, uint64_t mb_info_addr)
{
    console_init();

    printk("==============================================\n");
    printk("   Linux 0.01 (x86_64 Edition) Booting...    \n");
    printk("==============================================\n\n");

    uint64_t initrd_start = 0;
    uint64_t initrd_end = 0;

    if (mb_magic == MULTIBOOT_BOOTLOADER_MAGIC && mb_info_addr != 0) {
        struct mb_info *mbi = (struct mb_info *)mb_info_addr;
        if ((mbi->flags & MB_FLAG_MODS) && mbi->mods_count > 0 && mbi->mods_addr != 0) {
            struct mb_module *mod = (struct mb_module *)((uint64_t)mbi->mods_addr);
            initrd_start = (uint64_t)mod->mod_start;
            initrd_end   = (uint64_t)mod->mod_end;
            printk("[OK] Multiboot Initrd Module: %p - %p (%d KB)\n",
                   initrd_start, initrd_end, (int)((initrd_end - initrd_start) / 1024));
        }
    }

    time_init();
    gdt_init();
    trap_init();
    syscall_init();
    mem_init(initrd_end);
    sched_init();
    fs_init();

    if (initrd_start && initrd_end) {
        tarfs_mount(initrd_start, initrd_end);
    }

    task_create(user_trampoline, 10);

    __asm__ volatile ("sti");
    printk("[OK] System Initialized. Launching User Shell...\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
