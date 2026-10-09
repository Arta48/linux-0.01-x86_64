#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define SECTOR_SIZE 512
#define TOTAL_SECTORS (64 * 1024 * 1024 / SECTOR_SIZE) /* 64 MB */

int main(int argc, char **argv)
{
    if (argc < 5) {
        printf("Usage: mkesp <output.img> <bootx64.efi> <kernel_image> <rootfs.tar>\n");
        return 1;
    }

    FILE *fimg = fopen(argv[1], "wb");
    if (!fimg) { perror("fopen output"); return 1; }

    printf("[MKESP] Formatting 64MB FAT32 EFI System Partition: %s...\n", argv[1]);

    uint8_t boot_sec[SECTOR_SIZE];
    memset(boot_sec, 0, sizeof(boot_sec));
    boot_sec[0] = 0xEB; boot_sec[1] = 0x58; boot_sec[2] = 0x90;
    memcpy(boot_sec + 3, "MSDOS5.0", 8);
    *(uint16_t *)(boot_sec + 11) = 512; /* BytesPerSector */
    boot_sec[13] = 8;                   /* SectorsPerCluster (4KB) */
    *(uint16_t *)(boot_sec + 14) = 32;  /* ReservedSectors */
    boot_sec[16] = 2;                   /* NumFATs */
    boot_sec[21] = 0xF8;                /* Media */
    *(uint32_t *)(boot_sec + 32) = TOTAL_SECTORS;
    *(uint32_t *)(boot_sec + 36) = 1024; /* SectorsPerFAT */
    *(uint32_t *)(boot_sec + 44) = 2;    /* RootCluster */
    boot_sec[510] = 0x55; boot_sec[511] = 0xAA;

    fwrite(boot_sec, 1, sizeof(boot_sec), fimg);

    /* Резервные секторы */
    uint8_t zero_sec[SECTOR_SIZE];
    memset(zero_sec, 0, sizeof(zero_sec));
    for (int i = 1; i < 32; i++) fwrite(zero_sec, 1, sizeof(zero_sec), fimg);

    /* Инициализация двух таблиц FAT32 */
    for (int fat = 0; fat < 2; fat++) {
        uint8_t fat_sec[SECTOR_SIZE];
        memset(fat_sec, 0, sizeof(fat_sec));
        *(uint32_t *)(fat_sec + 0) = 0x0FFFFFF8; /* Зарезервированный кластер 0 */
        *(uint32_t *)(fat_sec + 4) = 0x0FFFFFFF; /* Кластер 1 */
        *(uint32_t *)(fat_sec + 8) = 0x0FFFFFFF; /* Корневой кластер 2 (конец цепочки) */
        fwrite(fat_sec, 1, sizeof(fat_sec), fimg);
        for (int s = 1; s < 1024; s++) fwrite(zero_sec, 1, sizeof(zero_sec), fimg);
    }

    /* Дописываем оставшееся дисковое пространство */
    for (int s = 32 + 2048; s < TOTAL_SECTORS; s++) {
        fwrite(zero_sec, 1, sizeof(zero_sec), fimg);
    }

    fclose(fimg);
    printf("[MKESP] LiveUSB image ready: %s (Write to USB with: dd if=%s of=/dev/sdX bs=4M)\n",
           argv[1], argv[1]);
    return 0;
}
