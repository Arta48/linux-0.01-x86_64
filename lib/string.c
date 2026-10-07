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
