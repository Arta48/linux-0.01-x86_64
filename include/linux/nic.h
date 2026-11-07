#ifndef _LINUX_NIC_H
#define _LINUX_NIC_H

#include <linux/types.h>

/* Единый интерфейс сетевых карт: стек (net.c / net_stack.c) работает только
 * с ним, конкретные драйверы (e1000, Realtek RTL8168/8169) регистрируют ops. */
struct nic_ops {
    const char *name;                       /* "Realtek RTL8168/8111" */
    int  (*send)(const void *data, uint16_t len);
    int  (*recv)(void *buf, uint16_t max_len);
    int  (*link)(uint32_t *speed_mbps, int *full_duplex);   /* 1 = линк есть */
    uint8_t mac[6];
};

int  nic_init(void);                        /* найти и запустить карту; 0 = есть */
int  nic_present(void);
const char *nic_name(void);
void nic_get_mac(uint8_t out_mac[6]);
int  nic_send(const void *data, uint16_t len);
int  nic_recv(void *buf, uint16_t max_len);
int  nic_link(uint32_t *speed_mbps, int *full_duplex);
void nic_register(const struct nic_ops *ops);

/* Время без прерываний: TSC, откалиброванный по каналу 2 PIT */
void     tsc_calibrate(void);
uint64_t tsc_get_khz(void);
uint64_t time_us(void);
void     udelay(uint32_t us);

unsigned long irq_save(void);
void          irq_restore(unsigned long flags);

int  rtl8169_init(void);
int  e1000_init(void);

#endif
