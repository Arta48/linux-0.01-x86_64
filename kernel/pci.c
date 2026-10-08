#include <linux/pci.h>
#include <linux/tty.h>
#include <asm/io.h>

#define MAX_PCI_DEVS 32
static struct pci_device pci_devices[MAX_PCI_DEVS];
static int pci_dev_count = 0;

static inline uint32_t pci_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    return (1U << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
    ((uint32_t)func << 8) | (offset & 0xFC);
}

uint32_t pci_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    outl(pci_addr(bus, slot, func, offset), PCI_CONFIG_ADDRESS);
    return inl(PCI_CONFIG_DATA);
}

void pci_write_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val)
{
    outl(pci_addr(bus, slot, func, offset), PCI_CONFIG_ADDRESS);
    outl(val, PCI_CONFIG_DATA);
}

uint16_t pci_read_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t dw = pci_read_dword(bus, slot, func, offset);
    return (uint16_t)((dw >> ((offset & 2) * 8)) & 0xFFFF);
}

void pci_write_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val)
{
    uint32_t dw = pci_read_dword(bus, slot, func, offset);
    if (offset & 2) {
        dw = (dw & 0x0000FFFF) | ((uint32_t)val << 16);
    } else {
        dw = (dw & 0xFFFF0000) | (uint32_t)val;
    }
    pci_write_dword(bus, slot, func, offset, dw);
}

int pci_find_device(uint16_t vendor_id, uint16_t device_id, struct pci_device *out_dev)
{
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t v = pci_read_dword((uint8_t)bus, slot, 0, 0);
            if ((v & 0xFFFF) == 0xFFFF) continue;

            uint16_t vend = (uint16_t)(v & 0xFFFF);
            uint16_t dev  = (uint16_t)(v >> 16);

            if (vend == vendor_id && dev == device_id) {
                out_dev->bus = (uint8_t)bus;
                out_dev->slot = slot;
                out_dev->func = 0;
                out_dev->vendor_id = vend;
                out_dev->device_id = dev;

                uint32_t bar0 = pci_read_dword((uint8_t)bus, slot, 0, 0x10);
                out_dev->bar0 = bar0 & 0xFFFFFFF0;
                out_dev->irq  = (uint8_t)(pci_read_dword((uint8_t)bus, slot, 0, 0x3C) & 0xFF);

                /* Активируем Bus Master и Memory Space */
                uint16_t cmd = pci_read_word((uint8_t)bus, slot, 0, 0x04);
                pci_write_word((uint8_t)bus, slot, 0, 0x04, cmd | 0x0007);
                return 0;
            }
        }
    }
    return -1;
}

const char *pci_class_to_string(uint8_t class_code, uint8_t subclass, uint8_t prog_if)
{
    if (class_code == 0x01) {
        if (subclass == 0x01) return "IDE Storage Controller";
        if (subclass == 0x06) return "SATA AHCI Controller";
        if (subclass == 0x08) return "NVMe SSD Controller";
        return "Mass Storage Controller";
    }
    if (class_code == 0x02) {
        if (subclass == 0x00) return "Ethernet Network Controller";
        return "Network Controller";
    }
    if (class_code == 0x03) return "VGA Display Adapter";
    if (class_code == 0x04) return "Multimedia Audio Controller";
    if (class_code == 0x06) return "PCI/Host System Bridge";
    if (class_code == 0x0C && subclass == 0x03) {
        if (prog_if == 0x00) return "USB UHCI Controller";
        if (prog_if == 0x10) return "USB OHCI Controller";
        if (prog_if == 0x20) return "USB 2.0 EHCI Controller";
        if (prog_if == 0x30) return "USB 3.0 xHCI Controller";
        return "USB Controller";
    }
    return "Generic Peripheral Device";
}

void pci_scan_all(void)
{
    pci_dev_count = 0;
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t v0 = pci_read_dword((uint8_t)bus, slot, 0, 0);
            if ((v0 & 0xFFFF) == 0xFFFF) continue;

            uint8_t hdr = (uint8_t)((pci_read_dword((uint8_t)bus, slot, 0, 0x0C) >> 16) & 0xFF);
            uint8_t max_func = (hdr & 0x80) ? 8 : 1;

            for (uint8_t func = 0; func < max_func; func++) {
                uint32_t val = pci_read_dword((uint8_t)bus, slot, func, 0);
                if ((val & 0xFFFF) == 0xFFFF) continue;

                uint16_t vend = (uint16_t)(val & 0xFFFF);
                uint16_t dev  = (uint16_t)(val >> 16);
                uint32_t c_rev = pci_read_dword((uint8_t)bus, slot, func, 0x08);

                if (pci_dev_count < MAX_PCI_DEVS) {
                    struct pci_device *d = &pci_devices[pci_dev_count++];
                    d->bus = (uint8_t)bus;
                    d->slot = slot;
                    d->func = func;
                    d->vendor_id = vend;
                    d->device_id = dev;
                    d->class_code = (uint8_t)(c_rev >> 24);
                    d->subclass   = (uint8_t)(c_rev >> 16);
                    d->prog_if   = (uint8_t)(c_rev >> 8);
                    d->bar0 = pci_read_dword((uint8_t)bus, slot, func, 0x10) & 0xFFFFFFF0;
                    d->irq  = (uint8_t)(pci_read_dword((uint8_t)bus, slot, func, 0x3C) & 0xFF);
                }
            }
        }
    }
    printk("[OK] PCI Bus Scanned: Identified %d devices\n", pci_dev_count);
}

int pci_get_device_count(void)
{
    return pci_dev_count;
}

const struct pci_device *pci_get_device(int idx)
{
    if (idx < 0 || idx >= pci_dev_count) return NULL;
    return &pci_devices[idx];
}

int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, struct pci_device *out_dev)
{
    for (int i = 0; i < pci_dev_count; i++) {
        if (pci_devices[i].class_code == class_code &&
            pci_devices[i].subclass == subclass &&
            (prog_if == 0xFF || pci_devices[i].prog_if == prog_if)) {
            *out_dev = pci_devices[i];
            /* Включаем Bus Master и MMIO */
            uint16_t cmd = pci_read_word(out_dev->bus, out_dev->slot, out_dev->func, 0x04);
            pci_write_word(out_dev->bus, out_dev->slot, out_dev->func, 0x04, cmd | 0x0006);
            return 0;
        }
    }
    return -1;
}
