#include <linux/mm.h>
#include <linux/tty.h>

extern char _end[];

static uint64_t low_mem = 0;
static uint32_t paging_pages = 0;

/* Максимальное число страниц для 128 МБ памяти (128 МБ / 4 КБ = 32768 страниц) */
#define MAX_PAGING_PAGES 32768
static uint16_t mem_map[MAX_PAGING_PAGES];

/* Сброс TLB (кэша страниц) для конкретного адреса */
static inline void invlpg(uint64_t addr)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
}

/* Получить адрес активной PML4-таблицы из CR3 */
static inline uint64_t *get_current_pml4(void)
{
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return (uint64_t *)(cr3 & ~0xFFFULL);
}

void mem_init(void)
{
    /* Память для динамического выделения начинается сразу после конца бинарника ядра */
    low_mem = PAGE_ALIGN((uint64_t)_end);
    paging_pages = (HIGH_MEMORY - low_mem) / PAGE_SIZE;

    if (paging_pages > MAX_PAGING_PAGES) {
        paging_pages = MAX_PAGING_PAGES;
    }

    /* Помечаем все страницы доступными (счетчик ссылок = 0) */
    for (uint32_t i = 0; i < paging_pages; i++) {
        mem_map[i] = 0;
    }

    printk("[OK] Memory Manager: low_mem = %p, high_mem = %p\n", low_mem, HIGH_MEMORY);
    printk("[OK] Free Physical Pages: %d (%d MB free for allocation)\n",
           paging_pages, (paging_pages * PAGE_SIZE) / (1024 * 1024));
}

/* Выделить свободную физическую страницу (4 КБ) */
uint64_t get_free_page(void)
{
    for (uint32_t i = 0; i < paging_pages; i++) {
        if (mem_map[i] == 0) {
            mem_map[i] = 1; /* Занимаем страницу */
            uint64_t page_addr = low_mem + ((uint64_t)i * PAGE_SIZE);

            /* Очищаем страницу нулями (как в оригинале) */
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

/* Освободить физическую страницу */
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

/*
 * Отображение виртуального адреса на физический через 4-уровневые таблицы:
 * PML4 -> PDPT -> PD -> PT
 */
int map_page(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (!pml4) {
        pml4 = get_current_pml4();
    }

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    /* 1. Уровень PML4 -> PDPT */
    if (!(pml4[pml4_idx] & PTE_PRESENT)) {
        uint64_t new_pdpt = get_free_page();
        if (!new_pdpt) return -1;
        pml4[pml4_idx] = new_pdpt | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    }
    uint64_t *pdpt = (uint64_t *)(pml4[pml4_idx] & ~0xFFFULL);

    /* 2. Уровень PDPT -> PD */
    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
        uint64_t new_pd = get_free_page();
        if (!new_pd) return -1;
        pdpt[pdpt_idx] = new_pd | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    }
    uint64_t *pd = (uint64_t *)(pdpt[pdpt_idx] & ~0xFFFULL);

    /* 3. Уровень PD -> PT */
    if (!(pd[pd_idx] & PTE_PRESENT)) {
        uint64_t new_pt = get_free_page();
        if (!new_pt) return -1;
        pd[pd_idx] = new_pt | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    }
    uint64_t *pt = (uint64_t *)(pd[pd_idx] & ~0xFFFULL);

    /* 4. Запись в PT */
    pt[pt_idx] = (phys & ~0xFFFULL) | flags | PTE_PRESENT;

    invlpg(virt);
    return 0;
}
