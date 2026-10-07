#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/time.h>

static const unsigned char bin_hello[] = {
    0x31, 0x30, 0x30, 0x53, 0x55, 0x4E, 0x49, 0x4C,
    0x18, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00,
    0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x48, 0xc7, 0xc0, 0x04, 0x00, 0x00, 0x00,
    0x48, 0xc7, 0xc7, 0x01, 0x00, 0x00, 0x00,
    0x48, 0x8d, 0x35, 0x15, 0x00, 0x00, 0x00,
    0x48, 0xc7, 0xc2, 0x34, 0x00, 0x00, 0x00,
    0x0f, 0x05,
    0x48, 0xc7, 0xc0, 0x01, 0x00, 0x00, 0x00,
    0x48, 0xc7, 0xc7, 0x2a, 0x00, 0x00, 0x00,
    0x0f, 0x05,
    'H', 'e', 'l', 'l', 'o', ' ', 'f', 'r', 'o', 'm', ' ',
    's', 't', 'a', 'n', 'd', 'a', 'l', 'o', 'n', 'e', ' ',
    'b', 'i', 'n', 'a', 'r', 'y', ' ', 'l', 'o', 'a', 'd', 'e', 'd', ' ',
    'b', 'y', ' ', 'e', 'x', 'e', 'c', 'v', 'e', '!', '\n', '\0'
};

static const unsigned char bin_calc[] = {
    0x31, 0x30, 0x30, 0x53, 0x55, 0x4E, 0x49, 0x4C,
    0x18, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00,
    0x4A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x48, 0xc7, 0xc0, 0x04, 0x00, 0x00, 0x00,
    0x48, 0xc7, 0xc7, 0x01, 0x00, 0x00, 0x00,
    0x48, 0x8d, 0x35, 0x24, 0x00, 0x00, 0x00,
    0x48, 0xc7, 0xc2, 0x2a, 0x00, 0x00, 0x00,
    0x0f, 0x05,
    0x48, 0xc7, 0xc0, 0x0a, 0x00, 0x00, 0x00,
    0x48, 0x05, 0x14, 0x00, 0x00, 0x00,
    0x48, 0x6b, 0xc0, 0x03,
    0x48, 0x89, 0xc7,
    0x48, 0xc7, 0xc0, 0x01, 0x00, 0x00, 0x00,
    0x0f, 0x05,
    '[', 'C', 'A', 'L', 'C', ']', ' ', 'C', 'o', 'm', 'p', 'u', 't', 'i', 'n', 'g', ' ',
    '(', '1', '0', ' ', '+', ' ', '2', '0', ')', ' ', '*', ' ', '3', ' ', 'i', 'n', ' ',
    'R', 'i', 'n', 'g', ' ', '3', '.', '.', '.', '\n', '\0'
};

static struct ram_file ram_files[MAX_FILES];

static void resolve_path(const char *in, char *out)
{
    char tmp[MAX_FILENAME];
    uint64_t ti = 0;

    if (in[0] == '/') {
        tmp[ti++] = '/';
        in++;
    } else {
        const char *c = current->cwd;
        while (*c && ti < MAX_FILENAME - 2) tmp[ti++] = *c++;
        if (tmp[ti - 1] != '/') tmp[ti++] = '/';
    }

    while (*in && ti < MAX_FILENAME - 1) {
        if (*in == '/') {
            in++;
            continue;
        }

        char comp[MAX_FILENAME];
        uint64_t ci = 0;
        while (*in && *in != '/' && ci < MAX_FILENAME - 1) {
            comp[ci++] = *in++;
        }
        comp[ci] = '\0';

        if (strcmp(comp, ".") == 0) {
            continue;
        } else if (strcmp(comp, "..") == 0) {
            if (ti > 1) {
                ti--;
                while (ti > 1 && tmp[ti - 1] != '/') ti--;
            }
        } else {
            if (tmp[ti - 1] != '/') tmp[ti++] = '/';
            for (uint64_t k = 0; k < ci && ti < MAX_FILENAME - 1; k++) {
                tmp[ti++] = comp[k];
            }
        }
    }

    if (ti > 1 && tmp[ti - 1] == '/') ti--;
    tmp[ti] = '\0';

    memcpy(out, tmp, ti + 1);
}

