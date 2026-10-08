#include <linux/ip.h>
#include <linux/e1000.h>
#include <linux/string.h>
#include <linux/tty.h>
#include <linux/time.h>

#define MY_IP      MAKE_IP(10, 0, 2, 15)
#define GATEWAY_IP MAKE_IP(10, 0, 2, 2)

static uint8_t my_mac[6];
static uint8_t gateway_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x02 };

uint16_t net_checksum(const void *buf, uint32_t len)
{
    const uint16_t *p = (const uint16_t *)buf;
    uint32_t sum = 0;
    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len == 1) {
        sum += *(const uint8_t *)p;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

void net_stack_init(void)
{
    e1000_get_mac(my_mac);
    printk("[OK] Network Stack Initialized: IP = 10.0.2.15, Mask = 255.255.255.0, GW = 10.0.2.2\n");
}

static void handle_arp(const struct arp_header *arp)
{
    uint32_t my_ip_net = htonl(MY_IP);
    if (ntohs(arp->opcode) == 1 && memcmp(&arp->target_ip, &my_ip_net, 4) == 0) {
        char frame[64];
        memset(frame, 0, sizeof(frame));

        struct eth_header *eh = (struct eth_header *)frame;
        struct arp_header *rep = (struct arp_header *)(frame + sizeof(struct eth_header));

        memcpy(eh->dest, arp->sender_mac, 6);
        memcpy(eh->src, my_mac, 6);
        eh->proto = htons(ETH_P_ARP);

        rep->hw_type = htons(1);
        rep->proto_type = htons(ETH_P_IP);
        rep->hw_size = 6;
        rep->proto_size = 4;
        rep->opcode = htons(2); /* ARP Reply */

        memcpy(rep->sender_mac, my_mac, 6);
        memcpy(&rep->sender_ip, &my_ip_net, 4);
        memcpy(rep->target_mac, arp->sender_mac, 6);
        memcpy(&rep->target_ip, &arp->sender_ip, 4);

        /* Запоминаем MAC хоста для последующих IP/ICMP ответов */
        memcpy(gateway_mac, arp->sender_mac, 6);

        e1000_send(frame, 60); /* Выравнивание до 60 байт */
        printk("[NET] Received ARP Request -> Sent ARP Reply to host!\n");
    }
}

static void handle_icmp(const struct ip_header *ip, const struct icmp_header *icmp, uint16_t len)
{
    if (icmp->type == 8) { /* Echo Request (Ping от хоста) */
        char reply[1500];
        uint16_t icmp_len = len;
        if (icmp_len > sizeof(reply)) icmp_len = sizeof(reply);

        memcpy(reply, icmp, icmp_len);
        struct icmp_header *rep = (struct icmp_header *)reply;
        rep->type = 0; /* Echo Reply */
        rep->code = 0;
        rep->checksum = 0;
        rep->checksum = net_checksum(rep, icmp_len);

        ip_send(ntohl(ip->src_ip), IP_PROTO_ICMP, reply, icmp_len);
        printk("[NET] Received Ping from host -> Sent ICMP Echo Reply!\n");
    } else if (icmp->type == 0) {
        printk("[PING] Reply received from %d.%d.%d.%d: seq=%d\n",
               (ntohl(ip->src_ip) >> 24) & 0xFF, (ntohl(ip->src_ip) >> 16) & 0xFF,
               (ntohl(ip->src_ip) >> 8)  & 0xFF, (ntohl(ip->src_ip) >> 0)  & 0xFF,
               ntohs(icmp->seq));
    }
}

void net_stack_rx(const void *frame, uint16_t len)
{
    if (len < sizeof(struct eth_header)) return;

    const struct eth_header *eh = (const struct eth_header *)frame;
    uint16_t proto = ntohs(eh->proto);

    /* Запоминаем MAC-адрес хоста из любого входящего пакета */
    if ((eh->src[0] & 1) == 0) {
        memcpy(gateway_mac, eh->src, 6);
    }

    if (proto == ETH_P_ARP) {
        if (len >= sizeof(struct eth_header) + sizeof(struct arp_header)) {
            handle_arp((const struct arp_header *)((const char *)frame + sizeof(struct eth_header)));
        }
    } else if (proto == ETH_P_IP) {
        if (len >= sizeof(struct eth_header) + sizeof(struct ip_header)) {
            const struct ip_header *ip = (const struct ip_header *)((const char *)frame + sizeof(struct eth_header));
            uint8_t ihl = (ip->ver_ihl & 0x0F) * 4;
            const void *payload = (const char *)ip + ihl;
            uint16_t payload_len = ntohs(ip->total_len) - ihl;

            if (ip->protocol == IP_PROTO_ICMP) {
                handle_icmp(ip, (const struct icmp_header *)payload, payload_len);
            }
        }
    }
}

void net_poll(void)
{
    char buf[1518];
    int len;
    while ((len = e1000_recv(buf, sizeof(buf))) > 0) {
        net_stack_rx(buf, (uint16_t)len);
    }
}

int ip_send(uint32_t dest_ip, uint8_t protocol, const void *payload, uint16_t len)
{
    char frame[1518];
    uint16_t total_ip = sizeof(struct ip_header) + len;
    uint16_t total_frame = sizeof(struct eth_header) + total_ip;
    if (total_frame > sizeof(frame)) return -1;

    struct eth_header *eh = (struct eth_header *)frame;
    struct ip_header *ip = (struct ip_header *)(frame + sizeof(struct eth_header));

    memcpy(eh->dest, gateway_mac, 6);
    memcpy(eh->src, my_mac, 6);
    eh->proto = htons(ETH_P_IP);

    ip->ver_ihl = 0x45; /* IPv4, 20 байт заголовок */
    ip->tos = 0;
    ip->total_len = htons(total_ip);
    static uint16_t ip_id = 1;
    uint16_t cur_id = ip_id++;
    ip->id = htons(cur_id);
    ip->flags_frag = htons(0x4000); /* Don't Fragment */
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->checksum = 0;
    ip->src_ip = htonl(MY_IP);
    ip->dest_ip = htonl(dest_ip);
    ip->checksum = net_checksum(ip, sizeof(struct ip_header));

    memcpy(frame + sizeof(struct eth_header) + sizeof(struct ip_header), payload, len);
    if (total_frame < 60) total_frame = 60;

    return e1000_send(frame, total_frame);
}

int icmp_ping(uint32_t target_ip, uint16_t seq)
{
    struct {
        struct icmp_header icmp;
        char data[32];
    } packet;

    packet.icmp.type = 8; /* Echo Request */
    packet.icmp.code = 0;
    packet.icmp.id = htons(0x1337);
    packet.icmp.seq = htons(seq);
    memset(packet.data, 'A', sizeof(packet.data));
    packet.icmp.checksum = 0;
    packet.icmp.checksum = net_checksum(&packet, sizeof(packet));

    return ip_send(target_ip, IP_PROTO_ICMP, &packet, sizeof(packet));
}

int udp_send(uint32_t dest_ip, uint16_t src_port, uint16_t dest_port, const void *data, uint16_t len)
{
    char buf[1500];
    struct udp_header *uh = (struct udp_header *)buf;
    uh->src_port = htons(src_port);
    uh->dest_port = htons(dest_port);
    uh->length = htons(sizeof(struct udp_header) + len);
    uh->checksum = 0;

    memcpy(buf + sizeof(struct udp_header), data, len);
    return ip_send(dest_ip, IP_PROTO_UDP, buf, sizeof(struct udp_header) + len);
}
