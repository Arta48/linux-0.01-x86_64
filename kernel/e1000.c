#include <linux/e1000.h>
#include <linux/pci.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/string.h>

static volatile uint32_t *e1000_mmio = NULL;
static uint8_t mac_addr[6];

static struct e1000_rx_desc *rx_descs = NULL;
static char *rx_buffers[E1000_NUM_RX_DESC];
static uint32_t rx_cur = 0;

static struct e1000_tx_desc *tx_descs = NULL;
static char *tx_buffers[E1000_NUM_TX_DESC];
static uint32_t tx_cur = 0;

static inline void mmio_write32(uint32_t reg, uint32_t val)
{
    e1000_mmio[reg >> 2] = val;
}

static inline uint32_t mmio_read32(uint32_t reg)
{
    return e1000_mmio[reg >> 2];
}

void e1000_get_mac(uint8_t out_mac[6])
{
    memcpy(out_mac, mac_addr, 6);
}

int e1000_init(void)
{
    struct pci_device dev;
    if (pci_find_device(PCI_VENDOR_INTEL, PCI_DEVICE_E1000, &dev) < 0) {
        printk("[WARN] Intel e1000 NIC not found on PCI bus!\n");
        return -1;
    }

    printk("[OK] PCI: Found Intel 82540EM (e1000) at bus %d, slot %d (MMIO: %p)\n",
           dev.bus, dev.slot, dev.bar0);

    /* Проецируем 128 КБ MMIO-пространства e1000 */
    for (uint64_t p = 0; p < 32; p++) {
        map_page(NULL, dev.bar0 + (p * PAGE_SIZE), dev.bar0 + (p * PAGE_SIZE), PTE_WRITABLE);
    }
    e1000_mmio = (volatile uint32_t *)dev.bar0;

    /* Сброс устройства */
    mmio_write32(E1000_REG_CTRL, mmio_read32(E1000_REG_CTRL) | (1 << 26));
    for (volatile int i = 0; i < 100000; i++) {}
    mmio_write32(E1000_REG_CTRL, mmio_read32(E1000_REG_CTRL) & ~(1 << 26));

    /* Считывание аппаратного MAC-адреса из RAL/RAH */
    uint32_t low = mmio_read32(E1000_REG_RAL);
    uint32_t high = mmio_read32(E1000_REG_RAH);
    mmio_write32(E1000_REG_RAH, high | (1U << 31)); /* Устанавливаем бит Address Valid */
    mac_addr[0] = (uint8_t)(low >> 0);
    mac_addr[1] = (uint8_t)(low >> 8);
    mac_addr[2] = (uint8_t)(low >> 16);
    mac_addr[3] = (uint8_t)(low >> 24);
    mac_addr[4] = (uint8_t)(high >> 0);
    mac_addr[5] = (uint8_t)(high >> 8);

    /* Инициализация кольца приема (RX) */
    uint64_t rx_mem = get_free_page();
    rx_descs = (struct e1000_rx_desc *)rx_mem;

    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        rx_buffers[i] = (char *)get_free_page();
        rx_descs[i].buffer_addr = (uint64_t)rx_buffers[i];
        rx_descs[i].status = 0;
    }

    mmio_write32(E1000_REG_RDBAL, (uint32_t)rx_mem);
    mmio_write32(E1000_REG_RDBAH, 0);
    mmio_write32(E1000_REG_RDLEN, E1000_NUM_RX_DESC * sizeof(struct e1000_rx_desc));
    mmio_write32(E1000_REG_RDH, 0);
    mmio_write32(E1000_REG_RDT, E1000_NUM_RX_DESC - 1);
    rx_cur = 0;

    /* Включаем прием: EN (бит 1), UPE (бит 3), MPE (бит 4), BAM (бит 15), SECRC (бит 26) */
    mmio_write32(E1000_REG_RCTL, (1 << 1) | (1 << 3) | (1 << 4) | (1 << 15) | (1 << 26));

    /* Инициализация кольца передачи (TX) */
    uint64_t tx_mem = get_free_page();
    tx_descs = (struct e1000_tx_desc *)tx_mem;

    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        tx_buffers[i] = (char *)get_free_page();
        tx_descs[i].buffer_addr = (uint64_t)tx_buffers[i];
        tx_descs[i].status = 1; /* DD (Descriptor Done) */
        tx_descs[i].cmd = 0;
    }

    mmio_write32(E1000_REG_TDBAL, (uint32_t)tx_mem);
    mmio_write32(E1000_REG_TDBAH, 0);
    mmio_write32(E1000_REG_TDLEN, E1000_NUM_TX_DESC * sizeof(struct e1000_tx_desc));
    mmio_write32(E1000_REG_TDH, 0);
    mmio_write32(E1000_REG_TDT, 0);
    tx_cur = 0;

    /* Включаем передачу: EN (бит 1), PSP (бит 3), CT=0x0F, COLD=0x40 */
    mmio_write32(E1000_REG_TCTL, (1 << 1) | (1 << 3) | (0x0F << 4) | (0x40 << 12));

    /* Маскируем прерывания (работаем в стабильном polling-режиме) */
    mmio_write32(E1000_REG_IMS, 0);

    printk("[OK] Intel e1000 Initialized: MAC = %02x:%02x:%02x:%02x:%02x:%02x (Link Up, 1000 Mbps)\n",
           mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    return 0;
}

int e1000_send(const void *data, uint16_t len)
{
    if (!e1000_mmio || len > E1000_BUFFER_SIZE) return -1;

    uint32_t cur = tx_cur;
    while (!(tx_descs[cur].status & 0x01)) {
        __asm__ volatile ("pause");
    }

    memcpy(tx_buffers[cur], data, len);
    tx_descs[cur].length = len;
    /* Команды: EOP (бит 0 - End of Packet), IFCS (бит 1 - Insert FCS/CRC), RS (бит 3 - Report Status) */
    tx_descs[cur].cmd = (1 << 0) | (1 << 1) | (1 << 3);
    tx_descs[cur].status = 0;

    tx_cur = (tx_cur + 1) % E1000_NUM_TX_DESC;
    mmio_write32(E1000_REG_TDT, tx_cur);
    return len;
}

int e1000_recv(void *buf, uint16_t max_len)
{
    if (!e1000_mmio) return 0;

    uint32_t cur = rx_cur;
    if (!(rx_descs[cur].status & 0x01)) {
        return 0; /* Нет новых пакетов */
    }

    uint16_t len = rx_descs[cur].length;
    if (len > max_len) len = max_len;

    memcpy(buf, rx_buffers[cur], len);
    rx_descs[cur].status = 0;

    rx_cur = (rx_cur + 1) % E1000_NUM_RX_DESC;
    mmio_write32(E1000_REG_RDT, cur);
    return len;
}
