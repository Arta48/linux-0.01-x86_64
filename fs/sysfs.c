/*
 * /sys: минимальный sysfs для настоящих lspci, lsblk и lscpu.
 * Дерево строится один раз после инициализации устройств: реальные файлы лежат в
 * /sys/devices/..., а /sys/bus/pci/devices, /sys/block, /sys/class/block и
 * /sys/dev/block -- символические ссылки на них, как в Linux.
 */
#include <linux/fs.h>
#include <linux/pci.h>
#include <linux/blkdev.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/nic.h>
#include <linux/tty.h>

#define PATH_MAX_ 80

/* ---- помощники ---- */
static void cat(char *d, const char *s)
{
    while (*d) d++;
    while ((*d++ = *s++)) ;
}

static char *num(char *p, uint64_t v, int base, int minw)
{
    char t[24];
    int n = 0;
    do { t[n++] = "0123456789abcdef"[v % (uint64_t)base]; v /= (uint64_t)base; } while (v);
    while (n < minw) t[n++] = '0';
    while (n) *p++ = t[--n];
    *p = '\0';
    return p;
}

static void mkpath(const char *path)
{
    char t[PATH_MAX_];
    for (int i = 1; path[i]; i++) {
        if (path[i] == '/') {
            memcpy(t, path, (uint64_t)i);
            t[i] = '\0';
            ramfs_create_dir(t, 0555);
        }
    }
    ramfs_create_dir(path, 0555);
}

static void wfile(const char *dir, const char *name, const char *content)
{
    char p[PATH_MAX_];
    strcpy(p, dir);
    cat(p, "/");
    cat(p, name);
    ramfs_create_file(p, content, strlen(content), 0444, 0, 0, 0);
}

static void wnum(const char *dir, const char *name, uint64_t v, int base, int minw, const char *prefix)
{
    char b[40];
    strcpy(b, prefix);
    char *p = b + strlen(b);
    p = num(p, v, base, minw);
    *p++ = '\n';
    *p = '\0';
    wfile(dir, name, b);
}

static void link(const char *dir, const char *name, const char *target)
{
    char p[PATH_MAX_];
    strcpy(p, dir);
    cat(p, "/");
    cat(p, name);
    ramfs_create_symlink(p, target);
}

/* ---- PCI ---- */
static void pci_sysfs(void)
{
    mkpath("/sys/bus/pci/devices");
    mkpath("/sys/bus/pci/drivers");
    int total = pci_get_device_count();
    for (int i = 0; i < total; i++) {
        const struct pci_device *d = pci_get_device(i);
        char name[16], dir[PATH_MAX_], hostdir[PATH_MAX_];
        char *p = name;
        strcpy(p, "0000:");
        p += 5;
        p = num(p, d->bus, 16, 2); *p++ = ':';
        p = num(p, d->slot, 16, 2); *p++ = '.';
        p = num(p, d->func, 16, 1);

        strcpy(hostdir, "/sys/devices/pci0000:");
        p = hostdir + strlen(hostdir);
        num(p, d->bus, 16, 2);
        mkpath(hostdir);
        strcpy(dir, hostdir);
        cat(dir, "/");
        cat(dir, name);
        mkpath(dir);

        uint32_t cfg[64];
        for (int k = 0; k < 64; k++) cfg[k] = pci_read_dword(d->bus, d->slot, d->func, (uint8_t)(k * 4));
        char cp[PATH_MAX_];
        strcpy(cp, dir);
        cat(cp, "/config");
        ramfs_create_file(cp, (const char *)cfg, sizeof(cfg), 0444, 0, 0, 0);

        wnum(dir, "vendor", d->vendor_id, 16, 4, "0x");
        wnum(dir, "device", d->device_id, 16, 4, "0x");
        wnum(dir, "class", ((uint32_t)d->class_code << 16) | ((uint32_t)d->subclass << 8) | d->prog_if, 16, 6, "0x");
        wnum(dir, "revision", cfg[2] & 0xFF, 16, 2, "0x");
        wnum(dir, "subsystem_vendor", cfg[11] & 0xFFFF, 16, 4, "0x");
        wnum(dir, "subsystem_device", cfg[11] >> 16, 16, 4, "0x");
        wnum(dir, "irq", d->irq, 10, 1, "");

        /* resource: BAR0-5 (начало, конец, флаги), как в Linux */
        char res[7 * 60];
        res[0] = '\0';
        int htype = (cfg[3] >> 16) & 0x7F;
        for (int b = 0; b < 6; b++) {
            uint64_t start = 0, size = 0, flags = 0;
            if (htype == 0) {
                uint32_t v = cfg[4 + b];
                if (v & 1) { start = v & ~3U; flags = 0x101; }
                else if (v) {
                    start = v & ~0xFU; flags = 0x200;
                    if (v & 8) flags |= 0x1000;
                    if (((v >> 1) & 3) == 2 && b < 5) { start |= (uint64_t)cfg[5 + b] << 32; flags |= 0x4; }
                }
                if (start) {
                    unsigned long fl = irq_save();
                    uint16_t cmd = pci_read_word(d->bus, d->slot, d->func, 4);
                    pci_write_word(d->bus, d->slot, d->func, 4, (uint16_t)(cmd & ~3U));
                    uint8_t off = (uint8_t)(0x10 + 4 * b);
                    pci_write_dword(d->bus, d->slot, d->func, off, 0xFFFFFFFFU);
                    uint32_t m = pci_read_dword(d->bus, d->slot, d->func, off);
                    pci_write_dword(d->bus, d->slot, d->func, off, v);
                    pci_write_word(d->bus, d->slot, d->func, 4, cmd);
                    irq_restore(fl);
                    m &= (v & 1) ? ~3U : ~0xFU;
                    if (m) size = (uint64_t)(~m + 1);
                    if (size && !(v & 1) && ((v >> 1) & 3) != 2) size &= 0xFFFFFFFFULL;
                    if (size == 0 || size > (1ULL << 32)) size = 0x1000;
                }
            }
            char *q = res + strlen(res);
            strcpy(q, "0x"); q += 2;
            q = num(q, start, 16, 16); strcpy(q, " 0x"); q += 3;
            q = num(q, start ? start + size - 1 : 0, 16, 16); strcpy(q, " 0x"); q += 3;
            q = num(q, flags, 16, 16);
            *q++ = '\n'; *q = '\0';
            if ((flags & 4) && b < 5) {                      /* 64-битный BAR занимает два регистра */
                b++;
                q = res + strlen(res);
                strcpy(q, "0x0000000000000000 0x0000000000000000 0x0000000000000000\n");
            }
        }
        wfile(dir, "resource", res);

        char tgt[PATH_MAX_];
        strcpy(tgt, "../../../devices/pci0000:");
        num(tgt + strlen(tgt), d->bus, 16, 2);
        cat(tgt, "/");
        cat(tgt, name);
        link("/sys/bus/pci/devices", name, tgt);
    }
}

