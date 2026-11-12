/*
 * Сетевой стек: ARP (с кэшем), IPv4, ICMP echo, UDP, DHCP-клиент, loopback;
 * TCP/UDP/ICMP-сокеты живут в kernel/sock.c.
 * Адрес берётся по DHCP; если сервер не ответил -- запасная статика QEMU
 * (10.0.2.15/24, шлюз 10.0.2.2). Кадры собираются в стеке ядра (16 КБ), поэтому
 * вызовы из IRQ таймера и из системных вызовов не делят статических буферов.
 */
#include <linux/ip.h>
#include <linux/nic.h>
#include <linux/string.h>
#include <linux/tty.h>
#include <linux/time.h>
#include <linux/tcp.h>
#include <linux/sock.h>

extern volatile uint64_t jiffies;

static uint8_t  my_mac[6];
uint32_t net_my_ip = 0, net_mask = 0, net_gw = 0, net_dns = 0;
#define my_ip net_my_ip
static int net_dhcp_bound = 0;     /* 1 = адрес выдан DHCP, 2 = запасной статический */

/* ---- контрольная сумма ---- */
uint16_t net_checksum(const void *buf, uint32_t len)
{
    const uint16_t *p = (const uint16_t *)buf;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len == 1) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* ---- ARP-кэш ---- */
#define ARP_CACHE 8
static struct { uint32_t ip; uint8_t mac[6]; uint8_t valid; } arp_cache[ARP_CACHE];
static int arp_next = 0;
static volatile int rx_busy = 0;        /* 1 -- идёт разбор принятого кадра */
static const uint8_t bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static void arp_learn(uint32_t ip, const uint8_t *mac)
{
    if (!ip) return;
    for (int i = 0; i < ARP_CACHE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    int i = arp_next;
    arp_next = (arp_next + 1) % ARP_CACHE;
    arp_cache[i].ip = ip;
    memcpy(arp_cache[i].mac, mac, 6);
    arp_cache[i].valid = 1;
}

static int arp_lookup(uint32_t ip, uint8_t *mac)
{
    for (int i = 0; i < ARP_CACHE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(mac, arp_cache[i].mac, 6);
            return 1;
        }
    }
    return 0;
}

static void send_arp(uint16_t op, const uint8_t *dst_mac, const uint8_t *tgt_mac, uint32_t tgt_ip)
{
    uint8_t frame[64];
    memset(frame, 0, sizeof(frame));
    struct eth_header *eh = (struct eth_header *)frame;
    struct arp_header *a = (struct arp_header *)(frame + sizeof(struct eth_header));
    memcpy(eh->dest, dst_mac, 6);
    memcpy(eh->src, my_mac, 6);
    eh->proto = htons(ETH_P_ARP);
    a->hw_type = htons(1);
    a->proto_type = htons(ETH_P_IP);
    a->hw_size = 6;
    a->proto_size = 4;
    a->opcode = htons(op);
    memcpy(a->sender_mac, my_mac, 6);
    a->sender_ip = htonl(my_ip);
    memcpy(a->target_mac, tgt_mac, 6);
    a->target_ip = htonl(tgt_ip);
    nic_send(frame, 60);
}

static void handle_arp(const struct arp_header *arp)
{
    uint32_t sip = ntohl(arp->sender_ip);
    uint32_t tip = ntohl(arp->target_ip);
    uint16_t op = ntohs(arp->opcode);

    if (my_ip && (net_mask == 0 || ((sip & net_mask) == (my_ip & net_mask))))
        arp_learn(sip, arp->sender_mac);

    if (op == 1 && my_ip && tip == my_ip)
        send_arp(2, arp->sender_mac, arp->sender_mac, sip);
}

/* MAC следующего перехода. wait=1 -- при промахе послать ARP и ждать ответ. */
static int next_hop_mac(uint32_t dst, uint8_t *mac, int wait)
{
    if (dst == 0xFFFFFFFFU || (net_mask && dst == ((my_ip & net_mask) | ~net_mask))) {
        memcpy(mac, bcast_mac, 6);
        return 0;
    }
    uint32_t hop = dst;
    if (net_mask && (dst & net_mask) != (my_ip & net_mask)) {
        if (!net_gw) return -1;
        hop = net_gw;
    }
    if (arp_lookup(hop, mac)) return 0;

    static const uint8_t zero[6] = { 0 };
    send_arp(1, bcast_mac, zero, hop);
    if (!wait) return -1;

    uint64_t t0 = time_us();
    uint64_t last = t0;
    while (time_us() - t0 < 1500000) {
        net_poll();
        if (arp_lookup(hop, mac)) return 0;
        if (time_us() - last > 300000) { send_arp(1, bcast_mac, zero, hop); last = time_us(); }
        udelay(200);
    }
    return -1;
}

