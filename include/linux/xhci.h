#ifndef _LINUX_XHCI_H
#define _LINUX_XHCI_H

#include <linux/types.h>

/* Смещения операционных регистров xHCI (Operational Registers) */
#define XHCI_REG_USBCMD    0x00
#define XHCI_REG_USBSTS    0x04
#define XHCI_REG_PAGESIZE  0x08
#define XHCI_REG_DNCTRL    0x14
#define XHCI_REG_CRCR_LOW  0x18
#define XHCI_REG_CRCR_HIGH 0x1C
#define XHCI_REG_DCBAAP_LO 0x30
#define XHCI_REG_DCBAAP_HI 0x34
#define XHCI_REG_CONFIG    0x38

/* Биты управления USBCMD / статуса USBSTS */
#define XHCI_CMD_RS     (1U << 0)  /* Run/Stop */
#define XHCI_CMD_HCRST  (1U << 1)  /* Host Controller Reset */
#define XHCI_STS_HCH    (1U << 0)  /* Host Controller Halted */
#define XHCI_STS_CNR    (1U << 11) /* Controller Not Ready */

/* Информация о контроллере и портах */
struct xhci_port_status {
    int port_num;
    int connected;
    int speed;      /* 1 = FullSpeed, 2 = LowSpeed, 3 = HighSpeed USB 2.0, 4 = SuperSpeed USB 3.0 */
    uint32_t raw_status;
};

struct xhci_info {
    int present;
    uint64_t mmio_base;
    uint8_t  cap_length;
    uint16_t hci_version;
    uint8_t  max_slots;
    uint8_t  max_ports;
    uint32_t operational_offset;
    uint32_t doorbell_offset;
    uint32_t runtime_offset;
    int active_ports_count;
    struct xhci_port_status ports[16];
};

int  xhci_init(void);
const struct xhci_info *xhci_get_info(void);

#endif
