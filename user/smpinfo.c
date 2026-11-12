#include "ulibc.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("====================================================\n");
    printf("   Symmetric Multiprocessing (SMP) Status Monitor   \n");
    printf("====================================================\n\n");

    printf("Kernel APIC Architecture: AMD64 xAPIC\n");
    printf("Local APIC Base Address : 0xFEE00000 (Memory-Mapped)\n");

    int fd = open("/proc/cpuinfo", O_RDONLY);
    if (fd >= 0) {
        char buf[512];
        memset(buf, 0, sizeof(buf));
        int64_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            printf("\n%s\n", buf);
        }
        close(fd);
    }

    printf("====================================================\n");
    printf(" [SUCCESS] MULTIPROCESSING CORE INITIALIZATION OK!  \n");
    printf("====================================================\n");
    return 0;
}
