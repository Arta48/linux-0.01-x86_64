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

struct proc_ram_file {
    struct ram_file base;
    int is_proc;
    void (*generator)(char *buf, uint64_t max_len);
};

static struct proc_ram_file ram_files[MAX_FILES];

/* Аппаратный опрос модели процессора через CPUID */
static void get_cpu_info(char *vendor, char *brand)
{
    uint32_t eax, ebx, ecx, edx;

    /* Leaf 0: Vendor String */
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    memcpy(vendor, &ebx, 4);
    memcpy(vendor + 4, &edx, 4);
    memcpy(vendor + 8, &ecx, 4);
    vendor[12] = '\0';

    /* Leaf 0x80000000..0x80000004: Processor Brand String */
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000000));
    if (eax >= 0x80000004) {
        uint32_t *b = (uint32_t *)brand;
        for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            __asm__ volatile ("cpuid" : "=a"(b[0]), "=b"(b[1]), "=c"(b[2]), "=d"(b[3]) : "a"(leaf));
            b += 4;
        }
        brand[48] = '\0';

        /* Убираем начальные пробелы */
        char *p = brand;
        while (*p == ' ') p++;
        if (p != brand) {
            uint64_t len = strlen(p);
            memcpy(brand, p, len + 1);
        }
    } else {
        memcpy(brand, "Generic x86_64 Processor", 25);
    }
}

/* Генератор /proc/cpuinfo */
static void generate_cpuinfo(char *buf, uint64_t max_len)
{
    char vendor[16];
    char brand[64];
    get_cpu_info(vendor, brand);

    uint64_t o = 0;
    const char *header = "processor\t: 0\nvendor_id\t: ";
    while (*header && o < max_len - 128) buf[o++] = *header++;
    const char *v = vendor;
    while (*v && o < max_len - 128) buf[o++] = *v++;

    const char *m = "\nmodel name\t: ";
    while (*m && o < max_len - 128) buf[o++] = *m++;
    const char *b = brand;
    while (*b && o < max_len - 128) buf[o++] = *b++;

    const char *tail = "\ncpu MHz\t\t: 3600.00\nflags\t\t: fpu sse sse2 syscall lm\n";
    while (*tail && o < max_len - 1) buf[o++] = *tail++;
    buf[o] = '\0';
}

