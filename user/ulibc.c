#include "ulibc.h"

#define __NR_exit    1
#define __NR_fork    2
#define __NR_read    3
#define __NR_write   4
#define __NR_open    5
#define __NR_close   6
#define __NR_waitpid 7
#define __NR_unlink  10
#define __NR_execve  11
#define __NR_chdir   12
#define __NR_time    13
#define __NR_chmod   15
#define __NR_stat    18
#define __NR_getpid  20
#define __NR_ps      21
#define __NR_list    22
#define __NR_setuid  23
#define __NR_getuid  24
#define __NR_pause   29
#define __NR_sync    36
#define __NR_kill    37
#define __NR_mkdir   39
#define __NR_rmdir   40
#define __NR_socket  41
#define __NR_pipe    42
#define __NR_accept  43
#define __NR_sendto  44
#define __NR_brk     45
#define __NR_signal  48
#define __NR_bind    49
#define __NR_listen  50
#define __NR_recvfrom 51
#define __NR_uname   59
#define __NR_dup2    63
#define __NR_getcwd  79

static inline int64_t syscall0(uint64_t nr)
{
    int64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(nr) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall1(uint64_t nr, uint64_t a1)
{
    int64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(nr), "D"(a1) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall2(uint64_t nr, uint64_t a1, uint64_t a2)
{
    int64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(nr), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall3(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3)
{
    int64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(nr), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall4(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    register uint64_t r10 __asm__("r10") = a4;
    int64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10) : "rcx", "r11", "memory");
    return ret;
}

int open(const char *path, int flags) { return (int)syscall2(__NR_open, (uint64_t)path, flags); }
int close(int fd) { return (int)syscall1(__NR_close, fd); }
int64_t read(int fd, void *buf, size_t count) { return syscall3(__NR_read, fd, (uint64_t)buf, count); }
int64_t write(int fd, const void *buf, size_t count) { return syscall3(__NR_write, fd, (uint64_t)buf, count); }
void exit(int status) { syscall1(__NR_exit, status); for(;;); }
int fork(void) {
    int64_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(__NR_fork) : "memory");
    return (int)ret;
}
int execve(const char *path, char **argv, char **envp) {
    int64_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(__NR_execve), "b"((uint64_t)path), "c"((uint64_t)argv), "d"((uint64_t)envp) : "memory");
    return (int)ret;
}
int waitpid(int pid, int *status, int options) { return (int)syscall3(__NR_waitpid, pid, (uint64_t)status, options); }
int getpid(void) { return (int)syscall0(__NR_getpid); }
int getuid(void) { return (int)syscall0(__NR_getuid); }
int setuid(uint16_t uid, const char *password) { return (int)syscall2(__NR_setuid, uid, (uint64_t)password); }
int64_t time(void) { return syscall0(__NR_time); }

int stat(const char *path, struct stat *buf) { return (int)syscall2(__NR_stat, (uint64_t)path, (uint64_t)buf); }
int chdir(const char *path) { return (int)syscall1(__NR_chdir, (uint64_t)path); }
int mkdir(const char *path) { return (int)syscall1(__NR_mkdir, (uint64_t)path); }
int rmdir(const char *path) { return (int)syscall1(__NR_rmdir, (uint64_t)path); }
int getcwd(char *buf, size_t size) { return (int)syscall2(__NR_getcwd, (uint64_t)buf, size); }
int pipe(int *pipefd) { return (int)syscall1(__NR_pipe, (uint64_t)pipefd); }
int dup2(int oldfd, int newfd) { return (int)syscall2(__NR_dup2, oldfd, newfd); }
int unlink(const char *path) { return (int)syscall1(__NR_unlink, (uint64_t)path); }
int chmod(const char *path, int mode) { return (int)syscall2(__NR_chmod, (uint64_t)path, mode); }
int kill(int pid, int sig) { return (int)syscall2(__NR_kill, pid, sig); }
int signal(int sig, void (*handler)(int)) { return (int)syscall2(__NR_signal, sig, (uint64_t)handler); }
int uname(struct utsname *name) { return (int)syscall1(__NR_uname, (uint64_t)name); }
int sync(void) { return (int)syscall0(__NR_sync); }
int socket(int domain, int type, int protocol) { return (int)syscall3(__NR_socket, domain, type, protocol); }
int bind(int sockfd, uint16_t port) { return (int)syscall2(__NR_bind, sockfd, port); }
int listen(int sockfd, int backlog) { return (int)syscall2(__NR_listen, sockfd, backlog); }
int accept(int sockfd) { return (int)syscall1(__NR_accept, sockfd); }
int64_t list(const char *path, char *buf, size_t max_len, int is_long) { return syscall4(__NR_list, (uint64_t)path, (uint64_t)buf, max_len, is_long); }
void ps(void) { syscall0(__NR_ps); }
void pause(void) { syscall0(__NR_pause); }

