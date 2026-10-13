/*
 * NVMe (PCIe SSD): собственный драйвер без прерываний (polling).
 *
 * Одна пара очередей Admin (QD 16) и одна пара IO (QD 16), синхронные команды,
 * передача данных через собственный буфер (PRP1/PRP2, до 8 КБ за команду).
 * Поддерживаются пространства имён с LBA 512 (чтение/запись) и 4096 (только чтение).
 * Диск виден блочному слою как nvme<N>n<NSID>; запись разрешает только blk_set_writable().
 */
#include <linux/blkdev.h>
#include <linux/pci.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/tty.h>
#include <asm/io.h>

#define QD            16
#define MAX_CTRL      2
#define MAX_NS        8
#define NS_PER_CTRL   4
#define BOUNCE_BYTES  8192
#define TMO_US        5000000U

struct nvme_ctrl {
    volatile uint8_t *regs;
    uint32_t stride;
    uint64_t asq, acq, isq, icq;
    uint16_t a_tail, a_head, i_tail, i_head;
    uint8_t  a_phase, i_phase;
    uint8_t  dead;
    uint16_t cid;
    uint8_t *bounce;
    uint8_t *ident;
    char     model[41];
    int      idx;
};

struct nvme_ns {
    struct nvme_ctrl *c;
    uint32_t nsid;
    uint32_t lba_bytes;
};

static struct nvme_ctrl CT[MAX_CTRL];
static struct nvme_ns   NS[MAX_NS];
static int nctrl, nns;

static void udelay(uint32_t us) { while (us--) outb(0, 0x80); }
static void mdelay(uint32_t ms) { while (ms--) udelay(1000); }

static inline uint32_t rd32(struct nvme_ctrl *c, uint32_t off) { return *(volatile uint32_t *)(c->regs + off); }
static inline void wr32(struct nvme_ctrl *c, uint32_t off, uint32_t v) { *(volatile uint32_t *)(c->regs + off) = v; }
static inline uint64_t rd64(struct nvme_ctrl *c, uint32_t off) { return rd32(c, off) | ((uint64_t)rd32(c, off + 4) << 32); }
static inline void wr64(struct nvme_ctrl *c, uint32_t off, uint64_t v) { wr32(c, off, (uint32_t)v); wr32(c, off + 4, (uint32_t)(v >> 32)); }
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

/* Одна команда (16 dword) в очередь Admin или IO, ожидание завершения опросом */
static int nvme_cmd(struct nvme_ctrl *c, int admin, uint32_t *cmd, uint32_t *result)
{
    if (c->dead) return -1;
    uint64_t sq = admin ? c->asq : c->isq;
    uint64_t cq = admin ? c->acq : c->icq;
    uint16_t *tail = admin ? &c->a_tail : &c->i_tail;
    uint16_t *head = admin ? &c->a_head : &c->i_head;
    uint8_t *phase = admin ? &c->a_phase : &c->i_phase;
    uint32_t qid = admin ? 0 : 1;

    cmd[0] = (cmd[0] & 0xFFFF) | ((uint32_t)(++c->cid) << 16);
    volatile uint32_t *slot = (volatile uint32_t *)(sq + (uint64_t)*tail * 64);
    for (int i = 0; i < 16; i++) slot[i] = cmd[i];
    *tail = (uint16_t)((*tail + 1) % QD);
    mb();
    wr32(c, 0x1000 + (2 * qid) * c->stride, *tail);

    volatile uint32_t *cqe = (volatile uint32_t *)(cq + (uint64_t)*head * 16);
    uint32_t waited = 0;
    for (;;) {
        uint32_t dw3 = cqe[3];
        if (((dw3 >> 16) & 1) == *phase) break;
        if (waited >= TMO_US) {
            c->dead = 1;                 /* контроллер не отвечает: больше не трогаем */
            printk("[NVMe] nvme%d: command timeout (opcode 0x%x)\n", (long)c->idx, (long)(cmd[0] & 0xFF));
            return -1000;
        }
        udelay(10);
        waited += 10;
    }
    mb();
    uint32_t dw0 = cqe[0], dw3 = cqe[3];
    if (result) *result = dw0;
    *head = (uint16_t)((*head + 1) % QD);
    if (*head == 0) *phase ^= 1;
    wr32(c, 0x1000 + (2 * qid + 1) * c->stride, *head);

    uint32_t status = (dw3 >> 17) & 0x7FFF;
    return status ? -(int)(status | 0x10000) : 0;
}

static void cmd_clear(uint32_t *cmd, uint32_t opcode, uint32_t nsid)
{
    for (int i = 0; i < 16; i++) cmd[i] = 0;
    cmd[0] = opcode;
    cmd[1] = nsid;
}

