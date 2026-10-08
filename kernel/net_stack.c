#include <linux/ip.h>
#include <linux/e1000.h>
#include <linux/string.h>
#include <linux/tty.h>
#include <linux/time.h>
#include <linux/tcp.h>

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
    static char reply[1500];
    if (icmp->type == 8) { /* Echo Request (Ping от хоста) */
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
            } else if (ip->protocol == IP_PROTO_TCP) {
                tcp_handle_packet(ip, (const struct tcp_header *)payload, payload_len, eh->src);
            }
        }
    }
}

void net_poll(void)
{
    static char rx_buf[1518];
    int len;
    while ((len = e1000_recv(rx_buf, sizeof(rx_buf))) > 0) {
        net_stack_rx(rx_buf, (uint16_t)len);
    }
}

int ip_send(uint32_t dest_ip, uint8_t protocol, const void *payload, uint16_t len)
{
    static char frame[1518];
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
    static char buf[1500];
    struct udp_header *uh = (struct udp_header *)buf;
    uh->src_port = htons(src_port);
    uh->dest_port = htons(dest_port);
    uh->length = htons(sizeof(struct udp_header) + len);
    uh->checksum = 0;

    memcpy(buf + sizeof(struct udp_header), data, len);
    return ip_send(dest_ip, IP_PROTO_UDP, buf, sizeof(struct udp_header) + len);
}

struct tcp_socket tcp_sockets[MAX_TCP_SOCKETS];

