/*
 * kernel/linux_abi.c -- слой совместимости с Linux x86_64 ABI.
 *
 * Процессы, собранные под Linux (musl, статически, EI_OSABI = 0x66), делают
 * системные вызовы с номерами Linux x86_64. Здесь они реализуются поверх
 * существующих внутренних функций ядра: VFS (ramfs + Minix), каналы, VMA/COW,
 * fork/execve. Реализовано то, что нужно GNU coreutils и bash (glibc, динамически) и статическим программам musl:
 *   - файлы: open/openat, read/write/readv/writev, lseek, dup/dup2/dup3, fcntl,
 *     pipe/pipe2, ioctl (termios, TIOCGWINSZ), stat/fstat/lstat/newfstatat,
 *     getdents64, getcwd/chdir/fchdir, mkdir/rmdir/unlink/rename/ftruncate,
 *     access, readlink (всегда EINVAL), chmod, /dev/null,zero,tty,urandom;
 *   - память: mmap (анонимный и файловый, копией), munmap, mprotect, brk;
 *   - процессы: fork/vfork/clone (в т.ч. CLONE_THREAD) и futex, execve, wait4, exit/exit_group,
 *     getpid/getppid/..., setpgid/setsid, kill;
 *   - сигналы: rt_sigaction/rt_sigprocmask/rt_sigreturn с настоящей доставкой
 *     обработчиков на пользовательский стек (при возврате из системного вызова);
 *   - TLS: arch_prctl(ARCH_SET_FS), set_tid_address;
 *   - время: clock_gettime, gettimeofday, time, nanosleep;
 *   - tty: канонический режим с эхом и стиранием, raw-режим (VMIN/VTIME).
 * Сокеты Linux (сеть доступна через собственные системные вызовы ядра) и mmap с MAP_SHARED не поддерживаются.
 */
#include <linux/syscall.h>
#include <linux/sched.h>
#include <linux/traps.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/tty.h>
#include <linux/time.h>
#include <linux/nic.h>
#include <linux/gdt.h>
#include <linux/utsname.h>
#include <linux/sock.h>
#include <linux/ip.h>

#define EPERM        1
#define ENOENT       2
#define ESRCH        3
#define EINTR        4
#define EIO          5
#define ENOEXEC      8
#define EBADF        9
#define ECHILD      10
#define EAGAIN      11
#define ENOMEM      12
#define ETIMEDOUT   110
#define EACCES      13
#define EFAULT      14
#define EEXIST      17
#define EXDEV       18
#define ENOTDIR     20
#define EISDIR      21
#define EINVAL      22
#define EMFILE      24
#define ENOTTY      25
#define ESPIPE      29
#define EPIPE       32
#define ERANGE      34
#define ENAMETOOLONG 36
#define ENOSYS      38
#define EOPNOTSUPP  95
#define EAFNOSUPPORT 97
#define ENOTEMPTY   39
#define ENOTSOCK    88
#define EDESTADDRREQ 89
#define EPROTONOSUPPORT 93
#define ENOPROTOOPT 92
#define EADDRNOTAVAIL 99

#define AT_FDCWD   (-100)

extern int64_t sys_read(int fd, char *buf, uint64_t count);
extern int64_t sys_write(int fd, const char *buf, uint64_t count);
extern int64_t sys_dup2(int oldfd, int newfd);
extern int64_t sys_waitpid(int64_t pid, int *stat_addr, int options);
extern int64_t sys_brk(uint64_t new_brk);
extern int64_t sys_lseek(int fd, int64_t off, int whence);
extern int64_t sys_exit(int status);
extern int64_t sys_exit_group(int status);
extern int64_t sys_clone_thread(struct trap_frame *tf, uint64_t flags, uint64_t stack, uint64_t ptid, uint64_t ctid, uint64_t tls);
extern int64_t sys_fork(struct trap_frame *tf);
extern int64_t sys_execve(const char *filename, char **argv, char **envp, struct trap_frame *tf);
extern char keyboard_getchar(void);
extern volatile uint64_t jiffies;
extern void console_get_size(int *cols, int *rows);

/* ===================================================================== */
/*  Доступ к памяти пользователя                                          */
/* ===================================================================== */

static int ucopy_from(void *dst, uint64_t src, uint64_t n)
{
    if (!user_access_ok(src, n, 0)) return -EFAULT;
    memcpy(dst, (const void *)src, n);
    return 0;
}

static int ucopy_to(uint64_t dst, const void *src, uint64_t n)
{
    if (!user_access_ok(dst, n, 1)) return -EFAULT;
    memcpy((void *)dst, src, n);
    return 0;
}

/* Строка пользователя -> буфер ядра (с проверкой каждой страницы) */
static int ustr(uint64_t src, char *dst, uint64_t max)
{
    for (uint64_t i = 0; i < max; i++) {
        if (i == 0 || ((src + i) & (PAGE_SIZE - 1)) == 0) {
            if (!user_access_ok(src + i, 1, 0)) return -EFAULT;
        }
        char c = *(const volatile char *)(src + i);
        dst[i] = c;
        if (!c) return 0;
    }
    return -ENAMETOOLONG;
}

/* ===================================================================== */
/*  Терминал (одна консоль)                                               */
/* ===================================================================== */

#define ICRNL  0x100
#define OPOST  0x1
#define ISIG   0x1
#define ICANON 0x2
#define ECHO   0x8
#define ECHOE  0x10
#define ECHOK  0x20
#define VKILL 3
#define VEOF  4
#define VTIME 5
#define VMIN  6

struct ktermios {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[19];
};

static struct ktermios tty_tio = {
    ICRNL | 0x400, OPOST | 0x4, 0xBF, ISIG | ICANON | ECHO | ECHOE | ECHOK | 0x200 | 0x800 | 0x8000,
    0,
    { 3, 28, 0x7F, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22, 0, 0, 0 }
};

static char    tty_line[512];         /* введённая строка (канонический режим) */
static int     tty_len = 0;
static int     tty_ready = 0;         /* строка завершена, можно отдавать */
static int     tty_pos = 0;           /* сколько уже отдано */
static int     tty_eof = 0;
static int     tty_esc = 0;
static volatile int tty_intr = 0;
static int     tty_fg_pgrp = 0;

static int pgrp_of(struct task_struct *t) { return t->pgid ? t->pgid : (int)(t->tgid ? t->tgid : t->pid); }

/* Есть ли живой процесс в группе pg */
static int pgrp_alive(int pg)
{
    for (int i = 2; i < NR_TASKS; i++) {
        struct task_struct *t = task[i];
        if (t && t->state != TASK_ZOMBIE && t->linux_abi && pgrp_of(t) == pg) return 1;
    }
    return 0;
}

static int sig_pending_for(struct task_struct *t)
{
    return (t->signal & ~(uint32_t)t->sigmask) != 0;
}

/* Ctrl+C: всем пользовательским Linux-процессам; если их нет -- старая логика */
void tty_sigint(void)
{
    int any = 0;
    int fg = (tty_fg_pgrp > 0 && pgrp_alive(tty_fg_pgrp)) ? tty_fg_pgrp : 0;
    for (int i = 2; i < NR_TASKS; i++) {
        struct task_struct *t = task[i];
        if (t && t->linux_abi && t->state != TASK_ZOMBIE && (!fg || pgrp_of(t) == fg)) {
            send_signal(t, SIGINT);
            any = 1;
        }
    }
    if (any) {
        tty_intr = 1;
    } else if (current && current->pid > 1) {
        send_signal(current, SIGINT);
    }
}

static void tty_echo_erase(void)
{
    console_putc('\b');
    console_putc(' ');
    console_putc('\b');
}

static int64_t tty_read(char *buf, uint64_t count)
{
    if (count == 0) return 0;

    if (tty_tio.c_lflag & ICANON) {
        for (;;) {
            if (tty_ready) {
                int avail = tty_len - tty_pos;
                int n = (uint64_t)avail < count ? avail : (int)count;
                memcpy(buf, tty_line + tty_pos, n);
                tty_pos += n;
                if (tty_pos >= tty_len) { tty_ready = 0; tty_len = tty_pos = 0; }
                return n;
            }
            if (tty_eof) { tty_eof = 0; return 0; }
            if (tty_intr) {
                tty_intr = 0;
                tty_len = 0;
                if (tty_tio.c_lflag & ECHO) { console_putc('^'); console_putc('C'); console_putc('\n'); }
                return -EINTR;
            }
            if (sig_pending_for(current)) return -EINTR;

            __asm__ volatile ("sti");
            char c = keyboard_getchar();
            if (!c) { __asm__ volatile ("hlt"); continue; }

            if (tty_esc == 1) { tty_esc = (c == '[' || c == 'O') ? 2 : 0; continue; }
            if (tty_esc == 2) { tty_esc = 0; continue; }
            if (c == 27) { tty_esc = 1; continue; }

            if (c == '\r' && (tty_tio.c_iflag & ICRNL)) c = '\n';
            if (c == '\b' || c == 0x7F) {
                if (tty_len > 0) { tty_len--; if (tty_tio.c_lflag & ECHO) tty_echo_erase(); }
                continue;
            }
            if (c == (char)tty_tio.c_cc[VKILL]) {
                while (tty_len > 0) { tty_len--; if (tty_tio.c_lflag & ECHO) tty_echo_erase(); }
                continue;
            }
            if (c == (char)tty_tio.c_cc[VEOF]) {
                if (tty_len == 0) { tty_eof = 1; } else { tty_ready = 1; tty_pos = 0; }
                continue;
            }
            if (tty_len < (int)sizeof(tty_line) - 1) {
                tty_line[tty_len++] = c;
                if (tty_tio.c_lflag & ECHO) console_putc(c);
                if (c == '\n') { tty_ready = 1; tty_pos = 0; }
            }
        }
    }

    /* raw / неканонический режим */
    uint64_t got = 0;
    int vmin = tty_tio.c_cc[VMIN], vtime = tty_tio.c_cc[VTIME];
    uint64_t deadline = 0;
    if (vmin == 0 && vtime > 0) deadline = jiffies + (uint64_t)vtime * 10;
    for (;;) {
        __asm__ volatile ("sti");
        char c = keyboard_getchar();
        if (c) {
            if (c == '\r' && (tty_tio.c_iflag & ICRNL)) c = '\n';
            buf[got++] = c;
            if (tty_tio.c_lflag & ECHO) console_putc(c);
            if (got >= count) return (int64_t)got;
            continue;                               /* забрать всё, что уже лежит в буфере */
        }
        if (got > 0) return (int64_t)got;
        if (vmin == 0 && vtime == 0) return 0;
        if (vmin == 0 && jiffies >= deadline) return 0;
        if (tty_intr) { tty_intr = 0; return -EINTR; }
        if (sig_pending_for(current)) return -EINTR;
        __asm__ volatile ("hlt");
    }
}

static int64_t tty_write(const char *buf, uint64_t count)
{
    for (uint64_t i = 0; i < count; i++) console_putc(buf[i]);
    return (int64_t)count;
}

