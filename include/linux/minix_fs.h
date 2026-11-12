#ifndef _LINUX_MINIX_FS_H
#define _LINUX_MINIX_FS_H

#include <linux/types.h>

#define MINIX_BLOCK_SIZE     1024
#define MINIX_SUPER_MAGIC    0x137F
#define MINIX_NAME_LEN       14
#define MINIX_ROOT_INO       1
#define MINIX_INODES_PER_BLK (MINIX_BLOCK_SIZE / sizeof(struct minix_inode))
#define MINIX_DIR_ENT_PER_BLK (MINIX_BLOCK_SIZE / sizeof(struct minix_dir_entry))

struct minix_super_block {
    uint16_t s_ninodes;         /* Количество инодов */
    uint16_t s_nzones;          /* Количество блоков (зон) */
    uint16_t s_imap_blocks;     /* Блоки битовой карты инодов */
    uint16_t s_zmap_blocks;     /* Блоки битовой карты зон */
    uint16_t s_firstdatazone;   /* Первый блок данных */
    uint16_t s_log_zone_size;   /* Размер зоны (0 для 1024B) */
    uint32_t s_max_size;        /* Максимальный размер файла */
    uint16_t s_magic;           /* Сигнатура: 0x137F */
    uint16_t s_state;           /* Состояние ФС (1 = чисто) */
    uint32_t s_zones;           /* Полное число зон */
} __attribute__((packed));

struct minix_inode {
    uint16_t i_mode;            /* Флаги доступа и тип файла (S_IFREG, S_IFDIR) */
    uint16_t i_uid;             /* Идентификатор владельца */
    uint32_t i_size;            /* Размер файла в байтах */
    uint32_t i_time;            /* Время последней модификации (epoch) */
    uint8_t  i_gid;             /* Идентификатор группы */
    uint8_t  i_nlinks;          /* Количество жестких ссылок */
    uint16_t i_zone[9];         /* 7 прямых, 1 косвенный, 1 дважды косвенный блок */
} __attribute__((packed));

struct minix_dir_entry {
    uint16_t inode;             /* Номер инода (0 = запись свободна) */
    char     name[MINIX_NAME_LEN]; /* Имя файла (до 14 символов) */
} __attribute__((packed));

struct stat;

void minix_init(void);
int  minix_mount(uint8_t dev);
int  minix_format(uint8_t dev, uint32_t total_blocks, uint32_t inodes_count);
int  minix_sys_open(const char *path, int flags, uint16_t mode, uint32_t *out_ino, uint64_t *out_size);
int64_t minix_file_read(uint32_t ino, uint64_t *pos, char *buf, uint64_t count);
int64_t minix_file_write(uint32_t ino, uint64_t *pos, const char *buf, uint64_t count);
int  minix_sys_unlink(const char *path);
int  minix_sys_mkdir(const char *path, uint16_t mode);
int  minix_sys_rmdir(const char *path);
int  minix_sys_stat(const char *path, struct stat *st);
int64_t minix_sys_list(const char *path, char *buf, uint64_t max_len, int is_long);
int  minix_is_dir(const char *path);
const char *minix_get_file_data(const char *path, uint64_t *out_size);
void minix_sync(void);

#endif
