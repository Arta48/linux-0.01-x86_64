#include <linux/minix_fs.h>
#include <linux/buffer.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/time.h>
#include <linux/tty.h>
#include <linux/mm.h>
#include <linux/hdreg.h>

static struct minix_super_block sb;
static int minix_mounted = 0;
static uint8_t minix_dev = 0;

static inline int test_bit(int nr, const void *addr)
{
    const uint8_t *p = (const uint8_t *)addr;
    return (p[nr >> 3] & (1 << (nr & 7))) != 0;
}

static inline void set_bit(int nr, void *addr)
{
    uint8_t *p = (uint8_t *)addr;
    p[nr >> 3] |= (1 << (nr & 7));
}

static inline void clear_bit(int nr, void *addr)
{
    uint8_t *p = (uint8_t *)addr;
    p[nr >> 3] &= ~(1 << (nr & 7));
}

int minix_format(uint8_t dev, uint32_t total_blocks, uint32_t inodes_count)
{
    printk("[MINIX] Formatting /dev/hda with Minix v1 Filesystem (%d blocks, %d inodes)...\n",
           total_blocks, inodes_count);

    uint16_t imap_blocks = (inodes_count + 8191) / 8192;
    uint16_t zmap_blocks = (total_blocks + 8191) / 8192;
    uint16_t inode_blocks = (inodes_count * sizeof(struct minix_inode) + MINIX_BLOCK_SIZE - 1) / MINIX_BLOCK_SIZE;
    uint16_t first_data_zone = 2 + imap_blocks + zmap_blocks + inode_blocks;

    struct minix_super_block nsb;
    memset(&nsb, 0, sizeof(nsb));
    nsb.s_ninodes = (uint16_t)inodes_count;
    nsb.s_nzones = (uint16_t)total_blocks;
    nsb.s_imap_blocks = imap_blocks;
    nsb.s_zmap_blocks = zmap_blocks;
    nsb.s_firstdatazone = first_data_zone;
    nsb.s_log_zone_size = 0;
    nsb.s_max_size = 7 * MINIX_BLOCK_SIZE + 512 * MINIX_BLOCK_SIZE;
    nsb.s_magic = MINIX_SUPER_MAGIC;
    nsb.s_state = 1;
    nsb.s_zones = total_blocks;

    /* 1. Записываем суперблок (блок 1) */
    struct buffer_head *bh = getblk(dev, 1);
    memset(bh->b_data, 0, MINIX_BLOCK_SIZE);
    memcpy(bh->b_data, &nsb, sizeof(nsb));
    bmark_dirty(bh);
    bwrite(bh);
    brelse(bh);

    /* 2. Инициализируем карту инодов (блок 2) */
    bh = getblk(dev, 2);
    memset(bh->b_data, 0, MINIX_BLOCK_SIZE);
    set_bit(0, bh->b_data); /* Инод 0 зарезервирован */
    set_bit(1, bh->b_data); /* Инод 1 - корневой каталог */
    bmark_dirty(bh);
    bwrite(bh);
    brelse(bh);

    /* 3. Инициализируем карту зон данных */
    for (uint16_t z = 0; z < zmap_blocks; z++) {
        bh = getblk(dev, 2 + imap_blocks + z);
        memset(bh->b_data, 0, MINIX_BLOCK_SIZE);
        if (z == 0) {
            set_bit(0, bh->b_data); /* Зона 0 зарезервирована */
            set_bit(1, bh->b_data); /* Зона 1 отдана под корневой каталог */
        }
        bmark_dirty(bh);
        bwrite(bh);
        brelse(bh);
    }

    /* 4. Инициализируем таблицу инодов */
    uint32_t itable_start = 2 + imap_blocks + zmap_blocks;
    for (uint16_t b = 0; b < inode_blocks; b++) {
        bh = getblk(dev, itable_start + b);
        memset(bh->b_data, 0, MINIX_BLOCK_SIZE);

        if (b == 0) {
            /* Инод 1: Корневой каталог / */
            struct minix_inode *root_ino = (struct minix_inode *)bh->b_data;
            root_ino->i_mode = 0040755; /* S_IFDIR | 0755 */
            root_ino->i_uid = 0;
            root_ino->i_gid = 0;
            root_ino->i_size = 2 * sizeof(struct minix_dir_entry);
            root_ino->i_time = (uint32_t)get_current_time();
            root_ino->i_nlinks = 2;
            root_ino->i_zone[0] = first_data_zone;
            for (int k = 1; k < 9; k++) root_ino->i_zone[k] = 0;
        }

        bmark_dirty(bh);
        bwrite(bh);
        brelse(bh);
    }

    /* 5. Записываем содержимое корневого каталога (блок first_data_zone) */
    bh = getblk(dev, first_data_zone);
    memset(bh->b_data, 0, MINIX_BLOCK_SIZE);
    struct minix_dir_entry *de = (struct minix_dir_entry *)bh->b_data;
    de[0].inode = 1;
    memcpy(de[0].name, ".", 2);
    de[1].inode = 1;
    memcpy(de[1].name, "..", 3);
    bmark_dirty(bh);
    bwrite(bh);
    brelse(bh);

    bflush();
    printk("[OK] Minix v1 Filesystem formatted successfully!\n");
    return 0;
}

