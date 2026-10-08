#ifndef _LINUX_BUFFER_H
#define _LINUX_BUFFER_H

#include <linux/types.h>

#define BLOCK_SIZE  1024
#define NR_BUFFERS  128
#define NR_HASH     127

struct buffer_head {
    char *b_data;                  /* Указатель на буфер данных (1024 байта) */
    uint16_t b_dev;                /* Номер устройства (0 = свободен) */
    uint32_t b_blocknr;            /* Номер блока на устройстве */
    uint8_t  b_uptodate;           /* 1 = данные актуальны */
    uint8_t  b_dirt;               /* 1 = буфер изменен, требует записи */
    uint16_t b_count;              /* Счетчик ссылок */
    struct buffer_head *b_next;    /* Хэш-цепочка */
    struct buffer_head *b_prev;
    struct buffer_head *b_next_free; /* Список свободных буферов (LRU) */
    struct buffer_head *b_prev_free;
};

void buffer_init(void);
struct buffer_head *getblk(uint16_t dev, uint32_t block);
struct buffer_head *bread(uint16_t dev, uint32_t block);
void bmark_dirty(struct buffer_head *bh);
void bwrite(struct buffer_head *bh);
void brelse(struct buffer_head *bh);
void bflush(void);

#endif