/* ---- IPv4 ---- */
static int ip_send_frame(const uint8_t *dst_mac, uint32_t src_ip, uint32_t dest_ip,
                         uint8_t protocol, const void *payload, uint16_t len)
{
    uint8_t frame[1518];
    uint16_t total_ip = sizeof(struct ip_header) + len;
    uint16_t total_frame = sizeof(struct eth_header) + total_ip;
    if (total_frame > sizeof(frame)) return -1;

    struct eth_header *eh = (struct eth_header *)frame;
    struct ip_header *ip = (struct ip_header *)(frame + sizeof(struct eth_header));

    memcpy(eh->dest, dst_mac, 6);
    memcpy(eh->src, my_mac, 6);
    eh->proto = htons(ETH_P_IP);

    static uint16_t ip_id = 1;
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons(total_ip);
    uint16_t cur_id = ip_id++;
    ip->id = htons(cur_id);
    ip->flags_frag = htons(0x4000);
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->checksum = 0;
    ip->src_ip = htonl(src_ip);
    ip->dest_ip = htonl(dest_ip);
    ip->checksum = net_checksum(ip, sizeof(struct ip_header));
    memcpy(frame + sizeof(struct eth_header) + sizeof(struct ip_header), payload, len);
    return nic_send(frame, total_frame);
}

/* Адрес источника для пакета к dst */
uint32_t net_src_for(uint32_t dst)
{
    return (dst >> 24) == 127 ? 0x7F000001U : my_ip;
}

/* Заранее выяснить MAC следующего перехода (с ожиданием), чтобы потом слать без задержки */
void net_arp_prepare(uint32_t dst)
{
    uint8_t mac[6];
    if (!nic_present() || !my_ip || (dst >> 24) == 127 || dst == my_ip) return;
    next_hop_mac(dst, mac, 1);
}

/* Очередь loopback: пакеты к самому себе разбирает net_poll, без рекурсии в сокетах */
#define LO_N 32
static struct { uint8_t proto; uint16_t len; uint32_t src, dst; uint8_t data[1500]; } lo_q[LO_N];
static int lo_head = 0, lo_n = 0;

static void ip_deliver(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t *payload,
                       uint16_t len, const struct ip_header *iph);

int ip_send_ex(uint32_t dest_ip, uint8_t protocol, const void *payload, uint16_t len, int wait)
{
    if ((dest_ip >> 24) == 127 || (my_ip && dest_ip == my_ip)) {
        if (len > sizeof(lo_q[0].data) || lo_n >= LO_N) return -1;
        int k = (lo_head + lo_n++) % LO_N;
        lo_q[k].proto = protocol; lo_q[k].len = len;
        lo_q[k].src = dest_ip; lo_q[k].dst = dest_ip;
        memcpy(lo_q[k].data, payload, len);
        return 0;
    }
    if (!nic_present() || !my_ip) return -1;
    uint8_t mac[6];
    if (next_hop_mac(dest_ip, mac, wait) < 0) return -1;
    return ip_send_frame(mac, my_ip, dest_ip, protocol, payload, len);
}

int ip_send(uint32_t dest_ip, uint8_t protocol, const void *payload, uint16_t len)
{
    return ip_send_ex(dest_ip, protocol, payload, len, !rx_busy);
}

/* ---- ICMP ---- */
static void handle_icmp(const struct ip_header *ip, const struct icmp_header *icmp, uint16_t len)
{
    if (len < sizeof(struct icmp_header)) return;
    sock_icmp_rx(ntohl(ip->src_ip), ntohl(ip->dest_ip), (const uint8_t *)icmp, len, (const uint8_t *)ip);
    if (icmp->type == 8) {                          /* Echo Request */
        uint8_t reply[1500];
        uint16_t n = len > sizeof(reply) ? (uint16_t)sizeof(reply) : len;
        memcpy(reply, icmp, n);
        struct icmp_header *rep = (struct icmp_header *)reply;
        rep->type = 0;
        rep->code = 0;
        rep->checksum = 0;
        rep->checksum = net_checksum(rep, n);
        ip_send(ntohl(ip->src_ip), IP_PROTO_ICMP, reply, n);
    }
}

