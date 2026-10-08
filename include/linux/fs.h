#ifndef _LINUX_FS_H
#define _LINUX_FS_H

#include <linux/types.h>
#include <linux/stat.h>

#define NR_OPEN       16
#define MAX_FILENAME  80
#define MAX_FILES     128
#define PIPE_BUF_SIZE 4064

#define FILE_TYPE_REGULAR 1
#define FILE_TYPE_PIPE    2
#define FILE_TYPE_BLOCK   3
#define FILE_TYPE_MINIX   4

#define O_RDONLY  00
#define O_WRONLY  01
#define O_RDWR    02
#define O_CREAT   0100
#define O_TRUNC   01000
#define O_APPEND  02000

#define USER_TEXT_BASE 0x60000000ULL
#define EXEC_MAGIC     0x4C494E5553303031ULL

struct exec_header {
    uint64_t magic;
    uint64_t entry;
    uint64_t text_size;
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

struct ram_file {
    char name[MAX_FILENAME];
    char *data;
    uint64_t size;
    uint64_t capacity;
    uint64_t mtime;
    uint16_t uid;
    uint16_t gid;
    uint16_t mode;
    int in_use;
    int is_readonly;
    int is_dir;
    int is_dev_blk;
    uint8_t dev_id;
};

struct file {
    int type;
    int mode;
    uint64_t pos;
    struct ram_file *rf;
    struct pipe *pipe;
    uint32_t minix_ino;        /* Номер инода на Minix v1 FS */
    int in_use;
};

void fs_init(void);
void tarfs_mount(uint64_t archive_start, uint64_t archive_end);
int ramfs_create_dir(const char *path, uint16_t mode);
int ramfs_create_file(const char *path, const char *data, uint64_t size, uint16_t mode, uint16_t uid, uint16_t gid, uint64_t mtime);

int64_t sys_open(const char *filename, int flags);
int64_t sys_close(int fd);
int64_t sys_file_read(int fd, char *buf, uint64_t count);
int64_t sys_file_write(int fd, const char *buf, uint64_t count);
int64_t sys_unlink(const char *filename);
int64_t sys_chmod(const char *filename, int mode);
int64_t sys_stat(const char *filename, struct stat *statbuf);
int64_t sys_list(const char *dir_path, char *buf, uint64_t max_len, int is_long);

int64_t sys_chdir(const char *path);
int64_t sys_mkdir(const char *path);
int64_t sys_rmdir(const char *path);
int64_t sys_getcwd(char *buf, uint64_t size);

int64_t sys_pipe(int *pipefd);
int64_t pipe_read(struct file *f, char *buf, uint64_t count);
int64_t pipe_write(struct file *f, const char *buf, uint64_t count);

const char *fs_get_file_data(const char *name, uint64_t *out_size);

#endif