/* ------------------------------------------------------------------ */
/* Ввод-вывод                                                         */
/* ------------------------------------------------------------------ */
static int nvme_xfer(struct nvme_ns *ns, int write, uint64_t lba, uint32_t nlb, void *buf)
{
    struct nvme_ctrl *c = ns->c;
    uint32_t cmd[16];
    cmd_clear(cmd, write ? 0x01 : 0x02, ns->nsid);
    uint64_t b = (uint64_t)c->bounce;
    cmd[6] = (uint32_t)b; cmd[7] = (uint32_t)(b >> 32);
    if ((uint64_t)nlb * ns->lba_bytes > 4096) {
        uint64_t b2 = b + 4096;
        cmd[8] = (uint32_t)b2; cmd[9] = (uint32_t)(b2 >> 32);
    }
    cmd[10] = (uint32_t)lba;
    cmd[11] = (uint32_t)(lba >> 32);
    cmd[12] = nlb - 1;
    (void)buf;
    return nvme_cmd(c, 0, cmd, NULL);
}

static int nvme_rw(void *priv, int write, uint64_t lba, uint32_t count, void *buf)
{
    struct nvme_ns *ns = (struct nvme_ns *)priv;
    struct nvme_ctrl *c = ns->c;
    uint8_t *p = (uint8_t *)buf;
    int rc = 0;

    if (write && ns->lba_bytes != 512) return -1;
    uint64_t fl = irq_save();

    if (ns->lba_bytes == 512) {
        while (count) {
            uint32_t n = count > BOUNCE_BYTES / 512 ? BOUNCE_BYTES / 512 : count;
            if (write) memcpy(c->bounce, p, n * 512);
            rc = nvme_xfer(ns, write, lba, n, p);
            if (rc < 0) break;
            if (!write) memcpy(p, c->bounce, n * 512);
            lba += n; p += n * 512; count -= n;
        }
    } else {                                   /* 4096: читаем целые блоки и вырезаем нужное */
        while (count) {
            uint64_t blk = lba / 8;
            uint32_t off = (uint32_t)(lba % 8);
            uint32_t n = 8 - off;
            if (n > count) n = count;
            rc = nvme_xfer(ns, 0, blk, 1, p);
            if (rc < 0) break;
            memcpy(p, c->bounce + off * 512, n * 512);
            lba += n; p += n * 512; count -= n;
        }
    }
    irq_restore(fl);
    return rc < 0 ? -1 : 0;
}

static int nvme_flush(void *priv)
{
    struct nvme_ns *ns = (struct nvme_ns *)priv;
    uint32_t cmd[16];
    cmd_clear(cmd, 0x00, ns->nsid);
    uint64_t fl = irq_save();
    int rc = nvme_cmd(ns->c, 0, cmd, NULL);
    irq_restore(fl);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Инициализация контроллера                                          */
/* ------------------------------------------------------------------ */
static int wait_rdy(struct nvme_ctrl *c, uint32_t want, uint32_t ms)
{
    while (ms--) {
        uint32_t st = rd32(c, 0x1C);
        if ((st & 1) == want) return 0;
        if (st & 2) return -2;                  /* CFS */
        mdelay(1);
    }
    return -1;
}

static void trim_copy(char *dst, const uint8_t *src, int n)
{
    int e = n;
    while (e > 0 && (src[e - 1] == ' ' || src[e - 1] == 0)) e--;
    int s = 0;
    while (s < e && src[s] == ' ') s++;
    int k = 0;
    for (int i = s; i < e; i++) dst[k++] = (src[i] >= 32 && src[i] < 127) ? (char)src[i] : '?';
    dst[k] = '\0';
}

static int identify_ns(struct nvme_ctrl *c, uint32_t nsid)
{
    uint32_t cmd[16];
    cmd_clear(cmd, 0x06, nsid);
    uint64_t id = (uint64_t)c->ident;
    memset(c->ident, 0, 4096);
    cmd[6] = (uint32_t)id; cmd[7] = (uint32_t)(id >> 32);
    cmd[10] = 0;                                  /* CNS 0: Identify Namespace */
    if (nvme_cmd(c, 1, cmd, NULL) < 0) return -1;

    const uint8_t *d = c->ident;
    uint64_t nsze = (uint64_t)(d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24)) |
                    ((uint64_t)(d[4] | (d[5] << 8) | (d[6] << 16) | ((uint32_t)d[7] << 24)) << 32);
    if (!nsze) return 0;                          /* пространство имён не подключено */

    uint8_t flbas = d[26];
    uint32_t fmt = flbas & 0x0F;
    if (flbas & 0x10) { printk("[NVMe] nvme%dn%d: extended-LBA metadata format unsupported\n", (long)c->idx, (long)nsid); return 0; }
    const uint8_t *f = d + 128 + fmt * 4;
    uint32_t lbads = f[2];
    if (lbads != 9 && lbads != 12) {
        printk("[NVMe] nvme%dn%d: LBA size 2^%d unsupported\n", (long)c->idx, (long)nsid, (long)lbads);
        return 0;
    }
    if (nns >= MAX_NS) return 0;

    struct nvme_ns *ns = &NS[nns++];
    ns->c = c; ns->nsid = nsid; ns->lba_bytes = 1U << lbads;

    struct blk_dev b;
    memset(&b, 0, sizeof(b));
    b.kind = BLK_NVME;
    b.lba_bytes = ns->lba_bytes;
    b.sectors = nsze * ns->lba_bytes / 512;
    b.writable = 0;                               /* по умолчанию только чтение */
    b.rw = nvme_rw;
    b.flush = nvme_flush;
    b.priv = ns;
    memcpy(b.model, c->model, sizeof(b.model));
    char *o = b.name;
    const char *pre = "nvme";
    while (*pre) *o++ = *pre++;
    *o++ = (char)('0' + c->idx);
    *o++ = 'n';
    if (nsid >= 10) *o++ = (char)('0' + nsid / 10);
    *o++ = (char)('0' + nsid % 10);
    *o = '\0';
    int dev = blk_add_disk(&b);
    if (dev >= 0)
        printk("[OK] NVMe %s: %d MB, LBA %d bytes, %s\n", b.name,
               (long)(b.sectors / 2048), (long)ns->lba_bytes, c->model);
    return dev >= 0 ? 1 : 0;
}