static int read_inode(uint32_t ino, struct minix_inode *out_ino)
{
    if (ino == 0 || ino > sb.s_ninodes) return -1;
    uint32_t itable_start = 2 + sb.s_imap_blocks + sb.s_zmap_blocks;
    uint32_t block = itable_start + (ino - 1) / MINIX_INODES_PER_BLK;
    uint32_t offset = ((ino - 1) % MINIX_INODES_PER_BLK) * sizeof(struct minix_inode);

    struct buffer_head *bh = bread(minix_dev, block);
    if (!bh) return -1;

    memcpy(out_ino, bh->b_data + offset, sizeof(struct minix_inode));
    brelse(bh);
    return 0;
}

static int write_inode(uint32_t ino, const struct minix_inode *in_ino)
{
    if (ino == 0 || ino > sb.s_ninodes) return -1;
    uint32_t itable_start = 2 + sb.s_imap_blocks + sb.s_zmap_blocks;
    uint32_t block = itable_start + (ino - 1) / MINIX_INODES_PER_BLK;
    uint32_t offset = ((ino - 1) % MINIX_INODES_PER_BLK) * sizeof(struct minix_inode);

    struct buffer_head *bh = bread(minix_dev, block);
    if (!bh) return -1;

    memcpy(bh->b_data + offset, in_ino, sizeof(struct minix_inode));
    bmark_dirty(bh);
    bwrite(bh);
    brelse(bh);
    return 0;
}

static uint32_t alloc_block(void)
{
    for (uint16_t zb = 0; zb < sb.s_zmap_blocks; zb++) {
        struct buffer_head *bh = bread(minix_dev, 2 + sb.s_imap_blocks + zb);
        if (!bh) return 0;

        for (int i = 0; i < MINIX_BLOCK_SIZE * 8; i++) {
            if (!test_bit(i, bh->b_data)) {
                set_bit(i, bh->b_data);
                bmark_dirty(bh);
                bwrite(bh);
                brelse(bh);

                uint32_t block = sb.s_firstdatazone + (zb * MINIX_BLOCK_SIZE * 8) + i - 1;
                /* Очищаем выделенный блок */
                struct buffer_head *new_b = getblk(minix_dev, block);
                memset(new_b->b_data, 0, MINIX_BLOCK_SIZE);
                bmark_dirty(new_b);
                bwrite(new_b);
                brelse(new_b);
                return block;
            }
        }
        brelse(bh);
    }
    return 0;
}

