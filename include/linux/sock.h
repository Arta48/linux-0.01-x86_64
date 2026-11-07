#ifndef _LINUX_SOCK_H
#define _LINUX_SOCK_H

#include <linux/types.h>

/* Сокеты BSD поверх сетевого стека ядра: TCP (клиент и сервер), UDP, ICMP (raw и "ping"),
 * а также пары AF_UNIX (socketpair). Идентификатор сокета хранится в struct file.sock_id. */

#define AF_UNIX_   1
#define AF_INET_   2
#define SOCK_STREAM_ 1
#define SOCK_DGRAM_  2
#define SOCK_RAW_    3

#define SOCK_POLLIN  0x001
#define SOCK_POLLOUT 0x004
#define SOCK_POLLERR 0x008
#define SOCK_POLLHUP 0x010

#define MSG_PEEK_      0x02
#define MSG_DONTWAIT_  0x40
#define MSG_WAITALL_   0x100

int  sock_create(int domain, int type, int proto);        /* id сокета или -errno */
int  sock_eventfd(unsigned initval, int flags);               /* eventfd2 */
int  sock_pair(int type, int ids[2]);
void sock_ref(int id);                                    /* ещё один дескриптор (dup, fork) */
void sock_close(int id);                                  /* один дескриптор закрыт */

int  sock_bind(int id, uint32_t ip, uint16_t port);
int  sock_listen(int id, int backlog);
int  sock_accept(int id, uint32_t *ip, uint16_t *port);   /* id нового сокета или -errno */
int  sock_connect(int id, uint32_t ip, uint16_t port);
int64_t sock_send(int id, const void *buf, uint64_t len, int flags, uint32_t ip, uint16_t port, int has_addr);
int64_t sock_recv(int id, void *buf, uint64_t len, int flags, uint32_t *ip, uint16_t *port);
int  sock_shutdown(int id, int how);
int  sock_poll(int id);                                   /* маска SOCK_POLL* */
int  sock_name(int id, int peer, uint32_t *ip, uint16_t *port);
int  sock_getsockopt(int id, int level, int opt, int *val);
int  sock_setsockopt(int id, int level, int opt, const void *val, uint32_t len);
int  sock_set_nonblock(int id, int on);
int  sock_get_nonblock(int id);
int  sock_domain(int id);                                 /* AF_* сокета */
int  sock_type(int id);
int  sock_unread(int id);                                 /* FIONREAD */

/* вызываются сетевым стеком */
void sock_tcp_rx(uint32_t src, uint32_t dst, const uint8_t *seg, uint16_t len);
void sock_udp_rx(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport, const uint8_t *data, uint16_t len);
void sock_icmp_rx(uint32_t src, uint32_t dst, const uint8_t *icmp, uint16_t len, const uint8_t *iphdr);
void sock_timer(void);

#endif
