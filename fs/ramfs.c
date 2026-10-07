#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/string.h>
#include <linux/mm.h>

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

/* Таблица файлов RamFS на 32 слота */
static struct ram_file ram_files[MAX_FILES];

void fs_init(void)
{
    for (int i = 0; i < MAX_FILES; i++) {
        ram_files[i].in_use = 0;
        ram_files[i].name[0] = '\0';
        ram_files[i].data = NULL;
        ram_files[i].size = 0;
        ram_files[i].capacity = 0;
        ram_files[i].is_readonly = 0;
    }

    /* Инициализируем системные файлы (Read-Only) */
    const char *init_names[] = { "README.txt", "version", "author", "motd", "hello", "calc" };
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
        "  - In-memory Virtual File System (RamFS)\n"
        "  - Inter-Process Communication (Unix Pipes)\n"
        "  - Binary program loader (sys_execve)\n"
        "  - Writable VFS, touch, rm & shell I/O redirection\n",

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
        uint64_t nlen = strlen(init_names[i]);
        if (nlen >= MAX_FILENAME) nlen = MAX_FILENAME - 1;
        memcpy(ram_files[i].name, init_names[i], nlen);
        ram_files[i].name[nlen] = '\0';
        ram_files[i].data = (char *)init_data[i];
        ram_files[i].size = (i < 4) ? strlen(init_data[i]) : init_sizes[i];
        ram_files[i].capacity = ram_files[i].size;
        ram_files[i].in_use = 1;
        ram_files[i].is_readonly = 1; /* Системные файлы защищены от удаления */
    }

    printk("[OK] Dynamic Virtual File System Initialized (32 file slots)\n");
}

const char *fs_get_file_data(const char *name, uint64_t *out_size)
{
    for (int i = 0; i < MAX_FILES; i++) {
        if (ram_files[i].in_use && strcmp(name, ram_files[i].name) == 0) {
            if (out_size) *out_size = ram_files[i].size;
            return ram_files[i].data;
        }
    }
    return NULL;
}

int64_t sys_open(const char *filename, int flags)
{
    int file_idx = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (ram_files[i].in_use && strcmp(filename, ram_files[i].name) == 0) {
            file_idx = i;
            break;
        }
    }

    /* Файл не найден: создаем, если передан O_CREAT */
    if (file_idx == -1) {
        if (!(flags & O_CREAT)) {
            return -1;
        }

        /* Ищем свободный слот в таблице RamFS */
        for (int i = 0; i < MAX_FILES; i++) {
            if (!ram_files[i].in_use) {
                file_idx = i;
                break;
            }
        }
        if (file_idx == -1) return -1; /* Нет свободных слотов */

            /* Выделяем страницу памяти 4 КБ под данные нового файла */
            uint64_t page = get_free_page();
        if (!page) return -1;

        uint64_t nlen = strlen(filename);
        if (nlen >= MAX_FILENAME) nlen = MAX_FILENAME - 1;
        memcpy(ram_files[file_idx].name, filename, nlen);
        ram_files[file_idx].name[nlen] = '\0';

        ram_files[file_idx].data = (char *)page;
        ram_files[file_idx].size = 0;
        ram_files[file_idx].capacity = PAGE_SIZE;
        ram_files[file_idx].in_use = 1;
        ram_files[file_idx].is_readonly = 0;
    }

    /* Очистка содержимого при O_TRUNC */
    if ((flags & O_TRUNC) && !ram_files[file_idx].is_readonly) {
        ram_files[file_idx].size = 0;
    }

    /* Ищем свободный дескриптор в filp[] процесса */
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
    if (fd < 3 || fd >= NR_OPEN || !current->filp[fd].in_use) {
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
    if (fd < 3 || fd >= NR_OPEN || !current->filp[fd].in_use) {
        return -1;
    }

    struct file *f = &current->filp[fd];

    if (f->type == FILE_TYPE_PIPE) {
        return pipe_write(f, buf, count);
    }

    struct ram_file *rf = f->rf;
    if (!rf || rf->is_readonly) {
        return -1; /* Запрещена запись в защищенные файлы */
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

    return bytes_to_write;
}

/* sys_unlink: удаление файла и освобождение его памяти */
int64_t sys_unlink(const char *filename)
{
    for (int i = 0; i < MAX_FILES; i++) {
        if (ram_files[i].in_use && strcmp(filename, ram_files[i].name) == 0) {
            if (ram_files[i].is_readonly) {
                return -1; /* Нельзя удалять системные файлы */
            }

            /* Освобождаем память данных файла */
            if (ram_files[i].data) {
                free_page((uint64_t)ram_files[i].data);
            }

            ram_files[i].in_use = 0;
            ram_files[i].name[0] = '\0';
            ram_files[i].data = NULL;
            ram_files[i].size = 0;
            ram_files[i].capacity = 0;
            return 0;
        }
    }
    return -1;
}

int64_t sys_list(char *buf, uint64_t max_len)
{
    uint64_t offset = 0;

    for (int i = 0; i < MAX_FILES; i++) {
        if (!ram_files[i].in_use) continue;

        const char *name = ram_files[i].name;
        while (*name && offset < max_len - 16) {
            buf[offset++] = *name++;
        }
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
