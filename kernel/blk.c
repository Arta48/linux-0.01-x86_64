/*
 * Блочный слой: таблица устройств, проверка границ и прав записи,
 * разбор разделов GPT/MBR, поиск Minix v1, /proc/disks.
 */
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/minix_fs.h>
#include <linux/string.h>
#include <linux/tty.h>
#include <asm/io.h>

static struct blk_dev devs[BLK_MAX];
static int ndevs;

/* ------------------------------------------------------------------ */
/* IDE как dev 0                                                      */
/* ------------------------------------------------------------------ */
static int ide_rw(void *priv, int write, uint64_t lba, uint32_t count, void *buf)
{
    uint8_t drive = (uint8_t)(uint64_t)priv;
    uint8_t *p = (uint8_t *)buf;
    while (count) {
        uint32_t n = count > 128 ? 128 : count;
        int r = write ? ide_write_sectors(drive, (uint32_t)lba, (uint8_t)n, p)
                      : ide_read_sectors(drive, (uint32_t)lba, (uint8_t)n, p);
        if (r < 0) return -1;
        lba += n; p += n * 512; count -= n;
    }
    return 0;
}

void blk_init(void)
{
    memset(devs, 0, sizeof(devs));
    ide_init();
    const struct hd_drive_info *hd = ide_get_drive(0);

    /* dev 0 занят всегда, даже если IDE-диска нет: номера остальных стабильны */
    struct blk_dev *d = &devs[0];
    memcpy(d->name, "hda", 4);
    d->kind = BLK_IDE;
    d->parent = -1;
    d->lba_bytes = 512;
    d->writable = 1;
    d->rw = ide_rw;
    d->priv = (void *)0;
    if (hd && hd->present) {
        d->present = 1;
        d->sectors = hd->sectors;
        memcpy(d->model, hd->model, sizeof(d->model));
    }
    ndevs = 1;
}

int blk_add_disk(const struct blk_dev *proto)
{
    if (ndevs >= BLK_MAX) return -1;
    int idx = ndevs++;
    devs[idx] = *proto;
    devs[idx].present = 1;
    devs[idx].parent = -1;
    devs[idx].is_part = 0;
    return idx;
}

int blk_count(void) { return ndevs; }

struct blk_dev *blk_get(int dev)
{
    if (dev < 0 || dev >= ndevs) return NULL;
    return &devs[dev];
}

int blk_rw(int dev, int write, uint64_t lba, uint32_t count, void *buf)
{
    struct blk_dev *d = blk_get(dev);
    if (!d || !d->present || !d->rw || count == 0) return -1;
    if (lba >= d->sectors || (uint64_t)count > d->sectors - lba) return -1;
    if (write && !d->writable) return -1;
    return d->rw(d->priv, write, lba + d->start, count, buf);
}

int blk_read(int dev, uint64_t lba, uint32_t count, void *buf)
{
    return blk_rw(dev, 0, lba, count, buf);
}

int blk_write(int dev, uint64_t lba, uint32_t count, const void *buf)
{
    return blk_rw(dev, 1, lba, count, (void *)buf);
}

void blk_flush_all(void)
{
    for (int i = 0; i < ndevs; i++) {
        struct blk_dev *d = &devs[i];
        if (d->present && !d->is_part && (d->writable || d->wr_child) && d->flush) d->flush(d->priv);
    }
}

void blk_set_writable(int dev, int on)
{
    struct blk_dev *d = blk_get(dev);
    if (!d) return;
    d->writable = on ? 1 : 0;
    /* Сброс кэша записи нужен корневому диску раздела */
    if (d->is_part && d->parent >= 0 && on) devs[d->parent].wr_child = 1;
}

/* ------------------------------------------------------------------ */
/* Minix v1                                                           */
/* ------------------------------------------------------------------ */
static int has_minix(int dev)
{
    uint8_t sec[512];
    struct blk_dev *d = blk_get(dev);
    if (!d || d->sectors < 128) return 0;
    if (blk_read(dev, 2, 1, sec) < 0) return 0;      /* суперблок: байт 1024 */
    uint16_t ninodes = (uint16_t)(sec[0] | (sec[1] << 8));
    uint16_t nzones  = (uint16_t)(sec[2] | (sec[3] << 8));
    uint16_t imap    = (uint16_t)(sec[4] | (sec[5] << 8));
    uint16_t zmap    = (uint16_t)(sec[6] | (sec[7] << 8));
    uint16_t first   = (uint16_t)(sec[8] | (sec[9] << 8));
    uint16_t logz    = (uint16_t)(sec[10] | (sec[11] << 8));
    uint16_t magic   = (uint16_t)(sec[16] | (sec[17] << 8));
    if (magic != MINIX_SUPER_MAGIC) return 0;
    if (!ninodes || !nzones || !imap || !zmap || logz != 0) return 0;
    if (first < 2 + imap + zmap || first >= nzones) return 0;
    if ((uint64_t)nzones * 2 > d->sectors) return 0;  /* ФС должна помещаться в раздел */
    return 1;
}

