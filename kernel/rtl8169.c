/*
 * Драйвер Realtek RTL8168/8111 (PCIe Gigabit), RTL8169/8110, RTL8101/8102
 * (Fast Ethernet) и экспериментально RTL8125 (2.5G). Опрос без прерываний,
 * кольца дескрипторов по 32 RX / 16 TX, буфер на дескриптор -- 2 КБ.
 *
 * Последовательность инициализации соответствует r8169 из Linux: сброс,
 * разблокировка Cfg9346, адреса колец, RxMaxSize, CR = TE|RE, TxConfig,
 * RxConfig, блокировка.
 */
#include <linux/nic.h>
#include <linux/pci.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/string.h>

#define RTL_VENDOR 0x10EC

#define REG_IDR0        0x00
#define REG_TXDESC_LO   0x20
#define REG_TXDESC_HI   0x24
#define REG_CR          0x37
#define REG_TXPOLL      0x38    /* 8168/8169 */
#define REG_IMR         0x3C    /* 8168/8169 (16 бит) */
#define REG_ISR         0x3E
#define REG_TXCFG       0x40
#define REG_RXCFG       0x44
#define REG_CFG9346     0x50
#define REG_PHYSTATUS   0x6C
#define REG_TXPOLL_8125 0x90
#define REG_IMR_8125    0x38    /* 32 бита */
#define REG_ISR_8125    0x3C
#define REG_RXMAXSIZE   0xDA
#define REG_CPLUSCMD    0xE0
#define REG_RXDESC_LO   0xE4
#define REG_RXDESC_HI   0xE8
#define REG_MAXTXPKT    0xEC

#define CR_RST  0x10
#define CR_RE   0x08
#define CR_TE   0x04

#define DESC_OWN 0x80000000U
#define DESC_EOR 0x40000000U
#define DESC_FS  0x20000000U
#define DESC_LS  0x10000000U
#define RX_RES   (1U << 21)

#define RX_COUNT  128
#define TX_COUNT  16
#define BUF_SIZE  2048

struct rtl_desc {
    uint32_t opts1;
    uint32_t opts2;
    uint64_t addr;
} __attribute__((packed));

static volatile uint8_t *regs;
static int is_8125;
static struct rtl_desc *rx_ring, *tx_ring;
static uint8_t *rx_buf[RX_COUNT], *tx_buf[TX_COUNT];
static uint32_t rx_cur, tx_cur;
static const char *chip_name = "Realtek";
static struct nic_ops rtl_ops;

static inline uint8_t  r8(uint32_t o)  { return regs[o]; }
static inline uint16_t r16(uint32_t o) { return *(volatile uint16_t *)(regs + o); }
static inline uint32_t r32(uint32_t o) { return *(volatile uint32_t *)(regs + o); }
static inline void w8(uint32_t o, uint8_t v)   { regs[o] = v; }
static inline void w16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(regs + o) = v; }
static inline void w32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(regs + o) = v; }
static inline void mb(void) { __asm__ volatile ("mfence" ::: "memory"); }

static const struct { uint16_t id; const char *name; int v8125; } rtl_ids[] = {
    { 0x8168, "Realtek RTL8168/8111", 0 },
    { 0x8161, "Realtek RTL8168/8111", 0 },
    { 0x8162, "Realtek RTL8168/8111", 0 },
    { 0x8169, "Realtek RTL8169", 0 },
    { 0x8167, "Realtek RTL8110SC/8169SC", 0 },
    { 0x8136, "Realtek RTL8101/8102 (Fast Ethernet)", 0 },
    { 0x8125, "Realtek RTL8125 2.5GbE (experimental)", 1 },
    { 0x3000, "Realtek RTL8125 2.5GbE (experimental)", 1 },
    { 0x2600, "Realtek RTL8125 2.5GbE (experimental)", 1 },
};

static void rx_arm(int i)
{
    uint32_t o = DESC_OWN | BUF_SIZE;
    if (i == RX_COUNT - 1) o |= DESC_EOR;
    rx_ring[i].opts2 = 0;
    mb();
    rx_ring[i].opts1 = o;
}

