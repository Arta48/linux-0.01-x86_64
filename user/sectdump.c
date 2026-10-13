#include "ulibc.h"

/* sectdump <устройство> [LBA] [число секторов]: hex-дамп секторов (только чтение).
 * Пример: sectdump nvme0n1 1   -- заголовок GPT ("EFI PART") */
static void dump_sector(const unsigned char *b, int64_t lba)
{
    printf("--- LBA %d ---\n", (int)lba);
    for (int row = 0; row < 512; row += 16) {
        printf("%x%x%x: ", (row >> 8) & 15, (row >> 4) & 15, row & 15);
        for (int i = 0; i < 16; i++) {
            unsigned char v = b[row + i];
            printf("%x%x ", v >> 4, v & 15);
        }
        printf(" |");
        for (int i = 0; i < 16; i++) {
            unsigned char v = b[row + i];
            printf("%c", (v >= 32 && v < 127) ? v : '.');
        }
        printf("|\n");
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: sectdump <device> [lba] [count]   (e.g. sectdump nvme0n1 1)\n");
        return 1;
    }
    char path[64];
    const char *dev = argv[1];
    if (dev[0] == '/') snprintf(path, sizeof(path), "%s", dev);
    else snprintf(path, sizeof(path), "/dev/%s", dev);

    int64_t lba = argc > 2 ? atoi(argv[2]) : 0;
    int64_t cnt = argc > 3 ? atoi(argv[3]) : 1;
    if (cnt < 1) cnt = 1;
    if (cnt > 8) cnt = 8;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("sectdump: cannot open %s\n", path);
        return 1;
    }
    if (lseek(fd, lba * 512, SEEK_SET) < 0) {
        printf("sectdump: LBA %d is beyond the end of %s\n", (int)lba, path);
        close(fd);
        return 1;
    }
    unsigned char sec[512];
    for (int64_t i = 0; i < cnt; i++) {
        if (read(fd, sec, 512) != 512) {
            printf("sectdump: read error at LBA %d\n", (int)(lba + i));
            close(fd);
            return 1;
        }
        dump_sector(sec, lba + i);
    }
    close(fd);
    return 0;
}