static void free_block(uint32_t block)
{
    if (block < sb.s_firstdatazone) return;
    uint32_t bit = block - sb.s_firstdatazone + 1;
    uint16_t zb = bit / (MINIX_BLOCK_SIZE * 8);
    uint32_t offset = bit % (MINIX_BLOCK_SIZE * 8);

    struct buffer_head *bh = bread(minix_dev, 2 + sb.s_imap_blocks + zb);
    if (!bh) return;
    clear_bit(offset, bh->b_data);
    bmark_dirty(bh);
    bwrite(bh);
    brelse(bh);
}

static uint32_t alloc_inode(void)
{
    struct buffer_head *bh = bread(minix_dev, 2);
    if (!bh) return 0;

    for (int i = 1; i < sb.s_ninodes; i++) {
        if (!test_bit(i, bh->b_data)) {
            set_bit(i, bh->b_data);
            bmark_dirty(bh);
            bwrite(bh);
            brelse(bh);
            return (uint32_t)i;
        }
    }
    brelse(bh);
    return 0;
}

static void free_inode_bit(uint32_t ino)
{
    if (ino == 0 || ino > sb.s_ninodes) return;
    struct buffer_head *bh = bread(minix_dev, 2);
    if (!bh) return;
    clear_bit(ino, bh->b_data);
    bmark_dirty(bh);
    bwrite(bh);
    brelse(bh);
}

static uint32_t bmap(struct minix_inode *inode, uint32_t blk_idx, int create)
{
    if (blk_idx < 7) {
        if (inode->i_zone[blk_idx] == 0 && create) {
            uint32_t nb = alloc_block();
            if (!nb) return 0;
            inode->i_zone[blk_idx] = (uint16_t)nb;
        }
        return inode->i_zone[blk_idx];
    }

    blk_idx -= 7;
    if (blk_idx < 512) {
        if (inode->i_zone[7] == 0) {
            if (!create) return 0;
            uint32_t nb = alloc_block();
            if (!nb) return 0;
            inode->i_zone[7] = (uint16_t)nb;
        }

        struct buffer_head *indir = bread(minix_dev, inode->i_zone[7]);
        if (!indir) return 0;
        uint16_t *zones = (uint16_t *)indir->b_data;

        if (zones[blk_idx] == 0 && create) {
            uint32_t nb = alloc_block();
            if (nb) {
                zones[blk_idx] = (uint16_t)nb;
                bmark_dirty(indir);
                bwrite(indir);
            }
        }
        uint32_t res = zones[blk_idx];
        brelse(indir);
        return res;
    }

    return 0;
}

static void minix_truncate(uint32_t ino, struct minix_inode *inode)
{
    for (int i = 0; i < 7; i++) {
        if (inode->i_zone[i]) {
            free_block(inode->i_zone[i]);
            inode->i_zone[i] = 0;
        }
    }
    if (inode->i_zone[7]) {
        struct buffer_head *indir = bread(minix_dev, inode->i_zone[7]);
        if (indir) {
            uint16_t *zones = (uint16_t *)indir->b_data;
            for (int k = 0; k < 512; k++) {
                if (zones[k]) free_block(zones[k]);
            }
            brelse(indir);
        }
        free_block(inode->i_zone[7]);
        inode->i_zone[7] = 0;
    }
    inode->i_size = 0;
    write_inode(ino, inode);
}

static uint32_t minix_lookup(const struct minix_inode *dir, const char *name)
{
    uint32_t total_ents = dir->i_size / sizeof(struct minix_dir_entry);
    uint32_t ent_idx = 0;
    uint32_t blk_idx = 0;

    while (ent_idx < total_ents) {
        uint32_t blk = bmap((struct minix_inode *)dir, blk_idx++, 0);
        if (!blk) break;

        struct buffer_head *bh = bread(minix_dev, blk);
        if (!bh) break;

        struct minix_dir_entry *de = (struct minix_dir_entry *)bh->b_data;
        for (uint32_t i = 0; i < MINIX_DIR_ENT_PER_BLK && ent_idx < total_ents; i++, ent_idx++) {
            if (de[i].inode != 0) {
                char dname[MINIX_NAME_LEN + 1];
                memcpy(dname, de[i].name, MINIX_NAME_LEN);
                dname[MINIX_NAME_LEN] = '\0';
                if (strcmp(dname, name) == 0) {
                    uint32_t found = de[i].inode;
                    brelse(bh);
                    return found;
                }
            }
        }
        brelse(bh);
    }
    return 0;
}

