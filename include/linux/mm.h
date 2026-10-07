#ifndef _LINUX_MM_H
#define _LINUX_MM_H

#include <linux/types.h>

#define PAGE_SIZE     4096ULL
#define PAGE_ALIGN(a) (((a) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)

#define HIGH_MEMORY   (128ULL * 1024 * 1024)

void mem_init(void);
uint64_t get_free_page(void);
void free_page(uint64_t addr);

int map_page(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
void unmap_page(uint64_t *pml4, uint64_t virt);

uint32_t get_free_pages_count(void);
uint32_t get_total_pages_count(void);

#endif
