#ifndef _LINUX_TCP_H
#define _LINUX_TCP_H

#include <linux/types.h>
#include <linux/ip.h>

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_URG 0x20

#define TCP_STATE_CLOSED      0
#define TCP_STATE_LISTEN      1
#define TCP_STATE_SYN_RCVD    2
#define TCP_STATE_ESTABLISHED 3
#define TCP_STATE_CLOSE_WAIT  4
#define TCP_STATE_LAST_ACK    5

struct tcp_header {
    uint16_t src_port;
    uint16_t dest_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset;   /* Смещение данных в 32-битных словах (верхние 4 бита) */
    uint8_t  flags;
    uint16_t window_size;
    uint16_t checksum;
    uint16_t urgent_ptr;
} __attribute__((packed));

struct tcp_socket {
    int in_use;
    int state;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t remote_ip;
    uint8_t  remote_mac[6];
    uint32_t my_seq;
    uint32_t remote_seq;
    char rx_buf[2048];
    uint16_t rx_len;
    uint16_t rx_pos;
    int is_accepted;       /* 1 = уже отдан в accept(), 0 = ожидает обработки */
};

#define MAX_TCP_SOCKETS 16
extern struct tcp_socket tcp_sockets[MAX_TCP_SOCKETS];

int tcp_socket_create(void);
int tcp_socket_bind(int sock_id, uint16_t port);
int tcp_socket_listen(int sock_id);
int tcp_socket_accept(int sock_id);
int tcp_socket_read(int sock_id, char *buf, uint64_t count);
int tcp_socket_write(int sock_id, const char *buf, uint64_t count);
int tcp_socket_close(int sock_id);

void tcp_handle_packet(const struct ip_header *ip, const struct tcp_header *tcp, uint16_t len, const uint8_t src_mac[6]);

#endif
