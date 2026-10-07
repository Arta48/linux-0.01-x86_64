#ifndef _LINUX_FS_H
#define _LINUX_FS_H

#include <linux/types.h>

#define NR_OPEN       16
#define MAX_FILENAME  32
#define MAX_FILES     16
#define PIPE_BUF_SIZE 4064

#define FILE_TYPE_REGULAR 1
#define FILE_TYPE_PIPE    2

/* Структура гарантированно укладывается в 4096 байт (24 байта заголовок + 4064 буфер = 4088 байт) */
struct pipe {
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    int readers;
    int writers;
    int ref_count; /* Единый счетчик дескрипторов для защиты от double free */
    char buffer[PIPE_BUF_SIZE];
};

struct file {
    int type;                  /* REGULAR или PIPE */
    int mode;                  /* 1 = чтение, 2 = запись */
    const char *name;
    const char *data;
    uint64_t size;
    uint64_t pos;
    struct pipe *pipe;
    int in_use;
};

void fs_init(void);
int64_t sys_open(const char *filename, int flags);
int64_t sys_close(int fd);
int64_t sys_file_read(int fd, char *buf, uint64_t count);
int64_t sys_list(char *buf, uint64_t max_len);

int64_t sys_pipe(int *pipefd);
int64_t pipe_read(struct file *f, char *buf, uint64_t count);
int64_t pipe_write(struct file *f, const char *buf, uint64_t count);

#endif
