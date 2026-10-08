#ifndef _LINUX_HDREG_H
#define _LINUX_HDREG_H

#include <linux/types.h>

#define ATA_PRIMARY_DATA         0x1F0
#define ATA_PRIMARY_ERR          0x1F1
#define ATA_PRIMARY_FEATURES     0x1F1
#define ATA_PRIMARY_SEC_CNT      0x1F2
#define ATA_PRIMARY_LBA_LOW      0x1F3
#define ATA_PRIMARY_LBA_MID      0x1F4
#define ATA_PRIMARY_LBA_HIGH     0x1F5
#define ATA_PRIMARY_DRIVE_HEAD   0x1F6
#define ATA_PRIMARY_STATUS       0x1F7
#define ATA_PRIMARY_COMMAND      0x1F7
#define ATA_PRIMARY_CONTROL      0x3F6

#define ATA_SR_ERR  0x01
#define ATA_SR_DRQ  0x08
#define ATA_SR_DF   0x20
#define ATA_SR_RDY  0x40
#define ATA_SR_BSY  0x80

#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_CACHE_FLUSH 0xE7
#define ATA_CMD_IDENTIFY    0xEC

#define SECTOR_SIZE 512

struct hd_drive_info {
    int present;
    uint32_t sectors;
    uint32_t size_mb;
    char model[41];
};

void ide_init(void);
int ide_read_sectors(uint8_t drive, uint32_t lba, uint8_t count, void *buf);
int ide_write_sectors(uint8_t drive, uint32_t lba, uint8_t count, const void *buf);
const struct hd_drive_info *ide_get_drive(uint8_t drive);

#endif