void fs_init(void)
{
    for (int i = 0; i < MAX_FILES; i++) {
        ram_files[i].in_use = 0;
        ram_files[i].name[0] = '\0';
        ram_files[i].data = NULL;
        ram_files[i].size = 0;
        ram_files[i].capacity = 0;
        ram_files[i].mtime = startup_time;
        ram_files[i].is_readonly = 0;
        ram_files[i].is_dir = 0;
    }

    const char *dirs[] = { "/", "/bin", "/etc", "/home" };
    for (int i = 0; i < 4; i++) {
        uint64_t len = strlen(dirs[i]);
        memcpy(ram_files[i].name, dirs[i], len + 1);
        ram_files[i].is_dir = 1;
        ram_files[i].in_use = 1;
        ram_files[i].is_readonly = 1;
        ram_files[i].mtime = startup_time;
    }

    const char *init_names[] = {
        "/README.txt", "/version", "/author", "/etc/motd", "/bin/hello", "/bin/calc"
    };
    const char *init_data[] = {
        "====================================================\n"
        "  Linux 0.01 (x86_64 Edition)\n"
        "  Re-engineered for 64-bit Long Mode from 1991 code.\n"
        "====================================================\n"
        "Features:\n"
        "  - 4-level paging (PML4, PDPT, PD, PT)\n"
        "  - Preemptive multitasking & decay scheduler\n"
        "  - Ring 3 user space isolation via TSS.rsp0\n"
        "  - Fast hardware MSR syscall / sysret\n"
        "  - Dynamic VFS with directories, cd, pwd & mkdir\n"
        "  - Unix Pipes (IPC) & dup2 redirection (> and |)\n"
        "  - Binary execution via fork() + execve()\n"
        "  - Signals & Ctrl+C interruption\n"
        "  - Real-Time Clock (CMOS RTC) & ls -l\n",

        "Linux version 0.01-x86_64 (root@arch) (gcc 14) #1 PREEMPT 2026\n",
        "Original: Linus Torvalds (Helsinki, 1991)\nx86_64 Port: Educational Project (2026)\n",
        "Welcome to 64-bit Unix! Have a lot of fun hacking kernels.\n",
        (const char *)bin_hello,
        (const char *)bin_calc
    };

    uint64_t init_sizes[] = {
        0, 0, 0, 0, sizeof(bin_hello), sizeof(bin_calc)
    };

    for (int i = 0; i < 6; i++) {
        int idx = 4 + i;
        uint64_t nlen = strlen(init_names[i]);
        memcpy(ram_files[idx].name, init_names[i], nlen + 1);
        ram_files[idx].data = (char *)init_data[i];
        ram_files[idx].size = (i < 4) ? strlen(init_data[i]) : init_sizes[i];
        ram_files[idx].capacity = ram_files[idx].size;
        ram_files[idx].in_use = 1;
        ram_files[idx].is_readonly = 1;
        ram_files[idx].is_dir = 0;
        ram_files[idx].mtime = startup_time;
    }

    printk("[OK] Hierarchical VFS Initialized (Dirs: /, /bin, /etc, /home)\n");
}

const char *fs_get_file_data(const char *name, uint64_t *out_size)
{
    char full[MAX_FILENAME];
    resolve_path(name, full);

    for (int i = 0; i < MAX_FILES; i++) {
        if (ram_files[i].in_use && !ram_files[i].is_dir && strcmp(full, ram_files[i].name) == 0) {
            if (out_size) *out_size = ram_files[i].size;
            return ram_files[i].data;
        }
    }
    return NULL;
}

int64_t sys_open(const char *filename, int flags)
{
    char full[MAX_FILENAME];
    resolve_path(filename, full);

    int file_idx = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (ram_files[i].in_use && strcmp(full, ram_files[i].name) == 0) {
            if (ram_files[i].is_dir) return -1;
            file_idx = i;
            break;
        }
    }

    if (file_idx == -1) {
        if (!(flags & O_CREAT)) {
            return -1;
        }

        for (int i = 0; i < MAX_FILES; i++) {
            if (!ram_files[i].in_use) {
                file_idx = i;
                break;
            }
        }
        if (file_idx == -1) return -1;

        uint64_t page = get_free_page();
        if (!page) return -1;

        uint64_t nlen = strlen(full);
        memcpy(ram_files[file_idx].name, full, nlen + 1);
        ram_files[file_idx].data = (char *)page;
        ram_files[file_idx].size = 0;
        ram_files[file_idx].capacity = PAGE_SIZE;
        ram_files[file_idx].mtime = get_current_time();
        ram_files[file_idx].in_use = 1;
        ram_files[file_idx].is_readonly = 0;
        ram_files[file_idx].is_dir = 0;
    }

    if ((flags & O_TRUNC) && !ram_files[file_idx].is_readonly) {
        ram_files[file_idx].size = 0;
        ram_files[file_idx].mtime = get_current_time();
    }

    for (int fd = 3; fd < NR_OPEN; fd++) {
        if (!current->filp[fd].in_use) {
            current->filp[fd].type = FILE_TYPE_REGULAR;
            current->filp[fd].rf   = &ram_files[file_idx];
            current->filp[fd].pos  = 0;
            current->filp[fd].pipe = NULL;
            current->filp[fd].in_use = 1;
            current->filp[fd].mode = (flags & 3) ? (flags & 3) : 1;
            return fd;
        }
    }

    return -1;
}