static int64_t tty_ioctl(uint64_t req, uint64_t arg)
{
    switch (req) {
    case 0x5401: /* TCGETS */
        return ucopy_to(arg, &tty_tio, sizeof(tty_tio));
    case 0x5402: case 0x5403: case 0x5404: { /* TCSETS / TCSETSW / TCSETSF */
        struct ktermios t;
        int r = ucopy_from(&t, arg, sizeof(t));
        if (r < 0) return r;
        if ((tty_tio.c_lflag & ICANON) && !(t.c_lflag & ICANON)) { tty_ready = 0; tty_len = tty_pos = 0; }
        tty_tio = t;
        return 0;
    }
    case 0x802C542A: { /* TCGETS2: struct termios2 = termios + c_ispeed + c_ospeed (новая glibc использует для isatty) */
        uint8_t t2[44];
        memset(t2, 0, sizeof(t2));
        memcpy(t2, &tty_tio, sizeof(tty_tio));
        uint32_t baud = 38400;
        memcpy(t2 + 36, &baud, 4);
        memcpy(t2 + 40, &baud, 4);
        return ucopy_to(arg, t2, sizeof(t2));
    }
    case 0x402C542B: case 0x402C542C: case 0x402C542D: { /* TCSETS2 / TCSETSW2 / TCSETSF2 */
        uint8_t t2[44];
        int r = ucopy_from(t2, arg, sizeof(t2));
        if (r < 0) return r;
        struct ktermios t;
        memcpy(&t, t2, sizeof(t));
        if ((tty_tio.c_lflag & ICANON) && !(t.c_lflag & ICANON)) { tty_ready = 0; tty_len = tty_pos = 0; }
        tty_tio = t;
        return 0;
    }
    case 0x5413: { /* TIOCGWINSZ */
        int c = 80, r = 25;
        console_get_size(&c, &r);
        uint16_t ws[4] = { (uint16_t)r, (uint16_t)c, 0, 0 };
        return ucopy_to(arg, ws, sizeof(ws));
    }
    case 0x5414: /* TIOCSWINSZ */
        return 0;
    case 0x540F: { /* TIOCGPGRP */
        if (tty_fg_pgrp <= 0 || !pgrp_alive(tty_fg_pgrp)) tty_fg_pgrp = pgrp_of(current);
        int p = tty_fg_pgrp;
        return ucopy_to(arg, &p, 4);
    }
    case 0x5410: { /* TIOCSPGRP */
        int p;
        if (ucopy_from(&p, arg, 4) < 0) return -EFAULT;
        tty_fg_pgrp = p;
        return 0;
    }
    case 0x540E: /* TIOCSCTTY */
    case 0x5422: /* TIOCNOTTY */
    case 0x540A: /* TCSBRK */
    case 0x540B: /* TCFLSH */
        return 0;
    case 0x541B: { /* FIONREAD */
        int n = tty_ready ? tty_len - tty_pos : 0;
        return ucopy_to(arg, &n, 4);
    }
    }
    return -ENOTTY;
}

/* ===================================================================== */
/*  Таблица файлов                                                        */
/* ===================================================================== */

#define DEV_NULL    1
#define DEV_ZERO    2
#define DEV_TTY     3
#define DEV_URANDOM 4

static int dev_kind(const char *p)
{
    if (!strcmp(p, "/dev/null")) return DEV_NULL;
    if (!strcmp(p, "/dev/zero")) return DEV_ZERO;
    if (!strcmp(p, "/dev/tty") || !strcmp(p, "/dev/console") || !strcmp(p, "/dev/tty0")) return DEV_TTY;
    if (!strcmp(p, "/dev/urandom") || !strcmp(p, "/dev/random")) return DEV_URANDOM;
    return 0;
}

static struct file *fd_get(int fd)
{
    if (fd < 0 || fd >= NR_OPEN || !current->files->f[fd].in_use) return NULL;
    return &current->files->f[fd];
}

static int fd_alloc(int minfd)
{
    for (int i = minfd; i < NR_OPEN; i++)
        if (!current->files->f[i].in_use) return i;
    return -1;
}

/* Переместить дескриптор, выданный старым кодом (всегда >= 3), на самый
 * маленький свободный -- как требует Linux. */
static int fd_lowest(int fd)
{
    int low = fd_alloc(0);
    if (low >= 0 && low < fd) {
        current->files->f[low] = current->files->f[fd];
        current->files->f[fd].in_use = 0;
        current->files->f[fd].type = 0;
        current->cloexec &= ~(1ULL << fd);
        return low;
    }
    return fd;
}

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;
static uint32_t krand(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    rng_state ^= ((uint64_t)hi << 32) | lo;
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

/* dirfd + путь пользователя -> полный абсолютный путь */
static int lx_path(int dirfd, uint64_t upath, char *out)
{
    char tmp[MAX_FILENAME];
    int r = ustr(upath, tmp, sizeof(tmp));
    if (r < 0) return r;
    if (tmp[0] == '\0') return -ENOENT;
    if (tmp[0] != '/' && dirfd != AT_FDCWD) {
        struct file *f = fd_get(dirfd);
        if (!f || f->type != FILE_TYPE_DIR) return -ENOTDIR;
        char join[MAX_FILENAME * 2];
        uint64_t pl = strlen(f->path);
        if (pl + 1 + strlen(tmp) >= MAX_FILENAME) return -ENAMETOOLONG;
        memcpy(join, f->path, pl);
        if (pl == 0 || join[pl - 1] != '/') join[pl++] = '/';
        strcpy(join + pl, tmp);
        fs_resolve_path(join, out);
        return 0;
    }
    fs_resolve_path(tmp, out);
    return 0;
}

/* ---- Linux struct stat ---- */
struct lx_stat {
    uint64_t st_dev, st_ino, st_nlink;
    uint32_t st_mode, st_uid, st_gid, pad0;
    uint64_t st_rdev;
    int64_t  st_size, st_blksize, st_blocks;
    int64_t  atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    int64_t  unused[3];
};

static void to_lx_stat(struct lx_stat *o, const struct stat *k)
{
    memset(o, 0, sizeof(*o));
    o->st_dev = k->st_dev;
    o->st_ino = k->st_ino;
    o->st_nlink = k->st_nlink ? k->st_nlink : 1;
    o->st_mode = k->st_mode;
    o->st_uid = k->st_uid;
    o->st_gid = k->st_gid;
    o->st_size = (int64_t)k->st_size;
    o->st_blksize = 4096;
    o->st_blocks = (int64_t)((k->st_size + 511) / 512);
    o->atime = o->mtime = o->ctime = (int64_t)k->st_mtime;
}

static int dirent_isdir(const char *full)
{
    struct stat k;
    memset(&k, 0, sizeof(k));
    if (sys_stat(full, &k) < 0) return !strcmp(full, "/");
    return (k.st_mode & 0170000) == 0040000;
}

static int64_t stat_path(const char *full, struct lx_stat *o)
{
    int dk = dev_kind(full);
    struct stat k;
    memset(&k, 0, sizeof(k));
    if (dk) {
        k.st_dev = 5; k.st_ino = 100 + dk; k.st_nlink = 1;
        k.st_mode = 0020666;
        to_lx_stat(o, &k);
        o->st_rdev = (dk == DEV_NULL) ? 0x103 : (dk == DEV_ZERO) ? 0x105 : (dk == DEV_URANDOM) ? 0x109 : 0x500;
        return 0;
    }
    if (sys_stat(full, &k) < 0) {
        if (!strcmp(full, "/")) { k.st_mode = 0040755; k.st_nlink = 2; to_lx_stat(o, &k); return 0; }
        return -ENOENT;
    }
    to_lx_stat(o, &k);
    return 0;
}

static int64_t lx_stat_common(int dirfd, uint64_t upath, uint64_t ubuf)
{
    char full[MAX_FILENAME];
    int r = lx_path(dirfd, upath, full);
    if (r < 0) return r;
    struct lx_stat o;
    int64_t rr = stat_path(full, &o);
    if (rr < 0) return rr;
    return ucopy_to(ubuf, &o, sizeof(o));
}

static int64_t lx_fstat(int fd, uint64_t ubuf)
{
    struct file *f = fd_get(fd);
    struct lx_stat o;
    struct stat k;
    if (!f) return -EBADF;
    memset(&k, 0, sizeof(k));
    if (f->type == FILE_TYPE_TTY || f->type == FILE_TYPE_DEV) {
        k.st_dev = 5; k.st_ino = 3; k.st_nlink = 1;
        k.st_mode = (f->type == FILE_TYPE_DEV) ? 0020666 : 0020620;
        to_lx_stat(&o, &k);
        o.st_rdev = 0x500;
        return ucopy_to(ubuf, &o, sizeof(o));
    }
    if (f->type == FILE_TYPE_PIPE) {
        k.st_dev = 6; k.st_ino = 4; k.st_nlink = 1; k.st_mode = 0010600;
        to_lx_stat(&o, &k);
        return ucopy_to(ubuf, &o, sizeof(o));
    }
    if (f->path[0]) {
        int64_t rr = stat_path(f->path, &o);
        if (rr < 0) return rr;
        return ucopy_to(ubuf, &o, sizeof(o));
    }
    return -EBADF;
}

/* ---- open ---- */
#define LX_O_CREAT     0100
#define LX_O_EXCL      0200
#define LX_O_TRUNC     01000
#define LX_O_APPEND    02000
#define LX_O_DIRECTORY 0200000
#define LX_O_CLOEXEC   02000000

static int64_t lx_open(int dirfd, uint64_t upath, int flags, int mode)
{
    (void)mode;
    char full[MAX_FILENAME];
    int r = lx_path(dirfd, upath, full);
    if (r < 0) return r;

    int dk = dev_kind(full);
    int fd;
    if (dk) {
        fd = fd_alloc(0);
        if (fd < 0) return -EMFILE;
        struct file *f = &current->files->f[fd];
        memset(f, 0, sizeof(*f));
        f->type = (dk == DEV_TTY) ? FILE_TYPE_TTY : FILE_TYPE_DEV;
        f->sock_id = dk;
        f->in_use = 1;
        f->mode = 3;
        strncpy(f->path, full, sizeof(f->path) - 1);
    } else if (dirent_isdir(full)) {
        if ((flags & 3) != 0) return -EISDIR;
        fd = fd_alloc(0);
        if (fd < 0) return -EMFILE;
        struct file *f = &current->files->f[fd];
        memset(f, 0, sizeof(*f));
        f->type = FILE_TYPE_DIR;
        f->in_use = 1;
        f->mode = 1;
        strncpy(f->path, full, sizeof(f->path) - 1);
    } else {
        if (flags & LX_O_DIRECTORY) return -ENOTDIR;
        struct stat k;
        memset(&k, 0, sizeof(k));
        int exists = (sys_stat(full, &k) == 0);
        if ((flags & LX_O_CREAT) && (flags & LX_O_EXCL) && exists) return -EEXIST;
        if (!exists && !(flags & LX_O_CREAT)) return -ENOENT;
        int lf = flags & (3 | LX_O_CREAT | LX_O_TRUNC | LX_O_APPEND);
        int64_t n = sys_open(full, lf);
        if (n < 0) return exists ? -EACCES : -ENOENT;
        fd = fd_lowest((int)n);
        strncpy(current->files->f[fd].path, full, sizeof(current->files->f[fd].path) - 1);
        current->files->f[fd].path[sizeof(current->files->f[fd].path) - 1] = 0;
    }
    if (flags & LX_O_CLOEXEC) current->cloexec |= (1ULL << fd);
    else current->cloexec &= ~(1ULL << fd);
    return fd;
}

static int64_t lx_close(int fd)
{
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    current->cloexec &= ~(1ULL << fd);
    if (f->type == FILE_TYPE_DIR || f->type == FILE_TYPE_DEV || f->type == FILE_TYPE_TTY) {
        memset(f, 0, sizeof(*f));
        return 0;
    }
    int64_t r = sys_close(fd);
    f->path[0] = 0;
    return r < 0 ? -EBADF : 0;
}

static int64_t lx_read(int fd, uint64_t ubuf, uint64_t count)
{
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (count == 0) return 0;
    if (!user_access_ok(ubuf, count, 1)) return -EFAULT;
    switch (f->type) {
    case FILE_TYPE_TTY: return tty_read((char *)ubuf, count);
    case FILE_TYPE_DEV:
        if (f->sock_id == DEV_NULL) return 0;
        if (f->sock_id == DEV_ZERO) { memset((void *)ubuf, 0, count); return (int64_t)count; }
        if (f->sock_id == DEV_URANDOM) {
            uint8_t *p = (uint8_t *)ubuf;
            for (uint64_t i = 0; i < count; i++) p[i] = (uint8_t)krand();
            return (int64_t)count;
        }
        return -EIO;
    case FILE_TYPE_DIR: return -EISDIR;
    case FILE_TYPE_SOCKET: return sock_recv(f->sock_id, (void *)ubuf, count, 0, NULL, NULL);
    case FILE_TYPE_PIPE: {
        int64_t r = pipe_read(f, (char *)ubuf, count);
        if (r < 0) return sig_pending_for(current) ? -EINTR : -EIO;
        return r;
    }
    }
    int64_t r = sys_file_read(fd, (char *)ubuf, count);
    return r < 0 ? -EIO : r;
}

static int64_t lx_write(int fd, uint64_t ubuf, uint64_t count)
{
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (count == 0) return 0;
    if (!user_access_ok(ubuf, count, 0)) return -EFAULT;
    switch (f->type) {
    case FILE_TYPE_TTY: return tty_write((const char *)ubuf, count);
    case FILE_TYPE_DEV: return (int64_t)count;
    case FILE_TYPE_DIR: return -EISDIR;
    case FILE_TYPE_SOCKET: {
        int64_t r = sock_send(f->sock_id, (const void *)ubuf, count, 0, 0, 0, 0);
        if (r == -EPIPE) send_signal(current, SIGPIPE);
        return r;
    }
    case FILE_TYPE_PIPE: {
        int64_t r = pipe_write(f, (const char *)ubuf, count);
        if (r < 0) { send_signal(current, SIGPIPE); return -EPIPE; }
        return r;
    }
    }
    int64_t r = sys_file_write(fd, (const char *)ubuf, count);
    return r < 0 ? -EIO : r;
}

struct lx_iovec { uint64_t base, len; };

static int64_t lx_rwv(int fd, uint64_t uiov, int cnt, int wr)
{
    if (cnt < 0 || cnt > 1024) return -EINVAL;
    int64_t total = 0;
    for (int i = 0; i < cnt; i++) {
        struct lx_iovec v;
        if (ucopy_from(&v, uiov + (uint64_t)i * sizeof(v), sizeof(v)) < 0) return total ? total : -EFAULT;
        if (v.len == 0) continue;
        int64_t r = wr ? lx_write(fd, v.base, v.len) : lx_read(fd, v.base, v.len);
        if (r < 0) return total ? total : r;
        total += r;
        if ((uint64_t)r < v.len) break;
    }
    return total;
}

static int64_t lx_lseek(int fd, int64_t off, int whence)
{
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->type == FILE_TYPE_PIPE || f->type == FILE_TYPE_TTY || f->type == FILE_TYPE_SOCKET) return -ESPIPE;
    if (f->type == FILE_TYPE_DEV) return 0;
    if (f->type == FILE_TYPE_DIR) {
        if (whence == 0 && off == 0) { f->pos = 0; return 0; }
        return -EINVAL;
    }
    int64_t r = sys_lseek(fd, off, whence);
    return r < 0 ? -EINVAL : r;
}

