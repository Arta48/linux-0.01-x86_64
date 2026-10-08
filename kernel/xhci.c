#include <linux/xhci.h>
#include <linux/pci.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/string.h>

static struct xhci_info x_info;
static volatile uint8_t  *xhci_cap = NULL;
static volatile uint32_t *xhci_op  = NULL;

const struct xhci_info *xhci_get_info(void)
{
    return &x_info;
}

static inline void op_write32(uint32_t reg, uint32_t val)
{
    xhci_op[reg >> 2] = val;
}

static inline uint32_t op_read32(uint32_t reg)
{
    return xhci_op[reg >> 2];
}

int xhci_init(void)
{
    memset(&x_info, 0, sizeof(x_info));
    struct pci_device dev;

    /* Поиск контроллера USB xHCI на шине PCI: Class 0x0C, Subclass 0x03, Prog IF 0x30 */
    if (pci_find_class(0x0C, 0x03, 0x30, &dev) < 0) {
        printk("[WARN] xHCI USB 3.0 Controller not found on PCI bus!\n");
        return -1;
    }

    printk("[OK] PCI: Found USB 3.0 xHCI Controller at bus %d, slot %d (MMIO: %p)\n",
           dev.bus, dev.slot, dev.bar0);

    /* Проецируем 64 КБ памяти MMIO для xHCI */
    for (uint64_t p = 0; p < 16; p++) {
        map_page(NULL, dev.bar0 + (p * PAGE_SIZE), dev.bar0 + (p * PAGE_SIZE), PTE_WRITABLE);
    }

    x_info.mmio_base = dev.bar0;
    xhci_cap = (volatile uint8_t *)dev.bar0;

    /* Считывание базовых параметров Capability Registers */
    x_info.cap_length  = xhci_cap[0];
    x_info.hci_version = *(volatile uint16_t *)(xhci_cap + 2);
    uint32_t hcsparams1 = *(volatile uint32_t *)(xhci_cap + 4);
    x_info.max_slots = (uint8_t)(hcsparams1 & 0xFF);
    x_info.max_ports = (uint8_t)((hcsparams1 >> 24) & 0xFF);
    if (x_info.max_ports > 16) x_info.max_ports = 16;

    x_info.doorbell_offset    = *(volatile uint32_t *)(xhci_cap + 0x14) & ~0x3;
    x_info.runtime_offset     = *(volatile uint32_t *)(xhci_cap + 0x18) & ~0x1F;
    x_info.operational_offset = x_info.cap_length;

    xhci_op = (volatile uint32_t *)(dev.bar0 + x_info.operational_offset);

    printk("[OK] xHCI Spec v%d.%d: Ports: %d, Device Slots: %d\n",
           (x_info.hci_version >> 8) & 0xF, (x_info.hci_version >> 4) & 0xF,
           x_info.max_ports, x_info.max_slots);

    /* 1. Остановка контроллера перед сбросом */
    uint32_t cmd = op_read32(XHCI_REG_USBCMD);
    cmd &= ~XHCI_CMD_RS;
    op_write32(XHCI_REG_USBCMD, cmd);

    int timeout = 50000;
    while (!(op_read32(XHCI_REG_USBSTS) & XHCI_STS_HCH) && --timeout) {
        __asm__ volatile ("pause");
    }

    /* 2. Аппаратный сброс хост-контроллера (HCRST) */
    op_write32(XHCI_REG_USBCMD, op_read32(XHCI_REG_USBCMD) | XHCI_CMD_HCRST);
    timeout = 100000;
    while ((op_read32(XHCI_REG_USBCMD) & XHCI_CMD_HCRST) && --timeout) {
        __asm__ volatile ("pause");
    }
    timeout = 100000;
    while ((op_read32(XHCI_REG_USBSTS) & XHCI_STS_CNR) && --timeout) {
        __asm__ volatile ("pause");
    }

    /* 3. Инициализация таблицы Device Context Base Address Array (DCBAA) */
    uint64_t dcbaa_page = get_free_page();
    op_write32(XHCI_REG_DCBAAP_LO, (uint32_t)dcbaa_page);
    op_write32(XHCI_REG_DCBAAP_HI, (uint32_t)(dcbaa_page >> 32));

    /* 4. Выделение кольца команд (Command Ring) */
    uint64_t cmd_ring = get_free_page();
    op_write32(XHCI_REG_CRCR_LOW, (uint32_t)cmd_ring | 1); /* RCS = 1 */
    op_write32(XHCI_REG_CRCR_HIGH, (uint32_t)(cmd_ring >> 32));

    /* 5. Установка максимального числа используемых слотов */
    op_write32(XHCI_REG_CONFIG, x_info.max_slots);

    /* 6. Запуск контроллера xHCI (Run/Stop = 1) */
    op_write32(XHCI_REG_USBCMD, op_read32(XHCI_REG_USBCMD) | XHCI_CMD_RS);
    timeout = 50000;
    while ((op_read32(XHCI_REG_USBSTS) & XHCI_STS_HCH) && --timeout) {
        __asm__ volatile ("pause");
    }

    /* 7. Опрос корневых портов (Port Status & Control - PORTSC) */
    x_info.active_ports_count = 0;
    for (int p = 1; p <= x_info.max_ports; p++) {
        uint32_t port_reg_offset = x_info.operational_offset + 0x400 + (p - 1) * 0x10;
        volatile uint32_t *p_portsc = (volatile uint32_t *)(dev.bar0 + port_reg_offset);
        uint32_t portsc = *p_portsc;

        x_info.ports[p - 1].port_num = p;
        x_info.ports[p - 1].connected = (portsc & 1);
        x_info.ports[p - 1].speed = (portsc >> 10) & 0xF;
        x_info.ports[p - 1].raw_status = portsc;

        if (portsc & 1) {
            x_info.active_ports_count++;
            const char *sp_str = "USB 2.0 HighSpeed";
            if (x_info.ports[p - 1].speed == 4) sp_str = "USB 3.0 SuperSpeed (5 Gbps)";
            else if (x_info.ports[p - 1].speed == 1) sp_str = "USB 1.1 FullSpeed";

            printk("[OK] xHCI Port %d: Device Attached [%s] (PORTSC: 0x%08x)\n",
                   p, sp_str, portsc);
        }
    }

    x_info.present = 1;
    printk("[OK] USB 3.0 xHCI Controller Initialized & Active (%d devices connected)\n",
           x_info.active_ports_count);
    return 0;
}
