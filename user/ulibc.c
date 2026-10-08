#include "ulibc.h"

#define __NR_exit    1
#define __NR_fork    2
#define __NR_read    3
#define __NR_write   4
#define __NR_open    5
#define __NR_close   6
#define __NR_waitpid 7
#define __NR_execve  11
#define __NR_time    13
#define __NR_getpid  20
#define __NR_getuid  24
#define __NR_brk     45

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
int64_t time(void) { return syscall0(__NR_time); }

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
                int64_t val = va_arg(ap, int64_t);
                char nb[64];
                int nlen = format_number(nb, sizeof(nb), (uint64_t)val, 10, 1, width, pad);
                for (int i = 0; i < nlen; i++) {
                    if (out + 1 < size) str[out] = nb[i];
                    out++;
                }
                break;
            }
            case 'u': {
                uint64_t val = va_arg(ap, uint64_t);
                char nb[64];
                int nlen = format_number(nb, sizeof(nb), val, 10, 0, width, pad);
                for (int i = 0; i < nlen; i++) {
                    if (out + 1 < size) str[out] = nb[i];
                    out++;
                }
                break;
            }
            case 'x':
            case 'X': {
                uint64_t val = va_arg(ap, uint64_t);
                char nb[64];
                int nlen = format_number(nb, sizeof(nb), val, 16, 0, width, pad);
                for (int i = 0; i < nlen; i++) {
                    if (out + 1 < size) str[out] = nb[i];
                    out++;
                }
                break;
            }
            case 'p': {
                uint64_t val = (uint64_t)va_arg(ap, void *);
                if (out + 1 < size) {
                    str[out] = '0';
                }
                out++;
                if (out + 1 < size) {
                    str[out] = 'x';
                }
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
