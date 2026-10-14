/*
 * SATA AHCI: собственный драйвер без прерываний (polling).
 *
 * Для каждого активного порта с SATA-диском (сигнатура 0x00000101) настраиваются
 * Command List / Received FIS / Command Table; команды выполняются синхронно в
 * слоте 0 через DMA и общий буфер (до 64 КБ = 128 секторов). Поддерживаются LBA48
 * и LBA28, логический сектор 512 байт (диски 4Kn пропускаются). Диск виден
 * блочному слою как sda, sdb, ...; запись разрешает только blk_set_writable().
 */
#include <linux/blkdev.h>
#include <linux/pci.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/tty.h>
#include <asm/io.h>

#define MAX_HBA      4
#define MAX_PORTS    12
#define BOUNCE_PAGES 16                       /* 64 КБ */
#define BOUNCE_SECT  (BOUNCE_PAGES * 8)       /* 128 секторов */
#define CMD_TMO_US   20000000U                /* HDD после простоя может долго раскручиваться */

/* Регистры HBA */
#define HBA_CAP    0x00
#define HBA_GHC    0x04
#define HBA_IS     0x08
#define HBA_PI     0x0C
#define HBA_VS     0x10
#define HBA_CAP2   0x24
#define HBA_BOHC   0x28
/* Регистры порта */
#define P_CLB   0x00
#define P_CLBU  0x04
#define P_FB    0x08
#define P_FBU   0x0C
#define P_IS    0x10
#define P_IE    0x14
#define P_CMD   0x18
#define P_TFD   0x20
#define P_SIG   0x24
#define P_SSTS  0x28
#define P_SERR  0x30
#define P_SACT  0x34
#define P_CI    0x38

#define CMD_ST   (1U << 0)
#define CMD_FRE  (1U << 4)
#define CMD_FR   (1U << 14)
#define CMD_CR   (1U << 15)
#define TFD_ERR  (1U << 0)
#define TFD_DRQ  (1U << 3)
#define TFD_BSY  (1U << 7)
#define TFD_DF   (1U << 5)
#define IS_TFES  (1U << 30)

struct ahci_hba {
    volatile uint8_t *regs;
    uint8_t *bounce;
    int idx;
};

struct ahci_port {
    struct ahci_hba *h;
    uint32_t num;
    uint8_t *page;                 /* CLB (1 КБ) + FIS (256) + Command Table */
    int lba48;
    uint64_t sectors;
    char model[41];
};

static struct ahci_hba H[MAX_HBA];
static struct ahci_port PT[MAX_PORTS];
static int nhba, nports, ndisks;

static void udelay(uint32_t us) { while (us--) outb(0, 0x80); }
static void mdelay(uint32_t ms) { while (ms--) udelay(1000); }
static inline void mb(void) { __asm__ volatile ("mfence" : : : "memory"); }

static inline uint64_t irq_save(void)
{
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) : : "memory");
    return f;
}
static inline void irq_restore(uint64_t f)
{
    __asm__ volatile ("pushq %0; popfq" : : "r"(f) : "memory", "cc");
}

static inline uint32_t hr32(struct ahci_hba *h, uint32_t off) { return *(volatile uint32_t *)(h->regs + off); }
static inline void hw32(struct ahci_hba *h, uint32_t off, uint32_t v) { *(volatile uint32_t *)(h->regs + off) = v; }
static inline uint32_t pr32(struct ahci_port *p, uint32_t off) { return hr32(p->h, 0x100 + p->num * 0x80 + off); }
static inline void pw32(struct ahci_port *p, uint32_t off, uint32_t v) { hw32(p->h, 0x100 + p->num * 0x80 + off, v); }

/* ------------------------------------------------------------------ */
/* Порт: остановка / запуск                                           */
/* ------------------------------------------------------------------ */
static int wait_clear(struct ahci_port *p, uint32_t reg, uint32_t mask, uint32_t ms)
{
    while (ms--) {
        if (!(pr32(p, reg) & mask)) return 0;
        mdelay(1);
    }
    return -1;
}

static int port_stop(struct ahci_port *p)
{
    pw32(p, P_CMD, pr32(p, P_CMD) & ~CMD_ST);
    if (wait_clear(p, P_CMD, CMD_CR, 600) < 0) return -1;
    pw32(p, P_CMD, pr32(p, P_CMD) & ~CMD_FRE);
    if (wait_clear(p, P_CMD, CMD_FR, 600) < 0) return -1;
    return 0;
}