static int64_t lx_dupfd(int oldfd, int minfd, int cloexec)
{
    if (!fd_get(oldfd)) return -EBADF;
    int nfd = fd_alloc(minfd);
    if (nfd < 0) return -EMFILE;
    if (sys_dup2(oldfd, nfd) < 0) return -EBADF;
    if (cloexec) current->cloexec |= (1ULL << nfd); else current->cloexec &= ~(1ULL << nfd);
    return nfd;
}

static int64_t lx_dup2(int oldfd, int newfd, int cloexec)
{
    if (!fd_get(oldfd)) return -EBADF;
    if (newfd < 0 || newfd >= NR_OPEN) return -EBADF;
    if (oldfd == newfd) return newfd;
    if (fd_get(newfd)) lx_close(newfd);
    if (sys_dup2(oldfd, newfd) < 0) return -EBADF;
    if (cloexec) current->cloexec |= (1ULL << newfd); else current->cloexec &= ~(1ULL << newfd);
    return newfd;
}

static int64_t lx_fcntl(int fd, int cmd, uint64_t arg)
{
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    switch (cmd) {
    case 0:    return lx_dupfd(fd, (int)arg, 0);               /* F_DUPFD */
    case 1030: return lx_dupfd(fd, (int)arg, 1);               /* F_DUPFD_CLOEXEC */
    case 1:    return (current->cloexec >> fd) & 1;            /* F_GETFD */
    case 2:    if (arg & 1) current->cloexec |= (1ULL << fd); else current->cloexec &= ~(1ULL << fd);
               return 0;                                       /* F_SETFD */
    case 3:    if (f->type == FILE_TYPE_SOCKET) return 2 | (sock_get_nonblock(f->sock_id) ? 04000 : 0);
               return (f->mode == 1 ? 0 : f->mode == 2 ? 1 : 2); /* F_GETFL */
    case 4:    if (f->type == FILE_TYPE_SOCKET) sock_set_nonblock(f->sock_id, (arg & 04000) != 0);
               return 0;                                       /* F_SETFL */
    case 5: case 6: case 7: return 0;                          /* F_GETLK/SETLK/SETLKW */
    }
    return -EINVAL;
}

static int64_t lx_pipe2(uint64_t upfd, int flags)
{
    if (!user_access_ok(upfd, 8, 1)) return -EFAULT;
    int p[2];
    if (sys_pipe(p) < 0) return -EMFILE;
    int a = fd_lowest(p[0]);
    int b = fd_lowest(p[1]);
    p[0] = a; p[1] = b;
    if (flags & LX_O_CLOEXEC) current->cloexec |= (1ULL << a) | (1ULL << b);
    else current->cloexec &= ~((1ULL << a) | (1ULL << b));
    memcpy((void *)upfd, p, 8);
    return 0;
}

static int64_t lx_ioctl(int fd, uint64_t req, uint64_t arg)
{
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->type == FILE_TYPE_TTY || (f->type == FILE_TYPE_DEV && f->sock_id == DEV_TTY))
        return tty_ioctl(req, arg);
    if (f->type == FILE_TYPE_SOCKET) {
        if (req == 0x5421) { int v = 0; int r = ucopy_from(&v, arg, 4); if (r < 0) return r; return sock_set_nonblock(f->sock_id, v != 0); }
        if (req == 0x541B) { int v = sock_unread(f->sock_id); return ucopy_to(arg, &v, 4); }
        return -ENOTTY;
    }
    if (req == 0x541B && f->type == FILE_TYPE_PIPE) { int z = 0; return ucopy_to(arg, &z, 4); }
    return -ENOTTY;
}

/* ---- getdents64 ---- */
struct lx_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

#define GETDENTS_PAGES 8      /* 32 КБ: список каталога на тысячу файлов */

static int64_t lx_getdents64_impl(int fd, uint64_t ubuf, uint64_t count, char *list, uint64_t lsize)
{
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->type != FILE_TYPE_DIR) return -ENOTDIR;
    if (!user_access_ok(ubuf, count, 1)) return -EFAULT;

    int64_t n = sys_list(f->path, list, lsize, 0);
    if (n < 0) n = 0;
    list[n < (int64_t)lsize ? n : (int64_t)lsize - 1] = 0;

    uint64_t out = 0;
    uint64_t idx = 0;                       /* номер записи: ".", "..", затем список */
    const char *p = list;
    for (;;) {
        char name[MAX_FILENAME];
        int type = 8;
        if (idx == 0) { strcpy(name, "."); type = 4; }
        else if (idx == 1) { strcpy(name, ".."); type = 4; }
        else {
            if (!*p) break;
            int l = 0;
            while (*p && *p != '\n' && l < MAX_FILENAME - 1) name[l++] = *p++;
            if (*p == '\n') p++;
            if (l > 0 && name[l - 1] == '/') { l--; type = 4; }
            name[l] = 0;
            if (l == 0) continue;
        }
        if (idx >= f->pos) {
            uint64_t nl = strlen(name);
            uint64_t reclen = (19 + nl + 1 + 7) & ~7ULL;
            if (out + reclen > count) {
                if (out == 0) return -EINVAL;
                break;
            }
            struct lx_dirent64 *d = (struct lx_dirent64 *)(ubuf + out);
            uint64_t h = 5381;
            for (const char *c = name; *c; c++) h = h * 33 + (uint8_t)*c;
            d->d_ino = (h & 0xFFFFFFFFULL) | 1;
            d->d_off = (int64_t)(idx + 1);
            d->d_reclen = (uint16_t)reclen;
            d->d_type = (uint8_t)type;
            memcpy(d->d_name, name, nl + 1);
            out += reclen;
            f->pos = idx + 1;
        }
        idx++;
    }
    return (int64_t)out;
}

static int64_t lx_getdents64(int fd, uint64_t ubuf, uint64_t count)
{
    uint64_t lpage = get_free_pages(GETDENTS_PAGES);
    if (!lpage) return -ENOMEM;
    int64_t r = lx_getdents64_impl(fd, ubuf, count, (char *)lpage, GETDENTS_PAGES * PAGE_SIZE);
    free_pages(lpage, GETDENTS_PAGES);
    return r;
}

/* ---- файловые операции с путями ---- */
static int64_t lx_access(int dirfd, uint64_t upath)
{
    char full[MAX_FILENAME];
    int r = lx_path(dirfd, upath, full);
    if (r < 0) return r;
    if (dev_kind(full)) return 0;
    struct stat k;
    memset(&k, 0, sizeof(k));
    if (sys_stat(full, &k) == 0) return 0;
    return !strcmp(full, "/") ? 0 : -ENOENT;
}

static int64_t lx_chdir_path(const char *full)
{
    if (!dirent_isdir(full)) {
        struct stat k;
        memset(&k, 0, sizeof(k));
        return sys_stat(full, &k) == 0 ? -ENOTDIR : -ENOENT;
    }
    if (sys_chdir(full) < 0) return -ENOENT;
    return 0;
}

static int64_t lx_mkdir(int dirfd, uint64_t upath)
{
    char full[MAX_FILENAME];
    int r = lx_path(dirfd, upath, full);
    if (r < 0) return r;
    struct stat k;
    memset(&k, 0, sizeof(k));
    if (sys_stat(full, &k) == 0) return -EEXIST;
    return sys_mkdir(full) < 0 ? -EACCES : 0;
}

static int64_t lx_unlink(int dirfd, uint64_t upath, int flags)
{
    char full[MAX_FILENAME];
    int r = lx_path(dirfd, upath, full);
    if (r < 0) return r;
    if (flags & 0x200) {                    /* AT_REMOVEDIR */
        if (!dirent_isdir(full)) return -ENOTDIR;
        return sys_rmdir(full) < 0 ? -ENOTEMPTY : 0;
    }
    if (dirent_isdir(full)) return -EISDIR;
    struct stat k;
    memset(&k, 0, sizeof(k));
    if (sys_stat(full, &k) < 0) return -ENOENT;
    return sys_unlink(full) < 0 ? -EACCES : 0;
}

static int64_t lx_rename(int odir, uint64_t uold, int ndir, uint64_t unew)
{
    char a[MAX_FILENAME], b[MAX_FILENAME];
    int r = lx_path(odir, uold, a);
    if (r < 0) return r;
    r = lx_path(ndir, unew, b);
    if (r < 0) return r;
    struct stat k;
    memset(&k, 0, sizeof(k));
    if (sys_stat(a, &k) < 0) return -ENOENT;
    return fs_rename(a, b) < 0 ? -EXDEV : 0;   /* mv при EXDEV копирует и удаляет */
}

static int64_t lx_chmod(int dirfd, uint64_t upath, int mode)
{
    char full[MAX_FILENAME];
    int r = lx_path(dirfd, upath, full);
    if (r < 0) return r;
    if (dev_kind(full) || dirent_isdir(full)) return 0;
    sys_chmod(full, mode & 07777);
    return 0;
}