/* Потоковое вычисление контрольной суммы TCP (RFC 793/1071) с 0 байт на стеке ядра */
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dest_ip, const void *tcp_seg, uint16_t len)
{
    uint32_t sum = 0;
    uint32_t s_ip = htonl(src_ip);
    uint32_t d_ip = htonl(dest_ip);

    sum += (s_ip >> 16) & 0xFFFF;
    sum += s_ip & 0xFFFF;
    sum += (d_ip >> 16) & 0xFFFF;
    sum += d_ip & 0xFFFF;
    sum += htons(IP_PROTO_TCP);
    sum += htons(len);

    const uint16_t *p = (const uint16_t *)tcp_seg;
    uint16_t l = len;
    while (l > 1) {
        sum += *p++;
        l -= 2;
    }
    if (l == 1) {
        sum += *(const uint8_t *)p;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

static int tcp_send_packet(struct tcp_socket *sock, uint8_t flags, const void *payload, uint16_t len)
{
    static char buf[1500];
    uint16_t tcp_len = sizeof(struct tcp_header) + len;
    if (tcp_len > sizeof(buf)) return -1;
    struct tcp_header *th = (struct tcp_header *)buf;

    th->src_port = htons(sock->local_port);
    th->dest_port = htons(sock->remote_port);
    th->seq_num = htonl(sock->my_seq);
    th->ack_num = htonl(sock->remote_seq);
    th->data_offset = (sizeof(struct tcp_header) / 4) << 4;
    th->flags = flags;
    th->window_size = htons(2048);
    th->checksum = 0;
    th->urgent_ptr = 0;

    if (payload && len > 0) {
        memcpy(buf + sizeof(struct tcp_header), payload, len);
    }

    th->checksum = tcp_checksum(MY_IP, sock->remote_ip, buf, tcp_len);
    if (len > 0) {
        sock->my_seq += len;
    }
    return ip_send(sock->remote_ip, IP_PROTO_TCP, buf, tcp_len);
}

void tcp_handle_packet(const struct ip_header *ip, const struct tcp_header *tcp, uint16_t len, const uint8_t src_mac[6])
{
    uint16_t dest_port = ntohs(tcp->dest_port);
    uint16_t src_port = ntohs(tcp->src_port);
    uint32_t seq = ntohl(tcp->seq_num);
    uint8_t flags = tcp->flags;

    uint8_t hlen = (tcp->data_offset >> 4) * 4;
    const char *payload = (const char *)tcp + hlen;
    uint16_t payload_len = (len > hlen) ? (len - hlen) : 0;

    /* 1. Обработка входящего SYN на слушающий порт */
    if (flags & TCP_FLAG_SYN) {
        for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
            if (tcp_sockets[i].in_use && tcp_sockets[i].state == TCP_STATE_LISTEN &&
                tcp_sockets[i].local_port == dest_port) {

                int client_id = -1;
                for (int c = 1; c < MAX_TCP_SOCKETS; c++) {
                    if (!tcp_sockets[c].in_use) { client_id = c; break; }
                }
                if (client_id < 0) return;

                struct tcp_socket *cs = &tcp_sockets[client_id];
                memset(cs, 0, sizeof(struct tcp_socket));
                cs->in_use = 1;
                cs->state = TCP_STATE_SYN_RCVD;
                cs->local_port = dest_port;
                cs->remote_port = src_port;
                cs->remote_ip = ntohl(ip->src_ip);
                memcpy(cs->remote_mac, src_mac, 6);
                cs->remote_seq = seq + 1;
                cs->my_seq = 0x20000000 + client_id * 1000;
                cs->rx_len = 0;
                cs->rx_pos = 0;
                cs->is_accepted = 0;

                /* Отправляем SYN + ACK */
                tcp_send_packet(cs, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
                cs->my_seq++;
                printk("[TCP] Handled SYN -> Sent SYN+ACK to %d.%d.%d.%d:%d!\n",
                       (cs->remote_ip >> 24) & 0xFF, (cs->remote_ip >> 16) & 0xFF,
                       (cs->remote_ip >> 8) & 0xFF, cs->remote_ip & 0xFF, cs->remote_port);
                return;
            }
        }
    }

    /* 2. Обработка данных и флагов на установленном соединении */
    for (int i = 1; i < MAX_TCP_SOCKETS; i++) {
        if (tcp_sockets[i].in_use &&
            (tcp_sockets[i].state == TCP_STATE_SYN_RCVD || tcp_sockets[i].state == TCP_STATE_ESTABLISHED) &&
            tcp_sockets[i].local_port == dest_port && tcp_sockets[i].remote_port == src_port) {

            struct tcp_socket *s = &tcp_sockets[i];
            if (s->state == TCP_STATE_SYN_RCVD) {
                s->state = TCP_STATE_ESTABLISHED;
            }


            if (payload_len > 0) {
                uint16_t space = sizeof(s->rx_buf) - s->rx_len;
                uint16_t copy_bytes = (payload_len < space) ? payload_len : space;
                memcpy(s->rx_buf + s->rx_len, payload, copy_bytes);
                s->rx_len += copy_bytes;
                s->remote_seq += payload_len;

                /* Отправляем ACK на полученные данные */
                tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
                printk("[TCP] Received %d bytes HTTP payload -> Sent ACK!\n", payload_len);
            }

            if (flags & TCP_FLAG_FIN) {
                s->remote_seq++;
                tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
                s->state = TCP_STATE_CLOSE_WAIT;
            }
            return;
        }
    }
}

/* Реализация функций Socket API для VFS */
int tcp_socket_create(void)
{
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (!tcp_sockets[i].in_use) {
            memset(&tcp_sockets[i], 0, sizeof(struct tcp_socket));
            tcp_sockets[i].in_use = 1;
            tcp_sockets[i].state = TCP_STATE_CLOSED;
            tcp_sockets[i].is_accepted = 0;
            return i;
        }
    }
    return -1;
}

int tcp_socket_bind(int sock_id, uint16_t port)
{
    if (sock_id < 0 || sock_id >= MAX_TCP_SOCKETS || !tcp_sockets[sock_id].in_use) return -1;
    tcp_sockets[sock_id].local_port = port;
    return 0;
}

int tcp_socket_listen(int sock_id)
{
    if (sock_id < 0 || sock_id >= MAX_TCP_SOCKETS || !tcp_sockets[sock_id].in_use) return -1;
    tcp_sockets[sock_id].state = TCP_STATE_LISTEN;
    return 0;
}

int tcp_socket_accept(int sock_id)
{
    if (sock_id < 0 || sock_id >= MAX_TCP_SOCKETS || !tcp_sockets[sock_id].in_use) return -1;
    uint16_t listen_port = tcp_sockets[sock_id].local_port;

    while (1) {
        net_poll();
        __asm__ volatile ("" ::: "memory");

        /* Отдаем соединение, когда есть входящие данные запроса или флаг ESTABLISHED */
        for (int i = 1; i < MAX_TCP_SOCKETS; i++) {
            if (tcp_sockets[i].in_use &&
                (tcp_sockets[i].state == TCP_STATE_ESTABLISHED || tcp_sockets[i].state == TCP_STATE_CLOSE_WAIT) &&
                tcp_sockets[i].rx_len > 0 && tcp_sockets[i].local_port == listen_port && !tcp_sockets[i].is_accepted) {
                tcp_sockets[i].is_accepted = 1;
                return i;
            }
        }

        for (volatile int k = 0; k < 1000; k++) {
            __asm__ volatile ("pause");
        }
    }
}

int tcp_socket_read(int sock_id, char *buf, uint64_t count)
{
    if (sock_id < 0 || sock_id >= MAX_TCP_SOCKETS || !tcp_sockets[sock_id].in_use) return -1;
    struct tcp_socket *s = &tcp_sockets[sock_id];

    /* Ожидаем прихода данных запроса */
    int timeout = 30000;
    while (s->rx_len == 0 && (s->state == TCP_STATE_ESTABLISHED || s->state == TCP_STATE_CLOSE_WAIT) && --timeout) {
        net_poll();
        __asm__ volatile ("" ::: "memory");
        for (volatile int k = 0; k < 1000; k++) {
            __asm__ volatile ("pause");
        }
    }

    if (s->rx_len == 0) return 0;

    uint64_t bytes = (count < s->rx_len) ? count : s->rx_len;
    memcpy(buf, s->rx_buf, bytes);

    if (bytes < s->rx_len) {
        memmove(s->rx_buf, s->rx_buf + bytes, s->rx_len - bytes);
        s->rx_len -= bytes;
    } else {
        s->rx_len = 0;
    }
    return (int)bytes;
}

int tcp_socket_write(int sock_id, const char *buf, uint64_t count)
{
    if (sock_id < 0 || sock_id >= MAX_TCP_SOCKETS || !tcp_sockets[sock_id].in_use) return -1;
    struct tcp_socket *s = &tcp_sockets[sock_id];
    return tcp_send_packet(s, TCP_FLAG_PSH | TCP_FLAG_ACK, buf, (uint16_t)count);
}

int tcp_socket_close(int sock_id)
{
    if (sock_id < 0 || sock_id >= MAX_TCP_SOCKETS || !tcp_sockets[sock_id].in_use) return -1;
    struct tcp_socket *s = &tcp_sockets[sock_id];
    if (s->state == TCP_STATE_ESTABLISHED || s->state == TCP_STATE_CLOSE_WAIT) {
        tcp_send_packet(s, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
        s->my_seq++;
    }
    for (volatile int k = 0; k < 200; k++) {
        net_poll();
        __asm__ volatile ("pause");
    }
    memset(s, 0, sizeof(struct tcp_socket));
    return 0;
}