int64_t sys_close(int fd)
{
    if (fd < 0 || fd >= NR_OPEN || !current->filp[fd].in_use) {
        return -1;
    }

    struct file *f = &current->filp[fd];

    if (f->type == FILE_TYPE_PIPE && f->pipe) {
        if (f->mode == 1) f->pipe->readers--;
        if (f->mode == 2) f->pipe->writers--;

        f->pipe->ref_count--;
        if (f->pipe->ref_count <= 0) {
            free_page((uint64_t)f->pipe);
        }
    }

    f->in_use = 0;
    f->type = 0;
    f->mode = 0;
    f->rf = NULL;
    f->pipe = NULL;
    return 0;
}

int64_t sys_file_read(int fd, char *buf, uint64_t count)
{
    if (fd < 0 || fd >= NR_OPEN || !current->filp[fd].in_use) {
        return -1;
    }

    struct file *f = &current->filp[fd];

    if (f->type == FILE_TYPE_PIPE) {
        return pipe_read(f, buf, count);
    }

    struct ram_file *rf = f->rf;
    if (!rf || f->pos >= rf->size) {
        return 0;
    }

    uint64_t bytes_to_read = count;
    if (f->pos + bytes_to_read > rf->size) {
        bytes_to_read = rf->size - f->pos;
    }

    for (uint64_t i = 0; i < bytes_to_read; i++) {
        buf[i] = rf->data[f->pos + i];
    }

    f->pos += bytes_to_read;
    return bytes_to_read;
}

int64_t sys_file_write(int fd, const char *buf, uint64_t count)
{
    if (fd < 0 || fd >= NR_OPEN || !current->filp[fd].in_use) {
        return -1;
    }

    struct file *f = &current->filp[fd];

    if (f->type == FILE_TYPE_PIPE) {
        return pipe_write(f, buf, count);
    }

    struct ram_file *rf = f->rf;
    if (!rf || rf->is_readonly) {
        return -1;
    }

    uint64_t bytes_to_write = count;
    if (f->pos + bytes_to_write > rf->capacity) {
        bytes_to_write = rf->capacity - f->pos;
    }

    for (uint64_t i = 0; i < bytes_to_write; i++) {
        rf->data[f->pos + i] = buf[i];
    }

    f->pos += bytes_to_write;
    if (f->pos > rf->size) {
        rf->size = f->pos;
    }
    rf->mtime = get_current_time();

    return bytes_to_write;
}

int64_t sys_unlink(const char *filename)
{
    char full[MAX_FILENAME];
    resolve_path(filename, full);

    for (int i = 0; i < MAX_FILES; i++) {
        if (ram_files[i].in_use && strcmp(full, ram_files[i].name) == 0) {
            if (ram_files[i].is_readonly || ram_files[i].is_dir) {
                return -1;
            }

            if (ram_files[i].data) {
                free_page((uint64_t)ram_files[i].data);
            }

            ram_files[i].in_use = 0;
            ram_files[i].name[0] = '\0';
            ram_files[i].data = NULL;
            ram_files[i].size = 0;
            return 0;
        }
    }
    return -1;
}

int64_t sys_chdir(const char *path)
{
    char full[MAX_FILENAME];
    resolve_path(path, full);

    for (int i = 0; i < MAX_FILES; i++) {
        if (ram_files[i].in_use && ram_files[i].is_dir && strcmp(full, ram_files[i].name) == 0) {
            uint64_t len = strlen(full);
            memcpy(current->cwd, full, len + 1);
            return 0;
        }
    }
    return -1;
}

int64_t sys_mkdir(const char *path)
{
    char full[MAX_FILENAME];
    resolve_path(path, full);

    for (int i = 0; i < MAX_FILES; i++) {
        if (ram_files[i].in_use && strcmp(full, ram_files[i].name) == 0) {
            return -1;
        }
    }

    for (int i = 0; i < MAX_FILES; i++) {
        if (!ram_files[i].in_use) {
            uint64_t len = strlen(full);
            memcpy(ram_files[i].name, full, len + 1);
            ram_files[i].is_dir = 1;
            ram_files[i].in_use = 1;
            ram_files[i].is_readonly = 0;
            ram_files[i].size = 0;
            ram_files[i].mtime = get_current_time();
            ram_files[i].data = NULL;
            return 0;
        }
    }
    return -1;
}

