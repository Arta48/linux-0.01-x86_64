#include <linux/buffer.h>
#include <linux/mm.h>
#include <linux/hdreg.h>
#include <linux/string.h>
#include <linux/tty.h>

static struct buffer_head bh_pool[NR_BUFFERS];
static struct buffer_head *hash_table[NR_HASH];
static struct buffer_head *free_list = NULL;

#define HASH_FN(dev, block) (((uint32_t)(dev) ^ (uint32_t)(block)) % NR_HASH)

void buffer_init(void)
{
    for (int i = 0; i < NR_HASH; i++) {
        hash_table[i] = NULL;
    }

    uint32_t pages_needed = (NR_BUFFERS * BLOCK_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t data_mem = get_free_pages(pages_needed);
    if (!data_mem) {
        printk("[PANIC] Out of memory initializing Buffer Cache!\n");
        return;
    }

    for (int i = 0; i < NR_BUFFERS; i++) {
        struct buffer_head *bh = &bh_pool[i];
        bh->b_data = (char *)(data_mem + (uint64_t)i * BLOCK_SIZE);
        bh->b_dev = 0;
        bh->b_blocknr = 0;
        bh->b_uptodate = 0;
        bh->b_dirt = 0;
        bh->b_count = 0;
        bh->b_next = NULL;
        bh->b_prev = NULL;

        bh->b_prev_free = (i > 0) ? &bh_pool[i - 1] : &bh_pool[NR_BUFFERS - 1];
        bh->b_next_free = (i < NR_BUFFERS - 1) ? &bh_pool[i + 1] : &bh_pool[0];
    }

    free_list = &bh_pool[0];
    printk("[OK] Buffer Cache Initialized (%d buffers, %d KB cache)\n",
           NR_BUFFERS, (NR_BUFFERS * BLOCK_SIZE) / 1024);
}

static void remove_from_hash(struct buffer_head *bh)
{
    if (bh->b_prev) {
        bh->b_prev->b_next = bh->b_next;
    } else {
        uint32_t h = HASH_FN(bh->b_dev, bh->b_blocknr);
        if (hash_table[h] == bh) {
            hash_table[h] = bh->b_next;
        }
    }
    if (bh->b_next) {
        bh->b_next->b_prev = bh->b_prev;
    }
    bh->b_next = NULL;
    bh->b_prev = NULL;
}

static void insert_into_hash(struct buffer_head *bh)
{
    uint32_t h = HASH_FN(bh->b_dev, bh->b_blocknr);
    bh->b_next = hash_table[h];
    bh->b_prev = NULL;
    if (hash_table[h]) {
        hash_table[h]->b_prev = bh;
    }
    hash_table[h] = bh;
}

static struct buffer_head *find_buffer(uint16_t dev, uint32_t block)
{
    uint32_t h = HASH_FN(dev, block);
    struct buffer_head *bh = hash_table[h];
    while (bh) {
        if (bh->b_dev == dev && bh->b_blocknr == block) {
            return bh;
        }
        bh = bh->b_next;
    }
    return NULL;
}

struct buffer_head *getblk(uint16_t dev, uint32_t block)
{
    struct buffer_head *bh = find_buffer(dev, block);
    if (bh) {
        bh->b_count++;
        return bh;
    }

    struct buffer_head *victim = free_list;
    int scanned = 0;
    while (scanned < NR_BUFFERS) {
        if (victim->b_count == 0) {
            break;
        }
        victim = victim->b_next_free;
        scanned++;
    }

    if (scanned >= NR_BUFFERS) {
        printk("[BUFFER] Error: All %d buffers are busy!\n", NR_BUFFERS);
        return NULL;
    }

    if (victim->b_dirt && victim->b_dev != 0) {
        bwrite(victim);
    }

    if (victim->b_dev != 0) {
        remove_from_hash(victim);
    }

    victim->b_dev = dev;
    victim->b_blocknr = block;
    victim->b_dirt = 0;
    victim->b_uptodate = 0;
    victim->b_count = 1;

    insert_into_hash(victim);

    free_list = victim->b_next_free;
    return victim;
}

struct buffer_head *bread(uint16_t dev, uint32_t block)
{
    struct buffer_head *bh = getblk(dev, block);
    if (!bh) return NULL;

    if (bh->b_uptodate) {
        return bh;
    }

    /* 1 блок = 2 сектора по 512 байт в режиме LBA28 */
    uint32_t lba = block * 2;
    if (ide_read_sectors((uint8_t)dev, lba, 2, bh->b_data) < 0) {
        printk("[BUFFER] Error reading block %d from dev %d!\n", block, dev);
        brelse(bh);
        return NULL;
    }

    bh->b_uptodate = 1;
    bh->b_dirt = 0;
    return bh;
}

void bmark_dirty(struct buffer_head *bh)
{
    if (bh) {
        bh->b_dirt = 1;
    }
}

void bwrite(struct buffer_head *bh)
{
    if (!bh || !bh->b_dirt) return;

    uint32_t lba = bh->b_blocknr * 2;
    if (ide_write_sectors((uint8_t)bh->b_dev, lba, 2, bh->b_data) < 0) {
        printk("[BUFFER] Error writing block %d to dev %d!\n", bh->b_blocknr, bh->b_dev);
        return;
    }

    bh->b_dirt = 0;
    bh->b_uptodate = 1;
}

void brelse(struct buffer_head *bh)
{
    if (!bh) return;
    if (bh->b_count > 0) {
        bh->b_count--;
    }
}

void bflush(void)
{
    for (int i = 0; i < NR_BUFFERS; i++) {
        struct buffer_head *bh = &bh_pool[i];
        if (bh->b_dirt && bh->b_dev != 0) {
            bwrite(bh);
        }
    }
}