static int rtl_send(const void *data, uint16_t len)
{
    if (!regs || len > BUF_SIZE) return -1;
    uint32_t cur = tx_cur;
    uint64_t t0 = time_us();
    while (tx_ring[cur].opts1 & DESC_OWN) {
        if (time_us() - t0 > 20000) return -1;      /* передатчик завис */
        __asm__ volatile ("pause");
    }
    memcpy(tx_buf[cur], data, len);
    uint16_t n = len;
    if (n < 60) { memset(tx_buf[cur] + n, 0, 60 - n); n = 60; }

    uint32_t o = DESC_OWN | DESC_FS | DESC_LS | n;
    if (cur == TX_COUNT - 1) o |= DESC_EOR;
    tx_ring[cur].opts2 = 0;
    mb();
    tx_ring[cur].opts1 = o;
    mb();
    tx_cur = (cur + 1) % TX_COUNT;
    if (is_8125) w16(REG_TXPOLL_8125, 0x0001);
    else         w8(REG_TXPOLL, 0x40);              /* NPQ: опросить очередь */
    return len;
}

static int rtl_recv(void *buf, uint16_t max_len)
{
    if (!regs) return 0;
    for (int guard = 0; guard < RX_COUNT; guard++) {
        uint32_t cur = rx_cur;
        uint32_t o = rx_ring[cur].opts1;
        if (o & DESC_OWN) return 0;
        mb();

        int good = (o & DESC_FS) && (o & DESC_LS) && !(o & RX_RES);
        uint32_t len = o & 0x3FFF;
        int ret = 0;
        if (good && len > 4) {
            len -= 4;                               /* CRC */
            if (len > max_len) len = max_len;
            memcpy(buf, rx_buf[cur], len);
            ret = (int)len;
        }
        rx_arm((int)cur);
        rx_cur = (cur + 1) % RX_COUNT;
        if (ret > 0) return ret;                    /* битые кадры пропускаем */
    }
    return 0;
}

static int rtl_link(uint32_t *speed, int *full)
{
    uint16_t s = r16(REG_PHYSTATUS);
    if (!(s & 0x02)) return 0;
    uint32_t sp = 10;
    if (s & 0x08) sp = 100;
    if (s & 0x10) sp = 1000;
    if (is_8125 && (s & 0x400)) sp = 2500;
    if (speed) *speed = sp;
    if (full) *full = (s & 0x01) ? 1 : 0;
    return 1;
}

static int find_mmio_bar(const struct pci_device *d, uint64_t *out)
{
    for (int i = 0; i < 6; i++) {
        uint32_t v = pci_read_dword(d->bus, d->slot, d->func, (uint8_t)(0x10 + i * 4));
        if (v & 1) continue;                        /* I/O-порты */
        uint64_t a = v & 0xFFFFFFF0U;
        if (((v >> 1) & 3) == 2) {                  /* 64-битный BAR */
            uint32_t hi = pci_read_dword(d->bus, d->slot, d->func, (uint8_t)(0x14 + i * 4));
            a |= (uint64_t)hi << 32;
            i++;
        }
        if (a) { *out = a; return 0; }
    }
    return -1;
}