static int minix_add_entry(uint32_t dir_ino, struct minix_inode *dir, const char *name, uint32_t ino)
{
    uint32_t blk_idx = 0;
    uint32_t ent_idx = 0;
    uint32_t total_ents = (dir->i_size + sizeof(struct minix_dir_entry) - 1) / sizeof(struct minix_dir_entry);

    while (1) {
        uint32_t blk = bmap(dir, blk_idx, 1);
        if (!blk) return -1;

        struct buffer_head *bh = bread(minix_dev, blk);
        if (!bh) return -1;

        struct minix_dir_entry *de = (struct minix_dir_entry *)bh->b_data;
        for (uint32_t i = 0; i < MINIX_DIR_ENT_PER_BLK; i++, ent_idx++) {
            if (de[i].inode == 0 || ent_idx >= total_ents) {
                de[i].inode = (uint16_t)ino;
                memset(de[i].name, 0, MINIX_NAME_LEN);
                strncpy(de[i].name, name, MINIX_NAME_LEN);
                bmark_dirty(bh);
                bwrite(bh);
                brelse(bh);

                if (ent_idx >= total_ents) {
                    dir->i_size = (ent_idx + 1) * sizeof(struct minix_dir_entry);
                }
                dir->i_time = (uint32_t)get_current_time();
                write_inode(dir_ino, dir);
                return 0;
            }
        }
        brelse(bh);
        blk_idx++;
    }
}

static uint32_t minix_namei(const char *path)
{
    if (!path || path[0] == '\0') return 0;
    if (strcmp(path, "/mnt") == 0 || strcmp(path, "/mnt/") == 0) return MINIX_ROOT_INO;

    const char *p = path;
    if (strncmp(p, "/mnt/", 5) == 0) p += 5;
    else if (p[0] == '/') p++;

    uint32_t cur_ino = MINIX_ROOT_INO;
    struct minix_inode cur_inode;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        char comp[MINIX_NAME_LEN + 1];
        int ci = 0;
        while (*p && *p != '/' && ci < MINIX_NAME_LEN) comp[ci++] = *p++;
        comp[ci] = '\0';
        while (*p && *p != '/') p++;

        if (read_inode(cur_ino, &cur_inode) < 0) return 0;
        if ((cur_inode.i_mode & 0170000) != 0040000) return 0; /* Не каталог */

            uint32_t next = minix_lookup(&cur_inode, comp);
        if (!next) return 0;
        cur_ino = next;
    }
    return cur_ino;
}

static uint32_t minix_dir_namei(const char *path, char *out_basename)
{
    const char *p = path;
    if (strncmp(p, "/mnt/", 5) == 0) p += 5;
    else if (p[0] == '/') p++;

    char tmp_path[80];
    strncpy(tmp_path, p, sizeof(tmp_path) - 1);
    tmp_path[sizeof(tmp_path) - 1] = '\0';

    int last_slash = -1;
    for (int i = 0; tmp_path[i]; i++) {
        if (tmp_path[i] == '/') last_slash = i;
    }

    if (last_slash == -1) {
        strncpy(out_basename, tmp_path, MINIX_NAME_LEN);
        out_basename[MINIX_NAME_LEN] = '\0';
        return MINIX_ROOT_INO;
    } else {
        tmp_path[last_slash] = '\0';
        strncpy(out_basename, tmp_path + last_slash + 1, MINIX_NAME_LEN);
        out_basename[MINIX_NAME_LEN] = '\0';

        char full_dir[96];
        memcpy(full_dir, "/mnt/", 5);
        memcpy(full_dir + 5, tmp_path, strlen(tmp_path) + 1);
        return minix_namei(full_dir);
    }
}