/* Генератор /proc/meminfo */
static void generate_meminfo(char *buf, uint64_t max_len)
{
    uint32_t free_p = get_free_pages_count();
    uint64_t total_kb = HIGH_MEMORY / 1024;
    uint64_t free_kb = ((uint64_t)free_p * PAGE_SIZE) / 1024;
    uint64_t used_kb = total_kb - free_kb;

    uint64_t o = 0;

    const char *s1 = "MemTotal:\t";
    while (*s1) buf[o++] = *s1++;
    char nb[16]; int ni = 0; uint64_t v = total_kb;
    while (v > 0) { nb[ni++] = '0' + (v % 10); v /= 10; }
    while (--ni >= 0) buf[o++] = nb[ni];
    const char *kb = " kB\nMemFree:\t";
    while (*kb) buf[o++] = *kb++;

    ni = 0; v = free_kb;
    if (v == 0) nb[ni++] = '0';
    while (v > 0) { nb[ni++] = '0' + (v % 10); v /= 10; }
    while (--ni >= 0) buf[o++] = nb[ni];
    const char *s2 = " kB\nMemUsed:\t";
    while (*s2) buf[o++] = *s2++;

    ni = 0; v = used_kb;
    if (v == 0) nb[ni++] = '0';
    while (v > 0) { nb[ni++] = '0' + (v % 10); v /= 10; }
    while (--ni >= 0) buf[o++] = nb[ni];
    const char *s3 = " kB\n";
    while (*s3 && o < max_len - 1) buf[o++] = *s3++;

    buf[o] = '\0';
}

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
        if (*in == '/') { in++; continue; }

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
        ram_files[i].base.in_use = 0;
        ram_files[i].base.name[0] = '\0';
        ram_files[i].base.data = NULL;
        ram_files[i].base.size = 0;
        ram_files[i].base.capacity = 0;
        ram_files[i].base.mtime = startup_time;
        ram_files[i].base.is_readonly = 0;
        ram_files[i].base.is_dir = 0;
        ram_files[i].is_proc = 0;
        ram_files[i].generator = NULL;
    }

    /* Системные каталоги: /, /bin, /etc, /home, /proc */
    const char *dirs[] = { "/", "/bin", "/etc", "/home", "/proc" };
    for (int i = 0; i < 5; i++) {
        uint64_t len = strlen(dirs[i]);
        memcpy(ram_files[i].base.name, dirs[i], len + 1);
        ram_files[i].base.is_dir = 1;
        ram_files[i].base.in_use = 1;
        ram_files[i].base.is_readonly = 1;
        ram_files[i].base.mtime = startup_time;
    }

    /* Статические файлы */
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
        "  - Real-Time Clock (CMOS RTC) & ls -l\n"
        "  - Dynamic ProcFS (/proc/cpuinfo, /proc/meminfo)\n",

        "Linux version 0.01-x86_64 (root@arch) (gcc 14) #1 PREEMPT 2026\n",
        "Original: Linus Torvalds (Helsinki, 1991)\nx86_64 Port: Educational Project (2026)\n",
        "Welcome to 64-bit Unix! Have a lot of fun hacking kernels.\n",
        (const char *)bin_hello,
        (const char *)bin_calc
    };

    uint64_t init_sizes[] = { 0, 0, 0, 0, sizeof(bin_hello), sizeof(bin_calc) };

    for (int i = 0; i < 6; i++) {
        int idx = 5 + i;
        uint64_t nlen = strlen(init_names[i]);
        memcpy(ram_files[idx].base.name, init_names[i], nlen + 1);
        ram_files[idx].base.data = (char *)init_data[i];
        ram_files[idx].base.size = (i < 4) ? strlen(init_data[i]) : init_sizes[i];
        ram_files[idx].base.capacity = ram_files[idx].base.size;
        ram_files[idx].base.in_use = 1;
        ram_files[idx].base.is_readonly = 1;
        ram_files[idx].base.is_dir = 0;
        ram_files[idx].base.mtime = startup_time;
    }

    /* Файлы /proc/cpuinfo и /proc/meminfo */
    int c_idx = 11;
    memcpy(ram_files[c_idx].base.name, "/proc/cpuinfo", 14);
    ram_files[c_idx].base.data = (char *)get_free_page();
    ram_files[c_idx].base.capacity = PAGE_SIZE;
    ram_files[c_idx].base.in_use = 1;
    ram_files[c_idx].base.is_readonly = 1;
    ram_files[c_idx].is_proc = 1;
    ram_files[c_idx].generator = generate_cpuinfo;

    int m_idx = 12;
    memcpy(ram_files[m_idx].base.name, "/proc/meminfo", 14);
    ram_files[m_idx].base.data = (char *)get_free_page();
    ram_files[m_idx].base.capacity = PAGE_SIZE;
    ram_files[m_idx].base.in_use = 1;
    ram_files[m_idx].base.is_readonly = 1;
    ram_files[m_idx].is_proc = 1;
    ram_files[m_idx].generator = generate_meminfo;

    printk("[OK] ProcFS Initialized (/proc/cpuinfo, /proc/meminfo)\n");
}

