#ifndef _LINUX_E1000_H
#define _LINUX_E1000_H

#include <linux/types.h>

#define E1000_REG_CTRL    0x0000
#define E1000_REG_STATUS  0x0008
#define E1000_REG_EECD    0x0010
#define E1000_REG_ICR     0x00C0
#define E1000_REG_IMS     0x00D0
#define E1000_REG_RCTL    0x0100
#define E1000_REG_RDBAL   0x2800
#define E1000_REG_RDBAH   0x2804
#define E1000_REG_RDLEN   0x2808
#define E1000_REG_RDH     0x2810
#define E1000_REG_RDT     0x2818
#define E1000_REG_TCTL    0x0400
#define E1000_REG_TDBAL   0x3800
#define E1000_REG_TDBAH   0x3804
#define E1000_REG_TDLEN   0x3808
#define E1000_REG_TDH     0x3810
#define E1000_REG_TDT     0x3818
#define E1000_REG_RAL     0x5400
#define E1000_REG_RAH     0x5404

#define E1000_NUM_RX_DESC 32
#define E1000_NUM_TX_DESC 16
#define E1000_BUFFER_SIZE 2048

struct e1000_rx_desc {
    uint64_t buffer_addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct e1000_tx_desc {
    uint64_t buffer_addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

int  e1000_init(void);
void e1000_get_mac(uint8_t out_mac[6]);
int  e1000_send(const void *data, uint16_t len);
int  e1000_recv(void *buf, uint16_t max_len);

#endif
