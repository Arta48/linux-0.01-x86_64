#include <linux/fs.h>
#include <linux/minix_fs.h>
#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/time.h>
#include <linux/multiboot.h>
#include <linux/hdreg.h>
#include <linux/tcp.h>
#include <linux/pci.h>

struct proc_ram_file {
    struct ram_file base;
    int is_proc;
    void (*generator)(char *buf, uint64_t max_len);
};

static struct proc_ram_file ram_files[MAX_FILES];

static void get_cpu_info(char *vendor, char *brand)
{
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    memcpy(vendor, &ebx, 4);
    memcpy(vendor + 4, &edx, 4);
    memcpy(vendor + 8, &ecx, 4);
    vendor[12] = '\0';

    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000000));
    if (eax >= 0x80000004) {
        uint32_t *b = (uint32_t *)brand;
        for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            __asm__ volatile ("cpuid" : "=a"(b[0]), "=b"(b[1]), "=c"(b[2]), "=d"(b[3]) : "a"(leaf));
            b += 4;
        }
        brand[48] = '\0';
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

extern volatile int smp_num_cpus;

static void generate_cpuinfo(char *buf, uint64_t max_len)
{
    char vendor[16];
    char brand[64];
    get_cpu_info(vendor, brand);

    uint64_t o = 0;
    int cpus = (smp_num_cpus > 0) ? smp_num_cpus : 1;

    for (int c = 0; c < cpus && o < max_len - 256; c++) {
        const char *p1 = "processor\t: ";
        while (*p1 && o < max_len - 256) buf[o++] = *p1++;
        buf[o++] = '0' + c;
        const char *p2 = "\nvendor_id\t: ";
        while (*p2 && o < max_len - 256) buf[o++] = *p2++;
        const char *v = vendor;
        while (*v && o < max_len - 256) buf[o++] = *v++;
        const char *m = "\nmodel name\t: ";
        while (*m && o < max_len - 256) buf[o++] = *m++;
        const char *b = brand;
        while (*b && o < max_len - 256) buf[o++] = *b++;
        const char *tail = "\ncpu MHz\t\t: 3600.00\nflags\t\t: fpu sse sse2 syscall lm smp apic\n\n";
        while (*tail && o < max_len - 1) buf[o++] = *tail++;
    }
    buf[o] = '\0';
}

static void generate_meminfo(char *buf, uint64_t max_len)
{
    uint32_t free_p = get_free_pages_count();
    uint64_t total_kb = HIGH_MEMORY / 1024;
    uint64_t free_kb = ((uint64_t)free_p * PAGE_SIZE) / 1024;
    uint64_t used_kb = total_kb - free_kb;

    uint64_t o = 0;
    const char *s1 = "MemTotal:\t";
    while (*s1) buf[o++] = *s1++;
    char nb[16]; int ni = 0; uint64_t val = total_kb;
    while (val > 0) { nb[ni++] = '0' + (val % 10); val /= 10; }
    while (--ni >= 0) buf[o++] = nb[ni];
    const char *kb = " kB\nMemFree:\t";
    while (*kb) buf[o++] = *kb++;

    ni = 0; val = free_kb;
    if (val == 0) nb[ni++] = '0';
    while (val > 0) { nb[ni++] = '0' + (val % 10); val /= 10; }
    while (--ni >= 0) buf[o++] = nb[ni];
    const char *s2 = " kB\nMemUsed:\t";
    while (*s2) buf[o++] = *s2++;

    ni = 0; val = used_kb;
    if (val == 0) nb[ni++] = '0';
    while (val > 0) { nb[ni++] = '0' + (val % 10); val /= 10; }
    while (--ni >= 0) buf[o++] = nb[ni];
    const char *s3 = " kB\n";
    while (*s3 && o < max_len - 1) buf[o++] = *s3++;

    buf[o] = '\0';
}

static void append_str(char *buf, uint64_t *o, uint64_t max, const char *s)
{
    while (*s && *o < max - 1) {
        buf[(*o)++] = *s++;
    }
}