/* ---- UDP + DHCP ---- */
int udp_send(uint32_t dest_ip, uint16_t src_port, uint16_t dest_port, const void *data, uint16_t len)
{
    uint8_t buf[1480];
    if (len + sizeof(struct udp_header) > sizeof(buf)) return -1;
    struct udp_header *uh = (struct udp_header *)buf;
    uh->src_port = htons(src_port);
    uh->dest_port = htons(dest_port);
    uh->length = htons(sizeof(struct udp_header) + len);
    uh->checksum = 0;
    memcpy(buf + sizeof(struct udp_header), data, len);
    return ip_send(dest_ip, IP_PROTO_UDP, buf, sizeof(struct udp_header) + len);
}

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

struct dhcp_msg {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  options[308];
} __attribute__((packed));

static volatile int dhcp_state = 0;     /* 0 нет, 1 DISCOVER, 2 REQUEST, 3 BOUND */
static uint32_t dhcp_xid;
static uint32_t dhcp_offer_ip, dhcp_server_ip;

static int dhcp_send(uint8_t type, uint32_t req_ip, uint32_t server_ip)
{
    struct dhcp_msg m;
    memset(&m, 0, sizeof(m));
    m.op = 1; m.htype = 1; m.hlen = 6;
    m.xid = htonl(dhcp_xid);
    m.flags = htons(0x8000);                        /* ответ широковещательно */
    memcpy(m.chaddr, my_mac, 6);
    m.magic = htonl(0x63825363);
    uint8_t *o = m.options;
    *o++ = 53; *o++ = 1; *o++ = type;
    if (type == DHCP_REQUEST) {
        *o++ = 50; *o++ = 4; uint32_t r = htonl(req_ip); memcpy(o, &r, 4); o += 4;
        *o++ = 54; *o++ = 4; uint32_t s = htonl(server_ip); memcpy(o, &s, 4); o += 4;
    }
    *o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51;   /* mask, router, dns, lease */
    *o++ = 255;

    uint16_t ulen = (uint16_t)(sizeof(struct udp_header) + sizeof(m));
    uint8_t buf[sizeof(struct udp_header) + sizeof(struct dhcp_msg)];
    struct udp_header *uh = (struct udp_header *)buf;
    uh->src_port = htons(68);
    uh->dest_port = htons(67);
    uh->length = htons(ulen);
    uh->checksum = 0;
    memcpy(buf + sizeof(*uh), &m, sizeof(m));
    return ip_send_frame(bcast_mac, 0, 0xFFFFFFFFU, IP_PROTO_UDP, buf, ulen);
}

static void dhcp_handle(const uint8_t *pkt, uint16_t len)
{
    if (len < 240 || dhcp_state == 0 || dhcp_state == 3) return;
    const struct dhcp_msg *m = (const struct dhcp_msg *)pkt;
    if (m->op != 2 || ntohl(m->xid) != dhcp_xid || ntohl(m->magic) != 0x63825363) return;
    if (memcmp(m->chaddr, my_mac, 6) != 0) return;

    uint8_t type = 0;
    uint32_t mask = 0, gw = 0, dns = 0, sid = 0;
    const uint8_t *o = m->options, *end = pkt + len;
    while (o < end && *o != 255) {
        if (*o == 0) { o++; continue; }
        if (o + 1 >= end) break;
        uint8_t code = o[0], l = o[1];
        if (o + 2 + l > end) break;
        const uint8_t *v = o + 2;
        uint32_t w = 0;
        if (l >= 4) w = ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) | ((uint32_t)v[2] << 8) | v[3];
        if (code == 53 && l >= 1) type = v[0];
        else if (code == 1) mask = w;
        else if (code == 3) gw = w;
        else if (code == 6) dns = w;
        else if (code == 54) sid = w;
        o += 2 + l;
    }

    if (type == DHCP_OFFER && dhcp_state == 1) {
        dhcp_offer_ip = ntohl(m->yiaddr);
        dhcp_server_ip = sid ? sid : ntohl(m->siaddr);
        dhcp_state = 2;
        dhcp_send(DHCP_REQUEST, dhcp_offer_ip, dhcp_server_ip);
    } else if (type == DHCP_ACK && dhcp_state == 2) {
        my_ip = ntohl(m->yiaddr);
        net_mask = mask ? mask : 0xFFFFFF00U;
        net_gw = gw;
        net_dns = dns;
        net_dhcp_bound = 1;
        dhcp_state = 3;
    }
}