int blk_find_minix(void)
{
    for (int i = 1; i < ndevs; i++) {
        struct blk_dev *d = &devs[i];
        if (!d->present) continue;
        if (!d->is_part && d->has_ptable) continue;   /* GPT/MBR: ищем только в разделах */
        if (has_minix(i)) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Разделы                                                            */
/* ------------------------------------------------------------------ */
static int add_part(int disk, int num, uint64_t first, uint64_t cnt, const char *ptype)
{
    struct blk_dev *p = &devs[disk];
    if (ndevs >= BLK_MAX) return -1;
    if (cnt == 0 || first >= p->sectors || cnt > p->sectors - first) return -1;

    struct blk_dev *d = &devs[ndevs];
    memset(d, 0, sizeof(*d));
    int n = 0;
    for (const char *s = p->name; *s && n < 12; s++) d->name[n++] = *s;
    if (n && d->name[n - 1] >= '0' && d->name[n - 1] <= '9') d->name[n++] = 'p';
    char tmp[4]; int t = 0;
    int v = num;
    do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v && t < 3);
    while (t) d->name[n++] = tmp[--t];
    d->name[n] = '\0';

    d->present = 1;
    d->kind = p->kind;
    d->is_part = 1;
    d->parent = (int16_t)disk;
    d->lba_bytes = p->lba_bytes;
    d->sectors = cnt;
    d->start = first;
    d->writable = 0;
    d->rw = p->rw;
    d->flush = p->flush;
    d->priv = p->priv;
    memcpy(d->model, p->model, sizeof(d->model));
    int i = 0;
    for (; ptype[i] && i < 19; i++) d->ptype[i] = ptype[i];
    d->ptype[i] = '\0';
    ndevs++;
    return ndevs - 1;
}

static int guid_is(const uint8_t *g, const uint8_t *ref) { return memcmp(g, ref, 16) == 0; }

static const char *gpt_type_name(const uint8_t *g)
{
    static const uint8_t efi[16]   = {0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B};
    static const uint8_t msdata[16]= {0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7};
    static const uint8_t linux_[16]= {0xAF,0x3D,0xC6,0x0F,0x83,0x84,0x72,0x47,0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4};
    static const uint8_t swap[16]  = {0x6D,0xFD,0x57,0x06,0xAB,0xA4,0xC4,0x43,0x84,0xE5,0x09,0x33,0xC8,0x4B,0x4F,0x4F};
    static const uint8_t winre[16] = {0xA4,0xBB,0x94,0xDE,0xD1,0x06,0x40,0x4D,0xA1,0x6A,0xBF,0xD5,0x01,0x79,0xD6,0xAC};
    if (guid_is(g, efi)) return "EFI System";
    if (guid_is(g, msdata)) return "Windows data";
    if (guid_is(g, linux_)) return "Linux";
    if (guid_is(g, swap)) return "Linux swap";
    if (guid_is(g, winre)) return "Windows recovery";
    return "other";
}

static const char *mbr_type_name(uint8_t t)
{
    switch (t) {
        case 0x01: case 0x04: case 0x06: case 0x0B: case 0x0C: case 0x0E: return "FAT";
        case 0x07: return "NTFS/exFAT";
        case 0x81: case 0x80: return "Minix";
        case 0x82: return "Linux swap";
        case 0x83: return "Linux";
        case 0xEF: return "EFI System";
        default:   return "other";
    }
}

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t le64(const uint8_t *p) { return le32(p) | ((uint64_t)le32(p + 4) << 32); }

static void mark_minix(int part)
{
    if (part >= 0 && has_minix(part)) memcpy(devs[part].ptype, "Minix v1", 9);
}

void blk_scan_partitions(int disk)
{
    struct blk_dev *dd = blk_get(disk);
    if (!dd || !dd->present || dd->is_part) return;

    uint8_t sec[512];
    if (blk_read(disk, 0, 1, sec) < 0) return;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return;

    int gpt = 0;
    for (int i = 0; i < 4; i++) if (sec[446 + i * 16 + 4] == 0xEE) gpt = 1;

    if (gpt) {
        uint8_t hdr[512];
        if (blk_read(disk, 1, 1, hdr) < 0) return;
        if (memcmp(hdr, "EFI PART", 8) != 0) return;
        uint64_t ent_lba = le64(hdr + 72);
        uint32_t nent = le32(hdr + 80), esz = le32(hdr + 84);
        if (esz < 128 || esz > 512 || (esz & (esz - 1)) || nent > 256) return;
        dd->has_ptable = 1;
        uint32_t per_sec = 512 / esz;
        int num = 0;
        for (uint32_t i = 0; i < nent; i++) {
            if (i % per_sec == 0) {
                if (blk_read(disk, ent_lba + i / per_sec, 1, sec) < 0) break;
            }
            const uint8_t *e = sec + (i % per_sec) * esz;
            num++;                                    /* номер = позиция в таблице */
            int zero = 1;
            for (int k = 0; k < 16; k++) if (e[k]) { zero = 0; break; }
            if (zero) continue;
            uint64_t first = le64(e + 32), last = le64(e + 40);
            if (last < first) continue;
            uint64_t f512 = first, c512 = last - first + 1;
            if (dd->lba_bytes == 4096) { f512 *= 8; c512 *= 8; }
            int p = add_part(disk, num, f512, c512, gpt_type_name(e));
            mark_minix(p);
        }
    } else {
        dd->has_ptable = 1;
        uint8_t mbr[512];
        memcpy(mbr, sec, 512);
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = mbr + 446 + i * 16;
            uint8_t type = e[4];
            uint32_t first = le32(e + 8), cnt = le32(e + 12);
            if (!type || !cnt || type == 0x05 || type == 0x0F || type == 0x85) continue;
            int p = add_part(disk, i + 1, first, cnt, mbr_type_name(type));
            mark_minix(p);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Инициализация хранилищ                                             */
/* ------------------------------------------------------------------ */
void storage_init(void)
{
    blk_init();
    nvme_init();
    ahci_init();

    int disks = ndevs;
    for (int i = 0; i < disks; i++) blk_scan_partitions(i);

    int nd = 0, np = 0;
    for (int i = 0; i < ndevs; i++) {
        if (!devs[i].present) continue;
        if (devs[i].is_part) np++; else nd++;
    }
    printk("[OK] Block layer: %d disk(s), %d partition(s)\n", nd, np);
}

/* ------------------------------------------------------------------ */
/* /proc/disks                                                        */
/* ------------------------------------------------------------------ */
static void sb_str(char *b, uint64_t *o, uint64_t max, const char *s)
{
    while (*s && *o + 1 < max) b[(*o)++] = *s++;
}
static void sb_pad(char *b, uint64_t *o, uint64_t max, const char *s, int width)
{
    int n = 0;
    while (*s && *o + 1 < max) { b[(*o)++] = *s++; n++; }
    while (n++ < width && *o + 1 < max) b[(*o)++] = ' ';
}
static void sb_dec(char *b, uint64_t *o, uint64_t max, uint64_t v)
{
    char t[24]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n && *o + 1 < max) b[(*o)++] = t[--n];
}

void blk_proc_dump(char *buf, uint64_t max_len)
{
    uint64_t o = 0;
    sb_pad(buf, &o, max_len, "NAME", 12);
    sb_pad(buf, &o, max_len, "TYPE", 6);
    sb_pad(buf, &o, max_len, "SIZE(MB)", 10);
    sb_pad(buf, &o, max_len, "START", 12);
    sb_pad(buf, &o, max_len, "RW", 4);
    sb_pad(buf, &o, max_len, "INFO", 4);
    sb_str(buf, &o, max_len, "\n");
    static const char *kinds[] = { "ata", "nvme", "sata" };
    for (int i = 0; i < ndevs; i++) {
        struct blk_dev *d = &devs[i];
        if (!d->present) continue;
        sb_pad(buf, &o, max_len, d->name, 12);
        sb_pad(buf, &o, max_len, d->is_part ? "part" : kinds[d->kind < 3 ? d->kind : 0], 6);
        char t[24]; uint64_t no = 0;
        sb_dec(t, &no, sizeof(t), d->sectors / 2048); t[no] = '\0';
        sb_pad(buf, &o, max_len, t, 10);
        no = 0; sb_dec(t, &no, sizeof(t), d->start); t[no] = '\0';
        sb_pad(buf, &o, max_len, t, 12);
        sb_pad(buf, &o, max_len, d->writable ? "rw" : "ro", 4);
        sb_str(buf, &o, max_len, d->is_part ? d->ptype : d->model);
        sb_str(buf, &o, max_len, "\n");
    }
    buf[o < max_len ? o : max_len - 1] = '\0';
}
