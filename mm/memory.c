#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/string.h>

extern char _end[];

static uint64_t low_mem = 0;
static uint32_t paging_pages = 0;

#define MAX_PAGING_PAGES 32768
static uint16_t mem_map[MAX_PAGING_PAGES];

static inline void invlpg(uint64_t addr)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
}

static inline uint64_t *get_current_pml4(void)
{
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return (uint64_t *)(cr3 & ~0xFFFULL);
}

void mem_init(uint64_t reserve_end)
{
    uint64_t base = (uint64_t)_end;
    if (reserve_end > base) {
        base = reserve_end;
    }

    low_mem = PAGE_ALIGN(base);
    paging_pages = (HIGH_MEMORY - low_mem) / PAGE_SIZE;

    if (paging_pages > MAX_PAGING_PAGES) {
        paging_pages = MAX_PAGING_PAGES;
    }

    for (uint32_t i = 0; i < paging_pages; i++) {
        mem_map[i] = 0;
    }

    printk("[OK] Memory Manager: low_mem = %p, high_mem = %p\n", low_mem, HIGH_MEMORY);
    printk("[OK] Free Physical Pages: %d (%d MB free for allocation)\n",
           paging_pages, (paging_pages * PAGE_SIZE) / (1024 * 1024));
}

uint64_t get_free_page(void)
{
    for (uint32_t i = 0; i < paging_pages; i++) {
        if (mem_map[i] == 0) {
            mem_map[i] = 1;
            uint64_t page_addr = low_mem + ((uint64_t)i * PAGE_SIZE);

            uint64_t *p = (uint64_t *)page_addr;
            for (int j = 0; j < 512; j++) {
                p[j] = 0;
            }
            return page_addr;
        }
    }
    printk("[PANIC] Out of memory!\n");
    return 0;
}

uint64_t get_free_pages(uint32_t count)
{
    if (count == 0) return 0;
    if (count == 1) return get_free_page();

    for (uint32_t i = 0; i <= paging_pages - count; i++) {
        int found = 1;
        for (uint32_t k = 0; k < count; k++) {
            if (mem_map[i + k] != 0) {
                found = 0;
                i += k;
                break;
            }
        }
        if (found) {
            for (uint32_t k = 0; k < count; k++) {
                mem_map[i + k] = 1;
            }
            uint64_t addr = low_mem + ((uint64_t)i * PAGE_SIZE);
            memset((void *)addr, 0, (uint64_t)count * PAGE_SIZE);
            return addr;
        }
    }
    printk("[PANIC] Out of contiguous memory for %d pages!\n", count);
    return 0;
}

void free_page(uint64_t addr)
{
    if (addr < low_mem || addr >= HIGH_MEMORY) {
        printk("[PANIC] Trying to free non-allocated page: %p\n", addr);
        return;
    }

    uint32_t index = (addr - low_mem) / PAGE_SIZE;
    if (mem_map[index] == 0) {
        printk("[PANIC] Double free of page: %p\n", addr);
        return;
    }

    mem_map[index]--;
}

void free_pages(uint64_t addr, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        free_page(addr + ((uint64_t)i * PAGE_SIZE));
    }
}

int map_page(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (!pml4) {
        pml4 = get_current_pml4();
    }

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    if (!(pml4[pml4_idx] & PTE_PRESENT)) {
        uint64_t new_pdpt = get_free_page();
        if (!new_pdpt) return -1;
        pml4[pml4_idx] = new_pdpt | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    }
    uint64_t *pdpt = (uint64_t *)(pml4[pml4_idx] & ~0xFFFULL);

    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
        uint64_t new_pd = get_free_page();
        if (!new_pd) return -1;
        pdpt[pdpt_idx] = new_pd | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    }
    uint64_t *pd = (uint64_t *)(pdpt[pdpt_idx] & ~0xFFFULL);

    if (!(pd[pd_idx] & PTE_PRESENT)) {
        uint64_t new_pt = get_free_page();
        if (!new_pt) return -1;
        pd[pd_idx] = new_pt | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    }
    uint64_t *pt = (uint64_t *)(pd[pd_idx] & ~0xFFFULL);

    pt[pt_idx] = (phys & ~0xFFFULL) | flags | PTE_PRESENT;

    invlpg(virt);
    return 0;
}

uint32_t get_free_pages_count(void)
{
    uint32_t free_cnt = 0;
    for (uint32_t i = 0; i < paging_pages; i++) {
        if (mem_map[i] == 0) free_cnt++;
    }
    return free_cnt;
}

uint32_t get_total_pages_count(void)
{
    return paging_pages;
}