/* ---- процессоры ---- */
/* Маска процессоров [lo..hi] в формате ядра Linux: 32-битные слова через запятую, старшее первым */
static void cpumask(char *out, int lo, int hi, int total)
{
    static const char hx[] = "0123456789abcdef";
    int words = (total + 31) / 32;
    char *p = out;
    for (int w = words - 1; w >= 0; w--) {
        uint32_t v = 0;
        for (int b = 0; b < 32; b++) {
            int cpu = w * 32 + b;
            if (cpu >= lo && cpu <= hi) v |= 1u << b;
        }
        int started = (w != words - 1);               /* как в Linux: старшее слово без ведущих нулей */
        for (int d = 7; d >= 0; d--) {
            int dig = (v >> (d * 4)) & 0xF;
            if (dig || started || d == 0) { *p++ = hx[dig]; started = 1; }
        }
        if (w) *p++ = ',';
    }
    *p++ = '\n'; *p = '\0';
}

static void cpu_sysfs(void)
{
    const char *base = "/sys/devices/system/cpu";
    mkpath(base);
    int n = smp_num_cpus > 0 ? smp_num_cpus : 1;
    char range[16];
    char *p = range;
    if (n == 1) { *p++ = '0'; *p = '\0'; }
    else { *p++ = '0'; *p++ = '-'; num(p, (uint64_t)(n - 1), 10, 1); }
    cat(range, "\n");
    wfile(base, "online", range);
    wfile(base, "possible", range);
    wfile(base, "present", range);
    wfile(base, "offline", "\n");
    char km[16];
    num(km, (uint64_t)(n - 1), 10, 1);
    cat(km, "\n");
    wfile(base, "kernel_max", km);
    for (int c = 0; c < n; c++) {
        char dir[PATH_MAX_], top[PATH_MAX_];
        strcpy(dir, base);
        cat(dir, "/cpu");
        num(dir + strlen(dir), (uint64_t)c, 10, 1);
        strcpy(top, dir);
        cat(top, "/topology");
        mkpath(top);
        if (c > 0) wfile(dir, "online", "1\n");
        wnum(top, "core_id", (uint64_t)c, 10, 1, "");
        wnum(top, "physical_package_id", 0, 10, 1, "");
        wnum(top, "die_id", 0, 10, 1, "");
        wnum(top, "cluster_id", 0, 10, 1, "");
        char self[16];
        num(self, (uint64_t)c, 10, 1);
        cat(self, "\n");
        {
            static char m1[MAX_CPUS / 4 + 16], mall[MAX_CPUS / 4 + 16];
            cpumask(m1, c, c, n);
            cpumask(mall, 0, n - 1, n);
            wfile(top, "thread_siblings", m1);
            wfile(top, "core_cpus", m1);
            wfile(top, "core_siblings", mall);
            wfile(top, "package_cpus", mall);
        }
        wfile(top, "thread_siblings_list", self);
        wfile(top, "core_cpus_list", self);
        wfile(top, "core_siblings_list", range);
        wfile(top, "package_cpus_list", range);
    }
}

