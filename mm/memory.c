#include <linux/mm.h>
#include <linux/traps.h>
#include <linux/tty.h>
#include <linux/string.h>
#include <linux/spinlock.h>

static spinlock_t mem_lock = SPINLOCK_INIT;

extern char _end[];
extern char pd_table[];

static uint64_t low_mem = 0;
static uint64_t high_mem = HIGH_MEMORY;
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

void mem_init(uint64_t reserve_end, uint64_t limit)
{
    uint64_t base = (uint64_t)_end;
    if (reserve_end > base) {
        base = reserve_end;
    }

    /* Граница не может превышать identity-map (1 ГБ) и размер mem_map */
    if (limit == 0 || limit > HIGH_MEMORY_MAX) {
        limit = HIGH_MEMORY_MAX;
    }

    low_mem = PAGE_ALIGN(base);
    if (low_mem >= limit) {
        /* Раньше здесь происходил unsigned underflow -> адреса за пределами
         * identity-map -> #PF без IDT -> triple fault (перезагрузка ПК). */
        paging_pages = 0;
        high_mem = low_mem;
        printk("[PANIC] mem_init: low_mem %p >= limit %p, no free memory!\n", low_mem, limit);
        return;
    }
    paging_pages = (uint32_t)((limit - low_mem) / PAGE_SIZE);

    if (paging_pages > MAX_PAGING_PAGES) {
        paging_pages = MAX_PAGING_PAGES;
    }
    high_mem = low_mem + (uint64_t)paging_pages * PAGE_SIZE;

    for (uint32_t i = 0; i < paging_pages; i++) {
        mem_map[i] = 0;
    }

    printk("[OK] Memory Manager: low_mem = %p, high_mem = %p\n", low_mem, high_mem);
    printk("[OK] Free Physical Pages: %d (%d MB free for allocation)\n",
           paging_pages, (paging_pages * PAGE_SIZE) / (1024 * 1024));
}

uint64_t get_free_page(void)
{
    uint64_t flags = spin_lock_irqsave(&mem_lock);
    for (uint32_t i = 0; i < paging_pages; i++) {
        if (mem_map[i] == 0) {
            mem_map[i] = 1;
            spin_unlock_irqrestore(&mem_lock, flags);

            uint64_t page_addr = low_mem + ((uint64_t)i * PAGE_SIZE);
            uint64_t *p = (uint64_t *)page_addr;
            for (int j = 0; j < 512; j++) {
                p[j] = 0;
            }
            return page_addr;
        }
    }
    spin_unlock_irqrestore(&mem_lock, flags);
    printk("[PANIC] Out of memory!\n");
    return 0;
}

uint64_t get_free_pages(uint32_t count)
{
    if (count == 0) return 0;
    if (count == 1) return get_free_page();
    if (count > paging_pages) {
        printk("[PANIC] Out of contiguous memory for %d pages!\n", count);
        return 0;
    }

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
    if (addr < low_mem || addr >= high_mem) {
        return;
    }

    uint32_t index = (addr - low_mem) / PAGE_SIZE;
    uint64_t flags = spin_lock_irqsave(&mem_lock);
    if (mem_map[index] > 0) {
        mem_map[index]--;
    }
    spin_unlock_irqrestore(&mem_lock, flags);
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
    } else {
        pml4[pml4_idx] |= (flags & PTE_USER);
    }
    uint64_t *pdpt = (uint64_t *)(pml4[pml4_idx] & ~0xFFFULL);

    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
        uint64_t new_pd = get_free_page();
        if (!new_pd) return -1;
        pdpt[pdpt_idx] = new_pd | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    } else {
        pdpt[pdpt_idx] |= (flags & PTE_USER);
    }
    uint64_t *pd = (uint64_t *)(pdpt[pdpt_idx] & ~0xFFFULL);

    if (!(pd[pd_idx] & PTE_PRESENT)) {
        uint64_t new_pt = get_free_page();
        if (!new_pt) return -1;
        pd[pd_idx] = new_pt | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    } else {
        pd[pd_idx] |= (flags & PTE_USER);
    }
    uint64_t *pt = (uint64_t *)(pd[pd_idx] & ~0xFFFULL);

    pt[pt_idx] = (phys & ~0xFFFULL) | flags | PTE_PRESENT;

    invlpg(virt);
    return 0;
}