int64_t sys_rmdir(const char *path)
{
    char full[MAX_FILENAME];
    resolve_path(path, full);

    if (strcmp(full, "/") == 0) return -1;

    int dir_idx = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (ram_files[i].in_use && ram_files[i].is_dir && strcmp(full, ram_files[i].name) == 0) {
            if (ram_files[i].is_readonly) return -1;
            dir_idx = i;
            break;
        }
    }
    if (dir_idx == -1) return -1;

    uint64_t dlen = strlen(full);
    for (int i = 0; i < MAX_FILES; i++) {
        if (i != dir_idx && ram_files[i].in_use) {
            if (strncmp(ram_files[i].name, full, dlen) == 0 && ram_files[i].name[dlen] == '/') {
                return -1;
            }
        }
    }

    ram_files[dir_idx].in_use = 0;
    ram_files[dir_idx].name[0] = '\0';
    return 0;
}

int64_t sys_getcwd(char *buf, uint64_t size)
{
    uint64_t len = strlen(current->cwd);
    if (len >= size) return -1;
    memcpy(buf, current->cwd, len + 1);
    return len;
}

static const char *month_names[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* Поддержка обычного ls и подробного ls -l */
int64_t sys_list(const char *dir_path, char *buf, uint64_t max_len, int is_long)
{
    char target[MAX_FILENAME];
    if (dir_path && dir_path[0] != '\0') {
        resolve_path(dir_path, target);
    } else {
        memcpy(target, current->cwd, strlen(current->cwd) + 1);
    }

    uint64_t tlen = strlen(target);
    uint64_t offset = 0;

    for (int i = 0; i < MAX_FILES; i++) {
        if (!ram_files[i].in_use) continue;
        if (strcmp(ram_files[i].name, target) == 0) continue;

        const char *name = ram_files[i].name;

        int match = 0;
        if (strcmp(target, "/") == 0) {
            if (name[0] == '/' && name[1] != '\0') {
                const char *sub = name + 1;
                while (*sub && *sub != '/') sub++;
                if (*sub == '\0') match = 1;
            }
        } else {
            if (strncmp(name, target, tlen) == 0 && name[tlen] == '/') {
                const char *sub = name + tlen + 1;
                while (*sub && *sub != '/') sub++;
                if (*sub == '\0') match = 1;
            }
        }

        if (match) {
            const char *display = (strcmp(target, "/") == 0) ? name + 1 : name + tlen + 1;

            if (is_long) {
                /* Права доступа: drwxr-xr-x для каталогов, -rw-r--r-- для файлов */
                const char *perms = ram_files[i].is_dir ? "drwxr-xr-x  " :
                (ram_files[i].is_readonly ? "-rwxr-xr-x  " : "-rw-r--r--  ");
                while (*perms && offset < max_len - 32) buf[offset++] = *perms++;

                /* Размер в байтах */
                char szbuf[16];
                int szi = 0;
                uint64_t sz = ram_files[i].size;
                if (sz == 0) szbuf[szi++] = '0';
                while (sz > 0) {
                    szbuf[szi++] = '0' + (sz % 10);
                    sz /= 10;
                }
                while (szi < 6) szbuf[szi++] = ' '; /* Выравнивание */
                    while (--szi >= 0 && offset < max_len - 16) buf[offset++] = szbuf[szi];
                    buf[offset++] = ' ';
                buf[offset++] = 'B';
                buf[offset++] = ' ';
                buf[offset++] = ' ';

                /* Имя файла */
                while (*display && offset < max_len - 16) buf[offset++] = *display++;
                if (ram_files[i].is_dir) buf[offset++] = '/';
                buf[offset++] = '\n';
            } else {
                /* Компактный вывод */
                while (*display && offset < max_len - 16) buf[offset++] = *display++;

                if (ram_files[i].is_dir) {
                    buf[offset++] = '/';
                    buf[offset++] = '\n';
                } else {
                    buf[offset++] = '\t';
                    buf[offset++] = '(';
                    char num[16];
                    int ni = 0;
                    uint64_t sz = ram_files[i].size;
                    if (sz == 0) num[ni++] = '0';
                    while (sz > 0) {
                        num[ni++] = '0' + (sz % 10);
                        sz /= 10;
                    }
                    while (--ni >= 0 && offset < max_len - 8) buf[offset++] = num[ni];
                    buf[offset++] = ' ';
                    buf[offset++] = 'B';
                    buf[offset++] = ')';
                    buf[offset++] = '\n';
                }
            }
        }
    }

    buf[offset] = '\0';
    return offset;
}
