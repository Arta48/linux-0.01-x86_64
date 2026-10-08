#ifndef _LINUX_IP_H
#define _LINUX_IP_H

#include <linux/types.h>
#include <linux/net.h>

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

#define htons(x) ((((uint16_t)(x) & 0xFF) << 8) | (((uint16_t)(x) >> 8) & 0xFF))
#define ntohs(x) htons(x)
#define htonl(x) ((((uint32_t)(x) & 0x000000FF) << 24) | \
(((uint32_t)(x) & 0x0000FF00) << 8)  | \
(((uint32_t)(x) & 0x00FF0000) >> 8)  | \
(((uint32_t)(x) & 0xFF000000) >> 24))
#define ntohl(x) htonl(x)

#define MAKE_IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
((uint32_t)(c) << 8)  | ((uint32_t)(d)))

struct arp_header {
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t  hw_size;
    uint8_t  proto_size;
    uint16_t opcode;
    uint8_t  sender_mac[6];
    uint32_t sender_ip;
    uint8_t  target_mac[6];
    uint32_t target_ip;
} __attribute__((packed));

struct ip_header {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dest_ip;
} __attribute__((packed));

struct icmp_header {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

struct udp_header {
    uint16_t src_port;
    uint16_t dest_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

uint16_t net_checksum(const void *buf, uint32_t len);

void net_stack_init(void);
void net_poll(void);
void net_stack_rx(const void *frame, uint16_t len);
int  ip_send(uint32_t dest_ip, uint8_t protocol, const void *payload, uint16_t len);
int  icmp_ping(uint32_t target_ip, uint16_t seq);
int  udp_send(uint32_t dest_ip, uint16_t src_port, uint16_t dest_port, const void *data, uint16_t len);

#endif