int rtl8169_init(void)
{
    const struct pci_device *dev = NULL;
    int idx = -1;
    int n = pci_get_device_count();
    for (int i = 0; i < n && !dev; i++) {
        const struct pci_device *d = pci_get_device(i);
        if (d->vendor_id != RTL_VENDOR) continue;
        for (unsigned k = 0; k < sizeof(rtl_ids) / sizeof(rtl_ids[0]); k++) {
            if (rtl_ids[k].id == d->device_id) {
                dev = d; idx = (int)k; break;
            }
        }
    }
    if (!dev) return -1;

    chip_name = rtl_ids[idx].name;
    is_8125 = rtl_ids[idx].v8125;

    uint64_t bar;
    if (find_mmio_bar(dev, &bar) < 0) {
        printk("[WARN] %s at %d:%d.%d: no MMIO BAR\n", chip_name, dev->bus, dev->slot, dev->func);
        return -1;
    }
    printk("[OK] PCI: %s [10ec:%04x] at %d:%d.%d, MMIO %p\n", chip_name, dev->device_id,
           dev->bus, dev->slot, dev->func, bar);

    /* I/O + Memory Space + Bus Master, INTx не нужен */
    uint16_t cmd = pci_read_word(dev->bus, dev->slot, dev->func, 0x04);
    pci_write_word(dev->bus, dev->slot, dev->func, 0x04, (uint16_t)(cmd | 0x0007 | 0x0400));

    regs = (volatile uint8_t *)mmio_map(bar, 0x1000, PTE_WRITABLE | (3ULL << 3));
    if (!regs) { printk("[WARN] RTL81xx: cannot map MMIO\n"); return -1; }

    /* Сброс */
    w8(REG_CR, CR_RST);
    uint64_t t0 = time_us();
    while (r8(REG_CR) & CR_RST) {
        if (time_us() - t0 > 100000) { printk("[WARN] RTL81xx: reset timeout\n"); regs = NULL; return -1; }
    }
    if (r8(REG_CR) == 0xFF && r32(REG_TXCFG) == 0xFFFFFFFF) { regs = NULL; return -1; }

    for (int i = 0; i < 6; i++) rtl_ops.mac[i] = r8(REG_IDR0 + i);

    /* Кольца и буферы */
    rx_ring = (struct rtl_desc *)get_free_page();
    tx_ring = (struct rtl_desc *)get_free_page();
    if (!rx_ring || !tx_ring) { regs = NULL; return -1; }
    memset(rx_ring, 0, 4096);
    memset(tx_ring, 0, 4096);
    for (int i = 0; i < RX_COUNT; i++) {
        rx_buf[i] = (uint8_t *)get_free_page();
        if (!rx_buf[i]) { regs = NULL; return -1; }
        rx_ring[i].addr = (uint64_t)rx_buf[i];
        rx_arm(i);
    }
    for (int i = 0; i < TX_COUNT; i++) {
        tx_buf[i] = (uint8_t *)get_free_page();
        if (!tx_buf[i]) { regs = NULL; return -1; }
        tx_ring[i].addr = (uint64_t)tx_buf[i];
        tx_ring[i].opts1 = (i == TX_COUNT - 1) ? DESC_EOR : 0;
    }
    rx_cur = tx_cur = 0;

    w8(REG_CFG9346, 0xC0);                          /* разблокировать конфиг */

    /* Прерывания не используем (опрос), статус сбросить */
    if (is_8125) { w32(REG_IMR_8125, 0); w32(REG_ISR_8125, 0xFFFFFFFF); }
    else         { w16(REG_IMR, 0);      w16(REG_ISR, 0xFFFF); }

    /* C+ Command: без VLAN-удаления и аппаратной контрольной суммы RX */
    if (!is_8125) w16(REG_CPLUSCMD, (uint16_t)(r16(REG_CPLUSCMD) & ~((1 << 6) | (1 << 5))));

    w32(REG_TXDESC_HI, (uint32_t)((uint64_t)tx_ring >> 32));
    w32(REG_TXDESC_LO, (uint32_t)(uint64_t)tx_ring);
    w32(REG_RXDESC_HI, (uint32_t)((uint64_t)rx_ring >> 32));
    w32(REG_RXDESC_LO, (uint32_t)(uint64_t)rx_ring);

    w16(REG_RXMAXSIZE, 0x0600);
    if (!is_8125) w8(REG_MAXTXPKT, 0x3B);

    w8(REG_CR, CR_TE | CR_RE);

    w32(REG_TXCFG, 0x03000700);                     /* IFG 96 бит, DMA burst unlimited */
    uint32_t rx = r32(REG_RXCFG) & ~0x3FU;
    rx |= 0x0E;                                     /* broadcast + multicast + свой MAC */
    rx |= (7U << 8);                                /* DMA burst unlimited */
    if (is_8125) rx |= (8U << 27);
    else         rx |= (7U << 13);                  /* без порога FIFO */
    w32(REG_RXCFG, rx);

    w8(REG_CFG9346, 0x00);                          /* заблокировать конфиг */

    rtl_ops.name = chip_name;
    rtl_ops.send = rtl_send;
    rtl_ops.recv = rtl_recv;
    rtl_ops.link = rtl_link;
    nic_register(&rtl_ops);

    uint32_t sp = 0; int fd = 0;
    int up = rtl_link(&sp, &fd);
    printk("[OK] %s initialized: MAC = %02x:%02x:%02x:%02x:%02x:%02x, link %s",
           chip_name, rtl_ops.mac[0], rtl_ops.mac[1], rtl_ops.mac[2],
           rtl_ops.mac[3], rtl_ops.mac[4], rtl_ops.mac[5], up ? "UP" : "down (cable / autoneg)");
    if (up) printk(" %d Mbps %s", (int)sp, fd ? "full" : "half");
    printk("\n");
    return 0;
}
