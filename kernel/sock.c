/*
 * Сокеты: TCP (клиент и сервер, скользящее окно, повторная передача, быстрая
 * повторная передача по 3 дубликатам ACK, простое управление перегрузкой), UDP,
 * ICMP (raw и "ping"-сокеты) и пары AF_UNIX.
 *
 * Приём ведёт net_poll() (из IRQ таймера и из ожидающих системных вызовов), поэтому
 * все изменения состояния сокетов делаются при выключенных прерываниях (irq_save);
 * в IRQ они и так выключены. Кадры уходят без ожидания ARP (ip_send_ex wait=0):
 * потерянный сегмент перешлёт таймер повторной передачи.
 */
#include <linux/sock.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/nic.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/tty.h>

#define E_INTR       4
#define E_NOMEM      12
#define E_AGAIN      11
#define E_INVAL      22
#define E_PIPE       32
#define E_NOTSOCK    88
#define E_DESTADDR   89
#define E_MSGSIZE    90
#define E_PROTONO    93
#define E_OPNOTSUPP  95
#define E_AFNO       97
#define E_ADDRINUSE  98
#define E_ADDRNOTAV  99
#define E_NETUNREACH 101
#define E_CONNRESET  104
#define E_NOBUFS     105
#define E_ISCONN     106
#define E_NOTCONN    107
#define E_TIMEDOUT   110
#define E_CONNREFUSED 111
#define E_ALREADY    114
#define E_INPROGRESS 115
#define E_NOENT      2

extern uint32_t net_my_ip;
extern uint32_t net_src_for(uint32_t dst);
extern void net_arp_prepare(uint32_t dst);
extern int ip_send_ex(uint32_t dest_ip, uint8_t protocol, const void *payload, uint16_t len, int wait);
extern volatile uint64_t jiffies;

#define NSOCK     64
#define TCP_BUF   65536
#define TCP_MSS   1460
#define DGQ       8
#define ACCQ      16

enum { K_FREE = 0, K_UDP, K_RAW, K_TCP, K_UNIX, K_EVENT };
enum { T_CLOSED = 0, T_LISTEN, T_SYN_SENT, T_SYN_RCVD, T_ESTAB, T_FIN_WAIT1, T_FIN_WAIT2,
       T_CLOSE_WAIT, T_CLOSING, T_LAST_ACK, T_TIME_WAIT };

struct dgram {
    uint32_t ip;
    uint16_t port, len;
    uint8_t  data[1500];
};

struct ksock {
    uint8_t  kind;
    uint8_t  used;
    int      refs;
    int      type, proto, domain;
    uint32_t lip, rip;
    uint16_t lport, rport;
    int      nonblock, err;
    int      shut_rd, shut_wr;
    int      rcvtimeo_ms, sndtimeo_ms;
    int      connected;
    int      orphan;

    /* TCP */
    int      st;
    uint32_t iss, snd_una, snd_nxt, snd_wnd, rcv_nxt, irs, mss, cwnd, ssthresh;
    uint8_t *rbuf, *sbuf;
    uint32_t r_head, r_len, s_head, s_len;
    int      fin_queued, fin_sent, fin_acked, fin_rcvd;
    uint64_t rto_deadline;                  /* мкс, 0 = таймер не взведён */
    uint32_t rto_ms, srtt_us, rttvar_us;
    int      retries, dupacks;
    int      rtt_timing; uint32_t rtt_seq; uint64_t rtt_t0;
    uint64_t tw_until;
    int      backlog, parent;
    int      accq[ACCQ], accn;

    /* UDP / RAW */
    struct dgram *dq;
    int      dq_head, dq_n;
    int      icmp_ping;                     /* SOCK_DGRAM + IPPROTO_ICMP */

    /* eventfd */
    uint64_t evcount;

    /* AF_UNIX */
    int      peer, peer_closed;
};

static struct ksock socks[NSOCK];
static uint16_t next_port = 0;

#define SEQ_LT(a, b) ((int32_t)((a) - (b)) < 0)
#define SEQ_LE(a, b) ((int32_t)((a) - (b)) <= 0)

static inline struct ksock *S(int id)
{
    if (id < 0 || id >= NSOCK || !socks[id].used) return NULL;
    return &socks[id];
}

static uint32_t rnd32(void)
{
    static uint64_t st = 0x2545F4914F6CDD1DULL;
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    st ^= ((uint64_t)hi << 32) | lo;
    st ^= st << 13; st ^= st >> 7; st ^= st << 17;
    return (uint32_t)(st >> 20);
}

/* ------------------------------------------------------------------ */
/* Таблица, порты, буферы                                              */
/* ------------------------------------------------------------------ */

static int slot_alloc(void)
{
    for (int i = 0; i < NSOCK; i++)
        if (!socks[i].used) {
            memset(&socks[i], 0, sizeof(socks[i]));
            socks[i].used = 1;
            socks[i].parent = -1;
            socks[i].peer = -1;
            return i;
        }
    return -1;
}

static void slot_release(struct ksock *s)
{
    if (s->rbuf) free_pages((uint64_t)s->rbuf, TCP_BUF / 4096);
    if (s->sbuf) free_pages((uint64_t)s->sbuf, TCP_BUF / 4096);
    if (s->dq) free_pages((uint64_t)s->dq, (DGQ * sizeof(struct dgram) + 4095) / 4096);
    memset(s, 0, sizeof(*s));
}

static int tcp_bufs(struct ksock *s)
{
    s->rbuf = (uint8_t *)get_free_pages(TCP_BUF / 4096);
    s->sbuf = (uint8_t *)get_free_pages(TCP_BUF / 4096);
    if (!s->rbuf || !s->sbuf) return -1;
    return 0;
}

static int port_in_use(uint16_t port, int kind)
{
    for (int i = 0; i < NSOCK; i++)
        if (socks[i].used && socks[i].kind == kind && socks[i].lport == port) return 1;
    return 0;
}

