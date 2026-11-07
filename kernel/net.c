#include <linux/net.h>
#include <linux/nic.h>
#include <linux/string.h>
#include <linux/tty.h>

static uint8_t host_mac[6];

void net_init(void)
{
    if (nic_init() == 0) {
        nic_get_mac(host_mac);
        printk("[OK] Ethernet Layer (%s): MAC %02x:%02x:%02x:%02x:%02x:%02x, DHCP in background (netd)\n",
               nic_name(), host_mac[0], host_mac[1], host_mac[2], host_mac[3], host_mac[4], host_mac[5]);
        extern void net_stack_init(void);
        net_stack_init();
    }
}

int net_send_packet(const uint8_t dest_mac[6], uint16_t ethertype, const void *payload, uint16_t len)
{
    uint8_t frame[1518];
    if (len + sizeof(struct eth_header) > sizeof(frame)) return -1;

    struct eth_header *eh = (struct eth_header *)frame;
    memcpy(eh->dest, dest_mac, 6);
    memcpy(eh->src, host_mac, 6);
    eh->proto = ((ethertype >> 8) & 0xFF) | ((ethertype << 8) & 0xFF00);

    memcpy(frame + sizeof(struct eth_header), payload, len);
    uint16_t total_len = sizeof(struct eth_header) + len;
    if (total_len < 60) total_len = 60;
    return nic_send(frame, total_len);
}

int net_recv_packet(void *buf, uint16_t max_len)
{
    return nic_recv(buf, max_len);
}