static void append_hex(char *buf, uint64_t *o, uint64_t max, uint64_t val, int digits)
{
    const char hex_chars[] = "0123456789abcdef";
    for (int i = (digits - 1) * 4; i >= 0; i -= 4) {
        if (*o < max - 1) {
            buf[(*o)++] = hex_chars[(val >> i) & 0xF];
        }
    }
}

static void append_dec(char *buf, uint64_t *o, uint64_t max, int val, int width)
{
    char tmp[16];
    int ti = 0;
    if (val == 0) tmp[ti++] = '0';
    else {
        int v = val;
        while (v > 0) {
            tmp[ti++] = '0' + (v % 10);
            v /= 10;
        }
    }
    while (ti < width && *o < max - 1) {
        buf[(*o)++] = ' ';
        width--;
    }
    while (--ti >= 0 && *o < max - 1) {
        buf[(*o)++] = tmp[ti];
    }
}

static void generate_pciinfo(char *buf, uint64_t max_len)
{
    uint64_t o = 0;
    int total = pci_get_device_count();
    append_str(buf, &o, max_len,
        "BUS  SLOT FUNC VENDOR DEVICE  CLASS   MMIO_BAR0   IRQ  DESCRIPTION\n"
        "--------------------------------------------------------------------------------\n");

    for (int i = 0; i < total && o < max_len - 128; i++) {
        const struct pci_device *d = pci_get_device(i);
        const char *desc = pci_class_to_string(d->class_code, d->subclass, d->prog_if);
        append_hex(buf, &o, max_len, d->bus, 2);
        append_str(buf, &o, max_len, "   ");
        append_hex(buf, &o, max_len, d->slot, 2);
        append_str(buf, &o, max_len, "   ");
        append_hex(buf, &o, max_len, d->func, 2);
        append_str(buf, &o, max_len, "   ");
        append_hex(buf, &o, max_len, d->vendor_id, 4);
        append_str(buf, &o, max_len, "   ");
        append_hex(buf, &o, max_len, d->device_id, 4);
        append_str(buf, &o, max_len, "    ");
        append_hex(buf, &o, max_len, d->class_code, 2);
        append_str(buf, &o, max_len, ":");
        append_hex(buf, &o, max_len, d->subclass, 2);
        append_str(buf, &o, max_len, "   0x");
        append_hex(buf, &o, max_len, d->bar0, 8);
        append_str(buf, &o, max_len, "  ");
        append_dec(buf, &o, max_len, d->irq, 2);
        append_str(buf, &o, max_len, "   ");
        append_str(buf, &o, max_len, desc);
        append_str(buf, &o, max_len, "\n");
    }
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

static inline int is_minix_path(const char *path)
{
    return (strncmp(path, "/mnt", 4) == 0 && (path[4] == '/' || path[4] == '\0'));
}

int ramfs_create_dir(const char *path, uint16_t mode)
{
    char full[MAX_FILENAME];
    resolve_path(path, full);

    if (strcmp(full, "/mnt") == 0 || strcmp(full, "/mnt/") == 0) {
        return 0;
    }

    if (is_minix_path(full)) {
        return minix_sys_mkdir(full, mode);
    }

    for (int i = 0; i < MAX_FILES; i++) {
        if (ram_files[i].base.in_use && strcmp(full, ram_files[i].base.name) == 0) {
            return 0;
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
            ram_files[i].base.capacity = 0;
            ram_files[i].base.mtime = get_current_time();
            ram_files[i].base.uid = 0;
            ram_files[i].base.gid = 0;
            ram_files[i].base.mode = mode ? mode : 0755;
            ram_files[i].base.data = NULL;
            ram_files[i].base.is_dev_blk = 0;
            ram_files[i].is_proc = 0;
            ram_files[i].generator = NULL;
            return 0;
        }
    }
    return -1;
}

int ramfs_create_file(const char *path, const char *data, uint64_t size, uint16_t mode, uint16_t uid, uint16_t gid, uint64_t mtime)
{
    char full[MAX_FILENAME];
    resolve_path(path, full);

    int idx = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (ram_files[i].base.in_use && strcmp(full, ram_files[i].base.name) == 0) {
            idx = i;
            break;
        }
    }

    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0) pages = 1;

    if (idx != -1) {
        if (ram_files[idx].base.capacity < size || !ram_files[idx].base.data) {
            uint64_t new_page = get_free_pages(pages);
            if (!new_page) return -1;
            ram_files[idx].base.data = (char *)new_page;
            ram_files[idx].base.capacity = pages * PAGE_SIZE;
        }
    } else {
        for (int i = 0; i < MAX_FILES; i++) {
            if (!ram_files[i].base.in_use) {
                idx = i;
                break;
            }
        }
        if (idx == -1) return -1;

        uint64_t page = get_free_pages(pages);
        if (!page) return -1;

        uint64_t len = strlen(full);
        memcpy(ram_files[idx].base.name, full, len + 1);
        ram_files[idx].base.data = (char *)page;
        ram_files[idx].base.capacity = pages * PAGE_SIZE;
        ram_files[idx].base.is_dev_blk = 0;
        ram_files[idx].is_proc = 0;
        ram_files[idx].generator = NULL;
    }

    if (data && size > 0) {
        memcpy(ram_files[idx].base.data, data, size);
    }
    ram_files[idx].base.data[size] = '\0';
    ram_files[idx].base.size = size;
    ram_files[idx].base.mtime = mtime ? mtime : get_current_time();
    ram_files[idx].base.uid = uid;
    ram_files[idx].base.gid = gid;
    ram_files[idx].base.mode = mode ? mode : 0644;
    ram_files[idx].base.in_use = 1;
    ram_files[idx].base.is_readonly = 0;
    ram_files[idx].base.is_dir = 0;

    return 0;
}

