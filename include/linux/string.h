#ifndef _LINUX_STRING_H
#define _LINUX_STRING_H

#include <linux/types.h>

void *memcpy(void *dest, const void *src, uint64_t n);
void *memset(void *dest, int c, uint64_t n);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, uint64_t n);
uint64_t strlen(const char *s);

#endif
