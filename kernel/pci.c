#include <linux/pci.h>
#include <linux/tty.h>
#include <asm/io.h>

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