/* ===================================================================== */
/*  Память                                                                */
/* ===================================================================== */

#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20

static uint32_t lx_prot(int prot)
{
    uint32_t p = 0;
    if (prot & 1) p |= VM_READ;
    if (prot & 2) p |= VM_WRITE | VM_READ;
    if (prot & 4) p |= VM_EXEC | VM_READ;
    return p;
}

static int64_t lx_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, int64_t off)
{
    if (len == 0) return -EINVAL;
    len = PAGE_ALIGN(len);
    uint64_t start;
    if (flags & MAP_FIXED) {
        if ((addr & (PAGE_SIZE - 1)) || !user_range_ok(addr, len)) return -EINVAL;
        start = addr;
        vma_unmap(current, start, start + len);
    } else {
        start = vma_find_free(current, len);
        if (!user_range_ok(start, len)) return -ENOMEM;
    }
    struct file *f = NULL;
    if (!(flags & MAP_ANONYMOUS)) {
        f = fd_get(fd);
        if (!f) return -EBADF;
        if (f->type != FILE_TYPE_REGULAR && f->type != FILE_TYPE_MINIX) return -EACCES;
    }
    uint32_t vp = lx_prot(prot);
    uint32_t tmp = f ? (vp | VM_WRITE | VM_READ) : vp;   /* файл копируется, нужна запись */
    if (vma_add(current, start, start + len, tmp, VM_ANON) < 0) return -ENOMEM;
    if (f) {
        uint64_t save = f->pos;
        f->pos = (uint64_t)off;
        uint64_t done = 0;
        while (done < len) {
            int64_t r = sys_file_read(fd, (char *)(start + done), len - done);
            if (r <= 0) break;
            done += (uint64_t)r;
        }
        f->pos = save;
        if (tmp != vp) vma_protect(current, start, start + len, vp);
    }
    return (int64_t)start;
}

/* ===================================================================== */
/*  Сигналы                                                               */
/* ===================================================================== */

#define SA_NODEFER   0x40000000ULL
#define SA_RESETHAND 0x80000000ULL

struct lx_ksigaction {
    uint64_t handler, flags, restorer, mask;
};

static int64_t lx_rt_sigaction(int sig, uint64_t uact, uint64_t uold)
{
    if (sig <= 0 || sig >= 32 || sig == 9 || sig == 19) return -EINVAL;
    if (uold) {
        struct lx_ksigaction o;
        o.handler = current->lx_sa[sig].handler;
        o.flags = current->lx_sa[sig].flags;
        o.restorer = current->lx_sa[sig].restorer;
        o.mask = current->lx_sa[sig].mask;
        int r = ucopy_to(uold, &o, sizeof(o));
        if (r < 0) return r;
    }
    if (uact) {
        struct lx_ksigaction a;
        int r = ucopy_from(&a, uact, sizeof(a));
        if (r < 0) return r;
        current->lx_sa[sig].handler = a.handler;
        current->lx_sa[sig].flags = a.flags;
        current->lx_sa[sig].restorer = a.restorer;
        current->lx_sa[sig].mask = a.mask;
    }
    return 0;
}

static int64_t lx_rt_sigprocmask(int how, uint64_t uset, uint64_t uold, uint64_t sz)
{
    if (sz != 8) return -EINVAL;
    if (uold) {
        uint64_t m = current->sigmask >> 1;          /* бит n-1 = сигнал n (формат Linux) */
        int r = ucopy_to(uold, &m, 8);
        if (r < 0) return r;
    }
    if (uset) {
        uint64_t s;
        int r = ucopy_from(&s, uset, 8);
        if (r < 0) return r;
        s <<= 1;
        s &= ~((1ULL << 9) | (1ULL << 19));
        if (how == 0) current->sigmask |= s;
        else if (how == 1) current->sigmask &= ~s;
        else if (how == 2) current->sigmask = s;
        else return -EINVAL;
    }
    return 0;
}

struct lx_sigframe {
    uint64_t retaddr;               /* адрес возврата = restorer (F, F % 16 == 8) */
    uint64_t magic;
    struct trap_frame saved;
    uint64_t saved_mask;
    uint8_t  siginfo[128];
};

#define SIGFRAME_MAGIC 0x4C58534947465231ULL

static int lx_default_ignored(int sig)
{
    return sig == 17 || sig == 23 || sig == 28 || sig == 18 || sig == 19 || sig == 20 || sig == 21 || sig == 22;
}

/* Доставить один ожидающий сигнал: настроить tf на запуск обработчика. 1 = доставлен. */
static int lx_deliver(struct trap_frame *tf)
{
    uint32_t pend = current->signal & ~(uint32_t)current->sigmask;
    if (current->signal & (1U << 9)) pend |= (1U << 9);
    if (!pend) return 0;
    for (int sig = 1; sig < 32; sig++) {
        if (!(pend & (1U << sig))) continue;
        current->signal &= ~(1U << sig);

        uint64_t h = current->lx_sa[sig].handler;
        if (sig == 9) { sys_exit_group(128 + 9); return 0; }
        if (h == 1) continue;                                  /* SIG_IGN */
        if (h == 0) {                                          /* SIG_DFL */
            if (lx_default_ignored(sig)) continue;
            sys_exit_group(128 + sig);
            return 0;
        }

        struct lx_sigframe fr;
        memset(&fr, 0, sizeof(fr));
        fr.magic = SIGFRAME_MAGIC;
        fr.saved = *tf;
        fr.saved_mask = current->sigmask;
        fr.retaddr = current->lx_sa[sig].restorer;
        *(int32_t *)&fr.siginfo[0] = sig;

        uint64_t sp = tf->rsp - 128;                           /* red zone */
        sp -= sizeof(fr);
        sp &= ~0xFULL;
        sp -= 8;                                               /* sp % 16 == 8 на входе в обработчик */
        if (!user_access_ok(sp, sizeof(fr), 1)) { sys_exit_group(128 + 11); return 0; }
        memcpy((void *)sp, &fr, sizeof(fr));

        current->sigmask |= current->lx_sa[sig].mask;
        if (!(current->lx_sa[sig].flags & SA_NODEFER)) current->sigmask |= (1ULL << sig);
        if (current->lx_sa[sig].flags & SA_RESETHAND) current->lx_sa[sig].handler = 0;

        tf->rip = h;
        tf->rsp = sp;
        tf->rdi = (uint64_t)sig;
        tf->rsi = sp + (uint64_t)&((struct lx_sigframe *)0)->siginfo;
        tf->rdx = sp + 16;
        tf->rax = 0;
        return 1;
    }
    return 0;
}

/* rt_sigreturn: после ret из обработчика rsp = F + 8 */
static int64_t lx_sigreturn(struct trap_frame *tf)
{
    uint64_t f = tf->rsp - 8;
    struct lx_sigframe fr;
    if (ucopy_from(&fr, f, sizeof(fr)) < 0 || fr.magic != SIGFRAME_MAGIC) {
        sys_exit_group(128 + 11);
        return 0;
    }
    uint64_t rip = fr.saved.rip, rsp = fr.saved.rsp;
    if (!user_range_ok(rip, 1) || !user_range_ok(rsp - 8, 8)) { sys_exit_group(128 + 11); return 0; }
    *tf = fr.saved;
    tf->cs = 0x23;
    tf->ss = 0x1B;
    tf->rflags = (fr.saved.rflags & 0xCD5ULL) | 0x202;        /* только арифметические флаги + IF */
    current->sigmask = fr.saved_mask;
    return (int64_t)tf->rax;
}

/* ===================================================================== */
/*  Процессы, время, прочее                                               */
/* ===================================================================== */

static int64_t lx_fork(struct trap_frame *tf, uint64_t child_stack)
{
    int64_t pid = sys_fork(tf);
    if (pid < 0) return -EAGAIN;
    if (child_stack) {
        struct trap_frame *ctf = (struct trap_frame *)(task[pid]->kstack + KSTACK_SIZE - sizeof(struct trap_frame));
        ctf->rsp = child_stack;
    }
    return pid;
}

/* Код выхода ядра (exit status или 128+сигнал) -> статус wait4 */
static int encode_status(int code)
{
    if (code >= 129 && code <= 159) return code - 128;       /* убит сигналом */
    return (code & 0xFF) << 8;
}

static int64_t lx_wait4(int64_t pid, uint64_t ustatus, int options)
{
    int64_t want = pid > 0 ? pid : -1;
    if (ustatus && !user_access_ok(ustatus, 4, 1)) return -EFAULT;
    for (;;) {
        int st = 0;
        int64_t r = sys_waitpid(want, &st, 1);
        if (r > 0) {
            if (ustatus) *(int *)ustatus = encode_status(st);
            return r;
        }
        if (r < 0) return -ECHILD;
        if (options & 1) return 0;                           /* WNOHANG */
        if (sig_pending_for(current)) return -EINTR;
        __asm__ volatile ("sti");
        schedule();
    }
}

static int64_t lx_execve(struct trap_frame *tf, uint64_t ufile, uint64_t uargv, uint64_t uenvp)
{
    char path[MAX_FILENAME];
    int r = ustr(ufile, path, sizeof(path));
    if (r < 0) return r;
    if (sys_execve(path, (char **)uargv, (char **)uenvp, tf) < 0) return -ENOENT;
    return 0;
}

static struct task_struct *task_by_pid(int64_t pid)
{
    if (pid <= 0 || pid >= NR_TASKS) return NULL;
    return task[pid];
}

static int64_t lx_kill(int64_t pid, int sig)
{
    if (sig < 0 || sig > 31) return -EINVAL;
    int found = 0;
    if (pid > 0) {
        struct task_struct *t = task_by_pid(pid);
        if (t && t->is_thread && task[t->tgid]) t = task[t->tgid];      /* сигнал процессу -> лидеру */
        if (!t || t->state == TASK_ZOMBIE) return -ESRCH;
        if (sig) send_signal(t, sig);
        return 0;
    }
    for (int i = 2; i < NR_TASKS; i++) {
        struct task_struct *t = task[i];
        if (!t || t->state == TASK_ZOMBIE || t->is_thread || !t->mm->vmas) continue;
        int match;
        if (pid == 0) match = pgrp_of(t) == pgrp_of(current);
        else if (pid == -1) match = (t != current);
        else match = pgrp_of(t) == (int)(-pid);
        if (match) { found = 1; if (sig) send_signal(t, sig); }
    }
    return found ? 0 : -ESRCH;
}

/* tkill/tgkill: сигнал конкретному потоку */
static int64_t lx_tkill(int64_t tid, int sig)
{
    if (sig < 0 || sig > 31) return -EINVAL;
    struct task_struct *t = task_by_pid(tid);
    if (!t || t->state == TASK_ZOMBIE) return -ESRCH;
    if (sig) send_signal(t, sig);
    return 0;
}

static uint64_t boot_off_us = 0;

static void now_ts(int clk, int64_t *sec, int64_t *nsec)
{
    uint64_t us = time_us();
    if (clk == 0 || clk == 5 || clk == 8) {                  /* REALTIME */
        if (!boot_off_us) boot_off_us = us - jiffies * 10000;
        uint64_t rt = startup_time * 1000000ULL + (us - boot_off_us);
        *sec = (int64_t)(rt / 1000000ULL);
        *nsec = (int64_t)(rt % 1000000ULL) * 1000;
    } else {
        *sec = (int64_t)(us / 1000000ULL);
        *nsec = (int64_t)(us % 1000000ULL) * 1000;
    }
}

static int64_t sleep_us(uint64_t us)
{
    uint64_t end = time_us() + us;
    for (;;) {
        if (sig_pending_for(current)) return -EINTR;
        if (time_us() >= end) return 0;
        sched_yield_sleep();
    }
}

