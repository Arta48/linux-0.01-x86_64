#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/string.h>

struct ram_file {
    char name[MAX_FILENAME];
    const char *data;
    uint64_t size;
};

static const struct ram_file files[] = {
    {
        .name = "README.txt",
        .data = "====================================================\n"
        "  Linux 0.01 (x86_64 Edition)\n"
        "  Re-engineered for 64-bit Long Mode from 1991 code.\n"
        "====================================================\n"
        "Features:\n"
        "  - 4-level paging (PML4, PDPT, PD, PT)\n"
        "  - Preemptive multitasking & decay scheduler\n"
        "  - Ring 3 user space isolation via TSS.rsp0\n"
        "  - Fast hardware MSR syscall / sysret\n"
        "  - In-memory Virtual File System (RamFS)\n",
        .size = 373
    },
    {
        .name = "version",
        .data = "Linux version 0.01-x86_64 (root@arch) (gcc 14) #1 PREEMPT 2026\n",
        .size = 64
    },
    {
        .name = "author",
        .data = "Original: Linus Torvalds (Helsinki, 1991)\n"
        "x86_64 Port: Educational Project (2026)\n",
        .size = 82
    },
    {
        .name = "motd",
        .data = "Welcome to 64-bit Unix! Have a lot of fun hacking kernels.\n",
        .size = 59
    }
};

#define TOTAL_FILES (sizeof(files) / sizeof(files[0]))

void fs_init(void)
{
    printk("[OK] Virtual File System (RamFS) Initialized (%d embedded files)\n", (int)TOTAL_FILES);
}

int64_t sys_open(const char *filename, int flags)
{
    (void)flags;

    int file_idx = -1;
    for (uint64_t i = 0; i < TOTAL_FILES; i++) {
        if (strcmp(filename, files[i].name) == 0) {
            file_idx = (int)i;
            break;
        }
    }

    if (file_idx == -1) {
        return -1;
    }

    for (int fd = 3; fd < NR_OPEN; fd++) {
        if (!current->filp[fd].in_use) {
            current->filp[fd].name = files[file_idx].name;
            current->filp[fd].data = files[file_idx].data;
            current->filp[fd].size = files[file_idx].size;
            current->filp[fd].pos  = 0;
            current->filp[fd].in_use = 1;
            return fd;
        }
    }

    return -1;
}

int64_t sys_close(int fd)
{
    if (fd < 3 || fd >= NR_OPEN || !current->filp[fd].in_use) {
        return -1;
    }
    current->filp[fd].in_use = 0;
    return 0;
}

int64_t sys_file_read(int fd, char *buf, uint64_t count)
{
    if (fd < 3 || fd >= NR_OPEN || !current->filp[fd].in_use) {
        return -1;
    }

    struct file *f = &current->filp[fd];
    if (f->pos >= f->size) {
        return 0;
    }

    uint64_t bytes_to_read = count;
    if (f->pos + bytes_to_read > f->size) {
        bytes_to_read = f->size - f->pos;
    }

    for (uint64_t i = 0; i < bytes_to_read; i++) {
        buf[i] = f->data[f->pos + i];
    }

    f->pos += bytes_to_read;
    return bytes_to_read;
}

int64_t sys_list(char *buf, uint64_t max_len)
{
    uint64_t offset = 0;

    for (uint64_t i = 0; i < TOTAL_FILES; i++) {
        const char *name = files[i].name;
        while (*name && offset < max_len - 16) {
            buf[offset++] = *name++;
        }
        buf[offset++] = '\t';
        buf[offset++] = '(';

        char num[16];
        int ni = 0;
        uint64_t sz = files[i].size;
        if (sz == 0) num[ni++] = '0';
        while (sz > 0) {
            num[ni++] = '0' + (sz % 10);
            sz /= 10;
        }
        while (--ni >= 0 && offset < max_len - 8) {
            buf[offset++] = num[ni];
        }

        buf[offset++] = ' ';
        buf[offset++] = 'B';
        buf[offset++] = ')';
        buf[offset++] = '\n';
    }

    buf[offset] = '\0';
    return offset;
}