static int port_start(struct ahci_port *p)
{
    if (wait_clear(p, P_TFD, TFD_BSY | TFD_DRQ, 3000) < 0) return -1;
    pw32(p, P_CMD, pr32(p, P_CMD) | CMD_FRE);
    pw32(p, P_CMD, pr32(p, P_CMD) | CMD_ST);
    return 0;
}

static int port_recover(struct ahci_port *p)
{
    port_stop(p);
    pw32(p, P_SERR, 0xFFFFFFFFU);
    pw32(p, P_IS, 0xFFFFFFFFU);
    return port_start(p);
}

/* ------------------------------------------------------------------ */
/* Команда ATA через слот 0                                           */
/* ------------------------------------------------------------------ */
static int ata_cmd(struct ahci_port *p, uint8_t cmd, uint64_t lba, uint32_t count, int write, uint32_t bytes)
{
    uint8_t *clb = p->page;
    uint8_t *ct = p->page + 2048;
    uint64_t ctp = (uint64_t)ct;

    volatile uint32_t *hdr = (volatile uint32_t *)clb;
    hdr[0] = 5U | (write ? (1U << 6) : 0U) | ((bytes ? 1U : 0U) << 16);   /* CFL=5 (20 байт), W, PRDTL */
    hdr[1] = 0;
    hdr[2] = (uint32_t)ctp;
    hdr[3] = (uint32_t)(ctp >> 32);

    memset(ct, 0, 0x90);
    ct[0] = 0x27; ct[1] = 0x80; ct[2] = cmd; ct[3] = 0;
    ct[4] = (uint8_t)lba; ct[5] = (uint8_t)(lba >> 8); ct[6] = (uint8_t)(lba >> 16);
    ct[7] = 0x40;                                     /* LBA mode */
    ct[8] = (uint8_t)(lba >> 24); ct[9] = (uint8_t)(lba >> 32); ct[10] = (uint8_t)(lba >> 40);
    ct[12] = (uint8_t)count; ct[13] = (uint8_t)(count >> 8);
    if (!p->lba48 && cmd != 0xEC) ct[7] = (uint8_t)(0x40 | ((lba >> 24) & 0x0F));  /* LBA28: биты 27:24 в Device */
    if (bytes) {
        uint64_t b = (uint64_t)p->h->bounce;
        volatile uint32_t *prd = (volatile uint32_t *)(ct + 0x80);
        prd[0] = (uint32_t)b; prd[1] = (uint32_t)(b >> 32); prd[2] = 0; prd[3] = bytes - 1;
    }

    pw32(p, P_IS, 0xFFFFFFFFU);
    pw32(p, P_SERR, 0xFFFFFFFFU);
    if (wait_clear(p, P_TFD, TFD_BSY | TFD_DRQ, 5000) < 0) { port_recover(p); return -1; }

    mb();
    pw32(p, P_CI, 1);

    uint32_t waited = 0;
    for (;;) {
        if (!(pr32(p, P_CI) & 1)) break;
        if (pr32(p, P_IS) & IS_TFES) { port_recover(p); return -2; }
        if (waited >= CMD_TMO_US) {
            printk("[AHCI] port %d: command 0x%x timeout\n", (long)p->num, (long)cmd);
            port_recover(p);
            return -3;
        }
        udelay(10);
        waited += 10;
    }
    mb();
    if (pr32(p, P_TFD) & (TFD_ERR | TFD_DF)) { port_recover(p); return -4; }
    return 0;
}

static int ahci_rw(void *priv, int write, uint64_t lba, uint32_t count, void *buf)
{
    struct ahci_port *p = (struct ahci_port *)priv;
    uint8_t *b = (uint8_t *)buf;
    uint64_t fl = irq_save();
    int rc = 0;

    while (count) {
        uint32_t n = count > BOUNCE_SECT ? BOUNCE_SECT : count;
        if (!p->lba48 && n > 255) n = 255;
        if (!p->lba48 && lba + n > 0x0FFFFFFFULL) { rc = -1; break; }
        uint8_t cmd = p->lba48 ? (write ? 0x35 : 0x25) : (write ? 0xCA : 0xC8);
        if (write) memcpy(p->h->bounce, b, n * 512);
        rc = ata_cmd(p, cmd, lba, n, write, n * 512);
        if (rc < 0) break;
        if (!write) memcpy(b, p->h->bounce, n * 512);
        lba += n; b += n * 512; count -= n;
    }
    irq_restore(fl);
    return rc < 0 ? -1 : 0;
}