static int64_t lx_nanosleep(uint64_t ureq, uint64_t urem)
{
    int64_t ts[2];
    int r = ucopy_from(ts, ureq, 16);
    if (r < 0) return r;
    if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000) return -EINVAL;
    int64_t rr = sleep_us((uint64_t)ts[0] * 1000000ULL + (uint64_t)ts[1] / 1000);
    if (rr < 0 && urem) { int64_t z[2] = { 0, 0 }; ucopy_to(urem, z, 16); }
    return rr;
}

/* ---- poll / select ---- */
extern int keyboard_pending(void);

static int tty_ready_or_pending(void)
{
    return tty_ready || tty_eof || keyboard_pending();
}

/* Маска готовности (POLLIN=1, POLLOUT=4, POLLERR=8, POLLHUP=0x10); -1 -- дескриптор не открыт */
static int fd_pollmask(int fd)
{
    struct file *f = fd_get(fd);
    if (!f) return -1;
    switch (f->type) {
    case FILE_TYPE_TTY: return (tty_ready_or_pending() ? 1 : 0) | 4;
    case FILE_TYPE_SOCKET: return sock_poll(f->sock_id);
    case FILE_TYPE_PIPE: {
        struct pipe *pp = f->pipe;
        if (!pp) return 0x10;
        if (f->mode == 1) return (pp->count > 0 ? 1 : 0) | (pp->writers <= 0 ? 0x10 : 0);
        return (pp->count < PIPE_BUF_SIZE ? 4 : 0) | (pp->readers <= 0 ? 8 : 0);
    }
    }
    return 5;
}

static int fd_readable(int fd)
{
    int m = fd_pollmask(fd);
    return m < 0 ? -1 : (m & 0x19) != 0;
}

static int fd_writable(int fd)
{
    int m = fd_pollmask(fd);
    return m < 0 ? 1 : (m & 0x0C) != 0;
}

struct lx_pollfd { int fd; int16_t events, revents; };

static int64_t lx_poll(uint64_t ufds, uint64_t nfds, int timeout_ms)
{
    if (nfds > 64) return -EINVAL;
    struct lx_pollfd p[64];
    int r = ucopy_from(p, ufds, nfds * sizeof(p[0]));
    if (r < 0) return r;
    uint64_t end = timeout_ms < 0 ? 0 : time_us() + (uint64_t)timeout_ms * 1000;
    for (;;) {
        net_poll();
        int ready = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            p[i].revents = 0;
            if (p[i].fd < 0) continue;
            int m = fd_pollmask(p[i].fd);
            if (m < 0) { p[i].revents = 0x20; ready++; continue; }       /* POLLNVAL */
            p[i].revents = (int16_t)(m & (p[i].events | 0x18));
            if (p[i].revents) ready++;
        }
        if (ready || timeout_ms == 0) {
            ucopy_to(ufds, p, nfds * sizeof(p[0]));
            return ready;
        }
        if (sig_pending_for(current)) return -EINTR;
        if (timeout_ms > 0 && time_us() >= end) { ucopy_to(ufds, p, nfds * sizeof(p[0])); return 0; }
        __asm__ volatile ("sti; hlt");
    }
}

static int64_t lx_select(int n, uint64_t urd, uint64_t uwr, uint64_t uex, int64_t timeout_us)
{
    if (n < 0 || n > 64) return -EINVAL;
    uint64_t rd = 0, wr = 0;
    if (urd && ucopy_from(&rd, urd, 8) < 0) return -EFAULT;
    if (uwr && ucopy_from(&wr, uwr, 8) < 0) return -EFAULT;
    uint64_t mask = n >= 64 ? ~0ULL : ((1ULL << n) - 1);
    rd &= mask; wr &= mask;
    uint64_t end = timeout_us < 0 ? 0 : time_us() + (uint64_t)timeout_us;
    for (;;) {
        net_poll();
        uint64_t rrd = 0, rwr = 0;
        int cnt = 0;
        for (int i = 0; i < n; i++) {
            if (rd & (1ULL << i)) {
                int x = fd_readable(i);
                if (x < 0) return -EBADF;
                if (x) { rrd |= 1ULL << i; cnt++; }
            }
            if ((wr & (1ULL << i)) && fd_writable(i)) { rwr |= 1ULL << i; cnt++; }
        }
        if (cnt || timeout_us == 0) {
            if (urd) ucopy_to(urd, &rrd, 8);
            if (uwr) ucopy_to(uwr, &rwr, 8);
            if (uex) { uint64_t z = 0; ucopy_to(uex, &z, 8); }
            return cnt;
        }
        if (sig_pending_for(current)) return -EINTR;
        if (timeout_us > 0 && time_us() >= end) {
            uint64_t z = 0;
            if (urd) ucopy_to(urd, &z, 8);
            if (uwr) ucopy_to(uwr, &z, 8);
            if (uex) ucopy_to(uex, &z, 8);
            return 0;
        }
        __asm__ volatile ("sti; hlt");
    }
}


/* ===================================================================== */
/*  Сокеты                                                                */
/* ===================================================================== */

struct lx_sockaddr_in { uint16_t family; uint16_t port; uint32_t addr; uint8_t zero[8]; };
struct lx_msghdr { uint64_t name; uint32_t namelen, pad; uint64_t iov; uint64_t iovlen; uint64_t ctl; uint64_t ctllen; int flags; };

static struct file *sock_file(int fd)
{
    struct file *f = fd_get(fd);
    return (f && f->type == FILE_TYPE_SOCKET) ? f : NULL;
}

static int sock_fd_new(int id, int type)
{
    int fd = fd_alloc(0);
    if (fd < 0) { sock_close(id); return -EMFILE; }
    struct file *f = &current->files->f[fd];
    memset(f, 0, sizeof(*f));
    f->type = FILE_TYPE_SOCKET;
    f->sock_id = id;
    f->in_use = 1;
    f->mode = 3;
    if (type & 0x80000) current->cloexec |= (1ULL << fd); else current->cloexec &= ~(1ULL << fd);
    return fd;
}

static int sa_get(uint64_t ua, uint64_t len, uint32_t *ip, uint16_t *port)
{
    struct lx_sockaddr_in sa;
    if (!ua || len < 8) return -EINVAL;
    if (len > sizeof(sa)) len = sizeof(sa);
    memset(&sa, 0, sizeof(sa));
    int r = ucopy_from(&sa, ua, len);
    if (r < 0) return r;
    if (sa.family != 2) return sa.family == 1 ? -ENOENT : -EAFNOSUPPORT;
    *ip = ntohl(sa.addr);
    *port = ntohs(sa.port);
    return 0;
}

static int sa_put(uint64_t ua, uint64_t ulen, uint32_t ip, uint16_t port)
{
    if (!ua) return 0;
    struct lx_sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.family = 2; sa.port = htons(port); sa.addr = htonl(ip);
    uint32_t have = 0;
    if (ulen) { int r = ucopy_from(&have, ulen, 4); if (r < 0) return r; }
    uint32_t n = have < sizeof(sa) ? have : (uint32_t)sizeof(sa);
    int r = ucopy_to(ua, &sa, n);
    if (r < 0) return r;
    if (ulen) { uint32_t full = sizeof(sa); return ucopy_to(ulen, &full, 4); }
    return 0;
}

static int64_t lx_socket(int domain, int type, int proto)
{
    int id = sock_create(domain, type, proto);
    if (id < 0) return id;
    return sock_fd_new(id, type);
}

static int64_t lx_socketpair(int domain, int type, int proto, uint64_t usv)
{
    (void)proto;
    if (domain != 1) return -EAFNOSUPPORT;
    if (!user_access_ok(usv, 8, 1)) return -EFAULT;
    int ids[2];
    int r = sock_pair(type & 0xF, ids);
    if (r < 0) return r;
    if (type & 0x800) { sock_set_nonblock(ids[0], 1); sock_set_nonblock(ids[1], 1); }
    int a = sock_fd_new(ids[0], type);
    if (a < 0) { sock_close(ids[1]); return a; }
    int b = sock_fd_new(ids[1], type);
    if (b < 0) { lx_close(a); return b; }
    int sv[2] = { a, b };
    return ucopy_to(usv, sv, 8);
}

static int64_t lx_connect(int fd, uint64_t ua, uint64_t len)
{
    struct file *f = sock_file(fd);
    if (!f) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    uint32_t ip; uint16_t port;
    int r = sa_get(ua, len, &ip, &port);
    if (r < 0) return r;
    return sock_connect(f->sock_id, ip, port);
}

static int64_t lx_bind(int fd, uint64_t ua, uint64_t len)
{
    struct file *f = sock_file(fd);
    if (!f) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    uint32_t ip; uint16_t port;
    int r = sa_get(ua, len, &ip, &port);
    if (r < 0) return r == -ENOENT ? -EACCES : r;
    return sock_bind(f->sock_id, ip, port);
}

static int64_t lx_listen(int fd, int backlog)
{
    struct file *f = sock_file(fd);
    if (!f) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    return sock_listen(f->sock_id, backlog);
}

static int64_t lx_accept(int fd, uint64_t ua, uint64_t ulen, int flags)
{
    struct file *f = sock_file(fd);
    if (!f) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    uint32_t ip = 0; uint16_t port = 0;
    int id = sock_accept(f->sock_id, &ip, &port);
    if (id < 0) return id;
    if (flags & 0x800) sock_set_nonblock(id, 1);
    int nfd = sock_fd_new(id, flags);
    if (nfd < 0) return nfd;
    sa_put(ua, ulen, ip, port);
    return nfd;
}

static int64_t lx_sendto(int fd, uint64_t ubuf, uint64_t len, int flags, uint64_t ua, uint64_t alen)
{
    struct file *f = sock_file(fd);
    if (!f) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    if (len && !user_access_ok(ubuf, len, 0)) return -EFAULT;
    uint32_t ip = 0; uint16_t port = 0; int has = 0;
    if (ua) { int r = sa_get(ua, alen, &ip, &port); if (r < 0) return r; has = 1; }
    int64_t r = sock_send(f->sock_id, (const void *)ubuf, len, flags, ip, port, has);
    if (r == -EPIPE && !(flags & 0x4000)) send_signal(current, SIGPIPE);
    return r;
}

static int64_t lx_recvfrom(int fd, uint64_t ubuf, uint64_t len, int flags, uint64_t ua, uint64_t ulen)
{
    struct file *f = sock_file(fd);
    if (!f) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    if (len && !user_access_ok(ubuf, len, 1)) return -EFAULT;
    uint32_t ip = 0; uint16_t port = 0;
    int64_t r = sock_recv(f->sock_id, (void *)ubuf, len, flags, &ip, &port);
    if (r >= 0 && ua && sock_domain(f->sock_id) == 2) sa_put(ua, ulen, ip, port);
    return r;
}

static int64_t lx_sendmsg(int fd, uint64_t umsg, int flags)
{
    struct file *f = sock_file(fd);
    if (!f) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    struct lx_msghdr m;
    int r = ucopy_from(&m, umsg, sizeof(m));
    if (r < 0) return r;
    if (m.iovlen > 64) return -EINVAL;
    uint32_t ip = 0; uint16_t port = 0; int has = 0;
    if (m.name && m.namelen) { r = sa_get(m.name, m.namelen, &ip, &port); if (r < 0) return r; has = 1; }
    int dgram = sock_type(f->sock_id) != SOCK_STREAM_;
    int64_t total = 0;
    if (dgram) {
        uint8_t buf[1500];
        uint64_t n = 0;
        for (uint64_t i = 0; i < m.iovlen; i++) {
            struct lx_iovec v;
            if (ucopy_from(&v, m.iov + i * sizeof(v), sizeof(v)) < 0) return -EFAULT;
            if (n + v.len > sizeof(buf)) return -90;
            if (ucopy_from(buf + n, v.base, v.len) < 0) return -EFAULT;
            n += v.len;
        }
        return sock_send(f->sock_id, buf, n, flags, ip, port, has);
    }
    for (uint64_t i = 0; i < m.iovlen; i++) {
        struct lx_iovec v;
        if (ucopy_from(&v, m.iov + i * sizeof(v), sizeof(v)) < 0) return total ? total : -EFAULT;
        if (!v.len) continue;
        if (!user_access_ok(v.base, v.len, 0)) return total ? total : -EFAULT;
        int64_t w = sock_send(f->sock_id, (const void *)v.base, v.len, flags, 0, 0, 0);
        if (w < 0) {
            if (w == -EPIPE && !(flags & 0x4000)) send_signal(current, SIGPIPE);
            return total ? total : w;
        }
        total += w;
        if ((uint64_t)w < v.len) break;
    }
    return total;
}