static uint16_t port_ephemeral(int kind)
{
    if (!next_port) next_port = (uint16_t)(49152 + rnd32() % 10000);
    for (int n = 0; n < 16384; n++) {
        uint16_t p = next_port++;
        if (next_port < 49152) next_port = 49152;
        if (p < 49152) continue;
        if (!port_in_use(p, kind)) return p;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Контрольная сумма TCP/UDP с псевдозаголовком                         */
/* ------------------------------------------------------------------ */

static uint16_t l4_csum(uint32_t src, uint32_t dst, uint8_t proto, const void *seg, uint16_t len)
{
    uint32_t sum = (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + proto + len;
    const uint8_t *p = (const uint8_t *)seg;
    uint16_t i = 0;
    for (; i + 1 < len; i += 2) sum += (uint32_t)((p[i] << 8) | p[i + 1]);
    if (len & 1) sum += (uint32_t)(p[len - 1] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

/* ------------------------------------------------------------------ */
/* Отправка сегментов TCP                                              */
/* ------------------------------------------------------------------ */

static void tcp_raw(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
                    uint8_t flags, uint16_t win, int syn_opt, const uint8_t *data, uint16_t dlen)
{
    uint8_t buf[1500];
    int hl = syn_opt ? 24 : 20;
    if (hl + dlen > (int)sizeof(buf)) return;
    struct tcp_header *h = (struct tcp_header *)buf;
    h->src_port = htons(sport);
    h->dest_port = htons(dport);
    h->seq_num = htonl(seq);
    h->ack_num = htonl(ack);
    h->data_offset = (uint8_t)((hl / 4) << 4);
    h->flags = flags;
    h->window_size = htons(win);
    h->checksum = 0;
    h->urgent_ptr = 0;
    if (syn_opt) { buf[20] = 2; buf[21] = 4; buf[22] = TCP_MSS >> 8; buf[23] = TCP_MSS & 0xFF; }
    if (dlen) memcpy(buf + hl, data, dlen);
    h->checksum = l4_csum(sip, dip, IP_PROTO_TCP, buf, (uint16_t)(hl + dlen));
    ip_send_ex(dip, IP_PROTO_TCP, buf, (uint16_t)(hl + dlen), 0);
}

static uint16_t rwnd(struct ksock *s)
{
    uint32_t free_b = TCP_BUF - s->r_len;
    return (uint16_t)(free_b > 65535 ? 65535 : free_b);
}

static void tcp_ack_now(struct ksock *s)
{
    tcp_raw(s->lip, s->rip, s->lport, s->rport, s->snd_nxt, s->rcv_nxt, TCP_FLAG_ACK, rwnd(s), 0, NULL, 0);
}

static void tcp_arm(struct ksock *s)
{
    s->rto_deadline = time_us() + (uint64_t)s->rto_ms * 1000;
}

static void tcp_data_seg(struct ksock *s, uint32_t seq, uint32_t n)
{
    uint8_t tmp[TCP_MSS];
    uint32_t off = (s->s_head + (seq - s->snd_una)) % TCP_BUF;
    for (uint32_t i = 0; i < n; i++) tmp[i] = s->sbuf[(off + i) % TCP_BUF];
    tcp_raw(s->lip, s->rip, s->lport, s->rport, seq, s->rcv_nxt, TCP_FLAG_ACK | TCP_FLAG_PSH, rwnd(s), 0, tmp, (uint16_t)n);
}

static int tcp_can_send(struct ksock *s)
{
    return s->st == T_ESTAB || s->st == T_CLOSE_WAIT || s->st == T_FIN_WAIT1 ||
           s->st == T_CLOSING || s->st == T_LAST_ACK;
}

static void tcp_output(struct ksock *s)
{
    if (!tcp_can_send(s)) return;
    for (;;) {
        uint32_t data_end = s->snd_una + s->s_len;
        uint32_t flight = s->snd_nxt - s->snd_una;
        uint32_t wnd = s->snd_wnd < s->cwnd ? s->snd_wnd : s->cwnd;
        if (SEQ_LT(s->snd_nxt, data_end)) {
            if (flight >= wnd) break;
            uint32_t n = data_end - s->snd_nxt;
            if (n > s->mss) n = s->mss;
            if (n > wnd - flight) n = wnd - flight;
            if (n == 0) break;
            tcp_data_seg(s, s->snd_nxt, n);
            if (!s->rtt_timing) { s->rtt_timing = 1; s->rtt_seq = s->snd_nxt; s->rtt_t0 = time_us(); }
            s->snd_nxt += n;
            if (!s->rto_deadline) tcp_arm(s);
        } else if (s->fin_queued && !s->fin_sent) {
            tcp_raw(s->lip, s->rip, s->lport, s->rport, s->snd_nxt, s->rcv_nxt, TCP_FLAG_FIN | TCP_FLAG_ACK, rwnd(s), 0, NULL, 0);
            s->fin_sent = 1;
            s->snd_nxt++;
            if (s->st == T_ESTAB) s->st = T_FIN_WAIT1;
            else if (s->st == T_CLOSE_WAIT) s->st = T_LAST_ACK;
            if (!s->rto_deadline) tcp_arm(s);
            break;
        } else break;
    }
}

static void tcp_free_or_close(struct ksock *s, int err)
{
    if (err && !s->err) s->err = err;
    s->st = T_CLOSED;
    s->rto_deadline = 0;
    if (s->orphan || s->refs == 0) slot_release(s);
}

/* ------------------------------------------------------------------ */
/* Приём TCP                                                           */
/* ------------------------------------------------------------------ */

static struct ksock *tcp_find(uint32_t dip, uint16_t dport, uint32_t sip, uint16_t sport)
{
    struct ksock *lst = NULL;
    for (int i = 0; i < NSOCK; i++) {
        struct ksock *s = &socks[i];
        if (!s->used || s->kind != K_TCP || s->lport != dport) continue;
        if (s->st == T_LISTEN) {
            if (s->lip == 0 || s->lip == dip) lst = s;
            continue;
        }
        if (s->rip == sip && s->rport == sport && (s->lip == dip || s->lip == 0)) return s;
    }
    return lst;
}

static void tcp_parse_mss(struct ksock *s, const uint8_t *opt, int olen)
{
    s->mss = 536;
    for (int i = 0; i < olen;) {
        uint8_t k = opt[i];
        if (k == 0) break;
        if (k == 1) { i++; continue; }
        if (i + 1 >= olen || opt[i + 1] < 2 || i + opt[i + 1] > olen) break;
        if (k == 2 && opt[i + 1] == 4) {
            uint32_t m = (uint32_t)((opt[i + 2] << 8) | opt[i + 3]);
            if (m >= 88 && m < 1460) s->mss = m; else if (m >= 1460) s->mss = 1460;
        }
        i += opt[i + 1];
    }
}

static void tcp_rtt_sample(struct ksock *s, uint32_t us)
{
    if (!s->srtt_us) { s->srtt_us = us; s->rttvar_us = us / 2; }
    else {
        uint32_t d = us > s->srtt_us ? us - s->srtt_us : s->srtt_us - us;
        s->rttvar_us = (s->rttvar_us * 3 + d) / 4;
        s->srtt_us = (s->srtt_us * 7 + us) / 8;
    }
    uint32_t rto = (s->srtt_us + 4 * s->rttvar_us) / 1000;
    if (rto < 200) rto = 200;
    if (rto > 60000) rto = 60000;
    s->rto_ms = rto;
}

static void tcp_to_accq(struct ksock *c)
{
    struct ksock *l = (c->parent >= 0) ? S(c->parent) : NULL;
    if (!l || l->st != T_LISTEN || l->accn >= ACCQ) {
        tcp_raw(c->lip, c->rip, c->lport, c->rport, c->snd_nxt, c->rcv_nxt, TCP_FLAG_RST | TCP_FLAG_ACK, 0, 0, NULL, 0);
        slot_release(c);
        return;
    }
    l->accq[l->accn++] = (int)(c - socks);
}

static void tcp_listener_syn(struct ksock *l, uint32_t src, uint32_t dst, uint16_t sport, uint32_t seq,
                             uint16_t win, const uint8_t *opt, int olen)
{
    int pending = 0;
    for (int i = 0; i < NSOCK; i++)
        if (socks[i].used && socks[i].parent == (int)(l - socks) && socks[i].st == T_SYN_RCVD) pending++;
    if (l->accn + pending >= l->backlog) return;
    int id = slot_alloc();
    if (id < 0) return;
    struct ksock *c = &socks[id];
    c->kind = K_TCP; c->type = SOCK_STREAM_; c->domain = AF_INET_;
    if (tcp_bufs(c) < 0) { slot_release(c); return; }
    c->lip = dst; c->lport = l->lport; c->rip = src; c->rport = sport;
    c->irs = seq; c->rcv_nxt = seq + 1;
    c->iss = rnd32(); c->snd_una = c->iss; c->snd_nxt = c->iss + 1;
    c->snd_wnd = win; c->parent = (int)(l - socks);
    c->st = T_SYN_RCVD; c->rto_ms = 1000; c->connected = 1;
    tcp_parse_mss(c, opt, olen);
    c->cwnd = 4 * c->mss; c->ssthresh = 65535;
    tcp_raw(c->lip, c->rip, c->lport, c->rport, c->iss, c->rcv_nxt, TCP_FLAG_SYN | TCP_FLAG_ACK, rwnd(c), 1, NULL, 0);
    tcp_arm(c);
}

void sock_tcp_rx(uint32_t src, uint32_t dst, const uint8_t *seg, uint16_t len)
{
    if (len < 20) return;
    const struct tcp_header *h = (const struct tcp_header *)seg;
    int doff = (h->data_offset >> 4) * 4;
    if (doff < 20 || doff > len) return;
    if (l4_csum(src, dst, IP_PROTO_TCP, seg, len) != 0) return;
    uint16_t sport = ntohs(h->src_port), dport = ntohs(h->dest_port);
    uint32_t seq = ntohl(h->seq_num), ack = ntohl(h->ack_num);
    uint8_t flags = h->flags;
    uint16_t win = ntohs(h->window_size);
    const uint8_t *data = seg + doff;
    uint32_t dlen = (uint32_t)(len - doff);

    struct ksock *s = tcp_find(dst, dport, src, sport);
    if (!s) {
        if (flags & TCP_FLAG_RST) return;
        if (flags & TCP_FLAG_ACK)
            tcp_raw(dst, src, dport, sport, ack, 0, TCP_FLAG_RST, 0, 0, NULL, 0);
        else
            tcp_raw(dst, src, dport, sport, 0, seq + dlen + ((flags & TCP_FLAG_SYN) ? 1 : 0) + ((flags & TCP_FLAG_FIN) ? 1 : 0),
                    TCP_FLAG_RST | TCP_FLAG_ACK, 0, 0, NULL, 0);
        return;
    }

    if (s->st == T_LISTEN) {
        if (flags & TCP_FLAG_RST) return;
        if (flags & TCP_FLAG_ACK) { tcp_raw(dst, src, dport, sport, ack, 0, TCP_FLAG_RST, 0, 0, NULL, 0); return; }
        if (flags & TCP_FLAG_SYN) tcp_listener_syn(s, src, dst, sport, seq, win, seg + 20, doff - 20);
        return;
    }

    if (s->st == T_SYN_SENT) {
        if (flags & TCP_FLAG_ACK) {
            if (ack != s->iss + 1) {
                if (!(flags & TCP_FLAG_RST)) tcp_raw(s->lip, s->rip, s->lport, s->rport, ack, 0, TCP_FLAG_RST, 0, 0, NULL, 0);
                return;
            }
        }
        if (flags & TCP_FLAG_RST) {
            if (flags & TCP_FLAG_ACK) tcp_free_or_close(s, E_CONNREFUSED);
            return;
        }
        if ((flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK)) {
            s->irs = seq; s->rcv_nxt = seq + 1;
            s->snd_una = ack; s->snd_wnd = win;
            tcp_parse_mss(s, seg + 20, doff - 20);
            s->cwnd = 4 * s->mss; s->ssthresh = 65535;
            s->st = T_ESTAB; s->connected = 1; s->retries = 0; s->rto_deadline = 0;
            if (s->rtt_timing) { tcp_rtt_sample(s, (uint32_t)(time_us() - s->rtt_t0)); s->rtt_timing = 0; }
            tcp_ack_now(s);
        }
        return;
    }

    if (s->st == T_TIME_WAIT) {
        if (!(flags & TCP_FLAG_RST)) tcp_ack_now(s);
        return;
    }

    if (flags & TCP_FLAG_RST) {
        if (SEQ_LE(s->rcv_nxt, seq) && SEQ_LT(seq, s->rcv_nxt + 65536)) {
            if (s->st == T_SYN_RCVD) slot_release(s);
            else tcp_free_or_close(s, s->st == T_CLOSE_WAIT ? E_PIPE : E_CONNRESET);
        }
        return;
    }

    if (s->st == T_SYN_RCVD) {
        if (flags & TCP_FLAG_SYN) {                         /* повтор SYN: повторить SYN-ACK */
            tcp_raw(s->lip, s->rip, s->lport, s->rport, s->iss, s->rcv_nxt, TCP_FLAG_SYN | TCP_FLAG_ACK, rwnd(s), 1, NULL, 0);
            return;
        }
        if (!(flags & TCP_FLAG_ACK) || ack != s->iss + 1) {
            tcp_raw(s->lip, s->rip, s->lport, s->rport, ack, 0, TCP_FLAG_RST, 0, 0, NULL, 0);
            return;
        }
        s->snd_una = ack; s->snd_wnd = win;
        s->st = T_ESTAB; s->retries = 0; s->rto_deadline = 0;
        tcp_to_accq(s);
        if (!s->used || s->st != T_ESTAB) return;
    }

    if (flags & TCP_FLAG_SYN) { tcp_ack_now(s); return; }
    if (!(flags & TCP_FLAG_ACK)) return;

    /* ---- ACK ---- */
    if (SEQ_LT(s->snd_nxt, ack)) { tcp_ack_now(s); return; }
    if (SEQ_LT(s->snd_una, ack)) {
        uint32_t acked = ack - s->snd_una;
        uint32_t data_acked = acked;
        if (s->fin_sent && ack == s->snd_nxt) { s->fin_acked = 1; if (data_acked) data_acked--; }
        if (data_acked > s->s_len) data_acked = s->s_len;
        s->s_head = (s->s_head + data_acked) % TCP_BUF;
        s->s_len -= data_acked;
        s->snd_una = ack;
        s->dupacks = 0; s->retries = 0;
        if (s->cwnd < s->ssthresh) s->cwnd += (acked < s->mss ? acked : s->mss);
        else s->cwnd += s->mss * s->mss / s->cwnd + 1;
        if (s->cwnd > 4 * TCP_BUF) s->cwnd = 4 * TCP_BUF;
        if (s->rtt_timing && SEQ_LT(s->rtt_seq, ack)) {
            tcp_rtt_sample(s, (uint32_t)(time_us() - s->rtt_t0));
            s->rtt_timing = 0;
        }
        if (s->snd_una == s->snd_nxt) s->rto_deadline = 0; else tcp_arm(s);
        s->snd_wnd = win;
        if (s->fin_acked) {
            if (s->st == T_FIN_WAIT1) s->st = T_FIN_WAIT2;
            else if (s->st == T_CLOSING) { s->st = T_TIME_WAIT; s->tw_until = time_us() + 2000000; }
            else if (s->st == T_LAST_ACK) { tcp_free_or_close(s, 0); return; }
            if (s->st == T_FIN_WAIT2 && s->orphan) s->tw_until = time_us() + 30000000;
        }
    } else if (ack == s->snd_una) {
        if (dlen == 0 && !(flags & TCP_FLAG_FIN) && s->snd_nxt != s->snd_una && win == s->snd_wnd) {
            if (++s->dupacks == 3) {                        /* быстрая повторная передача */
                uint32_t flight = s->snd_nxt - s->snd_una;
                s->ssthresh = flight / 2 > 2 * s->mss ? flight / 2 : 2 * s->mss;
                s->cwnd = s->ssthresh + 3 * s->mss;
                uint32_t n = s->snd_una + s->s_len - s->snd_una;
                if (n > s->mss) n = s->mss;
                if (n) tcp_data_seg(s, s->snd_una, n);
                s->rtt_timing = 0;
            } else if (s->dupacks > 3) s->cwnd += s->mss;
        }
        s->snd_wnd = win;
    }

    /* ---- данные и FIN ---- */
    if (s->st == T_ESTAB || s->st == T_FIN_WAIT1 || s->st == T_FIN_WAIT2) {
        uint32_t fin_seq = seq + dlen;
        int fin = (flags & TCP_FLAG_FIN) != 0;
        uint32_t sq = seq;
        const uint8_t *d = data;
        uint32_t dl = dlen;
        int need_ack = (dlen > 0 || fin);
        if (SEQ_LT(sq, s->rcv_nxt)) {
            uint32_t cut = s->rcv_nxt - sq;
            if (cut >= dl) { d += dl; dl = 0; } else { d += cut; dl -= cut; }
            sq = s->rcv_nxt;
            if (SEQ_LT(fin_seq, s->rcv_nxt)) fin = 0;
        }
        if (sq != s->rcv_nxt && (dl || fin)) {
            tcp_ack_now(s);                                 /* вне порядка: дубликат ACK */
        } else {
            uint32_t n = dl;
            if (s->orphan || s->shut_rd) {
                s->rcv_nxt += n;
            } else {
                uint32_t space = TCP_BUF - s->r_len;
                if (n > space) n = space;
                uint32_t pos = (s->r_head + s->r_len) % TCP_BUF;
                for (uint32_t i = 0; i < n; i++) s->rbuf[(pos + i) % TCP_BUF] = d[i];
                s->r_len += n;
                s->rcv_nxt += n;
            }
            if (n == dl && fin && fin_seq == s->rcv_nxt) {
                s->rcv_nxt++;
                s->fin_rcvd = 1;
                if (s->st == T_ESTAB) s->st = T_CLOSE_WAIT;
                else if (s->st == T_FIN_WAIT1) s->st = s->fin_acked ? T_TIME_WAIT : T_CLOSING;
                else if (s->st == T_FIN_WAIT2) s->st = T_TIME_WAIT;
                if (s->st == T_TIME_WAIT) s->tw_until = time_us() + 2000000;
            }
            if (need_ack) tcp_ack_now(s);
        }
    } else if (dlen > 0 || (flags & TCP_FLAG_FIN)) {
        tcp_ack_now(s);
    }
    tcp_output(s);
}

/* ------------------------------------------------------------------ */
/* Таймеры                                                             */
/* ------------------------------------------------------------------ */

void sock_timer(void)
{
    uint64_t now = time_us();
    for (int i = 0; i < NSOCK; i++) {
        struct ksock *s = &socks[i];
        if (!s->used || s->kind != K_TCP) continue;
        if (s->st == T_TIME_WAIT || (s->st == T_FIN_WAIT2 && s->orphan && s->tw_until)) {
            if (s->tw_until && now >= s->tw_until) tcp_free_or_close(s, 0);
            continue;
        }
        if (!s->rto_deadline || now < s->rto_deadline) continue;

        if (++s->retries > (s->st == T_SYN_SENT ? 5 : 12)) {
            if (s->st == T_SYN_RCVD) slot_release(s);
            else {
                tcp_raw(s->lip, s->rip, s->lport, s->rport, s->snd_nxt, s->rcv_nxt, TCP_FLAG_RST | TCP_FLAG_ACK, 0, 0, NULL, 0);
                tcp_free_or_close(s, E_TIMEDOUT);
            }
            continue;
        }
        s->rto_ms = s->rto_ms * 2 > 60000 ? 60000 : s->rto_ms * 2;
        s->rtt_timing = 0;
        if (s->st == T_SYN_SENT) {
            tcp_raw(s->lip, s->rip, s->lport, s->rport, s->iss, 0, TCP_FLAG_SYN, 65535, 1, NULL, 0);
        } else if (s->st == T_SYN_RCVD) {
            tcp_raw(s->lip, s->rip, s->lport, s->rport, s->iss, s->rcv_nxt, TCP_FLAG_SYN | TCP_FLAG_ACK, rwnd(s), 1, NULL, 0);
        } else {
            uint32_t flight = s->snd_nxt - s->snd_una;
            if (flight == 0 && s->s_len > 0) {              /* окно нулевое: зонд в 1 байт */
                tcp_data_seg(s, s->snd_nxt, 1);
                s->snd_nxt++;
            } else {
                s->ssthresh = flight / 2 > 2 * s->mss ? flight / 2 : 2 * s->mss;
                s->cwnd = s->mss;
                s->snd_nxt = s->snd_una;                    /* go-back-N */
                s->fin_sent = 0;
                s->dupacks = 0;
                tcp_output(s);
            }
        }
        tcp_arm(s);
    }
}

/* ------------------------------------------------------------------ */
/* UDP / ICMP приём                                                    */
/* ------------------------------------------------------------------ */

static void dq_push(struct ksock *s, uint32_t ip, uint16_t port, const uint8_t *data, uint16_t len)
{
    if (!s->dq || s->dq_n >= DGQ) return;
    if (len > sizeof(s->dq[0].data)) len = sizeof(s->dq[0].data);
    struct dgram *d = &s->dq[(s->dq_head + s->dq_n) % DGQ];
    d->ip = ip; d->port = port; d->len = len;
    memcpy(d->data, data, len);
    s->dq_n++;
}

void sock_udp_rx(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport, const uint8_t *data, uint16_t len)
{
    for (int i = 0; i < NSOCK; i++) {
        struct ksock *s = &socks[i];
        if (!s->used || s->kind != K_UDP || s->icmp_ping || s->lport != dport) continue;
        if (s->lip && s->lip != dst) continue;
        if (s->connected && (s->rip != src || s->rport != sport)) continue;
        dq_push(s, src, sport, data, len);
    }
}

void sock_icmp_rx(uint32_t src, uint32_t dst, const uint8_t *icmp, uint16_t len, const uint8_t *iphdr)
{
    (void)dst;
    if (len < 8) return;
    for (int i = 0; i < NSOCK; i++) {
        struct ksock *s = &socks[i];
        if (!s->used) continue;
        if (s->kind == K_RAW) {
            if (s->rip && s->rip != src) continue;
            if (iphdr) {
                uint8_t pkt[1500];
                uint16_t ihl = (uint16_t)((iphdr[0] & 0x0F) * 4);
                if (ihl + len > sizeof(pkt)) continue;
                memcpy(pkt, iphdr, ihl);
                memcpy(pkt + ihl, icmp, len);
                dq_push(s, src, 0, pkt, (uint16_t)(ihl + len));
            }
        } else if (s->kind == K_UDP && s->icmp_ping && icmp[0] == 0) {
            uint16_t id = (uint16_t)((icmp[4] << 8) | icmp[5]);
            if (id == s->lport) dq_push(s, src, 0, icmp, len);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Ожидание                                                            */
/* ------------------------------------------------------------------ */

/* Один шаг ожидания: обработать сеть, затем уступить процессор. 0 -- продолжать ждать. */
static int wait_step(uint64_t deadline_us, int *spin)
{
    net_poll();
    if ((current->signal & ~(uint32_t)current->sigmask) != 0) return -E_INTR;
    if (deadline_us && time_us() >= deadline_us) return -E_AGAIN;
    if ((*spin)++ < 40) udelay(50);
    else sched_yield_sleep();
    return 0;
}

static uint64_t deadline_of(int ms)
{
    return ms > 0 ? time_us() + (uint64_t)ms * 1000 : 0;
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

int sock_create(int domain, int type, int proto)
{
    int t = type & 0xF;
    if (domain == AF_UNIX_) {
        if (t != SOCK_STREAM_ && t != SOCK_DGRAM_) return -E_PROTONO;
    } else if (domain != AF_INET_) return -E_AFNO;
    unsigned long fl = irq_save();
    int id = slot_alloc();
    if (id < 0) { irq_restore(fl); return -E_NOBUFS; }
    struct ksock *s = &socks[id];
    s->refs = 1; s->type = t; s->proto = proto; s->domain = domain;
    s->nonblock = (type & 0x800) != 0;
    int r = 0;
    if (domain == AF_UNIX_) {
        s->kind = K_UNIX;
        s->rbuf = (uint8_t *)get_free_pages(TCP_BUF / 4096);
        if (!s->rbuf) r = -E_NOMEM;
    } else if (t == SOCK_STREAM_) {
        if (proto != 0 && proto != 6) r = -E_PROTONO;
        else {
            s->kind = K_TCP; s->st = T_CLOSED; s->rto_ms = 1000;
            if (tcp_bufs(s) < 0) r = -E_NOMEM;
        }
    } else if (t == SOCK_DGRAM_) {
        if (proto == 1) { s->kind = K_UDP; s->icmp_ping = 1; }
        else if (proto == 0 || proto == 17) s->kind = K_UDP;
        else r = -E_PROTONO;
    } else if (t == SOCK_RAW_) {
        if (proto == 1) s->kind = K_RAW; else r = -E_PROTONO;
    } else r = -E_PROTONO;
    if (!r && (s->kind == K_UDP || s->kind == K_RAW)) {
        s->dq = (struct dgram *)get_free_pages((DGQ * sizeof(struct dgram) + 4095) / 4096);
        if (!s->dq) r = -E_NOMEM;
    }
    if (r) { slot_release(s); irq_restore(fl); return r; }
    irq_restore(fl);
    return id;
}

/* eventfd: 64-битный счётчик; write прибавляет, read возвращает и обнуляет */
int sock_eventfd(unsigned initval, int flags)
{
    unsigned long fl = irq_save();
    int id = slot_alloc();
    if (id < 0) { irq_restore(fl); return -E_NOBUFS; }
    struct ksock *s = &socks[id];
    s->refs = 1; s->kind = K_EVENT; s->type = SOCK_STREAM_; s->domain = AF_UNIX_;
    s->evcount = initval; s->nonblock = (flags & 0x800) != 0;
    irq_restore(fl);
    return id;
}

int sock_pair(int type, int ids[2])
{
    int a = sock_create(AF_UNIX_, type, 0);
    if (a < 0) return a;
    int b = sock_create(AF_UNIX_, type, 0);
    if (b < 0) { sock_close(a); return b; }
    unsigned long fl = irq_save();
    socks[a].peer = b; socks[b].peer = a;
    socks[a].connected = socks[b].connected = 1;
    irq_restore(fl);
    ids[0] = a; ids[1] = b;
    return 0;
}

void sock_ref(int id)
{
    struct ksock *s = S(id);
    if (s) s->refs++;
}

void sock_close(int id)
{
    unsigned long fl = irq_save();
    struct ksock *s = S(id);
    if (!s || --s->refs > 0) { irq_restore(fl); return; }
    if (s->kind == K_UNIX) {
        struct ksock *p = S(s->peer);
        if (p) { p->peer_closed = 1; p->peer = -1; }
        slot_release(s);
    } else if (s->kind != K_TCP) {
        slot_release(s);
    } else if (s->st == T_LISTEN) {
        for (int i = 0; i < NSOCK; i++) {
            struct ksock *c = &socks[i];
            if (c->used && c->parent == id) {
                if (c->st == T_ESTAB || c->st == T_CLOSE_WAIT)
                    tcp_raw(c->lip, c->rip, c->lport, c->rport, c->snd_nxt, c->rcv_nxt, TCP_FLAG_RST | TCP_FLAG_ACK, 0, 0, NULL, 0);
                slot_release(c);
            }
        }
        slot_release(s);
    } else if (s->st == T_CLOSED || s->st == T_SYN_SENT) {
        slot_release(s);
    } else {
        s->orphan = 1;
        s->r_len = 0;
        if (s->st == T_ESTAB || s->st == T_CLOSE_WAIT || s->st == T_SYN_RCVD) {
            if (s->st == T_SYN_RCVD) s->st = T_ESTAB;
            s->fin_queued = 1;
            tcp_output(s);
        }
        if (s->st == T_FIN_WAIT2) s->tw_until = time_us() + 30000000;
    }
    irq_restore(fl);
}

int sock_bind(int id, uint32_t ip, uint16_t port)
{
    unsigned long fl = irq_save();
    struct ksock *s = S(id);
    int r = 0;
    if (!s) r = -E_NOTSOCK;
    else if (s->kind == K_UNIX) r = -E_OPNOTSUPP;
    else if (s->lport) r = -E_INVAL;
    else if (s->kind == K_RAW) { s->lip = ip; }
    else {
        if (ip && ip != net_my_ip && (ip >> 24) != 127) r = -E_ADDRNOTAV;
        else if (port == 0) { port = port_ephemeral(s->kind); if (!port) r = -E_ADDRINUSE; }
        else if (port_in_use(port, s->kind)) r = -E_ADDRINUSE;
        if (!r) { s->lport = port; s->lip = ip; }
    }
    irq_restore(fl);
    return r;
}

int sock_listen(int id, int backlog)
{
    unsigned long fl = irq_save();
    struct ksock *s = S(id);
    int r = 0;
    if (!s) r = -E_NOTSOCK;
    else if (s->kind != K_TCP) r = -E_OPNOTSUPP;
    else if (s->st != T_CLOSED && s->st != T_LISTEN) r = -E_INVAL;
    else {
        if (!s->lport) s->lport = port_ephemeral(K_TCP);
        s->st = T_LISTEN;
        s->backlog = backlog < 1 ? 1 : backlog > ACCQ ? ACCQ : backlog;
    }
    irq_restore(fl);
    return r;
}

int sock_accept(int id, uint32_t *ip, uint16_t *port)
{
    int spin = 0;
    for (;;) {
        unsigned long fl = irq_save();
        struct ksock *s = S(id);
        if (!s) { irq_restore(fl); return -E_NOTSOCK; }
        if (s->kind != K_TCP || s->st != T_LISTEN) { irq_restore(fl); return -E_INVAL; }
        if (s->accn > 0) {
            int cid = s->accq[0];
            for (int i = 1; i < s->accn; i++) s->accq[i - 1] = s->accq[i];
            s->accn--;
            struct ksock *c = &socks[cid];
            c->refs = 1; c->parent = -1;
            if (ip) *ip = c->rip;
            if (port) *port = c->rport;
            irq_restore(fl);
            return cid;
        }
        int nb = s->nonblock;
        irq_restore(fl);
        if (nb) return -E_AGAIN;
        int r = wait_step(0, &spin);
        if (r < 0) return r;
    }
}

int sock_connect(int id, uint32_t ip, uint16_t port)
{
    struct ksock *s = S(id);
    if (!s) return -E_NOTSOCK;
    if (s->kind == K_UNIX) return -E_NOENT;
    if (s->kind == K_RAW) { s->rip = ip; s->connected = 1; return 0; }
    if (!net_my_ip && (ip >> 24) != 127) return -E_NETUNREACH;
    net_arp_prepare(ip);

    if (s->kind == K_UDP) {
        unsigned long fl = irq_save();
        if (!s->lport && !s->icmp_ping) s->lport = port_ephemeral(K_UDP);
        if (s->icmp_ping && !s->lport) s->lport = (uint16_t)(rnd32() | 1);
        s->rip = ip; s->rport = port; s->connected = 1;
        s->lip = net_src_for(ip);
        irq_restore(fl);
        return 0;
    }

    unsigned long fl = irq_save();
    s = S(id);
    if (!s) { irq_restore(fl); return -E_NOTSOCK; }
    if (s->st == T_ESTAB || s->st == T_CLOSE_WAIT) { irq_restore(fl); return -E_ISCONN; }
    if (s->st == T_SYN_SENT) {
        if (s->nonblock) { irq_restore(fl); return -E_ALREADY; }
    } else if (s->st == T_CLOSED) {
        if (s->err) { int e = s->err; s->err = 0; irq_restore(fl); return -e; }
        if (!s->lport) s->lport = port_ephemeral(K_TCP);
        if (!s->lport) { irq_restore(fl); return -E_ADDRINUSE; }
        s->rip = ip; s->rport = port; s->lip = net_src_for(ip);
        s->iss = rnd32(); s->snd_una = s->iss; s->snd_nxt = s->iss + 1;
        s->mss = TCP_MSS; s->cwnd = 4 * TCP_MSS; s->ssthresh = 65535; s->snd_wnd = 65535;
        s->rto_ms = 1000; s->retries = 0;
        s->st = T_SYN_SENT;
        s->rtt_timing = 1; s->rtt_seq = s->iss; s->rtt_t0 = time_us();
        tcp_raw(s->lip, s->rip, s->lport, s->rport, s->iss, 0, TCP_FLAG_SYN, 65535, 1, NULL, 0);
        tcp_arm(s);
        if (s->nonblock) { irq_restore(fl); return -E_INPROGRESS; }
    } else { irq_restore(fl); return -E_INVAL; }
    irq_restore(fl);

    int spin = 0;
    for (;;) {
        fl = irq_save();
        s = S(id);
        if (!s) { irq_restore(fl); return -E_NOTSOCK; }
        if (s->st == T_ESTAB) { irq_restore(fl); return 0; }
        if (s->st == T_CLOSED) { int e = s->err ? s->err : E_CONNREFUSED; s->err = 0; irq_restore(fl); return -e; }
        irq_restore(fl);
        int r = wait_step(0, &spin);
        if (r < 0) return r;
    }
}

static int local_ready(struct ksock *s, int *mask_only)
{
    (void)mask_only; (void)s;
    return 0;
}

int sock_poll(int id)
{
    struct ksock *s = S(id);
    if (!s) return SOCK_POLLERR;
    (void)local_ready;
    int m = 0;
    switch (s->kind) {
    case K_EVENT:
        if (s->evcount > 0) m |= SOCK_POLLIN;
        m |= SOCK_POLLOUT;
        break;
    case K_UDP: case K_RAW:
        if (s->dq_n > 0) m |= SOCK_POLLIN;
        m |= SOCK_POLLOUT;
        break;
    case K_UNIX: {
        struct ksock *p = S(s->peer);
        if (s->r_len > 0 || s->peer_closed) m |= SOCK_POLLIN;
        if (s->peer_closed) m |= SOCK_POLLHUP;
        if (p && p->r_len < TCP_BUF) m |= SOCK_POLLOUT;
        if (!p && !s->peer_closed) m |= SOCK_POLLOUT;
        break;
    }
    case K_TCP:
        if (s->st == T_LISTEN) { if (s->accn > 0) m |= SOCK_POLLIN; break; }
        if (s->r_len > 0 || s->fin_rcvd) m |= SOCK_POLLIN;
        if ((s->st == T_ESTAB || s->st == T_CLOSE_WAIT) && s->s_len < TCP_BUF) m |= SOCK_POLLOUT;
        if (s->st == T_CLOSED && s->connected == 0 && s->err == 0 && s->lport == 0) m |= SOCK_POLLOUT | SOCK_POLLHUP;
        if (s->st == T_CLOSED && (s->err || s->rip)) m |= SOCK_POLLHUP | SOCK_POLLIN | SOCK_POLLOUT;
        if (s->err) m |= SOCK_POLLERR;
        if (s->fin_rcvd && (s->fin_acked || s->st == T_CLOSE_WAIT)) m |= SOCK_POLLHUP * 0;
        break;
    }
    return m;
}

int64_t sock_send(int id, const void *buf, uint64_t len, int flags, uint32_t ip, uint16_t port, int has_addr)
{
    struct ksock *s = S(id);
    if (!s) return -E_NOTSOCK;
    const uint8_t *p = (const uint8_t *)buf;

    if (s->kind == K_EVENT) {
        if (len < 8) return -E_INVAL;
        uint64_t v;
        memcpy(&v, p, 8);
        unsigned long fl = irq_save();
        s->evcount += v;
        irq_restore(fl);
        return 8;
    }

    if (s->kind == K_UDP || s->kind == K_RAW) {
        if (has_addr) { /* адрес назначения из sendto */ }
        else if (s->connected) { ip = s->rip; port = s->rport; }
        else return -E_DESTADDR;
        if (len > 1472) return -E_MSGSIZE;
        if (!net_my_ip && (ip >> 24) != 127) return -E_NETUNREACH;
        net_arp_prepare(ip);
        unsigned long fl = irq_save();
        s = S(id);
        if (!s) { irq_restore(fl); return -E_NOTSOCK; }
        int r = 0;
        if (s->kind == K_UDP && s->icmp_ping) {
            uint8_t pk[1500];
            if (len < 8) r = -E_INVAL;
            else {
                memcpy(pk, p, len);
                if (!s->lport) s->lport = (uint16_t)(rnd32() | 1);
                pk[2] = pk[3] = 0;
                pk[4] = (uint8_t)(s->lport >> 8); pk[5] = (uint8_t)s->lport;
                uint16_t c = net_checksum(pk, len);
                memcpy(pk + 2, &c, 2);
                r = ip_send_ex(ip, IP_PROTO_ICMP, pk, (uint16_t)len, 0);
            }
        } else if (s->kind == K_RAW) {
            r = ip_send_ex(ip, IP_PROTO_ICMP, p, (uint16_t)len, 0);
        } else {
            uint8_t pk[1500];
            if (!s->lport) s->lport = port_ephemeral(K_UDP);
            struct udp_header *u = (struct udp_header *)pk;
            u->src_port = htons(s->lport); u->dest_port = htons(port);
            u->length = htons((uint16_t)(8 + len)); u->checksum = 0;
            memcpy(pk + 8, p, len);
            r = ip_send_ex(ip, IP_PROTO_UDP, pk, (uint16_t)(8 + len), 0);
        }
        irq_restore(fl);
        return r < 0 ? -E_NETUNREACH : (int64_t)len;
    }

    if (s->kind == K_UNIX) {
        uint64_t done = 0;
        int spin = 0;
        uint64_t dl = deadline_of(s->sndtimeo_ms);
        while (done < len) {
            unsigned long fl = irq_save();
            s = S(id);
            struct ksock *q = s ? S(s->peer) : NULL;
            if (!s) { irq_restore(fl); return -E_NOTSOCK; }
            if (!q) { irq_restore(fl); return done ? (int64_t)done : -E_PIPE; }
            uint32_t space = TCP_BUF - q->r_len;
            if (space > 0) {
                uint32_t n = (uint32_t)(len - done < space ? len - done : space);
                uint32_t pos = (q->r_head + q->r_len) % TCP_BUF;
                for (uint32_t i = 0; i < n; i++) q->rbuf[(pos + i) % TCP_BUF] = p[done + i];
                q->r_len += n; done += n;
                irq_restore(fl);
                continue;
            }
            int nb = s->nonblock || (flags & MSG_DONTWAIT_);
            irq_restore(fl);
            if (nb) return done ? (int64_t)done : -E_AGAIN;
            int r = wait_step(dl, &spin);
            if (r < 0) return done ? (int64_t)done : r;
        }
        return (int64_t)done;
    }

    /* TCP */
    uint64_t done = 0;
    int spin = 0;
    uint64_t dl = deadline_of(s->sndtimeo_ms);
    if (len == 0) return 0;
    while (done < len) {
        unsigned long fl = irq_save();
        s = S(id);
        if (!s) { irq_restore(fl); return -E_NOTSOCK; }
        if (s->st == T_SYN_SENT || s->st == T_SYN_RCVD) {
            int nb = s->nonblock || (flags & MSG_DONTWAIT_);
            irq_restore(fl);
            if (nb) return -E_AGAIN;
            int r = wait_step(dl, &spin);
            if (r < 0) return r;
            continue;
        }
        if ((s->st != T_ESTAB && s->st != T_CLOSE_WAIT) || s->shut_wr || s->fin_queued) {
            int e = s->err ? s->err : (s->st == T_CLOSED && !s->rip ? E_NOTCONN : E_PIPE);
            s->err = 0;
            irq_restore(fl);
            return done ? (int64_t)done : -e;
        }
        uint32_t space = TCP_BUF - s->s_len;
        if (space > 0) {
            uint32_t n = (uint32_t)(len - done < space ? len - done : space);
            uint32_t pos = (s->s_head + s->s_len) % TCP_BUF;
            for (uint32_t i = 0; i < n; i++) s->sbuf[(pos + i) % TCP_BUF] = p[done + i];
            s->s_len += n; done += n;
            tcp_output(s);
            irq_restore(fl);
            spin = 0;
            continue;
        }
        tcp_output(s);
        int nb = s->nonblock || (flags & MSG_DONTWAIT_);
        irq_restore(fl);
        if (nb) return done ? (int64_t)done : -E_AGAIN;
        int r = wait_step(dl, &spin);
        if (r < 0) return done ? (int64_t)done : r;
    }
    return (int64_t)done;
}

int64_t sock_recv(int id, void *buf, uint64_t len, int flags, uint32_t *ip, uint16_t *port)
{
    struct ksock *s = S(id);
    if (!s) return -E_NOTSOCK;
    uint8_t *out = (uint8_t *)buf;
    int spin = 0;
    uint64_t dl = deadline_of(s->rcvtimeo_ms);
    uint64_t got = 0;

    for (;;) {
        unsigned long fl = irq_save();
        s = S(id);
        if (!s) { irq_restore(fl); return -E_NOTSOCK; }
        int nb = s->nonblock || (flags & MSG_DONTWAIT_);

        if (s->kind == K_EVENT) {
            if (len < 8) { irq_restore(fl); return -E_INVAL; }
            if (s->evcount > 0) {
                uint64_t v = s->evcount;
                s->evcount = 0;
                memcpy(out, &v, 8);
                irq_restore(fl);
                return 8;
            }
            irq_restore(fl);
            if (nb) return -E_AGAIN;
        } else if (s->kind == K_UDP || s->kind == K_RAW) {
            if (s->dq_n > 0) {
                struct dgram *d = &s->dq[s->dq_head];
                uint64_t n = d->len < len ? d->len : len;
                memcpy(out, d->data, n);
                if (ip) *ip = d->ip;
                if (port) *port = d->port;
                if (!(flags & MSG_PEEK_)) { s->dq_head = (s->dq_head + 1) % DGQ; s->dq_n--; }
                irq_restore(fl);
                return (int64_t)n;
            }
            irq_restore(fl);
            if (nb) return -E_AGAIN;
        } else {
            /* поток: TCP или AF_UNIX */
            if (s->r_len > 0) {
                uint32_t before = s->r_len;
                uint64_t n = s->r_len < len - got ? s->r_len : len - got;
                for (uint64_t i = 0; i < n; i++) out[got + i] = s->rbuf[(s->r_head + i) % TCP_BUF];
                if (!(flags & MSG_PEEK_)) {
                    s->r_head = (uint32_t)((s->r_head + n) % TCP_BUF);
                    s->r_len -= (uint32_t)n;
                    if (s->kind == K_TCP && before > TCP_BUF - 4 * TCP_MSS && s->r_len <= TCP_BUF - 4 * TCP_MSS)
                        tcp_ack_now(s);                     /* окно открылось */
                }
                got += n;
                irq_restore(fl);
                if (got >= len || !(flags & MSG_WAITALL_) || (flags & MSG_PEEK_)) return (int64_t)got;
                continue;
            }
            int eof = 0, err = 0;
            if (s->kind == K_UNIX) eof = s->peer_closed || s->peer < 0;
            else {
                if (s->fin_rcvd || s->shut_rd) eof = 1;
                if (s->st == T_CLOSED) { eof = 1; err = s->err; s->err = 0; }
                else if (s->err && s->st != T_ESTAB) { err = s->err; s->err = 0; }
            }
            if (got) { irq_restore(fl); return (int64_t)got; }
            if (err) { irq_restore(fl); return -err; }
            if (eof) { irq_restore(fl); return 0; }
            if (s->kind == K_TCP && (s->st == T_CLOSED || s->st == T_LISTEN) && !s->rip) { irq_restore(fl); return -E_NOTCONN; }
            irq_restore(fl);
            if (nb) return -E_AGAIN;
        }
        int r = wait_step(dl, &spin);
        if (r < 0) return got ? (int64_t)got : r;
    }
}

int sock_shutdown(int id, int how)
{
    unsigned long fl = irq_save();
    struct ksock *s = S(id);
    if (!s) { irq_restore(fl); return -E_NOTSOCK; }
    if (s->kind == K_TCP && s->st == T_CLOSED) { irq_restore(fl); return -E_NOTCONN; }
    if (how == 0 || how == 2) s->shut_rd = 1;
    if (how == 1 || how == 2) {
        s->shut_wr = 1;
        if (s->kind == K_TCP && (s->st == T_ESTAB || s->st == T_CLOSE_WAIT)) {
            s->fin_queued = 1;
            tcp_output(s);
        }
        if (s->kind == K_UNIX) { struct ksock *p = S(s->peer); if (p) p->peer_closed = 1; }
    }
    irq_restore(fl);
    return 0;
}

int sock_name(int id, int peer, uint32_t *ip, uint16_t *port)
{
    struct ksock *s = S(id);
    if (!s) return -E_NOTSOCK;
    if (s->kind == K_UNIX) return -E_OPNOTSUPP;
    if (peer) {
        if (!s->rip) return -E_NOTCONN;
        if (s->kind == K_TCP && s->st != T_ESTAB && s->st != T_CLOSE_WAIT && s->st != T_FIN_WAIT1 &&
            s->st != T_FIN_WAIT2 && s->st != T_SYN_RCVD) return -E_NOTCONN;
        *ip = s->rip; *port = s->rport;
    } else {
        *ip = s->lip; *port = s->lport;
    }
    return 0;
}

int sock_getsockopt(int id, int level, int opt, int *val)
{
    struct ksock *s = S(id);
    if (!s) return -E_NOTSOCK;
    *val = 0;
    if (level == 1) {                                       /* SOL_SOCKET */
        if (opt == 4) { *val = s->err; s->err = 0; }        /* SO_ERROR */
        else if (opt == 3) *val = s->type;                  /* SO_TYPE */
        else if (opt == 7 || opt == 8) *val = TCP_BUF;      /* SO_SNDBUF / SO_RCVBUF */
        else if (opt == 39) *val = s->domain == AF_INET_ ? 0 : 0;
        else if (opt == 38) *val = s->proto;                /* SO_PROTOCOL */
        else if (opt == 30) *val = s->st == T_LISTEN;       /* SO_ACCEPTCONN */
    }
    return 0;
}

int sock_setsockopt(int id, int level, int opt, const void *val, uint32_t len)
{
    struct ksock *s = S(id);
    if (!s) return -E_NOTSOCK;
    if (level == 1 && (opt == 20 || opt == 21) && len >= 16) {     /* SO_RCVTIMEO / SO_SNDTIMEO */
        const int64_t *tv = (const int64_t *)val;
        int ms = (int)(tv[0] * 1000 + tv[1] / 1000);
        if (opt == 20) s->rcvtimeo_ms = ms; else s->sndtimeo_ms = ms;
    }
    return 0;
}

int sock_set_nonblock(int id, int on)
{
    struct ksock *s = S(id);
    if (!s) return -E_NOTSOCK;
    s->nonblock = on;
    return 0;
}

int sock_get_nonblock(int id)
{
    struct ksock *s = S(id);
    return s ? s->nonblock : 0;
}

int sock_domain(int id) { struct ksock *s = S(id); return s ? s->domain : 0; }
int sock_type(int id)   { struct ksock *s = S(id); return s ? s->type : 0; }

int sock_unread(int id)
{
    struct ksock *s = S(id);
    if (!s) return 0;
    if (s->kind == K_UDP || s->kind == K_RAW) return s->dq_n ? s->dq[s->dq_head].len : 0;
    return (int)s->r_len;
}