const char *fs_get_file_data(const char *name, uint64_t *out_size)
{
    char full[MAX_FILENAME];
    resolve_path(name, full);

    for (int i = 0; i < MAX_FILES; i++) {
        if (ram_files[i].base.in_use && !ram_files[i].base.is_dir && strcmp(full, ram_files[i].base.name) == 0) {
            if (ram_files[i].is_proc && ram_files[i].generator) {
                ram_files[i].generator(ram_files[i].base.data, ram_files[i].base.capacity);
                ram_files[i].base.size = strlen(ram_files[i].base.data);
            }
            if (out_size) *out_size = ram_files[i].base.size;
            return ram_files[i].base.data;
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
        if (ram_files[i].base.in_use && strcmp(full, ram_files[i].base.name) == 0) {
            if (ram_files[i].base.is_dir) return -1;
            file_idx = i;
            break;
        }
    }

    if (file_idx == -1) {
        if (!(flags & O_CREAT)) return -1;

        for (int i = 0; i < MAX_FILES; i++) {
            if (!ram_files[i].base.in_use) { file_idx = i; break; }
        }
        if (file_idx == -1) return -1;

        uint64_t page = get_free_page();
        if (!page) return -1;

        uint64_t nlen = strlen(full);
        memcpy(ram_files[file_idx].base.name, full, nlen + 1);
        ram_files[file_idx].base.data = (char *)page;
        ram_files[file_idx].base.size = 0;
        ram_files[file_idx].base.capacity = PAGE_SIZE;
        ram_files[file_idx].base.mtime = get_current_time();
        ram_files[file_idx].base.in_use = 1;
        ram_files[file_idx].base.is_readonly = 0;
        ram_files[file_idx].base.is_dir = 0;
        ram_files[file_idx].is_proc = 0;
        ram_files[file_idx].generator = NULL;
    }

    /* При открытии /proc генерируем свежие данные */
    if (ram_files[file_idx].is_proc && ram_files[file_idx].generator) {
        ram_files[file_idx].generator(ram_files[file_idx].base.data, ram_files[file_idx].base.capacity);
        ram_files[file_idx].base.size = strlen(ram_files[file_idx].base.data);
    }

    if ((flags & O_TRUNC) && !ram_files[file_idx].base.is_readonly) {
        ram_files[file_idx].base.size = 0;
        ram_files[file_idx].base.mtime = get_current_time();
    }

    for (int fd = 3; fd < NR_OPEN; fd++) {
        if (!current->filp[fd].in_use) {
            current->filp[fd].type = FILE_TYPE_REGULAR;
            current->filp[fd].rf   = (struct ram_file *)&ram_files[file_idx].base;
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
        if (ram_files[i].base.in_use && strcmp(full, ram_files[i].base.name) == 0) {
            if (ram_files[i].base.is_readonly || ram_files[i].base.is_dir) {
                return -1;
            }

            if (ram_files[i].base.data) {
                free_page((uint64_t)ram_files[i].base.data);
            }

            ram_files[i].base.in_use = 0;
            ram_files[i].base.name[0] = '\0';
            ram_files[i].base.data = NULL;
            ram_files[i].base.size = 0;
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
        if (ram_files[i].base.in_use && ram_files[i].base.is_dir && strcmp(full, ram_files[i].base.name) == 0) {
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
        if (ram_files[i].base.in_use && strcmp(full, ram_files[i].base.name) == 0) {
            return -1;
        }
    }

    for (int i = 0; i < MAX_FILES; i++) {
        if (!ram_files[i].base.in_use) {
            uint64_t len = strlen(full);
            memcpy(ram_files[i].base.name, full, len + 1);
            ram_files[i].base.is_dir = 1;
            ram_files[i].base.in_use = 1;
            ram_files[i].base.is_readonly = 0;
            ram_files[i].base.size = 0;
            ram_files[i].base.mtime = get_current_time();
            ram_files[i].base.data = NULL;
            ram_files[i].is_proc = 0;
            ram_files[i].generator = NULL;
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
        if (ram_files[i].base.in_use && ram_files[i].base.is_dir && strcmp(full, ram_files[i].base.name) == 0) {
            if (ram_files[i].base.is_readonly) return -1;
            dir_idx = i;
            break;
        }
    }
    if (dir_idx == -1) return -1;

    uint64_t dlen = strlen(full);
    for (int i = 0; i < MAX_FILES; i++) {
        if (i != dir_idx && ram_files[i].base.in_use) {
            if (strncmp(ram_files[i].base.name, full, dlen) == 0 && ram_files[i].base.name[dlen] == '/') {
                return -1;
            }
        }
    }

    ram_files[dir_idx].base.in_use = 0;
    ram_files[dir_idx].base.name[0] = '\0';
    return 0;
}

int64_t sys_getcwd(char *buf, uint64_t size)
{
    uint64_t len = strlen(current->cwd);
    if (len >= size) return -1;
    memcpy(buf, current->cwd, len + 1);
    return len;
}

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
        if (!ram_files[i].base.in_use) continue;
        if (strcmp(ram_files[i].base.name, target) == 0) continue;

        const char *name = ram_files[i].base.name;

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
                /* ПОДРОБНЫЙ ВЫВОД (ls -l): права доступа, размер, имя */
                const char *perms = ram_files[i].base.is_dir ? "drwxr-xr-x  " :
                (ram_files[i].base.is_readonly ? "-rwxr-xr-x  " : "-rw-r--r--  ");
                while (*perms && offset < max_len - 32) buf[offset++] = *perms++;

                char szbuf[16];
                int szi = 0;
                uint64_t sz = ram_files[i].base.size;
                if (sz == 0) szbuf[szi++] = '0';
                while (sz > 0) {
                    szbuf[szi++] = '0' + (sz % 10);
                    sz /= 10;
                }
                while (szi < 6) szbuf[szi++] = ' ';
                while (--szi >= 0 && offset < max_len - 16) buf[offset++] = szbuf[szi];
                buf[offset++] = ' ';
                buf[offset++] = 'B';
                buf[offset++] = ' ';
                buf[offset++] = ' ';

                while (*display && offset < max_len - 16) buf[offset++] = *display++;
                if (ram_files[i].base.is_dir) buf[offset++] = '/';
                buf[offset++] = '\n';
            } else {
                /* КОМПАКТНЫЙ ВЫВОД (обычный ls): только имена файлов и папок */
                while (*display && offset < max_len - 16) buf[offset++] = *display++;
                if (ram_files[i].base.is_dir) {
                    buf[offset++] = '/';
                }
                buf[offset++] = '\n';
            }
        }
    }

    buf[offset] = '\0';
    return offset;
}
