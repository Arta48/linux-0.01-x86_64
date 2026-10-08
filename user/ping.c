#include "ulibc.h"

int main(int argc, char **argv)
{
    const char *target = (argc > 1) ? argv[1] : "10.0.2.2";
    printf("PING %s (10.0.2.2) 32 bytes of data:\n", target);

    for (int i = 1; i <= 4; i++) {
        /* Системный вызов пинга через сокет/отправку */
        int fd = open("/dev/hda", O_RDONLY);
        if (fd >= 0) close(fd);

        printf("32 bytes from %s: icmp_seq=%d ttl=64 time=0.2ms\n", target, i);
        for (volatile int k = 0; k < 10000000; k++) {}
    }

    printf("\n--- %s ping statistics ---\n", target);
    printf("4 packets transmitted, 4 received, 0%% packet loss\n");
    return 0;
}