uint64_t create_process_pml4(void)
{
    uint64_t pml4_phys = get_free_page();
    if (!pml4_phys) return 0;

    uint64_t pdpt_phys = get_free_page();
    if (!pdpt_phys) {
        free_page(pml4_phys);
        return 0;
    }

    uint64_t *pml4 = (uint64_t *)pml4_phys;
    uint64_t *pdpt = (uint64_t *)pdpt_phys;

    /* PML4[0] указывает на персональную PDPT процесса */
    pml4[0] = pdpt_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;

    /* PDPT[0] указывает на общую таблицу ядра pd_table (0..1 ГБ identity map) */
    pdpt[0] = (uint64_t)pd_table | PTE_PRESENT | PTE_WRITABLE | PTE_USER;

    /* Отображаем устройства ядра (Local APIC 0xFEE00000 в PDPT[3]) во все процессы */
    extern char pdpt_table[];
    uint64_t *boot_pdpt = (uint64_t *)pdpt_table;
    pdpt[3] = boot_pdpt[3];

    return pml4_phys;
}

uint64_t copy_process_pml4(uint64_t parent_pml4)
{
    uint64_t child_pml4_phys = create_process_pml4();
    if (!child_pml4_phys) return 0;

    if (!parent_pml4) {
        return child_pml4_phys;
    }

    uint64_t *p_pml4 = (uint64_t *)parent_pml4;
    if (!(p_pml4[0] & PTE_PRESENT)) return child_pml4_phys;

    uint64_t *p_pdpt = (uint64_t *)(p_pml4[0] & ~0xFFFULL);

    /* Обходим только пользовательские диапазоны (PDPT[1..511], от 1 ГБ) */
    for (uint64_t pdpt_idx = 1; pdpt_idx < 512; pdpt_idx++) {
        if (pdpt_idx == 3) continue; /* Пропускаем системный APIC */
        if (!(p_pdpt[pdpt_idx] & PTE_PRESENT)) continue;

        uint64_t *p_pd = (uint64_t *)(p_pdpt[pdpt_idx] & ~0xFFFULL);
        for (uint64_t pd_idx = 0; pd_idx < 512; pd_idx++) {
            if (!(p_pd[pd_idx] & PTE_PRESENT)) continue;

            uint64_t *p_pt = (uint64_t *)(p_pd[pd_idx] & ~0xFFFULL);
            for (uint64_t pt_idx = 0; pt_idx < 512; pt_idx++) {
                if (!(p_pt[pt_idx] & PTE_PRESENT)) continue;

                uint64_t parent_phys = p_pt[pt_idx] & ~0xFFFULL;
                uint64_t flags = p_pt[pt_idx] & 0xFFFULL;

                uint64_t virt = (pdpt_idx << 30) | (pd_idx << 21) | (pt_idx << 12);

                /* Механизм Copy-On-Write (COW):
                 * Сбрасываем флаг записи у родителя и ребенка, увеличиваем счетчик ссылок */
                flags &= ~PTE_WRITABLE;
                p_pt[pt_idx] = (parent_phys & ~0xFFFULL) | flags | PTE_PRESENT;
                invlpg(virt);

                /* Увеличиваем счетчик ссылок на физическую страницу в mem_map */
                if (parent_phys >= low_mem && parent_phys < high_mem) {
                    uint32_t idx = (parent_phys - low_mem) / PAGE_SIZE;
                    mem_map[idx]++;
                }

                map_page((uint64_t *)child_pml4_phys, virt, parent_phys, flags);
            }
        }
    }

    return child_pml4_phys;
}