/* ---- блочные устройства ---- */
static void blk_dir_files(const char *dir, const struct blk_dev *b, int major, int minor, int part)
{
    char mm[24];
    char *p = num(mm, (uint64_t)major, 10, 1);
    *p++ = ':';
    p = num(p, (uint64_t)minor, 10, 1);
    *p++ = '\n'; *p = '\0';
    wfile(dir, "dev", mm);
    wnum(dir, "size", b->sectors, 10, 1, "");
    wfile(dir, "ro", b->writable ? "0\n" : "1\n");
    char pd[PATH_MAX_];
    if (part) {
        wnum(dir, "start", b->start, 10, 1, "");
        wnum(dir, "partition", (uint64_t)part, 10, 1, "");
    } else {
        wfile(dir, "removable", "0\n");
        wfile(dir, "hidden", "0\n");
        strcpy(pd, dir); cat(pd, "/holders"); mkpath(pd);
        strcpy(pd, dir); cat(pd, "/slaves"); mkpath(pd);
        strcpy(pd, dir); cat(pd, "/queue"); mkpath(pd);
        wnum(pd, "logical_block_size", b->lba_bytes ? b->lba_bytes : 512, 10, 1, "");
        wnum(pd, "physical_block_size", b->lba_bytes ? b->lba_bytes : 512, 10, 1, "");
        wnum(pd, "hw_sector_size", b->lba_bytes ? b->lba_bytes : 512, 10, 1, "");
        wfile(pd, "rotational", b->kind == BLK_NVME ? "0\n" : "1\n");
        strcpy(pd, dir); cat(pd, "/device"); mkpath(pd);
        char model[48];
        strcpy(model, b->model);
        cat(model, "\n");
        wfile(pd, "model", model);
        wfile(pd, "type", "0\n");
    }
}

static void blk_sysfs(void)
{
    mkpath("/sys/block");
    mkpath("/sys/class/block");
    mkpath("/sys/dev/block");
    mkpath("/sys/devices/virtual/block");
    int seq[3] = { 0, 0, 0 };
    int parent_minor[BLK_MAX], parent_major[BLK_MAX], nparts[BLK_MAX];
    for (int i = 0; i < blk_count() && i < BLK_MAX; i++) {
        const struct blk_dev *b = blk_get(i);
        parent_minor[i] = parent_major[i] = nparts[i] = 0;
        if (!b || !b->present) continue;
        int kind = b->kind < 3 ? b->kind : 0;
        char dir[PATH_MAX_], rel[PATH_MAX_], mm[24], tgt[PATH_MAX_];
        if (!b->is_part) {
            int major = kind == BLK_IDE ? 3 : kind == BLK_AHCI ? 8 : 259;
            int minor = (kind == BLK_IDE ? 64 : 16) * seq[kind]++;
            parent_major[i] = major; parent_minor[i] = minor;
            strcpy(dir, "/sys/devices/virtual/block/");
            cat(dir, b->name);
            mkpath(dir);
            blk_dir_files(dir, b, major, minor, 0);
            strcpy(tgt, "../devices/virtual/block/");
            cat(tgt, b->name);
            link("/sys/block", b->name, tgt);
            strcpy(tgt, "../../devices/virtual/block/");
            cat(tgt, b->name);
            link("/sys/class/block", b->name, tgt);
            char *p = num(mm, (uint64_t)major, 10, 1);
            *p++ = ':';
            num(p, (uint64_t)minor, 10, 1);
            link("/sys/dev/block", mm, tgt);
        } else {
            int pa = b->parent;
            if (pa < 0 || pa >= BLK_MAX) continue;
            const struct blk_dev *pb = blk_get(pa);
            if (!pb || !pb->present) continue;
            int partno = ++nparts[pa];
            int major = parent_major[pa];
            int minor = parent_minor[pa] + partno;
            strcpy(dir, "/sys/devices/virtual/block/");
            cat(dir, pb->name);
            cat(dir, "/");
            cat(dir, b->name);
            mkpath(dir);
            blk_dir_files(dir, b, major, minor, partno);
            strcpy(rel, pb->name);
            cat(rel, "/");
            cat(rel, b->name);
            strcpy(tgt, "../../devices/virtual/block/");
            cat(tgt, rel);
            link("/sys/class/block", b->name, tgt);
            char *p = num(mm, (uint64_t)major, 10, 1);
            *p++ = ':';
            num(p, (uint64_t)minor, 10, 1);
            link("/sys/dev/block", mm, tgt);
        }
    }
}

void sysfs_init(void)
{
    mkpath("/sys");
    mkpath("/sys/class");
    mkpath("/sys/devices");
    pci_sysfs();
    cpu_sysfs();
    blk_sysfs();
    printk("[OK] sysfs: /sys/bus/pci, /sys/block, /sys/devices/system/cpu\n");
}