static int ctrl_init(const struct pci_device *pd)
{
    if (nctrl >= MAX_CTRL) return 0;
    struct nvme_ctrl *c = &CT[nctrl];
    memset(c, 0, sizeof(*c));
    c->idx = nctrl;

    uint32_t lo = pci_read_dword(pd->bus, pd->slot, pd->func, 0x10);
    uint64_t bar = lo & 0xFFFFFFF0U;
    if (((lo >> 1) & 3) == 2) bar |= (uint64_t)pci_read_dword(pd->bus, pd->slot, pd->func, 0x14) << 32;
    if (!bar) { printk("[NVMe] %x:%x.%d: BAR0 not assigned\n", (long)pd->bus, (long)pd->slot, (long)pd->func); return 0; }

    /* Регистры должны быть видны из любого процесса: общая часть адресного
     * пространства -- первый гигабайт и PDPT[3] (3..4 ГБ) */
    if (!(bar < 0x40000000ULL || (bar >= 0xC0000000ULL && bar + 0x4000 <= 0x100000000ULL))) {
        printk("[NVMe] %x:%x.%d: BAR at %p is outside the shared 3-4 GB window, skipped\n",
               (long)pd->bus, (long)pd->slot, (long)pd->func, bar);
        return 0;
    }
    uint16_t cmd = pci_read_word(pd->bus, pd->slot, pd->func, 0x04);
    pci_write_word(pd->bus, pd->slot, pd->func, 0x04, (uint16_t)(cmd | 0x0406));  /* MEM + BusMaster + INTx off */

    if (bar >= 0x40000000ULL) {
        for (uint64_t pg = 0; pg < 4; pg++)
            map_page(NULL, bar + pg * PAGE_SIZE, bar + pg * PAGE_SIZE, PTE_WRITABLE | (3ULL << 3));
    }
    c->regs = (volatile uint8_t *)bar;

    uint32_t vs = rd32(c, 0x08);
    if (vs == 0xFFFFFFFFU) { printk("[NVMe] nvme%d: registers read as all-ones\n", (long)c->idx); return 0; }
    uint64_t cap = rd64(c, 0x00);
    uint32_t mqes = (uint32_t)(cap & 0xFFFF) + 1;
    uint32_t to = (uint32_t)((cap >> 24) & 0xFF);
    c->stride = 4U << ((cap >> 32) & 0xF);
    uint32_t mpsmin = (uint32_t)((cap >> 48) & 0xF);
    if (mqes < QD || mpsmin != 0 || !((cap >> 37) & 1)) {
        printk("[NVMe] nvme%d: unsupported controller (CAP=%p)\n", (long)c->idx, cap);
        return 0;
    }
    uint32_t tmo_ms = (to ? to : 20) * 500;
    if (tmo_ms > 20000) tmo_ms = 20000;

    /* Сброс: CC.EN = 0 */
    uint32_t cc = rd32(c, 0x14);
    if ((cc & 1) || (rd32(c, 0x1C) & 1)) {
        wr32(c, 0x14, cc & ~1U);
        if (wait_rdy(c, 0, tmo_ms) < 0) { printk("[NVMe] nvme%d: reset timeout\n", (long)c->idx); return 0; }
    }

    c->asq = get_free_page();
    c->acq = get_free_page();
    c->isq = get_free_page();
    c->icq = get_free_page();
    c->bounce = (uint8_t *)get_free_pages(2);
    c->ident = (uint8_t *)get_free_page();
    if (!c->asq || !c->acq || !c->isq || !c->icq || !c->bounce || !c->ident) {
        printk("[NVMe] nvme%d: out of memory\n", (long)c->idx);
        return 0;
    }
    c->a_phase = c->i_phase = 1;

    wr32(c, 0x0C, 0xFFFFFFFFU);                              /* INTMS: маскируем все векторы */
    wr32(c, 0x24, ((QD - 1) << 16) | (QD - 1));              /* AQA */
    wr64(c, 0x28, c->asq);
    wr64(c, 0x30, c->acq);
    wr32(c, 0x14, (6U << 16) | (4U << 20) | 1U);             /* IOSQES=64, IOCQES=16, MPS=4K, EN */
    int r = wait_rdy(c, 1, tmo_ms);
    if (r < 0) { printk("[NVMe] nvme%d: controller did not become ready (%d)\n", (long)c->idx, (long)r); return 0; }

    uint32_t cm[16];
    uint64_t id = (uint64_t)c->ident;

    /* Identify Controller */
    cmd_clear(cm, 0x06, 0);
    cm[6] = (uint32_t)id; cm[7] = (uint32_t)(id >> 32);
    cm[10] = 1;
    if (nvme_cmd(c, 1, cm, NULL) < 0) { printk("[NVMe] nvme%d: Identify Controller failed\n", (long)c->idx); return 0; }
    trim_copy(c->model, c->ident + 24, 40);
    uint32_t nn = c->ident[516] | (c->ident[517] << 8) | (c->ident[518] << 16) | ((uint32_t)c->ident[519] << 24);

    /* Number of Queues: просим по одной очереди */
    cmd_clear(cm, 0x09, 0);
    cm[10] = 7;
    nvme_cmd(c, 1, cm, NULL);

    /* IO Completion Queue, затем IO Submission Queue (QID 1) */
    cmd_clear(cm, 0x05, 0);
    cm[6] = (uint32_t)c->icq; cm[7] = (uint32_t)(c->icq >> 32);
    cm[10] = ((QD - 1) << 16) | 1;
    cm[11] = 1;                                               /* PC=1, прерывания выключены */
    if (nvme_cmd(c, 1, cm, NULL) < 0) { printk("[NVMe] nvme%d: Create IO CQ failed\n", (long)c->idx); return 0; }
    cmd_clear(cm, 0x01, 0);
    cm[6] = (uint32_t)c->isq; cm[7] = (uint32_t)(c->isq >> 32);
    cm[10] = ((QD - 1) << 16) | 1;
    cm[11] = (1U << 16) | 1;
    if (nvme_cmd(c, 1, cm, NULL) < 0) { printk("[NVMe] nvme%d: Create IO SQ failed\n", (long)c->idx); return 0; }

    nctrl++;
    printk("[OK] NVMe nvme%d: PCI %x:%x.%d v%d.%d, %s, %d namespace(s)\n", (long)c->idx,
           (long)pd->bus, (long)pd->slot, (long)pd->func,
           (long)(vs >> 16), (long)((vs >> 8) & 0xFF), c->model, (long)nn);

    /* Активные пространства имён: список (CNS 2), иначе перебор 1..nn */
    int found = 0, listed = 0;
    cmd_clear(cm, 0x06, 0);
    cm[6] = (uint32_t)id; cm[7] = (uint32_t)(id >> 32);
    cm[10] = 2;
    memset(c->ident, 0, 4096);
    if (nvme_cmd(c, 1, cm, NULL) == 0) {
        uint32_t list[NS_PER_CTRL];
        int n = 0;
        for (int i = 0; i < 1024 && n < NS_PER_CTRL; i++) {
            uint32_t v = c->ident[i * 4] | (c->ident[i * 4 + 1] << 8) | (c->ident[i * 4 + 2] << 16) |
                         ((uint32_t)c->ident[i * 4 + 3] << 24);
            if (!v) break;
            list[n++] = v;
        }
        listed = 1;
        for (int i = 0; i < n; i++) found += identify_ns(c, list[i]) > 0;
    }
    if (!listed) {
        for (uint32_t i = 1; i <= nn && i <= NS_PER_CTRL; i++) found += identify_ns(c, i) > 0;
    }
    if (!found) printk("[NVMe] nvme%d: no usable namespaces\n", (long)c->idx);
    return found;
}

int nvme_init(void)
{
    int total = 0;
    for (int i = 0; i < pci_get_device_count(); i++) {
        const struct pci_device *pd = pci_get_device(i);
        if (pd->class_code == 0x01 && pd->subclass == 0x08 && pd->prog_if == 0x02)
            total += ctrl_init(pd);
    }
    if (!total && !nctrl) printk("[INFO] No NVMe controller found\n");
    return total;
}
