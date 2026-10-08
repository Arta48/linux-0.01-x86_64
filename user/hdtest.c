#include "ulibc.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("========================================\n");
    printf("  ATA Hard Disk (/dev/hda) Sector Test  \n");
    printf("========================================\n");

    int fd = open("/dev/hda", O_RDWR);
    if (fd < 0) {
        printf("[FAIL] Cannot open /dev/hda (device not found or permission denied)\n");
        return 1;
    }

    /* 1. Запись сигнатуры в сектор 1 (смещение 512 байт) */
    const char *payload = "=== LINUX 0.01 x86_64 HARD DISK PERSISTENCE TEST PASSED! ===\n";
    size_t payload_len = strlen(payload);

    printf("[TEST] Writing test payload to /dev/hda at offset 512...\n");
    /* Смещение на сектор 1 */
    char dummy[512];
    read(fd, dummy, 512);

    int64_t written = write(fd, payload, payload_len);
    if (written < 0) {
        printf("[FAIL] Failed to write to /dev/hda\n");
        close(fd);
        return 1;
    }
    printf("[PASS] Successfully wrote %d bytes to sector 1\n", (int)written);
    close(fd);

    /* 2. Повторное открытие и верификация содержимого */
    printf("[TEST] Re-opening /dev/hda and verifying written data...\n");
    fd = open("/dev/hda", O_RDONLY);
    if (fd < 0) {
        printf("[FAIL] Cannot re-open /dev/hda\n");
        return 1;
    }

    read(fd, dummy, 512); /* Пропускаем сектор 0 */

    char readback[128];
    memset(readback, 0, sizeof(readback));
    int64_t bytes_read = read(fd, readback, payload_len);
    close(fd);

    if (bytes_read != (int64_t)payload_len) {
        printf("[FAIL] Readback byte count mismatch (%d vs %d)\n", (int)bytes_read, (int)payload_len);
        return 1;
    }

    if (strcmp(readback, payload) == 0) {
        printf("[PASS] Verified readback: %s", readback);
        printf("[SUCCESS] ATA Driver read/write operations fully functional!\n");
        return 0;
    } else {
        printf("[FAIL] Data mismatch! Read back: '%s'\n", readback);
        return 1;
    }
}