static void handle_udp(uint32_t src, uint32_t dst, const uint8_t *udp, uint16_t len)
{
    if (len < sizeof(struct udp_header)) return;
    const struct udp_header *uh = (const struct udp_header *)udp;
    uint16_t dport = ntohs(uh->dest_port);
    if (dport == 68) dhcp_handle(udp + sizeof(*uh), (uint16_t)(len - sizeof(*uh)));
    sock_udp_rx(src, dst, ntohs(uh->src_port), dport, udp + sizeof(*uh), (uint16_t)(len - sizeof(*uh)));
}

/* Возврат: 1 -- получили адрес по DHCP */
static int dhcp_run(void)
{
    for (int attempt = 0; attempt < 4; attempt++) {
        dhcp_xid = (uint32_t)time_us() ^ 0x5A17C0DE ^ ((uint32_t)attempt << 20);
        dhcp_state = 1;
        net_my_ip = 0; net_mask = 0; net_gw = 0;
        dhcp_send(DHCP_DISCOVER, 0, 0);
        uint64_t until = jiffies + 300 * (uint64_t)(attempt + 1) / 3;   /* 1,2,3,4 с */
        while (jiffies < until && dhcp_state != 3) __asm__ volatile ("hlt");
        if (dhcp_state == 3) return 1;
    }
    dhcp_state = 0;
    return 0;
}

static int netd_thread(void *arg)
{
    (void)arg;
    /* Ждём линк (автосогласование 1-5 с), не дольше 8 с */
    uint64_t until = jiffies + 800;
    uint32_t sp = 0; int full = 0;
    while (!nic_link(&sp, &full) && jiffies < until) __asm__ volatile ("hlt");
    if (nic_link(&sp, &full))
        printk("[NET] Link UP: %d Mbps %s duplex\n", (int)sp, full ? "full" : "half");
    else
        printk("[NET] Link is down (no cable?) -- trying DHCP anyway\n");

    if (dhcp_run()) {
        printk("[NET] DHCP: IP %d.%d.%d.%d mask %d.%d.%d.%d gw %d.%d.%d.%d dns %d.%d.%d.%d\n",
               (my_ip >> 24) & 255, (my_ip >> 16) & 255, (my_ip >> 8) & 255, my_ip & 255,
               (net_mask >> 24) & 255, (net_mask >> 16) & 255, (net_mask >> 8) & 255, net_mask & 255,
               (net_gw >> 24) & 255, (net_gw >> 16) & 255, (net_gw >> 8) & 255, net_gw & 255,
               (net_dns >> 24) & 255, (net_dns >> 16) & 255, (net_dns >> 8) & 255, net_dns & 255);
    } else {
        my_ip = MAKE_IP(10, 0, 2, 15);
        net_mask = MAKE_IP(255, 255, 255, 0);
        net_gw = MAKE_IP(10, 0, 2, 2);
        net_dns = MAKE_IP(10, 0, 2, 3);
        net_dhcp_bound = 2;
        printk("[NET] DHCP: no answer, using fallback 10.0.2.15/24 gw 10.0.2.2\n");
    }
    /* Узнать MAC шлюза заранее */
    if (net_gw) { uint8_t m[6]; next_hop_mac(net_gw, m, 1); }
    return 0;
}

int net_dhcp_renew(void)
{
    if (!nic_present()) return -1;
    return dhcp_run() ? 0 : -1;
}

int net_get_info(struct net_info *ni)
{
    memset(ni, 0, sizeof(*ni));
    ni->present = nic_present();
    if (!ni->present) return -1;
    strncpy(ni->name, nic_name(), sizeof(ni->name) - 1);
    nic_get_mac(ni->mac);
    uint32_t sp = 0; int full = 0;
    ni->link = nic_link(&sp, &full);
    ni->speed = sp;
    ni->full = full;
    ni->dhcp = net_dhcp_bound;
    ni->ip = my_ip; ni->mask = net_mask; ni->gw = net_gw; ni->dns = net_dns;
    return 0;
}

