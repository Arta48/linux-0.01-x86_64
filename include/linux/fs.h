#ifndef _LINUX_FS_H
#define _LINUX_FS_H

#include <linux/types.h>

#define NR_OPEN      16
#define MAX_FILENAME 32
#define MAX_FILES    16

struct file {
    const char *name;
    const char *data;
    uint64_t size;
    uint64_t pos;
    int in_use;
};

void fs_init(void);
int64_t sys_open(const char *filename, int flags);
int64_t sys_close(int fd);
int64_t sys_file_read(int fd, char *buf, uint64_t count);
int64_t sys_list(char *buf, uint64_t max_len);

#endif