int minix_mount(uint8_t dev)
{
    minix_dev = dev;
    struct buffer_head *bh = bread(dev, 1);
    if (!bh) {
        printk("[MINIX] Error: Cannot read superblock from /dev/hda!\n");
        return -1;
    }

    memcpy(&sb, bh->b_data, sizeof(sb));
    brelse(bh);

    if (sb.s_magic != MINIX_SUPER_MAGIC) {
        /* Автоформатирование уничтожало данные на реальном диске. Форматируем
         * только если весь суперблок и первые блоки состоят из нулей (чистый диск). */
        int blank = 1;
        for (uint32_t i = 0; i < sizeof(sb); i++) {
            if (((uint8_t *)&sb)[i]) { blank = 0; break; }
        }
        if (!blank) {
            printk("[MINIX] Not a Minix v1 disk (magic 0x%x) - refusing to format, /mnt disabled\n", sb.s_magic);
            return -1;
        }
        printk("[MINIX] Blank disk. Initializing filesystem...\n");
        minix_format(dev, 32768, 1024);

        bh = bread(dev, 1);
        if (!bh) return -1;
        memcpy(&sb, bh->b_data, sizeof(sb));
        brelse(bh);
    }

    minix_mounted = 1;
    printk("[OK] Minix v1 Filesystem mounted on /mnt (/dev/hda: %d zones, %d inodes)\n",
           sb.s_nzones, sb.s_ninodes);
    return 0;
}

void minix_init(void)
{
    buffer_init();
    minix_mount(0);
}

int minix_sys_open(const char *path, int flags, uint16_t mode, uint32_t *out_ino, uint64_t *out_size)
{
    uint32_t ino = minix_namei(path);

    if (ino == 0) {
        if (!(flags & 0100)) return -1; /* O_CREAT */

            char basename[MINIX_NAME_LEN + 1];
        uint32_t dir_ino = minix_dir_namei(path, basename);
        if (!dir_ino) return -1;

        struct minix_inode dir_inode;
        if (read_inode(dir_ino, &dir_inode) < 0) return -1;

        uint32_t new_ino = alloc_inode();
        if (!new_ino) return -1;

        struct minix_inode new_inode;
        memset(&new_inode, 0, sizeof(new_inode));
        new_inode.i_mode = 0100000 | (mode ? mode : 0644); /* S_IFREG */
        new_inode.i_size = 0;
        new_inode.i_nlinks = 1;
        new_inode.i_time = (uint32_t)get_current_time();
        write_inode(new_ino, &new_inode);

        if (minix_add_entry(dir_ino, &dir_inode, basename, new_ino) < 0) {
            free_inode_bit(new_ino);
            return -1;
        }

        ino = new_ino;
    }

    struct minix_inode cur;
    if (read_inode(ino, &cur) < 0) return -1;

    if (flags & 01000) { /* O_TRUNC */
        minix_truncate(ino, &cur);
    }

    if (out_ino) *out_ino = ino;
    if (out_size) *out_size = cur.i_size;
    return 0;
}

int64_t minix_file_read(uint32_t ino, uint64_t *pos, char *buf, uint64_t count)
{
    struct minix_inode inode;
    if (read_inode(ino, &inode) < 0) return -1;

    if (*pos >= inode.i_size) return 0;
    if (*pos + count > inode.i_size) {
        count = inode.i_size - *pos;
    }

    uint64_t bytes_read = 0;
    while (bytes_read < count) {
        uint32_t blk_idx = (*pos) / MINIX_BLOCK_SIZE;
        uint32_t offset  = (*pos) % MINIX_BLOCK_SIZE;
        uint32_t chunk   = MINIX_BLOCK_SIZE - offset;
        if (chunk > count - bytes_read) chunk = count - bytes_read;

        uint32_t blk = bmap(&inode, blk_idx, 0);
        if (blk != 0) {
            struct buffer_head *bh = bread(minix_dev, blk);
            if (!bh) break;
            memcpy(buf + bytes_read, bh->b_data + offset, chunk);
            brelse(bh);
        } else {
            memset(buf + bytes_read, 0, chunk);
        }

        bytes_read += chunk;
        *pos += chunk;
    }
    return bytes_read;
}

