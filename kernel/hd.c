#include <linux/hdreg.h>
#include <linux/tty.h>
#include <linux/string.h>
#include <asm/io.h>

static struct hd_drive_info drives[2];

static void ata_io_wait(void)
{
    inb(ATA_PRIMARY_CONTROL);
    inb(ATA_PRIMARY_CONTROL);
    inb(ATA_PRIMARY_CONTROL);
    inb(ATA_PRIMARY_CONTROL);
}

static int ata_poll(int check_err)
{
    ata_io_wait();

    int timeout = 100000;
    while ((inb(ATA_PRIMARY_STATUS) & ATA_SR_BSY) && --timeout);
    if (timeout == 0) return -1;

    uint8_t status = inb(ATA_PRIMARY_STATUS);
    if (check_err) {
        if (status & ATA_SR_ERR) return -1;
        if (status & ATA_SR_DF)  return -1;
        if (!(status & ATA_SR_DRQ)) return -1;
    }
    return 0;
}

static void ide_identify(uint8_t drive)
{
    drives[drive].present = 0;

    outb(0xA0 | (drive << 4), ATA_PRIMARY_DRIVE_HEAD);
    ata_io_wait();

    outb(0, ATA_PRIMARY_SEC_CNT);
    outb(0, ATA_PRIMARY_LBA_LOW);
    outb(0, ATA_PRIMARY_LBA_MID);
    outb(0, ATA_PRIMARY_LBA_HIGH);
    outb(ATA_CMD_IDENTIFY, ATA_PRIMARY_COMMAND);
    ata_io_wait();

    uint8_t status = inb(ATA_PRIMARY_STATUS);
    if (status == 0) return;

    if (ata_poll(0) < 0) return;

    uint8_t mid  = inb(ATA_PRIMARY_LBA_MID);
    uint8_t high = inb(ATA_PRIMARY_LBA_HIGH);
    if (mid != 0 || high != 0) return;

    if (ata_poll(1) < 0) return;

    uint16_t id_buf[256];
    insw(ATA_PRIMARY_DATA, id_buf, 256);

    char *raw_model = (char *)&id_buf[27];
    for (int i = 0; i < 40; i += 2) {
        drives[drive].model[i]     = raw_model[i + 1];
        drives[drive].model[i + 1] = raw_model[i];
    }
    drives[drive].model[40] = '\0';

    int len = 39;
    while (len >= 0 && drives[drive].model[len] == ' ') {
        drives[drive].model[len--] = '\0';
    }

    /* Безопасное чтение 32-битного значения без нарушения strict-aliasing */
    uint32_t sectors = (uint32_t)id_buf[60] | ((uint32_t)id_buf[61] << 16);
    drives[drive].sectors = sectors;
    drives[drive].size_mb = (sectors / 2048);
    drives[drive].present = 1;

    printk("[OK] ATA Drive %d: %s (%d MB, %d sectors)\n",
           drive, drives[drive].model, (int)drives[drive].size_mb, (int)sectors);
}

void ide_init(void)
{
    ide_identify(0);
}

const struct hd_drive_info *ide_get_drive(uint8_t drive)
{
    if (drive > 1) return NULL;
    return &drives[drive];
}

int ide_read_sectors(uint8_t drive, uint32_t lba, uint8_t count, void *buf)
{
    if (count == 0 || !drives[drive].present) return -1;

    if (ata_poll(0) < 0) return -1;

    outb(0xE0 | (drive << 4) | ((lba >> 24) & 0x0F), ATA_PRIMARY_DRIVE_HEAD);
    ata_io_wait();

    outb(count, ATA_PRIMARY_SEC_CNT);
    outb((uint8_t)(lba & 0xFF), ATA_PRIMARY_LBA_LOW);
    outb((uint8_t)((lba >> 8) & 0xFF), ATA_PRIMARY_LBA_MID);
    outb((uint8_t)((lba >> 16) & 0xFF), ATA_PRIMARY_LBA_HIGH);
    outb(ATA_CMD_READ_PIO, ATA_PRIMARY_COMMAND);

    uint16_t *ptr = (uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (ata_poll(1) < 0) return -1;
        insw(ATA_PRIMARY_DATA, ptr, 256);
        ptr += 256;
    }
    return 0;
}

int ide_write_sectors(uint8_t drive, uint32_t lba, uint8_t count, const void *buf)
{
    if (count == 0 || !drives[drive].present) return -1;

    if (ata_poll(0) < 0) return -1;

    outb(0xE0 | (drive << 4) | ((lba >> 24) & 0x0F), ATA_PRIMARY_DRIVE_HEAD);
    ata_io_wait();

    outb(count, ATA_PRIMARY_SEC_CNT);
    outb((uint8_t)(lba & 0xFF), ATA_PRIMARY_LBA_LOW);
    outb((uint8_t)((lba >> 8) & 0xFF), ATA_PRIMARY_LBA_MID);
    outb((uint8_t)((lba >> 16) & 0xFF), ATA_PRIMARY_LBA_HIGH);
    outb(ATA_CMD_WRITE_PIO, ATA_PRIMARY_COMMAND);

    const uint16_t *ptr = (const uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (ata_poll(1) < 0) return -1;
        outsw(ATA_PRIMARY_DATA, ptr, 256);
        ptr += 256;
    }

    outb(ATA_CMD_CACHE_FLUSH, ATA_PRIMARY_COMMAND);
    ata_poll(0);
    return 0;
}