static int64_t lx_recvmsg(int fd, uint64_t umsg, int flags)
{
    struct file *f = sock_file(fd);
    if (!f) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    struct lx_msghdr m;
    int r = ucopy_from(&m, umsg, sizeof(m));
    if (r < 0) return r;
    if (m.iovlen > 64) return -EINVAL;
    uint32_t ip = 0; uint16_t port = 0;
    int64_t total = 0;
    int dgram = sock_type(f->sock_id) != SOCK_STREAM_;
    if (dgram) {
        uint8_t buf[1500];
        int64_t n = sock_recv(f->sock_id, buf, sizeof(buf), flags, &ip, &port);
        if (n < 0) return n;
        uint64_t off = 0;
        for (uint64_t i = 0; i < m.iovlen && off < (uint64_t)n; i++) {
            struct lx_iovec v;
            if (ucopy_from(&v, m.iov + i * sizeof(v), sizeof(v)) < 0) return -EFAULT;
            uint64_t c = (uint64_t)n - off < v.len ? (uint64_t)n - off : v.len;
            if (ucopy_to(v.base, buf + off, c) < 0) return -EFAULT;
            off += c;
        }
        total = n;
    } else {
        for (uint64_t i = 0; i < m.iovlen; i++) {
            struct lx_iovec v;
            if (ucopy_from(&v, m.iov + i * sizeof(v), sizeof(v)) < 0) return total ? total : -EFAULT;
            if (!v.len) continue;
            if (!user_access_ok(v.base, v.len, 1)) return total ? total : -EFAULT;
            int64_t n = sock_recv(f->sock_id, (void *)v.base, v.len, total ? (flags | MSG_DONTWAIT_) : flags, NULL, NULL);
            if (n < 0) { if (total && n == -EAGAIN) break; return total ? total : n; }
            total += n;
            if ((uint64_t)n < v.len) break;
        }
    }
    if (m.name && m.namelen && sock_domain(f->sock_id) == 2) {
        uint32_t have = m.namelen;
        struct lx_sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.family = 2; sa.port = htons(port); sa.addr = htonl(ip);
        ucopy_to(m.name, &sa, have < sizeof(sa) ? have : sizeof(sa));
        uint32_t full = sizeof(sa);
        ucopy_to(umsg + 8, &full, 4);
    }
    int z = 0;
    ucopy_to(umsg + 40, &m.ctllen, 0);
    uint64_t zero = 0;
    ucopy_to(umsg + 40, &zero, 8);       /* msg_controllen */
    ucopy_to(umsg + 48, &z, 4);          /* msg_flags */
    return total;
}

static int64_t lx_shutdown(int fd, int how)
{
    struct file *f = sock_file(fd);
    if (!f) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    return sock_shutdown(f->sock_id, how);
}

static int64_t lx_getsockname(int fd, uint64_t ua, uint64_t ulen, int peer)
{
    struct file *f = sock_file(fd);
    if (!f) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    uint32_t ip = 0; uint16_t port = 0;
    int r = sock_name(f->sock_id, peer, &ip, &port);
    if (r < 0) return r;
    return sa_put(ua, ulen, ip, port);
}

static int64_t lx_getsockopt(int fd, int level, int opt, uint64_t uval, uint64_t ulen)
{
    struct file *f = sock_file(fd);
    if (!f) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    int v = 0;
    int r = sock_getsockopt(f->sock_id, level, opt, &v);
    if (r < 0) return r;
    uint32_t have = 0;
    if (ulen && ucopy_from(&have, ulen, 4) < 0) return -EFAULT;
    if (uval && have >= 4) { r = ucopy_to(uval, &v, 4); if (r < 0) return r; have = 4; }
    if (ulen) ucopy_to(ulen, &have, 4);
    return 0;
}

static int64_t lx_setsockopt(int fd, int level, int opt, uint64_t uval, uint64_t len)
{
    struct file *f = sock_file(fd);
    if (!f) return fd_get(fd) ? -ENOTSOCK : -EBADF;
    uint8_t buf[32];
    if (len > sizeof(buf)) len = sizeof(buf);
    if (uval && len && ucopy_from(buf, uval, len) < 0) return -EFAULT;
    return sock_setsockopt(f->sock_id, level, opt, buf, (uint32_t)len);
}

/* ---- разное ---- */
static int64_t lx_uname(uint64_t ubuf)
{
    char u[6][65];
    memset(u, 0, sizeof(u));
    strcpy(u[0], "Linux");
    strcpy(u[1], "linux64");
    strcpy(u[2], "0.01-x86_64");
    strcpy(u[3], "#1 PREEMPT 2026");
    strcpy(u[4], "x86_64");
    strcpy(u[5], "(none)");
    return ucopy_to(ubuf, u, sizeof(u));
}

static int64_t lx_getcwd(uint64_t ubuf, uint64_t size)
{
    uint64_t l = strlen(current->cwd);
    if (size < l + 1) return -ERANGE;
    int r = ucopy_to(ubuf, current->cwd, l + 1);
    return r < 0 ? r : (int64_t)(l + 1);
}

static int64_t lx_prlimit(int res, uint64_t unew, uint64_t uold)
{
    (void)unew;
    if (uold) {
        uint64_t lim[2] = { ~0ULL, ~0ULL };
        if (res == 3) { lim[0] = 8ULL << 20; lim[1] = ~0ULL; }          /* RLIMIT_STACK */
        if (res == 7) { lim[0] = NR_OPEN; lim[1] = NR_OPEN; }           /* RLIMIT_NOFILE */
        return ucopy_to(uold, lim, 16);
    }
    return 0;
}

static int64_t lx_sysinfo(uint64_t ubuf)
{
    uint64_t si[14];
    memset(si, 0, sizeof(si));
    si[0] = time_us() / 1000000ULL;
    si[4] = (uint64_t)get_total_pages_count();
    si[5] = (uint64_t)get_free_pages_count();
    uint16_t procs = 0;
    for (int i = 0; i < NR_TASKS; i++) if (task[i]) procs++;
    si[10] = procs;
    si[13] = 4096;
    return ucopy_to(ubuf, si, 112);
}

static int64_t lx_getrandom(uint64_t ubuf, uint64_t len)
{
    if (len > (1 << 20)) len = 1 << 20;
    if (!user_access_ok(ubuf, len, 1)) return -EFAULT;
    uint8_t *p = (uint8_t *)ubuf;
    for (uint64_t i = 0; i < len; i++) p[i] = (uint8_t)krand();
    return (int64_t)len;
}

static void wr_fs_base(uint64_t v)
{
    __asm__ volatile ("wrmsr" : : "c"(0xC0000100U), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static int64_t lx_pread(int fd, uint64_t ubuf, uint64_t cnt, int64_t off, int wr)
{
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->type != FILE_TYPE_REGULAR && f->type != FILE_TYPE_MINIX) return -ESPIPE;
    uint64_t save = f->pos;
    f->pos = (uint64_t)off;
    int64_t r = wr ? lx_write(fd, ubuf, cnt) : lx_read(fd, ubuf, cnt);
    f->pos = save;
    return r;
}

/* ---- sendfile ---- */
static int64_t lx_sendfile(int out_fd, int in_fd, uint64_t uoff, uint64_t count)
{
    struct file *fi = fd_get(in_fd);
    if (!fi || !fd_get(out_fd)) return -EBADF;
    if (fi->type != FILE_TYPE_REGULAR && fi->type != FILE_TYPE_MINIX && fi->type != FILE_TYPE_PIPE) return -EINVAL;
    int64_t off = 0;
    if (uoff) {
        if (fi->type == FILE_TYPE_PIPE) return -ESPIPE;
        if (ucopy_from(&off, uoff, 8) < 0) return -EFAULT;
    }
    uint64_t pg = get_free_pages(1);
    if (!pg) return -ENOMEM;
    int64_t total = 0, err = 0;
    while ((uint64_t)total < count) {
        uint64_t chunk = count - (uint64_t)total;
        if (chunk > PAGE_SIZE) chunk = PAGE_SIZE;
        kernel_buf_access = 1;
        int64_t r = uoff ? lx_pread(in_fd, pg, chunk, off + total, 0) : lx_read(in_fd, pg, chunk);
        kernel_buf_access = 0;
        if (r < 0) { err = r; break; }
        if (r == 0) break;
        kernel_buf_access = 1;
        int64_t w = lx_write(out_fd, pg, (uint64_t)r);
        kernel_buf_access = 0;
        if (w < 0) { err = w; break; }
        total += w;
        if (w < r) break;
    }
    free_pages(pg, 1);
    if (uoff && total > 0) { int64_t no = off + total; ucopy_to(uoff, &no, 8); }
    return total ? total : err;
}

/* ---- statfs / fstatfs ---- */
struct lx_statfs {
    int64_t f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int32_t f_fsid[2];
    int64_t f_namelen, f_frsize, f_flags, f_spare[4];
};

static int64_t statfs_fill(const char *full, uint64_t ubuf)
{
    struct lx_statfs s;
    memset(&s, 0, sizeof(s));
    int minix = (strncmp(full, "/mnt", 4) == 0 && (full[4] == 0 || full[4] == '/'));
    s.f_type = minix ? 0x137F : 0x858458F6;            /* MINIX_SUPER_MAGIC / RAMFS_MAGIC */
    s.f_bsize = s.f_frsize = 4096;
    s.f_blocks = get_total_pages_count();
    s.f_bfree = s.f_bavail = get_free_pages_count();
    s.f_files = MAX_FILES;
    s.f_ffree = MAX_FILES / 2;
    s.f_namelen = MAX_FILENAME - 1;
    return ucopy_to(ubuf, &s, sizeof(s));
}

static int64_t lx_statfs(uint64_t upath, uint64_t ubuf)
{
    char full[MAX_FILENAME];
    int r = lx_path(AT_FDCWD, upath, full);
    if (r < 0) return r;
    struct lx_stat o;
    int64_t rr = stat_path(full, &o);
    if (rr < 0) return rr;
    return statfs_fill(full, ubuf);
}

static int64_t lx_fstatfs(int fd, uint64_t ubuf)
{
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    return statfs_fill(f->path[0] ? f->path : "/", ubuf);
}

/* ---- statx ---- */
struct lx_statx_ts { int64_t sec; uint32_t nsec; int32_t rsv; };
struct lx_statx {
    uint32_t mask, blksize;
    uint64_t attributes;
    uint32_t nlink, uid, gid;
    uint16_t mode, pad;
    uint64_t ino, size, blocks, attributes_mask;
    struct lx_statx_ts atime, btime, ctime, mtime;
    uint32_t rdev_major, rdev_minor, dev_major, dev_minor;
    uint64_t spare[14];
};

static int64_t lx_statx(int dirfd, uint64_t upath, int flags, uint64_t ubuf)
{
    struct lx_stat o;
    char c = 1;
    if (ucopy_from(&c, upath, 1) < 0) return -EFAULT;
    if (c == 0 && (flags & 0x1000)) {
        /* AT_EMPTY_PATH: fstat; результат через временный буфер ядра */
        kernel_buf_access = 1;
        int64_t r = lx_fstat(dirfd, (uint64_t)&o);
        kernel_buf_access = 0;
        if (r < 0) return r;
    } else {
        char full[MAX_FILENAME];
        int r = lx_path(dirfd, upath, full);
        if (r < 0) return r;
        int64_t rr = stat_path(full, &o);
        if (rr < 0) return rr;
    }
    struct lx_statx x;
    memset(&x, 0, sizeof(x));
    x.mask = 0x7FF;                                    /* STATX_BASIC_STATS */
    x.blksize = (uint32_t)o.st_blksize;
    x.nlink = (uint32_t)o.st_nlink;
    x.uid = o.st_uid;
    x.gid = o.st_gid;
    x.mode = (uint16_t)o.st_mode;
    x.ino = o.st_ino;
    x.size = (uint64_t)o.st_size;
    x.blocks = (uint64_t)o.st_blocks;
    x.atime.sec = o.atime; x.mtime.sec = o.mtime; x.ctime.sec = o.ctime;
    x.rdev_major = (uint32_t)((o.st_rdev >> 8) & 0xFFF);
    x.rdev_minor = (uint32_t)(o.st_rdev & 0xFF);
    x.dev_major = (uint32_t)((o.st_dev >> 8) & 0xFFF);
    x.dev_minor = (uint32_t)(o.st_dev & 0xFF);
    return ucopy_to(ubuf, &x, sizeof(x));
}

/* ---- futex ---- */
static void futex_wake_n(uint64_t cr3, uint64_t addr, int n, uint32_t bitset, int *woken)
{
    for (int i = 1; i < NR_TASKS && *woken < n; i++) {
        struct task_struct *t = task[i];
        if (t && t->futex_addr == addr && t->futex_cr3 == cr3 && (t->futex_bitset & bitset)
            && t->state == TASK_INTERRUPTIBLE) {
            t->futex_addr = 0;
            t->state = TASK_RUNNING;
            (*woken)++;
        }
    }
}

/* Выход потока: CLONE_CHILD_CLEARTID -- обнулить tid и разбудить ждущих pthread_join */
void thread_release(struct task_struct *t)
{
    if (!t->clear_child_tid) return;
    uint64_t addr = t->clear_child_tid;
    t->clear_child_tid = 0;
    if (user_access_ok(addr, 4, 1)) {
        *(volatile uint32_t *)addr = 0;
        int w = 0;
        futex_wake_n(t->cr3, addr, 1, 0xFFFFFFFFU, &w);
    }
}

static int64_t futex_wait(uint64_t uaddr, uint32_t val, uint64_t utimeout, int absolute, int clk, uint32_t bitset)
{
    uint32_t cur;
    if (uaddr & 3) return -EINVAL;
    if (ucopy_from(&cur, uaddr, 4) < 0) return -EFAULT;
    if (cur != val) return -EAGAIN;
    if (!bitset) return -EINVAL;

    uint64_t deadline = 0;
    if (utimeout) {
        int64_t ts[2];
        if (ucopy_from(ts, utimeout, 16) < 0) return -EFAULT;
        if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000) return -EINVAL;
        int64_t sec = ts[0], nsec = ts[1];
        if (absolute) {
            int64_t ns, nn;
            now_ts(clk, &ns, &nn);
            sec -= ns; nsec -= nn;
            if (nsec < 0) { nsec += 1000000000; sec--; }
            if (sec < 0) return -ETIMEDOUT;
        }
        uint64_t ticks = (uint64_t)sec * 100 + (uint64_t)(nsec + 9999999) / 10000000;
        if (ticks == 0) ticks = 1;
        deadline = jiffies + ticks;
    }

    current->futex_addr = uaddr;
    current->futex_cr3 = current->cr3;
    current->futex_bitset = bitset;
    current->futex_deadline = deadline;
    for (;;) {
        current->state = TASK_INTERRUPTIBLE;
        __asm__ volatile ("sti");
        schedule();
        __asm__ volatile ("cli");
        if (!current->futex_addr) { current->futex_deadline = 0; return 0; }   /* разбудили FUTEX_WAKE */
        if (sig_pending_for(current)) { current->futex_addr = 0; current->futex_deadline = 0; return -EINTR; }
        if (deadline && jiffies >= deadline) { current->futex_addr = 0; current->futex_deadline = 0; return -ETIMEDOUT; }
    }
}

