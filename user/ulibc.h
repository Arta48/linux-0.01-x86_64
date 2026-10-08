#ifndef _ULIBC_H
#define _ULIBC_H

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;

typedef uint64_t           size_t;
typedef int64_t            intptr_t;

#define NULL ((void *)0)

#define O_RDONLY  00
#define O_WRONLY  01
#define O_RDWR    02
#define O_CREAT   0100
#define O_TRUNC   01000
#define O_APPEND  02000

typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v)      __builtin_va_end(v)
#define va_arg(v, l)   __builtin_va_arg(v, l)
#define va_copy(d, s)  __builtin_va_copy(d, s)

/* Системные вызовы */
int     open(const char *path, int flags);
int     close(int fd);
int64_t read(int fd, void *buf, size_t count);
int64_t write(int fd, const void *buf, size_t count);
void    exit(int status);
int     fork(void);
int     execve(const char *path, char **argv, char **envp);
int     waitpid(int pid, int *status, int options);
int     getpid(void);
int     getuid(void);
int64_t time(void);
void   *sbrk(intptr_t increment);

/* Строковые и служебные функции */
size_t  strlen(const char *s);
int     strcmp(const char *s1, const char *s2);
int     strncmp(const char *s1, const char *s2, size_t n);
char   *strcpy(char *dest, const char *src);
char   *strncpy(char *dest, const char *src, size_t n);
void   *memcpy(void *dest, const void *src, size_t n);
void   *memset(void *s, int c, size_t n);
int64_t atoi(const char *s);

/* Форматированный ввод-вывод */
int     vsnprintf(char *str, size_t size, const char *format, va_list ap);
int     snprintf(char *str, size_t size, const char *format, ...);
int     printf(const char *format, ...);
int     dprintf(int fd, const char *format, ...);

/* Управление динамической памятью */
void   *malloc(size_t size);
void    free(void *ptr);

#endif