int64_t minix_file_write(uint32_t ino, uint64_t *pos, const char *buf, uint64_t count)
{
    struct minix_inode inode;
    if (read_inode(ino, &inode) < 0) return -1;

    uint64_t bytes_written = 0;
    while (bytes_written < count) {
        uint32_t blk_idx = (*pos) / MINIX_BLOCK_SIZE;
        uint32_t offset  = (*pos) % MINIX_BLOCK_SIZE;
        uint32_t chunk   = MINIX_BLOCK_SIZE - offset;
        if (chunk > count - bytes_written) chunk = count - bytes_written;

        uint32_t blk = bmap(&inode, blk_idx, 1);
        if (!blk) break;

        struct buffer_head *bh = bread(minix_dev, blk);
        if (!bh) break;

        memcpy(bh->b_data + offset, buf + bytes_written, chunk);
        bmark_dirty(bh);
        bwrite(bh);
        brelse(bh);

        bytes_written += chunk;
        *pos += chunk;
        if (*pos > inode.i_size) {
            inode.i_size = (uint32_t)*pos;
        }
    }

    inode.i_time = (uint32_t)get_current_time();
    write_inode(ino, &inode);
    return bytes_written;
}

int minix_sys_unlink(const char *path)
{
    char basename[MINIX_NAME_LEN + 1];
    uint32_t dir_ino = minix_dir_namei(path, basename);
    if (!dir_ino) return -1;

    struct minix_inode dir;
    if (read_inode(dir_ino, &dir) < 0) return -1;

    uint32_t target_ino = minix_lookup(&dir, basename);
    if (!target_ino) return -1;

    struct minix_inode target;
    if (read_inode(target_ino, &target) < 0) return -1;
    if ((target.i_mode & 0170000) == 0040000) return -1; /* Каталог через rmdir */

        /* 1. Удаляем запись из каталога */
        uint32_t total_ents = dir.i_size / sizeof(struct minix_dir_entry);
    uint32_t ent_idx = 0;
    uint32_t blk_idx = 0;

    while (ent_idx < total_ents) {
        uint32_t blk = bmap(&dir, blk_idx++, 0);
        if (!blk) break;
        struct buffer_head *bh = bread(minix_dev, blk);
        if (!bh) break;
        struct minix_dir_entry *de = (struct minix_dir_entry *)bh->b_data;
        for (uint32_t i = 0; i < MINIX_DIR_ENT_PER_BLK && ent_idx < total_ents; i++, ent_idx++) {
            if (de[i].inode == target_ino) {
                de[i].inode = 0;
                bmark_dirty(bh);
                bwrite(bh);
                brelse(bh);
                goto entry_removed;
            }
        }
        brelse(bh);
    }

    entry_removed:
    if (target.i_nlinks > 0) target.i_nlinks--;
    if (target.i_nlinks == 0) {
        minix_truncate(target_ino, &target);
        free_inode_bit(target_ino);
    } else {
        write_inode(target_ino, &target);
    }
    return 0;
}