static uint64_t parse_octal(const char *s, int len)
{
    uint64_t val = 0;
    while (len > 0 && (*s == ' ' || *s == '\0')) {
        s++;
        len--;
    }
    while (len > 0 && *s >= '0' && *s <= '7') {
        val = (val << 3) | (*s - '0');
        s++;
        len--;
    }
    return val;
}

void tarfs_mount(uint64_t archive_start, uint64_t archive_end)
{
    if (archive_start == 0 || archive_end <= archive_start) return;

    printk("[TARFS] Unpacking Initrd archive at %p - %p (%d KB)...\n",
           archive_start, archive_end, (int)((archive_end - archive_start) / 1024));

    uint64_t ptr = archive_start;
    int files_extracted = 0;

    while (ptr + 512 <= archive_end) {
        struct tar_header *hdr = (struct tar_header *)ptr;

        if (hdr->name[0] == '\0') {
            break;
        }

        uint64_t file_size = parse_octal(hdr->size, sizeof(hdr->size));
        uint16_t mode = (uint16_t)parse_octal(hdr->mode, sizeof(hdr->mode));
        uint16_t uid = (uint16_t)parse_octal(hdr->uid, sizeof(hdr->uid));
        uint16_t gid = (uint16_t)parse_octal(hdr->gid, sizeof(hdr->gid));
        uint64_t mtime = parse_octal(hdr->mtime, sizeof(hdr->mtime));
        if (mtime == 0) mtime = startup_time;

        char full_path[MAX_FILENAME];
        const char *name_src = hdr->name;
        if (name_src[0] == '.' && name_src[1] == '/') name_src += 2;

        int fi = 0;
        if (name_src[0] != '/') full_path[fi++] = '/';
        while (*name_src && fi < MAX_FILENAME - 1) {
            full_path[fi++] = *name_src++;
        }
        full_path[fi] = '\0';

        while (fi > 1 && full_path[fi - 1] == '/') {
            full_path[--fi] = '\0';
        }

        if (hdr->typeflag == '5' || (hdr->typeflag == '\0' && hdr->name[strlen(hdr->name) - 1] == '/')) {
            ramfs_create_dir(full_path, mode ? mode : 0755);
        } else {
            const char *file_data = (const char *)(ptr + 512);
            ramfs_create_file(full_path, file_data, file_size, mode ? mode : 0644, uid, gid, mtime);
            files_extracted++;
        }

        uint64_t aligned_size = (file_size + 511) & ~511ULL;
        ptr += 512 + aligned_size;
    }

    printk("[OK] TarFS Mounted: Extracted %d files into root filesystem.\n", files_extracted);
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
        ram_files[i].base.uid = 0;
        ram_files[i].base.gid = 0;
        ram_files[i].base.mode = 0644;
        ram_files[i].base.is_readonly = 0;
        ram_files[i].base.is_dir = 0;
        ram_files[i].base.is_dev_blk = 0;
        ram_files[i].is_proc = 0;
        ram_files[i].generator = NULL;
    }

    /* Точки монтирования и стандартные каталоги: добавлен /mnt */
    const char *dirs[] = { "/", "/bin", "/etc", "/home", "/proc", "/dev", "/mnt" };
    for (int i = 0; i < 7; i++) {
        uint64_t len = strlen(dirs[i]);
        memcpy(ram_files[i].base.name, dirs[i], len + 1);
        ram_files[i].base.is_dir = 1;
        ram_files[i].base.in_use = 1;
        ram_files[i].base.is_readonly = (i == 6) ? 0 : 1; /* /mnt доступен для монтирования */
        ram_files[i].base.mtime = startup_time;
        ram_files[i].base.mode = 0755;
    }

    const char *init_names[] = {
        "/README.txt", "/version", "/author", "/etc/motd", "/etc/passwd", "/etc/init.sh"
    };
    const char *init_data[] = {
        "====================================================\n"
        "  Linux 0.01 (x86_64 Edition)\n"
        "  Re-engineered for 64-bit Long Mode from 1991 code.\n"
        "====================================================\n"
        "Features:\n"
        "  - 4-level paging (PML4, PDPT, PD, PT)\n"
        "  - Per-process isolated address spaces (CR3 isolation)\n"
        "  - Preemptive multitasking & decay scheduler\n"
        "  - Ring 3 user space isolation via TSS.rsp0\n"
        "  - Fast hardware MSR syscall / sysret\n"
        "  - Minix v1 Disk Filesystem mounted on /mnt\n"
        "  - Buffer Cache (1024B blocks, hash table, LRU)\n"
        "  - Persistent files & directories across reboots\n"
        "  - Shell scripts execution & full-screen nano\n",

        "Linux version 0.01-x86_64 (root@arch) (gcc 14) #1 PREEMPT 2026\n",
        "Original: Linus Torvalds (Helsinki, 1991)\nx86_64 Port: Educational Project (2026)\n",
        "Welcome to 64-bit Unix! Have a lot of fun hacking kernels.\n",
        "root:root:0:0:Superuser:/root\nuser:user:1000:1000:Regular User:/home\nguest:guest:1001:1001:Guest Account:/home\n",
        "# /etc/init.sh - System startup script\n"
        "echo [INIT] Running startup script /etc/init.sh...\n"
        "uname -a\n"
        "export SHELL=/bin/sh\n"
        "export HOSTNAME=linux64\n"
        "echo [INIT] Minix v1 persistence active on /mnt\n"
        "echo [INIT] Initialization complete.\n"
    };

    for (int i = 0; i < 6; i++) {
        int idx = 7 + i;
        uint64_t nlen = strlen(init_names[i]);
        memcpy(ram_files[idx].base.name, init_names[i], nlen + 1);

        uint64_t init_sz = strlen(init_data[i]);
        uint32_t p_count = (init_sz + PAGE_SIZE - 1) / PAGE_SIZE;
        if (p_count == 0) p_count = 1;

        uint64_t p_addr = get_free_pages(p_count);
        if (p_addr) {
            memcpy((void *)p_addr, init_data[i], init_sz);
            ram_files[idx].base.data = (char *)p_addr;
            ram_files[idx].base.capacity = (uint64_t)p_count * PAGE_SIZE;
        } else {
            ram_files[idx].base.data = (char *)init_data[i];
            ram_files[idx].base.capacity = init_sz;
        }

        ram_files[idx].base.size = init_sz;
        ram_files[idx].base.in_use = 1;
        ram_files[idx].base.is_readonly = 0;
        ram_files[idx].base.is_dir = 0;
        ram_files[idx].base.is_dev_blk = 0;
        ram_files[idx].base.mtime = startup_time;
        ram_files[idx].base.mode = 0644;
    }

    int c_idx = 13;
    memcpy(ram_files[c_idx].base.name, "/proc/cpuinfo", 14);
    ram_files[c_idx].base.data = (char *)get_free_page();
    ram_files[c_idx].base.capacity = PAGE_SIZE;
    ram_files[c_idx].base.in_use = 1;
    ram_files[c_idx].base.is_readonly = 1;
    ram_files[c_idx].is_proc = 1;
    ram_files[c_idx].generator = generate_cpuinfo;

    int m_idx = 14;
    memcpy(ram_files[m_idx].base.name, "/proc/meminfo", 14);
    ram_files[m_idx].base.data = (char *)get_free_page();
    ram_files[m_idx].base.capacity = PAGE_SIZE;
    ram_files[m_idx].base.in_use = 1;
    ram_files[m_idx].base.is_readonly = 1;
    ram_files[m_idx].is_proc = 1;
    ram_files[m_idx].generator = generate_meminfo;

    /* Псевдофайл /proc/pci для опроса оборудования */
    int p_idx = 15;
    memcpy(ram_files[p_idx].base.name, "/proc/pci", 10);
    ram_files[p_idx].base.data = (char *)get_free_page();
    ram_files[p_idx].base.capacity = PAGE_SIZE;
    ram_files[p_idx].base.in_use = 1;
    ram_files[p_idx].base.is_readonly = 1;
    ram_files[p_idx].is_proc = 1;
    ram_files[p_idx].generator = generate_pciinfo;

    /* Блочное устройство /dev/hda для жесткого диска */
    int hd_idx = 16;
    const struct hd_drive_info *hd = ide_get_drive(0);
    memcpy(ram_files[hd_idx].base.name, "/dev/hda", 9);
    ram_files[hd_idx].base.data = NULL;
    ram_files[hd_idx].base.size = hd ? ((uint64_t)hd->sectors * 512) : 0;
    ram_files[hd_idx].base.capacity = ram_files[hd_idx].base.size;
    ram_files[hd_idx].base.in_use = 1;
    ram_files[hd_idx].base.is_readonly = 0;
    ram_files[hd_idx].base.is_dir = 0;
    ram_files[hd_idx].base.is_dev_blk = 1;
    ram_files[hd_idx].base.dev_id = 0;
    ram_files[hd_idx].base.mode = 0660;

    printk("[OK] Multi-user VFS Initialized (/mnt ready for Minix FS)\n");
}

