#ifndef _LINUX_NET_H
#define _LINUX_NET_H

#include <linux/types.h>

#define ETH_ALEN       6
#define ETH_P_IP       0x0800
#define ETH_P_ARP      0x0806

struct eth_header {
    uint8_t  dest[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t proto;
} __attribute__((packed));

void net_init(void);
int  net_send_packet(const uint8_t dest_mac[6], uint16_t ethertype, const void *payload, uint16_t len);
int  net_recv_packet(void *buf, uint16_t max_len);

#endif