static int ahci_flush(void *priv)
{
    struct ahci_port *p = (struct ahci_port *)priv;
    uint64_t fl = irq_save();
    int rc = ata_cmd(p, p->lba48 ? 0xEA : 0xE7, 0, 0, 0, 0);
    irq_restore(fl);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Инициализация порта и контроллера                                  */
/* ------------------------------------------------------------------ */
static int port_init(struct ahci_hba *h, uint32_t num)
{
    if (nports >= MAX_PORTS) return 0;
    struct ahci_port *p = &PT[nports];
    memset(p, 0, sizeof(*p));
    p->h = h;
    p->num = num;

    uint32_t ssts = pr32(p, P_SSTS);
    if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1) return 0;        /* нет устройства / link не активен */
    uint32_t sig = pr32(p, P_SIG);
    if (sig != 0x00000101U) {
        printk("[AHCI] %d:%d: device with signature %p ignored (not an ATA disk)\n",
               (long)h->idx, (long)num, (uint64_t)sig);
        return 0;
    }

    p->page = (uint8_t *)get_free_page();
    if (!p->page) return 0;

    if (port_stop(p) < 0) { printk("[AHCI] %d:%d: cannot stop port engine\n", (long)h->idx, (long)num); return 0; }
    uint64_t clb = (uint64_t)p->page, fb = clb + 1024;
    pw32(p, P_CLB, (uint32_t)clb); pw32(p, P_CLBU, (uint32_t)(clb >> 32));
    pw32(p, P_FB, (uint32_t)fb);   pw32(p, P_FBU, (uint32_t)(fb >> 32));
    pw32(p, P_SERR, 0xFFFFFFFFU);
    pw32(p, P_IS, 0xFFFFFFFFU);
    pw32(p, P_IE, 0);
    if (port_start(p) < 0) { printk("[AHCI] %d:%d: device busy, skipped\n", (long)h->idx, (long)num); return 0; }

    /* IDENTIFY DEVICE */
    memset(h->bounce, 0, 512);
    uint64_t fl = irq_save();
    int rc = ata_cmd(p, 0xEC, 0, 0, 0, 512);
    irq_restore(fl);
    if (rc < 0) { printk("[AHCI] %d:%d: IDENTIFY failed (%d)\n", (long)h->idx, (long)num, (long)rc); return 0; }

    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = (uint16_t)(h->bounce[i * 2] | (h->bounce[i * 2 + 1] << 8));

    if ((id[106] & 0xC000) == 0x4000 && (id[106] & (1U << 12))) {
        uint32_t words = id[117] | ((uint32_t)id[118] << 16);
        if (words != 256) {
            printk("[AHCI] %d:%d: logical sector %d bytes is not supported\n", (long)h->idx, (long)num, (long)(words * 2));
            return 0;
        }
    }
    p->lba48 = (id[83] & (1U << 10)) ? 1 : 0;
    if (p->lba48)
        p->sectors = (uint64_t)id[100] | ((uint64_t)id[101] << 16) | ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    else
        p->sectors = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
    if (!p->sectors) return 0;

    for (int i = 0; i < 20; i++) {
        p->model[i * 2] = (char)(id[27 + i] >> 8);
        p->model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    p->model[40] = '\0';
    int e = 39;
    while (e >= 0 && (p->model[e] == ' ' || p->model[e] == 0)) p->model[e--] = '\0';
    for (int i = 0; i < 40; i++) if (p->model[i] && (p->model[i] < 32 || p->model[i] > 126)) p->model[i] = '?';

    struct blk_dev b;
    memset(&b, 0, sizeof(b));
    b.kind = BLK_AHCI;
    b.lba_bytes = 512;
    b.sectors = p->sectors;
    b.writable = 0;                                   /* по умолчанию только чтение */
    b.rw = ahci_rw;
    b.flush = ahci_flush;
    b.priv = p;
    memcpy(b.model, p->model, sizeof(b.model));
    b.name[0] = 's'; b.name[1] = 'd';
    b.name[2] = (char)('a' + (ndisks % 26));
    b.name[3] = '\0';
    int dev = blk_add_disk(&b);
    if (dev < 0) return 0;
    ndisks++;
    nports++;
    printk("[OK] SATA %s: %d MB, %s (AHCI %d port %d, LBA%d)\n", b.name,
           (long)(p->sectors / 2048), p->model, (long)h->idx, (long)num, (long)(p->lba48 ? 48 : 28));
    return 1;
}

static int hba_init(const struct pci_device *pd)
{
    if (nhba >= MAX_HBA) return 0;
    struct ahci_hba *h = &H[nhba];
    memset(h, 0, sizeof(*h));
    h->idx = nhba;

    uint64_t abar = pci_read_dword(pd->bus, pd->slot, pd->func, 0x24) & 0xFFFFFFF0U;
    if (!abar) { printk("[AHCI] %x:%x.%d: ABAR not assigned\n", (long)pd->bus, (long)pd->slot, (long)pd->func); return 0; }
    if (!(abar < 0x40000000ULL || (abar >= 0xC0000000ULL && abar + 0x3000 <= 0x100000000ULL))) {
        printk("[AHCI] %x:%x.%d: ABAR %p is outside the shared 3-4 GB window, skipped\n",
               (long)pd->bus, (long)pd->slot, (long)pd->func, abar);
        return 0;
    }
    uint16_t cmd = pci_read_word(pd->bus, pd->slot, pd->func, 0x04);
    pci_write_word(pd->bus, pd->slot, pd->func, 0x04, (uint16_t)(cmd | 0x0406));
    if (abar >= 0x40000000ULL) {
        for (uint64_t pg = 0; pg < 3; pg++)
            map_page(NULL, abar + pg * PAGE_SIZE, abar + pg * PAGE_SIZE, PTE_WRITABLE | (3ULL << 3));
    }
    h->regs = (volatile uint8_t *)abar;

    uint32_t cap = hr32(h, HBA_CAP);
    if (cap == 0xFFFFFFFFU) { printk("[AHCI] %d: registers read as all-ones\n", (long)h->idx); return 0; }

    /* Передача управления от BIOS (BOHC), если поддерживается */
    if (hr32(h, HBA_CAP2) & 1) {
        uint32_t bohc = hr32(h, HBA_BOHC);
        if (bohc & 1) {
            hw32(h, HBA_BOHC, bohc | 2);
            for (int i = 0; i < 2000 && (hr32(h, HBA_BOHC) & 1); i++) mdelay(1);
        }
    }
    hw32(h, HBA_GHC, (hr32(h, HBA_GHC) | (1U << 31)) & ~2U);     /* AE = 1, прерывания выключены */
    hw32(h, HBA_IS, hr32(h, HBA_IS));

    h->bounce = (uint8_t *)get_free_pages(BOUNCE_PAGES);
    if (!h->bounce) { printk("[AHCI] %d: out of memory\n", (long)h->idx); return 0; }
    nhba++;

    uint32_t pi = hr32(h, HBA_PI);
    uint32_t vs = hr32(h, HBA_VS);
    int npi = 0;
    for (uint32_t i = 0; i < 32; i++) if (pi & (1U << i)) npi++;
    printk("[OK] AHCI %d: PCI %x:%x.%d, v%d.%d, %d port(s) implemented\n", (long)h->idx,
           (long)pd->bus, (long)pd->slot, (long)pd->func,
           (long)(vs >> 16), (long)((vs & 0xFFFF) >> 4), (long)npi);

    int found = 0;
    for (uint32_t i = 0; i < 32; i++)
        if (pi & (1U << i)) found += port_init(h, i);
    return found;
}

int ahci_init(void)
{
    int total = 0, seen = 0;
    for (int i = 0; i < pci_get_device_count(); i++) {
        const struct pci_device *pd = pci_get_device(i);
        if (pd->class_code == 0x01 && pd->subclass == 0x06 && pd->prog_if == 0x01) {
            seen++;
            total += hba_init(pd);
        }
    }
    if (!seen) printk("[INFO] No AHCI SATA controller found\n");
    return total;
}