int minix_sys_mkdir(const char *path, uint16_t mode)
{
    char basename[MINIX_NAME_LEN + 1];
    uint32_t dir_ino = minix_dir_namei(path, basename);
    if (!dir_ino) return -1;

    struct minix_inode dir;
    if (read_inode(dir_ino, &dir) < 0) return -1;

    if (minix_lookup(&dir, basename) != 0) return -1; /* Уже существует */

        uint32_t new_ino = alloc_inode();
    if (!new_ino) return -1;

    uint32_t data_blk = alloc_block();
    if (!data_blk) {
        free_inode_bit(new_ino);
        return -1;
    }

    /* Инициализируем . и .. */
    struct buffer_head *bh = bread(minix_dev, data_blk);
    if (!bh) {
        free_block(data_blk);
        free_inode_bit(new_ino);
        return -1;
    }
    memset(bh->b_data, 0, MINIX_BLOCK_SIZE);
    struct minix_dir_entry *de = (struct minix_dir_entry *)bh->b_data;
    de[0].inode = (uint16_t)new_ino;
    memcpy(de[0].name, ".", 2);
    de[1].inode = (uint16_t)dir_ino;
    memcpy(de[1].name, "..", 3);
    bmark_dirty(bh);
    bwrite(bh);
    brelse(bh);

    struct minix_inode new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.i_mode = 0040000 | (mode ? mode : 0755); /* S_IFDIR */
    new_inode.i_size = 2 * sizeof(struct minix_dir_entry);
    new_inode.i_nlinks = 2;
    new_inode.i_time = (uint32_t)get_current_time();
    new_inode.i_zone[0] = (uint16_t)data_blk;
    write_inode(new_ino, &new_inode);

    minix_add_entry(dir_ino, &dir, basename, new_ino);
    dir.i_nlinks++;
    write_inode(dir_ino, &dir);
    return 0;
}

int minix_sys_rmdir(const char *path)
{
    char basename[MINIX_NAME_LEN + 1];
    uint32_t dir_ino = minix_dir_namei(path, basename);
    if (!dir_ino) return -1;

    struct minix_inode dir;
    if (read_inode(dir_ino, &dir) < 0) return -1;

    uint32_t target_ino = minix_lookup(&dir, basename);
    if (!target_ino || target_ino == MINIX_ROOT_INO) return -1;

    struct minix_inode target;
    if (read_inode(target_ino, &target) < 0) return -1;
    if ((target.i_mode & 0170000) != 0040000) return -1;

    /* Проверяем, что каталог пуст (только . и ..) */
    if (target.i_size > 2 * sizeof(struct minix_dir_entry)) {
        return -1;
    }

    minix_truncate(target_ino, &target);
    free_inode_bit(target_ino);

    /* Удаляем из родителя */
    uint32_t total_ents = dir.i_size / sizeof(struct minix_dir_entry);
    uint32_t ent_idx = 0;
    uint32_t blk_idx = 0;
    while (ent_idx < total_ents) {
        uint32_t blk = bmap(&dir, blk_idx++, 0);
        if (!blk) break;
        struct buffer_head *bh = bread(minix_dev, blk);
        if (!bh) break;
        struct minix_dir_entry *de = (struct minix_dir_entry *)bh->b_data;
        for (uint32_t i = 0; i < MINIX_DIR_ENT_PER_BLK && ent_idx < total_ents; i++, ent_idx++) {
            if (de[i].inode == target_ino) {
                de[i].inode = 0;
                bmark_dirty(bh);
                bwrite(bh);
                brelse(bh);
                goto rmdir_done;
            }
        }
        brelse(bh);
    }

    rmdir_done:
    if (dir.i_nlinks > 2) dir.i_nlinks--;
    write_inode(dir_ino, &dir);
    return 0;
}

int minix_sys_stat(const char *path, struct stat *st)
{
    uint32_t ino = minix_namei(path);
    if (!ino) return -1;

    struct minix_inode inode;
    if (read_inode(ino, &inode) < 0) return -1;

    st->st_dev = 2; /* 2 = /dev/hda Minix FS */
    st->st_ino = ino;
    st->st_mode = inode.i_mode;
    st->st_nlink = inode.i_nlinks;
    st->st_uid = inode.i_uid;
    st->st_gid = inode.i_gid;
    st->st_size = inode.i_size;
    st->st_mtime = inode.i_time;
    return 0;
}

