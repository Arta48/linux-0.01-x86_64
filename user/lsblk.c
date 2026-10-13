#include "ulibc.h"

/* lsblk: диски и разделы из /proc/disks */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    int fd = open("/proc/disks", O_RDONLY);
    if (fd < 0) {
        printf("lsblk: cannot open /proc/disks\n");
        return 1;
    }

    char buf[2048];
    int n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }
    close(fd);
    return 0;
}
