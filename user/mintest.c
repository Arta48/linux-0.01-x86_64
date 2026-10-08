#include "ulibc.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("====================================================\n");
    printf("   Minix v1 Filesystem Persistence Test (/mnt)      \n");
    printf("====================================================\n\n");

    /* 1. Проверяем счетчик перезапусков /mnt/boot_count.txt */
    int boot_count = 1;
    int fd = open("/mnt/boot_count.txt", O_RDONLY);
    if (fd >= 0) {
        char buf[32];
        memset(buf, 0, sizeof(buf));
        int64_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            boot_count = (int)atoi(buf) + 1;
        }
        printf("[INFO] Existing boot counter found: %d. Incrementing...\n", boot_count - 1);
    } else {
        printf("[INFO] No existing boot counter found. Starting first session (count = 1).\n");
    }

    /* Записываем обновленный счетчик */
    fd = open("/mnt/boot_count.txt", O_CREAT | O_WRONLY | O_TRUNC);
    if (fd < 0) {
        printf("[FAIL] Cannot open /mnt/boot_count.txt for writing!\n");
        return 1;
    }
    char count_str[32];
    snprintf(count_str, sizeof(count_str), "%d\n", boot_count);
    write(fd, count_str, strlen(count_str));
    close(fd);
    printf("[PASS] Updated /mnt/boot_count.txt -> %d\n", boot_count);

    /* 2. Создаем структуру каталогов на диске */
    mkdir("/mnt/test_dir");

    /* 3. Создаем тестовый файл внутри подкаталога */
    const char *test_path = "/mnt/test_dir/persist.log";
    fd = open(test_path, O_CREAT | O_WRONLY | O_APPEND);
    if (fd < 0) {
        printf("[FAIL] Cannot create file %s\n", test_path);
        return 1;
    }

    char log_entry[128];
    snprintf(log_entry, sizeof(log_entry), "Boot session #%d: Minix v1 persistence verified at %d!\n",
             boot_count, (int)time());
    write(fd, log_entry, strlen(log_entry));
    close(fd);
    printf("[PASS] Appended session log to %s\n", test_path);

    /* 4. Синхронизируем буферы на диск */
    sync();
    printf("[PASS] Filesystem synchronized with disk via sys_sync().\n");

    /* 5. Читаем и выводим метаданные stat */
    struct stat st;
    if (stat(test_path, &st) == 0) {
        printf("\nFile Metadata (%s):\n", test_path);
        printf("  Inode number : %d\n", (int)st.st_ino);
        printf("  File size    : %d bytes\n", (int)st.st_size);
        printf("  Permissions  : 0%o\n", st.st_mode & 0777);
        printf("  Device ID    : %d (Minix v1 on /dev/hda)\n", (int)st.st_dev);
    }

    /* 6. Считываем обратно лог-файл целиком */
    printf("\nReading back all session records from persistent disk:\n");
    fd = open(test_path, O_RDONLY);
    if (fd >= 0) {
        char readbuf[512];
        int64_t bytes;
        while ((bytes = read(fd, readbuf, sizeof(readbuf) - 1)) > 0) {
            readbuf[bytes] = '\0';
            printf("%s", readbuf);
        }
        close(fd);
    }

    printf("\n====================================================\n");
    if (boot_count > 1) {
        printf(" [SUCCESS] PERSISTENCE CONFIRMED ACROSS %d REBOOTS! \n", boot_count);
    } else {
        printf(" [SUCCESS] First boot session written to disk.\n");
        printf(" -> Restart QEMU and run mintest again to verify persistence!\n");
    }
    printf("====================================================\n");
    return 0;
}