int64_t minix_sys_list(const char *path, char *buf, uint64_t max_len, int is_long)
{
    uint32_t ino = minix_namei(path);
    if (!ino) return -1;

    struct minix_inode dir;
    if (read_inode(ino, &dir) < 0) return -1;
    if ((dir.i_mode & 0170000) != 0040000) return -1;

    uint32_t total_ents = dir.i_size / sizeof(struct minix_dir_entry);
    uint32_t ent_idx = 0;
    uint32_t blk_idx = 0;
    uint64_t offset = 0;

    while (ent_idx < total_ents) {
        uint32_t blk = bmap(&dir, blk_idx++, 0);
        if (!blk) break;
        struct buffer_head *bh = bread(minix_dev, blk);
        if (!bh) break;
        struct minix_dir_entry *de = (struct minix_dir_entry *)bh->b_data;

        for (uint32_t i = 0; i < MINIX_DIR_ENT_PER_BLK && ent_idx < total_ents; i++, ent_idx++) {
            if (de[i].inode != 0) {
                struct minix_inode entry_ino;
                read_inode(de[i].inode, &entry_ino);
                int is_dir = (entry_ino.i_mode & 0170000) == 0040000;

                if (is_long) {
                    const char *perms = is_dir ? "drwxr-xr-x  " : "-rw-r--r--  ";
                    while (*perms && offset < max_len - 32) buf[offset++] = *perms++;

                    char szbuf[16];
                    int szi = 0;
                    uint64_t sz = entry_ino.i_size;
                    if (sz == 0) szbuf[szi++] = '0';
                    while (sz > 0) {
                        szbuf[szi++] = '0' + (sz % 10);
                        sz /= 10;
                    }
                    while (szi < 6) szbuf[szi++] = ' ';
                    while (--szi >= 0 && offset < max_len - 16) buf[offset++] = szbuf[szi];
                    buf[offset++] = ' ';
                    buf[offset++] = 'B';
                    buf[offset++] = ' ';
                    buf[offset++] = ' ';

                    char np[MINIX_NAME_LEN + 1];
                    memcpy(np, de[i].name, MINIX_NAME_LEN);
                    np[MINIX_NAME_LEN] = '\0';
                    const char *p = np;
                    while (*p && offset < max_len - 16) buf[offset++] = *p++;
                    if (is_dir) buf[offset++] = '/';
                    buf[offset++] = '\n';
                } else {
                    char np[MINIX_NAME_LEN + 1];
                    memcpy(np, de[i].name, MINIX_NAME_LEN);
                    np[MINIX_NAME_LEN] = '\0';
                    const char *p = np;
                    while (*p && offset < max_len - 16) buf[offset++] = *p++;
                    if (is_dir) buf[offset++] = '/';
                    buf[offset++] = '\n';
                }
            }
        }
        brelse(bh);
    }

    buf[offset] = '\0';
    return offset;
}

int minix_is_dir(const char *path)
{
    uint32_t ino = minix_namei(path);
    if (!ino) return 0;
    struct minix_inode inode;
    if (read_inode(ino, &inode) < 0) return 0;
    return (inode.i_mode & 0170000) == 0040000;
}

static char minix_exec_buf[32 * 1024];

const char *minix_get_file_data(const char *path, uint64_t *out_size)
{
    uint32_t ino = minix_namei(path);
    if (!ino) return NULL;

    struct minix_inode inode;
    if (read_inode(ino, &inode) < 0) return NULL;
    if ((inode.i_mode & 0170000) == 0040000) return NULL;

    uint64_t pos = 0;
    int64_t r = minix_file_read(ino, &pos, minix_exec_buf, sizeof(minix_exec_buf));
    if (r < 0) return NULL;

    if (out_size) *out_size = inode.i_size;
    return minix_exec_buf;
}

void minix_sync(void)
{
    bflush();
}