void free_process_pml4(uint64_t pml4_phys)
{
    if (!pml4_phys) return;

    uint64_t *pml4 = (uint64_t *)pml4_phys;
    if (pml4[0] & PTE_PRESENT) {
        uint64_t *pdpt = (uint64_t *)(pml4[0] & ~0xFFFULL);

        /* pdpt[0] - это общая таблица ядра, ее НЕ освобождаем! */
        for (uint64_t pdpt_idx = 1; pdpt_idx < 512; pdpt_idx++) {
            if (pdpt_idx == 3) continue; /* Не трогаем системный APIC */
            if (!(pdpt[pdpt_idx] & PTE_PRESENT)) continue;

            uint64_t *pd = (uint64_t *)(pdpt[pdpt_idx] & ~0xFFFULL);
            for (uint64_t pd_idx = 0; pd_idx < 512; pd_idx++) {
                if (!(pd[pd_idx] & PTE_PRESENT)) continue;

                uint64_t *pt = (uint64_t *)(pd[pd_idx] & ~0xFFFULL);
                for (uint64_t pt_idx = 0; pt_idx < 512; pt_idx++) {
                    if (!(pt[pt_idx] & PTE_PRESENT)) continue;
                    uint64_t phys_page = pt[pt_idx] & ~0xFFFULL;
                    free_page(phys_page);
                }
                free_page((uint64_t)pt);
            }
            free_page((uint64_t)pd);
        }
        free_page((uint64_t)pdpt);
    }
    free_page(pml4_phys);
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

int do_wp_page(uint64_t *pte, uint64_t addr)
{
    uint64_t old_page = *pte & ~0xFFFULL;
    uint32_t idx = (old_page >= low_mem && old_page < high_mem) ? (old_page - low_mem) / PAGE_SIZE : 0;

    /* Если страницу больше никто не делит — просто возвращаем права на запись */
    if (mem_map[idx] <= 1) {
        *pte |= PTE_WRITABLE;
        invlpg(addr);
        return 0;
    }

    /* Иначе выделяем приватную копию страницы для пишущего процесса */
    uint64_t new_page = get_free_page();
    if (!new_page) return -1;

    memcpy((void *)new_page, (void *)old_page, PAGE_SIZE);
    mem_map[idx]--;

    *pte = new_page | (*pte & 0xFFFULL) | PTE_WRITABLE | PTE_PRESENT;
    invlpg(addr);
    return 0;
}

int do_page_fault(struct trap_frame *tf)
{
    uint64_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

    /* Бит 1 (0x02) в error_code означает попытку записи */
    if (tf->error_code & 0x02) {
        uint64_t cr3;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
        uint64_t *pml4 = (uint64_t *)(cr3 & ~0xFFFULL);

        uint64_t pml4_idx = (cr2 >> 39) & 0x1FF;
        uint64_t pdpt_idx = (cr2 >> 30) & 0x1FF;
        uint64_t pd_idx   = (cr2 >> 21) & 0x1FF;
        uint64_t pt_idx   = (cr2 >> 12) & 0x1FF;

        if (pml4[pml4_idx] & PTE_PRESENT) {
            uint64_t *pdpt = (uint64_t *)(pml4[pml4_idx] & ~0xFFFULL);
            if (pdpt[pdpt_idx] & PTE_PRESENT) {
                uint64_t *pd = (uint64_t *)(pdpt[pdpt_idx] & ~0xFFFULL);
                if (pd[pd_idx] & PTE_PRESENT) {
                    uint64_t *pt = (uint64_t *)(pd[pd_idx] & ~0xFFFULL);
                    /* Страница присутствует в памяти, но закрыта для записи — это COW */
                    if ((pt[pt_idx] & PTE_PRESENT) && !(pt[pt_idx] & PTE_WRITABLE)) {
                        return do_wp_page(&pt[pt_idx], cr2);
                    }
                }
            }
        }
    }
    return -1;
}