void net_stack_init(void)
{
    nic_get_mac(my_mac);
    extern int kthread_create(int (*fn)(void *), void *arg, const char *name);
    kthread_create(netd_thread, NULL, "netd");
}

/* ---- приём ---- */
void net_stack_rx(const void *frame, uint16_t len)
{
    if (len < sizeof(struct eth_header)) return;

    const struct eth_header *eh = (const struct eth_header *)frame;
    uint16_t proto = ntohs(eh->proto);

    if (proto == ETH_P_ARP) {
        if (len >= sizeof(struct eth_header) + sizeof(struct arp_header))
            handle_arp((const struct arp_header *)((const char *)frame + sizeof(struct eth_header)));
    } else if (proto == ETH_P_IP) {
        if (len < sizeof(struct eth_header) + sizeof(struct ip_header)) return;
        const struct ip_header *ip = (const struct ip_header *)((const char *)frame + sizeof(struct eth_header));
        uint8_t ihl = (ip->ver_ihl & 0x0F) * 4;
        uint16_t tot = ntohs(ip->total_len);
        if ((ip->ver_ihl >> 4) != 4 || ihl < 20 || tot < ihl ||
            tot > len - sizeof(struct eth_header)) return;
        uint32_t dst = ntohl(ip->dest_ip), src = ntohl(ip->src_ip);
        int for_me = (dst == my_ip) || dst == 0xFFFFFFFFU ||
                     (net_mask && dst == ((my_ip & net_mask) | ~net_mask));
        if (!for_me && !(my_ip == 0 && ip->protocol == IP_PROTO_UDP)) return;

        if (my_ip && net_mask && (src & net_mask) == (my_ip & net_mask) && (eh->src[0] & 1) == 0)
            arp_learn(src, eh->src);

        const uint8_t *payload = (const uint8_t *)ip + ihl;
        uint16_t payload_len = (uint16_t)(tot - ihl);
        if (ip->protocol == IP_PROTO_ICMP)
            handle_icmp(ip, (const struct icmp_header *)payload, payload_len);
        else if (ip->protocol == IP_PROTO_UDP)
            handle_udp(src, dst, payload, payload_len);
        else if (ip->protocol == IP_PROTO_TCP && dst == my_ip)
            sock_tcp_rx(src, dst, payload, payload_len);
    }
}

static void ip_deliver(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t *payload,
                       uint16_t len, const struct ip_header *iph)
{
    (void)iph;
    if (proto == IP_PROTO_TCP) sock_tcp_rx(src, dst, payload, len);
    else if (proto == IP_PROTO_UDP) handle_udp(src, dst, payload, len);
    else if (proto == IP_PROTO_ICMP && len >= 8) {
        uint8_t hdr[20] = { 0x45 };
        hdr[8] = 64; hdr[9] = IP_PROTO_ICMP;
        uint32_t s_ = htonl(src), d_ = htonl(dst);
        memcpy(hdr + 12, &s_, 4); memcpy(hdr + 16, &d_, 4);
        if (payload[0] == 8) {                       /* эхо к самому себе */
            uint8_t r[1500];
            memcpy(r, payload, len);
            r[0] = 0; r[2] = r[3] = 0;
            uint16_t c = net_checksum(r, len);
            memcpy(r + 2, &c, 2);
            sock_icmp_rx(src, dst, r, len, hdr);
        }
    }
}

void net_poll(void)
{
    static uint8_t rx_buf[1518];
    static volatile int busy = 0;
    if (!nic_present()) return;
    unsigned long fl = irq_save();
    if (__sync_lock_test_and_set(&busy, 1)) { irq_restore(fl); return; }   /* уже кто-то разбирает приём */
    int len, n = 0;
    rx_busy = 1;
    while (n++ < 64 && (len = nic_recv(rx_buf, sizeof(rx_buf))) > 0)
        net_stack_rx(rx_buf, (uint16_t)len);
    while (lo_n > 0) {
        int k = lo_head;
        lo_head = (lo_head + 1) % LO_N; lo_n--;
        ip_deliver(lo_q[k].src, lo_q[k].dst, lo_q[k].proto, lo_q[k].data, lo_q[k].len, NULL);
    }
    sock_timer();
    rx_busy = 0;
    __sync_lock_release(&busy);
    irq_restore(fl);
}
