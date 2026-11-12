#include <linux/string.h>

void *memcpy(void *dest, const void *src, uint64_t n)
{
    void *d = dest;
    __asm__ volatile (
        "cld\n\t"
        "rep movsb"
        : "+c"(n), "+S"(src), "+D"(d)
        :
        : "memory"
    );
    return dest;
}

void *memset(void *dest, int c, uint64_t n)
{
    void *d = dest;
    __asm__ volatile (
        "cld\n\t"
        "rep stosb"
        : "+c"(n), "+D"(d)
        : "a"((uint8_t)c)
        : "memory"
    );
    return dest;
}

int memcmp(const void *s1, const void *s2, uint64_t n)
{
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;

    while (n--) {
        if (*p1 != *p2) {
            return (int)(*p1 - *p2);
        }
        p1++;
        p2++;
    }
    return 0;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, uint64_t n)
{
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

uint64_t strlen(const char *s)
{
    uint64_t len = 0;
    while (s[len]) len++;
    return len;
}

char *strcpy(char *dest, const char *src)
{
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strncpy(char *dest, const char *src, uint64_t n)
{
    char *d = dest;
    while (n && (*d++ = *src++)) n--;
    while (n--) *d++ = '\0';
    return dest;
}

void *memmove(void *dest, const void *src, uint64_t n)
{
    char *d = (char *)dest;
    const char *s = (const char *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}
