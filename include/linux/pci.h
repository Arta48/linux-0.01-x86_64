#ifndef _LINUX_PCI_H
#define _LINUX_PCI_H

#include <linux/types.h>

#define PCI_CONFIG_ADDRESS 0x0CF8
#define PCI_CONFIG_DATA    0x0CFC

#define PCI_VENDOR_INTEL   0x8086
#define PCI_DEVICE_E1000    0x100E

struct pci_device {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint64_t bar0;
    uint8_t  irq;
};

uint32_t pci_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_write_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);
uint16_t pci_read_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_write_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val);
int      pci_find_device(uint16_t vendor_id, uint16_t device_id, struct pci_device *out_dev);

#endif
