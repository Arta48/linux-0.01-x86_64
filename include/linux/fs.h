#ifndef _LINUX_FS_H
#define _LINUX_FS_H

#include <linux/types.h>

#define NR_OPEN       16
#define MAX_FILENAME  32
#define MAX_FILES     16
#define PIPE_BUF_SIZE 4064

#define FILE_TYPE_REGULAR 1
#define FILE_TYPE_PIPE    2

/* Базовый виртуальный адрес для загрузки исполняемых файлов (1.5 ГБ) */
#define USER_TEXT_BASE 0x60000000ULL

/* Сигнатура исполняемого бинарного формата: "LINUS001" */
#define EXEC_MAGIC 0x4C494E5553303031ULL

struct exec_header {
    uint64_t magic;      /* "LINUS001" */
    uint64_t entry;      /* Точка входа (RIP) */
    uint64_t text_size;  /* Размер машинного кода */
} __attribute__((packed));

struct pipe {
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    int readers;
    int writers;
    int ref_count;
    char buffer[PIPE_BUF_SIZE];
};

struct file {
    int type;
    int mode;
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

const char *fs_get_file_data(const char *name, uint64_t *out_size);

#endif
