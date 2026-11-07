#include "ulibc.h"

static void pr_ip(const char *label, uint32_t ip)
{
    printf("%s: %d.%d.%d.%d\n", label, (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

int main(int argc, char **argv)
{
    struct net_info ni;
    if (argc > 1 && strcmp(argv[1], "renew") == 0) {
        printf("DHCP renew...\n");
        if (netctl(NETCTL_DHCP, 0, 0, 0) < 0) printf("DHCP: no answer\n");
    }
    if (netctl(NETCTL_INFO, (uint64_t)&ni, 0, 0) < 0 || !ni.present) {
        printf("No supported Ethernet controller (Realtek RTL81xx / Intel e1000) found.\n");
        return 1;
    }
    printf("Ethernet Controller: %s\n", ni.name);
    printf("MAC Address        : %02x:%02x:%02x:%02x:%02x:%02x\n",
           ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5]);
    if (ni.link) printf("Link               : UP, %d Mbps, %s duplex\n", (int)ni.speed, ni.full ? "full" : "half");
    else         printf("Link               : DOWN (no cable or auto-negotiation in progress)\n");
    if (!ni.ip) {
        printf("IPv4               : not configured yet (DHCP in progress; try `netinfo renew`)\n");
        return 0;
    }
    printf("Address source     : %s\n", ni.dhcp == 1 ? "DHCP" : "fallback static (DHCP gave no answer)");
    pr_ip("IPv4 address       ", ni.ip);
    pr_ip("Netmask            ", ni.mask);
    pr_ip("Gateway            ", ni.gw);
    pr_ip("DNS server         ", ni.dns);
    return 0;
}