static uint64_t current_brk = 0;

void *sbrk(intptr_t increment)
{
    if (current_brk == 0) {
        current_brk = (uint64_t)syscall1(__NR_brk, 0);
    }
    if (increment == 0) {
        return (void *)current_brk;
    }
    uint64_t old_brk = current_brk;
    uint64_t new_brk = current_brk + increment;
    uint64_t res = (uint64_t)syscall1(__NR_brk, new_brk);
    if (res < new_brk) {
        return (void *)-1;
    }
    current_brk = new_brk;
    return (void *)old_brk;
}

size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    while (n && *s1 && (*s1 == *s2)) { s1++; s2++; n--; }
    if (n == 0) return 0;
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

char *strcpy(char *dest, const char *src)
{
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n)
{
    char *d = dest;
    while (n && (*d++ = *src++)) n--;
    while (n--) *d++ = '\0';
    return dest;
}

char *strcat(char *dest, const char *src)
{
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

const char *strstr(const char *haystack, const char *needle)
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

void *memcpy(void *dest, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dest;
}

void *memset(void *s, int c, size_t n)
{
    uint8_t *p = (uint8_t *)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

int64_t atoi(const char *s)
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

static int format_number(char *buf, size_t buf_size, uint64_t val, int base, int is_signed, int width, char pad_char)
{
    char temp[65];
    int ti = 0;
    int negative = 0;

    if (is_signed && ((int64_t)val < 0)) {
        negative = 1;
        val = (uint64_t)(-(int64_t)val);
    }

    static const char digits[] = "0123456789abcdef";
    if (val == 0) {
        temp[ti++] = '0';
    } else {
        while (val > 0) {
            temp[ti++] = digits[val % base];
            val /= base;
        }
    }

    int total_len = ti + (negative ? 1 : 0);
    int pad_len = (width > total_len) ? (width - total_len) : 0;
    size_t written = 0;

    if (negative && pad_char == '0') {
        if (written < buf_size) buf[written++] = '-';
        negative = 0;
    }

    while (pad_len-- > 0) {
        if (written < buf_size) buf[written++] = pad_char;
    }

    if (negative) {
        if (written < buf_size) buf[written++] = '-';
    }

    while (ti > 0) {
        if (written < buf_size) buf[written++] = temp[--ti];
    }

    return (int)written;
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
    size_t out = 0;

    for (const char *p = format; *p != '\0'; p++) {
        if (*p != '%') {
            if (out + 1 < size) str[out] = *p;
            out++;
            continue;
        }

        p++;
        char pad = ' ';
        int width = 0;

        if (*p == '0') {
            pad = '0';
            p++;
        }
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        switch (*p) {
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                while (*s) {
                    if (out + 1 < size) str[out] = *s;
                    out++;
                    s++;
                }
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                if (out + 1 < size) str[out] = c;
                out++;
                break;
            }
            case 'd':
            case 'i': {
                int val = va_arg(ap, int);
                char nb[64];
                int nlen = format_number(nb, sizeof(nb), (uint64_t)(int64_t)val, 10, 1, width, pad);
                for (int i = 0; i < nlen; i++) {
                    if (out + 1 < size) str[out] = nb[i];
                    out++;
                }
                break;
            }
            case 'u': {
                unsigned int val = va_arg(ap, unsigned int);
                char nb[64];
                int nlen = format_number(nb, sizeof(nb), (uint64_t)val, 10, 0, width, pad);
                for (int i = 0; i < nlen; i++) {
                    if (out + 1 < size) str[out] = nb[i];
                    out++;
                }
                break;
            }
            case 'x':
            case 'X': {
                unsigned int val = va_arg(ap, unsigned int);
                char nb[64];
                int nlen = format_number(nb, sizeof(nb), (uint64_t)val, 16, 0, width, pad);
                for (int i = 0; i < nlen; i++) {
                    if (out + 1 < size) str[out] = nb[i];
                    out++;
                }
                break;
            }
            case 'p': {
                uint64_t val = (uint64_t)va_arg(ap, void *);
                if (out + 1 < size) str[out] = '0';
                out++;
                if (out + 1 < size) str[out] = 'x';
                out++;
                char nb[64];
                int nlen = format_number(nb, sizeof(nb), val, 16, 0, 0, ' ');
                for (int i = 0; i < nlen; i++) {
                    if (out + 1 < size) str[out] = nb[i];
                    out++;
                }
                break;
            }
            case '%': {
                if (out + 1 < size) str[out] = '%';
                out++;
                break;
            }
            default: {
                if (out + 1 < size) str[out] = *p;
                out++;
                break;
            }
        }
    }

    if (size > 0) {
        if (out < size) str[out] = '\0';
        else str[size - 1] = '\0';
    }

    return (int)out;
}

int snprintf(char *str, size_t size, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int res = vsnprintf(str, size, format, ap);
    va_end(ap);
    return res;
}

int dprintf(int fd, const char *format, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, format);
    int len = vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);

    if (len > 0) {
        size_t to_write = (len < (int)sizeof(buf)) ? (size_t)len : (sizeof(buf) - 1);
        write(fd, buf, to_write);
    }
    return len;
}

