#ifndef _LINUX_BLKDEV_H
#define _LINUX_BLKDEV_H

#include <linux/types.h>

/*
 * Общий блочный слой. Все диски и разделы -- устройства с номером dev
 * (индекс в таблице); секторы всегда по 512 байт. Номер 0 зарезервирован
 * за ATA/IDE hda (исторически dev 0 = первый диск).
 *
 * ЗАЩИТА ДАННЫХ: NVMe/SATA-диски и их разделы по умолчанию доступны только
 * на чтение. Запись разрешается лишь разделу, на котором ядро нашло Minix v1
 * и смонтировало его в /mnt (blk_set_writable()).
 */
#define BLK_MAX   40
#define BLK_IDE    0
#define BLK_NVME   1
#define BLK_AHCI   2

struct blk_dev {
    uint8_t  present;
    uint8_t  kind;            /* BLK_IDE / BLK_NVME / BLK_AHCI */
    uint8_t  is_part;
    uint8_t  writable;
    uint8_t  has_ptable;      /* на диске найдена таблица разделов (MBR/GPT) */
    uint8_t  wr_child;        /* у диска есть записываемый раздел (нужен flush) */
    uint8_t  pad[2];
    int16_t  parent;          /* индекс диска для раздела, -1 для диска */
    uint32_t lba_bytes;       /* родной размер сектора диска (512 или 4096) */
    uint64_t sectors;         /* размер в секторах по 512 байт */
    uint64_t start;           /* первый сектор раздела на диске (для раздела) */
    char     name[16];        /* "hda", "nvme0n1", "nvme0n1p2", "sda", "sda1" */
    char     model[41];
    char     ptype[20];       /* тип раздела: "EFI System", "Linux", "Minix v1" ... */
    /* Операции корневого диска (копируются в разделы) */
    int    (*rw)(void *priv, int write, uint64_t lba, uint32_t count, void *buf);
    int    (*flush)(void *priv);
    void    *priv;
};

void  storage_init(void);                       /* IDE + NVMe + AHCI + разделы */
void  blk_init(void);                           /* регистрирует IDE (hda) как dev 0 */
int   blk_add_disk(const struct blk_dev *proto);/* возвращает dev или -1 */
int   blk_count(void);
struct blk_dev *blk_get(int dev);
int   blk_rw(int dev, int write, uint64_t lba, uint32_t count, void *buf);
int   blk_read(int dev, uint64_t lba, uint32_t count, void *buf);
int   blk_write(int dev, uint64_t lba, uint32_t count, const void *buf);
void  blk_flush_all(void);
void  blk_set_writable(int dev, int on);
int   blk_find_minix(void);                     /* dev с Minix v1 (не IDE) или -1 */
void  blk_scan_partitions(int disk);
void  blk_proc_dump(char *buf, uint64_t max_len);

/* Драйверы накопителей */
int   nvme_init(void);                          /* число найденных пространств имён */
int   ahci_init(void);                          /* число найденных SATA-дисков */

#endif