static int futex_cmp(int cmp, int32_t a, int32_t b)
{
    switch (cmp) {
    case 0: return a == b;
    case 1: return a != b;
    case 2: return a < b;
    case 3: return a <= b;
    case 4: return a > b;
    case 5: return a >= b;
    }
    return 0;
}

static int64_t lx_futex(uint64_t uaddr, int op, uint32_t val, uint64_t a4, uint64_t uaddr2, uint32_t val3)
{
    int cmd = op & 127;
    int clk = (op & 256) ? 0 : 1;                               /* FUTEX_CLOCK_REALTIME */
    uint64_t cr3 = current->cr3;
    int woken = 0;
    switch (cmd) {
    case 0:  return futex_wait(uaddr, val, a4, 0, clk, 0xFFFFFFFFU);               /* FUTEX_WAIT */
    case 9:  return futex_wait(uaddr, val, a4, 1, clk, val3);                      /* FUTEX_WAIT_BITSET */
    case 1:  futex_wake_n(cr3, uaddr, (int)val, 0xFFFFFFFFU, &woken); return woken; /* FUTEX_WAKE */
    case 10: futex_wake_n(cr3, uaddr, (int)val, val3, &woken); return woken;       /* FUTEX_WAKE_BITSET */
    case 3: case 4: {                                                              /* REQUEUE / CMP_REQUEUE */
        if (cmd == 4) {
            uint32_t cur;
            if (ucopy_from(&cur, uaddr, 4) < 0) return -EFAULT;
            if (cur != val3) return -EAGAIN;
        }
        futex_wake_n(cr3, uaddr, (int)val, 0xFFFFFFFFU, &woken);
        int moved = 0, lim = (int)a4;
        for (int i = 1; i < NR_TASKS && moved < lim; i++) {
            struct task_struct *t = task[i];
            if (t && t->futex_addr == uaddr && t->futex_cr3 == cr3 && t->state == TASK_INTERRUPTIBLE) {
                t->futex_addr = uaddr2;
                moved++;
            }
        }
        return woken + moved;
    }
    case 5: {                                                                      /* FUTEX_WAKE_OP */
        int32_t oldv;
        if (ucopy_from(&oldv, uaddr2, 4) < 0) return -EFAULT;
        int op2 = (val3 >> 28) & 7, cmp = (val3 >> 24) & 15;
        int32_t oparg = (int32_t)(val3 << 8) >> 20, cmparg = (int32_t)(val3 << 20) >> 20;
        if (val3 & 0x80000000U) { oparg = 1 << (oparg & 31); }
        int32_t nv = oldv;
        switch (op2 & 7) { case 0: nv = oparg; break; case 1: nv = oldv + oparg; break;
                           case 2: nv = oldv | oparg; break; case 3: nv = oldv & ~oparg; break;
                           case 4: nv = oldv ^ oparg; break; }
        if (ucopy_to(uaddr2, &nv, 4) < 0) return -EFAULT;
        futex_wake_n(cr3, uaddr, (int)val, 0xFFFFFFFFU, &woken);
        if (futex_cmp(cmp, oldv, cmparg)) futex_wake_n(cr3, uaddr2, (int)a4, 0xFFFFFFFFU, &woken);
        return woken;
    }
    }
    return -ENOSYS;
}