int printf(const char *format, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, format);
    int len = vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);

    if (len > 0) {
        size_t to_write = (len < (int)sizeof(buf)) ? (size_t)len : (sizeof(buf) - 1);
        write(1, buf, to_write);
    }
    return len;
}

struct block_header {
    size_t size;
    int is_free;
    struct block_header *next;
};

static struct block_header *heap_start = NULL;

void *malloc(size_t size)
{
    if (size == 0) return NULL;
    size = (size + 15) & ~15ULL;

    struct block_header *curr = heap_start;
    struct block_header *prev = NULL;

    while (curr) {
        if (curr->is_free && curr->size >= size) {
            curr->is_free = 0;
            return (void *)(curr + 1);
        }
        prev = curr;
        curr = curr->next;
    }

    size_t total_size = sizeof(struct block_header) + size;
    void *p = sbrk((intptr_t)total_size);
    if (p == (void *)-1) return NULL;

    struct block_header *new_block = (struct block_header *)p;
    new_block->size = size;
    new_block->is_free = 0;
    new_block->next = NULL;

    if (prev) {
        prev->next = new_block;
    } else {
        heap_start = new_block;
    }

    return (void *)(new_block + 1);
}

void free(void *ptr)
{
    if (!ptr) return;
    struct block_header *header = ((struct block_header *)ptr) - 1;
    header->is_free = 1;

    struct block_header *curr = heap_start;
    while (curr && curr->next) {
        if (curr->is_free && curr->next->is_free) {
            curr->size += sizeof(struct block_header) + curr->next->size;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}