const char *fs_get_file_data(const char *name, uint64_t *out_size)
{
    char full[MAX_FILENAME];
    resolve_path(name, full);

    if (is_minix_path(full)) {
        return minix_get_file_data(full, out_size);
    }

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

    if (is_minix_path(full)) {
        uint32_t minix_ino = 0;
        uint64_t fsz = 0;
        if (minix_sys_open(full, flags, 0644, &minix_ino, &fsz) < 0) {
            return -1;
        }

        for (int fd = 3; fd < NR_OPEN; fd++) {
            if (!current->filp[fd].in_use) {
                current->filp[fd].type = FILE_TYPE_MINIX;
                current->filp[fd].rf = NULL;
                current->filp[fd].pipe = NULL;
                current->filp[fd].minix_ino = minix_ino;
                current->filp[fd].pos = (flags & O_APPEND) ? fsz : 0;
                current->filp[fd].in_use = 1;
                current->filp[fd].mode = (flags & 3) ? (flags & 3) : 1;
                return fd;
            }
        }
        return -1;
    }

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
        ram_files[file_idx].base.uid = current->euid;
        ram_files[file_idx].base.gid = current->egid;
        ram_files[file_idx].base.mode = 0644;
        ram_files[file_idx].base.in_use = 1;
        ram_files[file_idx].base.is_readonly = 0;
        ram_files[file_idx].base.is_dir = 0;
        ram_files[file_idx].base.is_dev_blk = 0;
        ram_files[file_idx].is_proc = 0;
        ram_files[file_idx].generator = NULL;
    }

    if (ram_files[file_idx].is_proc && ram_files[file_idx].generator) {
        ram_files[file_idx].generator(ram_files[file_idx].base.data, ram_files[file_idx].base.capacity);
        ram_files[file_idx].base.size = strlen(ram_files[file_idx].base.data);
    }

    if ((flags & O_TRUNC) && !ram_files[file_idx].base.is_readonly && !ram_files[file_idx].base.is_dev_blk) {
        ram_files[file_idx].base.size = 0;
        ram_files[file_idx].base.mtime = get_current_time();
    }

    for (int fd = 3; fd < NR_OPEN; fd++) {
        if (!current->filp[fd].in_use) {
            current->filp[fd].type = ram_files[file_idx].base.is_dev_blk ? FILE_TYPE_BLOCK : FILE_TYPE_REGULAR;
            current->filp[fd].rf   = (struct ram_file *)&ram_files[file_idx].base;

            if (flags & O_APPEND) {
                current->filp[fd].pos = ram_files[file_idx].base.size;
            } else {
                current->filp[fd].pos = 0;
            }

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

    if (f->type == FILE_TYPE_SOCKET) {
        tcp_socket_close(f->sock_id);
    }

    f->in_use = 0;
    f->type = 0;
    f->mode = 0;
    f->rf = NULL;
    f->pipe = NULL;
    f->minix_ino = 0;
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

    if (f->type == FILE_TYPE_MINIX) {
        return minix_file_read(f->minix_ino, &f->pos, buf, count);
    }

    if (f->type == FILE_TYPE_SOCKET) {
        return tcp_socket_read(f->sock_id, buf, count);
    }

    struct ram_file *rf = f->rf;
    if (!rf) return -1;

    /* Чтение блочного устройства (/dev/hda) */
    if (rf->is_dev_blk) {
        if (f->pos >= rf->size) return 0;
        uint64_t bytes_to_read = count;
        if (f->pos + bytes_to_read > rf->size) {
            bytes_to_read = rf->size - f->pos;
        }

        uint64_t lba = f->pos / 512;
        uint64_t offset = f->pos % 512;
        uint64_t read_bytes = 0;
        char sec_buf[512];

        while (read_bytes < bytes_to_read) {
            if (ide_read_sectors(rf->dev_id, (uint32_t)lba, 1, sec_buf) < 0) {
                break;
            }
            uint64_t chunk = 512 - offset;
            if (chunk > bytes_to_read - read_bytes) {
                chunk = bytes_to_read - read_bytes;
            }
            memcpy(buf + read_bytes, sec_buf + offset, chunk);
            read_bytes += chunk;
            f->pos += chunk;
            lba++;
            offset = 0;
        }
        return read_bytes;
    }

    if (f->pos >= rf->size) return 0;

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

    if (f->type == FILE_TYPE_MINIX) {
        return minix_file_write(f->minix_ino, &f->pos, buf, count);
    }

    if (f->type == FILE_TYPE_SOCKET) {
        return tcp_socket_write(f->sock_id, buf, count);
    }

    struct ram_file *rf = f->rf;
    if (!rf || rf->is_readonly) {
        return -1;
    }

    if (current->euid != 0 && current->euid != rf->uid) {
        return -1;
    }

    /* Запись в блочное устройство (/dev/hda) */
    if (rf->is_dev_blk) {
        uint64_t lba = f->pos / 512;
        uint64_t offset = f->pos % 512;
        uint64_t written_bytes = 0;
        char sec_buf[512];

        while (written_bytes < count) {
            uint64_t chunk = 512 - offset;
            if (chunk > count - written_bytes) {
                chunk = count - written_bytes;
            }

            if (offset != 0 || chunk < 512) {
                ide_read_sectors(rf->dev_id, (uint32_t)lba, 1, sec_buf);
            }
            memcpy(sec_buf + offset, buf + written_bytes, chunk);

            if (ide_write_sectors(rf->dev_id, (uint32_t)lba, 1, sec_buf) < 0) {
                break;
            }

            written_bytes += chunk;
            f->pos += chunk;
            lba++;
            offset = 0;
        }
        return written_bytes;
    }

    if (f->pos + count > rf->capacity) {
        uint64_t needed_cap = f->pos + count;
        uint32_t needed_pages = (needed_cap + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t new_page = get_free_pages(needed_pages);
        if (new_page) {
            if (rf->data && rf->size > 0) {
                memcpy((void *)new_page, rf->data, rf->size);
            }
            rf->data = (char *)new_page;
            rf->capacity = (uint64_t)needed_pages * PAGE_SIZE;
        } else {
            return -1;
        }
    }

    for (uint64_t i = 0; i < count; i++) {
        rf->data[f->pos + i] = buf[i];
    }

    f->pos += count;
    if (f->pos > rf->size) {
        rf->size = f->pos;
    }
    rf->mtime = get_current_time();

    return count;
}

int64_t sys_unlink(const char *filename)
{
    char full[MAX_FILENAME];
    resolve_path(filename, full);

    if (is_minix_path(full)) {
        return minix_sys_unlink(full);
    }

    for (int i = 0; i < MAX_FILES; i++) {
        if (ram_files[i].base.in_use && strcmp(full, ram_files[i].base.name) == 0) {
            if (ram_files[i].base.is_readonly || ram_files[i].base.is_dir || ram_files[i].base.is_dev_blk) {
                return -1;
            }

            if (current->euid != 0 && current->euid != ram_files[i].base.uid) {
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

int64_t sys_chmod(const char *filename, int mode)
{
    char full[MAX_FILENAME];
    resolve_path(filename, full);

    for (int i = 0; i < MAX_FILES; i++) {
        if (ram_files[i].base.in_use && strcmp(full, ram_files[i].base.name) == 0) {
            if (current->euid != 0 && current->euid != ram_files[i].base.uid) {
                return -1;
            }
            ram_files[i].base.mode = (uint16_t)mode;
            return 0;
        }
    }
    return -1;
}

int64_t sys_stat(const char *filename, struct stat *statbuf)
{
    if (!statbuf) return -1;

    char full[MAX_FILENAME];
    resolve_path(filename, full);

    if (is_minix_path(full)) {
        return minix_sys_stat(full, statbuf);
    }

    for (int i = 0; i < MAX_FILES; i++) {
        if (ram_files[i].base.in_use && strcmp(full, ram_files[i].base.name) == 0) {
            statbuf->st_dev   = 1;
            statbuf->st_ino   = (uint64_t)(i + 1);
            statbuf->st_mode  = (ram_files[i].base.is_dir ? S_IFDIR : (ram_files[i].base.is_dev_blk ? 0060000 : S_IFREG)) | ram_files[i].base.mode;
            statbuf->st_nlink = ram_files[i].base.is_dir ? 2 : 1;
            statbuf->st_uid   = ram_files[i].base.uid;
            statbuf->st_gid   = ram_files[i].base.gid;
            statbuf->st_size  = ram_files[i].base.size;
            statbuf->st_mtime = ram_files[i].base.mtime;
            return 0;
        }
    }

    return -1;
}

int64_t sys_chdir(const char *path)
{
    char full[MAX_FILENAME];
    resolve_path(path, full);

    if (is_minix_path(full)) {
        if (!minix_is_dir(full)) return -1;
        uint64_t len = strlen(full);
        memcpy(current->cwd, full, len + 1);
        return 0;
    }

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
    return ramfs_create_dir(path, 0755);
}

int64_t sys_rmdir(const char *path)
{
    char full[MAX_FILENAME];
    resolve_path(path, full);

    if (is_minix_path(full)) {
        return minix_sys_rmdir(full);
    }

    if (strcmp(full, "/") == 0) return -1;

    int dir_idx = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (ram_files[i].base.in_use && ram_files[i].base.is_dir && strcmp(full, ram_files[i].base.name) == 0) {
            if (ram_files[i].base.is_readonly) return -1;
            if (current->euid != 0 && current->euid != ram_files[i].base.uid) return -1;
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

    if (is_minix_path(target)) {
        return minix_sys_list(target, buf, max_len, is_long);
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
                const char *perms = ram_files[i].base.is_dir ? "drwxr-xr-x  " :
                (ram_files[i].base.is_dev_blk ? "brw-rw----  " :
                (ram_files[i].base.is_readonly ? "-rwxr-xr-x  " : "-rw-r--r--  "));
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