static int64_t lx_dispatch(struct trap_frame *tf, uint64_t nr, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    switch (nr) {
    case 0:   return lx_read((int)a1, a2, a3);
    case 1:   return lx_write((int)a1, a2, a3);
    case 2:   return lx_open(AT_FDCWD, a1, (int)a2, (int)a3);
    case 3:   return lx_close((int)a1);
    case 4: case 6: return lx_stat_common(AT_FDCWD, a1, a2);
    case 5:   return lx_fstat((int)a1, a2);
    case 7:   return lx_poll(a1, a2, (int)a3);
    case 8:   return lx_lseek((int)a1, (int64_t)a2, (int)a3);
    case 9:   return lx_mmap(a1, a2, (int)a3, (int)a4, (int)a5, (int64_t)tf->r9);
    case 10:  if (a1 & (PAGE_SIZE - 1)) return -EINVAL;
              return vma_protect(current, a1, a1 + a2, lx_prot((int)a3)) < 0 ? -ENOMEM : 0;
    case 11:  if (a1 & (PAGE_SIZE - 1)) return -EINVAL;
              return vma_unmap(current, a1, a1 + a2) < 0 ? -ENOMEM : 0;
    case 12:  return sys_brk(a1);
    case 13:  return lx_rt_sigaction((int)a1, a2, a3);
    case 14:  return lx_rt_sigprocmask((int)a1, a2, a3, a4);
    case 15:  return lx_sigreturn(tf);
    case 16:  return lx_ioctl((int)a1, a2, a3);
    case 17:  return lx_pread((int)a1, a2, a3, (int64_t)a4, 0);
    case 18:  return lx_pread((int)a1, a2, a3, (int64_t)a4, 1);
    case 19:  return lx_rwv((int)a1, a2, (int)a3, 0);
    case 20:  return lx_rwv((int)a1, a2, (int)a3, 1);
    case 21:  return lx_access(AT_FDCWD, a1);
    case 22:  return lx_pipe2(a1, 0);
    case 23:  { int64_t us = -1;
                if (a5) { int64_t tv[2]; if (ucopy_from(tv, a5, 16) < 0) return -EFAULT; us = tv[0] * 1000000 + tv[1]; }
                return lx_select((int)a1, a2, a3, a4, us); }
    case 24:  schedule(); return 0;
    case 25:  return -ENOMEM;                                  /* mremap */
    case 28:  return 0;                                        /* madvise */
    case 32:  return lx_dupfd((int)a1, 0, 0);
    case 33:  return lx_dup2((int)a1, (int)a2, 0);
    case 34:  while (!sig_pending_for(current)) __asm__ volatile ("sti; hlt"); return -EINTR;
    case 35:  return lx_nanosleep(a1, a2);
    case 37:  { uint64_t old = current->alarm > jiffies ? (current->alarm - jiffies + 99) / 100 : 0;
                current->alarm = a1 ? jiffies + a1 * 100 : 0; return (int64_t)old; }
    case 39:  return current->tgid;
    case 56:  { if (a1 & 0x10000) return sys_clone_thread(tf, a1, a2, a3, a4, a5);   /* CLONE_THREAD */
                return lx_fork(tf, a2); }
    case 57: case 58: return lx_fork(tf, 0);
    case 59:  return lx_execve(tf, a1, a2, a3);
    case 60:  sys_exit((int)(a1 & 0xFF)); return 0;           /* выход потока */
    case 231: sys_exit_group((int)(a1 & 0xFF)); return 0;
    case 61:  return lx_wait4((int64_t)(int)a1, a2, (int)a3);
    case 62:  return lx_kill((int64_t)(int)a1, (int)a2);
    case 63:  return lx_uname(a1);
    case 72:  return lx_fcntl((int)a1, (int)a2, a3);
    case 73:  return 0;                                        /* flock */
    case 74: case 75: return 0;                                /* fsync */
    case 77:  { struct file *f = fd_get((int)a1); if (!f) return -EBADF;
                return fs_truncate_path(f->path, a2) < 0 ? -EPERM : 0; }
    case 76:  { char p[MAX_FILENAME]; int r = lx_path(AT_FDCWD, a1, p); if (r < 0) return r;
                return fs_truncate_path(p, a2) < 0 ? -EPERM : 0; }
    case 78:  case 217: return lx_getdents64((int)a1, a2, a3);
    case 79:  return lx_getcwd(a1, a2);
    case 80:  { char p[MAX_FILENAME]; int r = lx_path(AT_FDCWD, a1, p); if (r < 0) return r; return lx_chdir_path(p); }
    case 81:  { struct file *f = fd_get((int)a1); if (!f || f->type != FILE_TYPE_DIR) return -ENOTDIR;
                return lx_chdir_path(f->path); }
    case 82:  return lx_rename(AT_FDCWD, a1, AT_FDCWD, a2);
    case 83:  return lx_mkdir(AT_FDCWD, a1);
    case 84:  return lx_unlink(AT_FDCWD, a1, 0x200);
    case 85:  return lx_open(AT_FDCWD, a1, 01 | LX_O_CREAT | LX_O_TRUNC, (int)a2);
    case 86: case 88: case 265: case 266: return -EPERM;     /* link / symlink */
    case 87:  return lx_unlink(AT_FDCWD, a1, 0);
    case 89:  { char p[MAX_FILENAME]; int r = lx_path(AT_FDCWD, a1, p); return r < 0 ? r : -EINVAL; }
    case 90:  return lx_chmod(AT_FDCWD, a1, (int)a2);
    case 91:  return 0;                                        /* fchmod */
    case 92: case 93: case 94: case 260: return 0;           /* chown */
    case 95:  { uint32_t o = current->umask; current->umask = (uint32_t)a1 & 0777; return o; }
    case 96:  { int64_t s, ns; now_ts(0, &s, &ns); int64_t tv[2] = { s, ns / 1000 };
                return a1 ? ucopy_to(a1, tv, 16) : 0; }
    case 97:  return lx_prlimit((int)a1, 0, a2);
    case 98:  { if (!user_access_ok(a2, 144, 1)) return -EFAULT; memset((void *)a2, 0, 144); return 0; }
    case 99:  return lx_sysinfo(a1);
    case 100: { if (a1) { if (!user_access_ok(a1, 32, 1)) return -EFAULT; memset((void *)a1, 0, 32); }
                return (int64_t)jiffies; }
    case 102: return current->uid;
    case 107: return current->euid;
    case 104: return current->gid;
    case 108: return current->egid;
    case 118: case 120: {                                      /* getresuid / getresgid */
        uint32_t id = (nr == 118) ? current->uid : current->gid;
        uint32_t eid = (nr == 118) ? current->euid : current->egid;
        int r;
        if ((r = ucopy_to(a1, &id, 4)) < 0 || (r = ucopy_to(a2, &eid, 4)) < 0 || (r = ucopy_to(a3, &eid, 4)) < 0) return r;
        return 0;
    }
    case 105: case 117: if (current->euid == 0) { current->uid = current->euid = (uint16_t)a1; } return 0;
    case 106: case 114: case 119: return 0;
    case 109: { struct task_struct *t = a1 ? task_by_pid((int64_t)(int)a1) : current; if (!t) return -ESRCH;
                t->pgid = (int)a2 ? (int)a2 : (int)t->pid; return 0; }
    case 110: return current->father;
    case 111: return pgrp_of(current);
    case 112: current->sid = (int)current->pid; current->pgid = (int)current->pid; return current->pid;
    case 115: return 0;                                        /* getgroups */
    case 121: { struct task_struct *t = a1 ? task_by_pid((int64_t)(int)a1) : current; return t ? pgrp_of(t) : -ESRCH; }
    case 124: return current->sid ? current->sid : (int)current->pid;
    case 127: { uint64_t m = (current->signal & current->sigmask) >> 1; return ucopy_to(a1, &m, 8); }
    case 130: { uint64_t old = current->sigmask, m;
                if (ucopy_from(&m, a1, 8) < 0) return -EFAULT;
                current->sigmask = (m << 1) & ~((1ULL << 9) | (1ULL << 19));
                while (!sig_pending_for(current)) __asm__ volatile ("sti; hlt");
                current->sigmask = old; return -EINTR; }
    case 131: return 0;                                        /* sigaltstack */
    case 132: case 235: case 280: return 0;                    /* utime / utimes / utimensat */
    case 137: return lx_statfs(a1, a2);
    case 138: return lx_fstatfs((int)a1, a2);
    case 103: {                                                /* syslog: чтение журнала ядра (dmesg) */
        if (a1 == 10) return 16384;
        if (a1 == 3 || a1 == 4) {
            static char kb[16384];
            if (a3 > sizeof(kb)) a3 = sizeof(kb);
            if (!user_access_ok(a2, a3, 1)) return -EFAULT;
            int n = klog_read(kb, (int)a3);
            int r = ucopy_to(a2, kb, (uint64_t)n);
            return r < 0 ? r : n;
        }
        return 0;
    }
    case 125: {                                                /* capget: root имеет все права */
        uint32_t hdr[2] = { 0x20080522, 0 };
        if (a1) { int r = ucopy_to(a1, hdr, 4); if (r < 0) return r; }
        if (a2) { uint32_t d[6] = { 0xFFFFFFFF, 0xFFFFFFFF, 0, 0xFFFFFFFF, 0xFFFFFFFF, 0 }; return ucopy_to(a2, d, sizeof(d)); }
        return 0;
    }
    case 126: return 0;                                        /* capset */
    case 157: return 0;                                        /* prctl */
    case 158: if (a1 == 0x1002) { current->fs_base = a2; wr_fs_base(a2); return 0; }
              if (a1 == 0x1003) return ucopy_to(a2, &current->fs_base, 8);
              return -EINVAL;
    case 162: sys_sync(); return 0;
    case 186: return current->pid;                                 /* gettid */
    case 200: return lx_tkill((int64_t)(int)a1, (int)a2);               /* tkill(tid, sig) */
    case 201: { int64_t s, ns; now_ts(0, &s, &ns); if (a1) { int r = ucopy_to(a1, &s, 8); if (r < 0) return r; } return s; }
    case 202: return lx_futex(a1, (int)a2, (uint32_t)a3, a4, a5, (uint32_t)tf->r9);
    case 218: current->clear_child_tid = a1; return current->pid;   /* set_tid_address */
    case 228: { int64_t ts[2]; if (a1 > 15) return -EINVAL; now_ts((int)a1, &ts[0], &ts[1]); return ucopy_to(a2, ts, 16); }
    case 229: { int64_t ts[2] = { 0, 1000000 }; return a2 ? ucopy_to(a2, ts, 16) : 0; }
    case 230: return lx_nanosleep(a3, a4);
    case 234: return lx_tkill((int64_t)(int)a2, (int)a3);            /* tgkill(tgid, tid, sig) */
    case 257: return lx_open((int)a1, a2, (int)a3, (int)a4);
    case 258: return lx_mkdir((int)a1, a2);
    case 262: { if (a4 & 0x1000) { char c; if (ucopy_from(&c, a2, 1) == 0 && c == 0) return lx_fstat((int)a1, a3); }
                return lx_stat_common((int)a1, a2, a3); }
    case 263: return lx_unlink((int)a1, a2, (int)a3);
    case 264: case 316: return lx_rename((int)a1, a2, (int)a3, a4);
    case 267: { char p[MAX_FILENAME]; int r = lx_path((int)a1, a2, p); return r < 0 ? r : -EINVAL; }
    case 268: return lx_chmod((int)a1, a2, (int)a3);
    case 269: case 439: return lx_access((int)a1, a2);
    case 270: { int64_t us = -1;
                if (a5) { int64_t ts[2]; if (ucopy_from(ts, a5, 16) < 0) return -EFAULT; us = ts[0] * 1000000 + ts[1] / 1000; }
                return lx_select((int)a1, a2, a3, a4, us); }
    case 271: { int64_t ms = -1;
                if (a3) { int64_t ts[2]; if (ucopy_from(ts, a3, 16) < 0) return -EFAULT; ms = ts[0] * 1000 + ts[1] / 1000000; }
                return lx_poll(a1, a2, (int)ms); }
    case 292: return lx_dup2((int)a1, (int)a2, (int)(a3 & LX_O_CLOEXEC));
    case 293: return lx_pipe2(a1, (int)a2);
    case 302: return lx_prlimit((int)a2, a3, a4);
    case 318: return lx_getrandom(a1, a2);
    case 40:  return lx_sendfile((int)a1, (int)a2, a3, a4);
    case 332: return lx_statx((int)a1, a2, (int)a3, a5);
    case 188: case 189: case 190: case 191: case 192: case 193: case 194: case 195: case 196:
    case 197: case 198: case 199: return -EOPNOTSUPP;              /* *xattr: расширенных атрибутов нет */
    case 221: return 0;                                         /* fadvise64: совет, можно игнорировать */
    case 204: { uint64_t m = 1; if (a2 < 8) return -EINVAL; int r = ucopy_to(a3, &m, 8); if (r < 0) return r; return 8; }  /* sched_getaffinity: один ЦП */
    case 41:  return lx_socket((int)a1, (int)a2, (int)a3);
    case 42:  return lx_connect((int)a1, a2, a3);
    case 43:  return lx_accept((int)a1, a2, a3, 0);
    case 288: return lx_accept((int)a1, a2, a3, (int)a4);
    case 44:  return lx_sendto((int)a1, a2, a3, (int)a4, a5, a6);
    case 45:  return lx_recvfrom((int)a1, a2, a3, (int)a4, a5, a6);
    case 46:  return lx_sendmsg((int)a1, a2, (int)a3);
    case 47:  return lx_recvmsg((int)a1, a2, (int)a3);
    case 48:  return lx_shutdown((int)a1, (int)a2);
    case 49:  return lx_bind((int)a1, a2, a3);
    case 50:  return lx_listen((int)a1, (int)a2);
    case 51:  return lx_getsockname((int)a1, a2, a3, 0);
    case 52:  return lx_getsockname((int)a1, a2, a3, 1);
    case 284: case 290: {                                      /* eventfd, eventfd2 */
        int id = sock_eventfd((unsigned)a1, nr == 290 ? (int)a2 : 0);
        if (id < 0) return id;
        return sock_fd_new(id, nr == 290 ? (int)a2 : 0);
    }
    case 53:  return lx_socketpair((int)a1, (int)a2, (int)a3, a4);
    case 54:  return lx_setsockopt((int)a1, (int)a2, (int)a3, a4, a5);
    case 55:  return lx_getsockopt((int)a1, (int)a2, (int)a3, a4, a5);
    case 273: case 334: case 435: return -ENOSYS;              /* set_robust_list, rseq, clone3 */
    }
    return -ENOSYS;
}

int64_t linux_syscall(struct trap_frame *tf)
{
    int64_t ret = lx_dispatch(tf, tf->rax, tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9);
    tf->rax = (uint64_t)ret;
    lx_deliver(tf);
    return ret;
}

/* Вызывается из execve нового Linux-образа: закрыть O_CLOEXEC, подготовить fd 0..2,
 * сбросить обработчики сигналов (SIG_IGN сохраняется) */
void linux_fd_exec_cleanup(void)
{
    for (int fd = 0; fd < NR_OPEN; fd++) {
        if (current->files->f[fd].in_use && ((current->cloexec >> fd) & 1)) lx_close(fd);
    }
    current->cloexec = 0;
    for (int fd = 0; fd < 3; fd++) {
        if (!current->files->f[fd].in_use) {
            memset(&current->files->f[fd], 0, sizeof(struct file));
            current->files->f[fd].type = FILE_TYPE_TTY;
            current->files->f[fd].in_use = 1;
            current->files->f[fd].mode = 3;
            strcpy(current->files->f[fd].path, "/dev/tty");
        }
    }
    for (int s = 0; s < 32; s++) {
        if (current->lx_sa[s].handler != 1) current->lx_sa[s].handler = 0;
        current->lx_sa[s].flags = current->lx_sa[s].restorer = current->lx_sa[s].mask = 0;
    }
    current->mm->mmap_cur = 0;
    current->fs_base = 0;
    wr_fs_base(0);
}
