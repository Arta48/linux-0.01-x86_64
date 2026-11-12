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
static uint32_t paging_pages = 0;     /* длина mem_map: от low_mem до верхней границы, с дырами */
static uint32_t total_pages = 0;      /* страниц под управлением ядра (без дыр и зарезервированного) */
static uint32_t scan_hint = 0;        /* ниже этого индекса свободных страниц нет */

/* Счётчики ссылок живут в самой ОЗУ (2 байта на страницу): место берётся из первого
 * большого свободного участка ниже 1 ГБ при старте. Размер ОЗУ ограничен только
 * окном identity-map ядра (PDPT[2..511], до 512 ГБ) */
static uint16_t *mem_map;

/* Старший PDPT-индекс общей identity-карты ядра (процессы копируют PDPT[2..kram_pdpt_last]) */
int kram_pdpt_last = KMMIO_PDPT_LAST;

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

/* Общие каталоги страниц окна MMIO (PDPT[2..63]): создаются заранее и копируются
 * в PML4 каждого процесса, поэтому mapping устройства, сделанный в любом
 * контексте, виден всем. */
static uint64_t kio_next = KIO_BASE;

void kernel_mmio_init(void)
{
    extern char pdpt_table[];
    extern char pml4_table[];
    /* PML4[511]: общая область ioremap (для BAR выше 64 ГБ) */
    uint64_t *boot_pml4 = (uint64_t *)pml4_table;
    if (!(boot_pml4[511] & PTE_PRESENT)) {
        uint64_t pdpt = get_free_page();
        if (pdpt) boot_pml4[511] = pdpt | PTE_PRESENT | PTE_WRITABLE;
    }
    uint64_t *boot_pdpt = (uint64_t *)pdpt_table;
    for (int i = KMMIO_PDPT_FIRST; i <= KMMIO_PDPT_LAST; i++) {
        if (boot_pdpt[i] & PTE_PRESENT) continue;
        uint64_t pd = get_free_page();
        if (!pd) break;
        boot_pdpt[i] = pd | PTE_PRESENT | PTE_WRITABLE;
    }
}

#define KRAM_TOP        (512ULL << 30)    /* identity-карта ядра: PDPT[0..511] */
#define KRAM_GAP_LO     (1ULL << 30)      /* 1..2 ГБ -- пользовательское окно PDPT[1], ядру недоступно */
#define KRAM_GAP_HI     (2ULL << 30)
#define PTE_PS          (1ULL << 7)
#define MAX_SEGS        512

/* Подготовить список участков: вычесть окно 1..2 ГБ, обрезать по limit */
static int build_segs(const uint64_t (*r)[2], int n, uint64_t limit, uint64_t (*seg)[2])
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        uint64_t s = PAGE_ALIGN(r[i][0]), e = r[i][1] & ~(uint64_t)(PAGE_SIZE - 1);
        if (e > limit) e = limit;
        if (e <= s) continue;
        uint64_t pieces[2][2] = { { s, e < KRAM_GAP_LO ? e : KRAM_GAP_LO },
                                  { s > KRAM_GAP_HI ? s : KRAM_GAP_HI, e } };
        for (int k = 0; k < 2; k++) {
            if (pieces[k][1] <= pieces[k][0] || m >= MAX_SEGS) continue;
            seg[m][0] = pieces[k][0]; seg[m][1] = pieces[k][1]; m++;
        }
    }
    return m;
}

/* RAM выше 2 ГБ: identity-отображение (WB, supervisor). Целые 2 МБ-блоки -- большими
 * страницами, края -- обычными */
static void map_ram_identity(uint64_t s, uint64_t e)
{
    extern char pdpt_table[];
    uint64_t *boot_pdpt = (uint64_t *)pdpt_table;
    uint64_t a = s;
    while (a < e) {
        if ((a & 0x1FFFFF) == 0 && a + 0x200000 <= e) {
            uint64_t *pd = (uint64_t *)(boot_pdpt[a >> 30] & ~0xFFFULL);
            pd[(a >> 21) & 0x1FF] = a | PTE_PRESENT | PTE_WRITABLE | PTE_PS;
            a += 0x200000;
        } else {
            map_page(NULL, a, a, PTE_WRITABLE);
            a += PAGE_SIZE;
        }
    }
}

/* Свободная память -- набор участков (карта UEFI или multiboot): всё остальное занято */
void mem_init_ranges(const uint64_t (*r)[2], int n)
{
    static uint64_t seg[MAX_SEGS][2];
    uint64_t limit = KRAM_TOP;
    int m = build_segs(r, n, limit, seg);
    uint64_t lo, hi, bytes;
    int home = -1;

    for (int attempt = 0; attempt < 2; attempt++) {
        lo = ~0ULL; hi = 0;
        for (int i = 0; i < m; i++) {
            if (seg[i][0] < lo) lo = seg[i][0];
            if (seg[i][1] > hi) hi = seg[i][1];
        }
        if (m == 0) break;
        bytes = PAGE_ALIGN(((hi - lo) / PAGE_SIZE) * sizeof(uint16_t));
        /* дом для mem_map: первый участок ниже 1 ГБ, куда он влезает */
        home = -1;
        uint64_t best = 0;
        for (int i = 0; i < m && home < 0; i++)
            if (seg[i][1] <= KRAM_GAP_LO && seg[i][1] - seg[i][0] > bytes) home = i;
        if (home >= 0) break;
        for (int i = 0; i < m; i++)
            if (seg[i][1] <= KRAM_GAP_LO && seg[i][1] - seg[i][0] > best) best = seg[i][1] - seg[i][0];
        if (best <= 2 * PAGE_SIZE) break;
        limit = lo + ((best - PAGE_SIZE) / sizeof(uint16_t)) * PAGE_SIZE;   /* не вся ОЗУ: сколько позволит mem_map */
        m = build_segs(r, n, limit, seg);
    }
    if (m == 0 || home < 0) {
        paging_pages = 0; total_pages = 0; low_mem = high_mem = PAGE_ALIGN((uint64_t)_end);
        printk("[PANIC] mem_init_ranges: no free memory\n");
        return;
    }

    low_mem = lo;
    paging_pages = (uint32_t)((hi - lo) / PAGE_SIZE);
    high_mem = hi;
    mem_map = (uint16_t *)seg[home][0];
    for (uint32_t i = 0; i < paging_pages; i++) mem_map[i] = 1;
    uint64_t free_pages = 0;
    for (int k = 0; k < m; k++)
        for (uint64_t a = seg[k][0]; a < seg[k][1]; a += PAGE_SIZE) { mem_map[(a - lo) / PAGE_SIZE] = 0; free_pages++; }
    for (uint64_t a = (uint64_t)mem_map; a < (uint64_t)mem_map + bytes; a += PAGE_SIZE) {
        mem_map[(a - lo) / PAGE_SIZE] = 1; free_pages--;
    }
    total_pages = (uint32_t)free_pages;
    scan_hint = 0;

    kernel_mmio_init();
    /* Каталоги PDPT[64..] под ОЗУ выше 64 ГБ (до 64 ГБ они созданы в kernel_mmio_init) */
    extern char pdpt_table[];
    uint64_t *boot_pdpt = (uint64_t *)pdpt_table;
    int last = (int)((hi - 1) >> 30);
    if (last > 511) last = 511;
    for (int i = KMMIO_PDPT_LAST + 1; i <= last; i++) {
        if (boot_pdpt[i] & PTE_PRESENT) continue;
        uint64_t pd = get_free_page();
        if (!pd) { last = i - 1; break; }
        boot_pdpt[i] = pd | PTE_PRESENT | PTE_WRITABLE;
    }
    if (last > kram_pdpt_last) kram_pdpt_last = last;
    for (int k = 0; k < m; k++) {
        if (seg[k][1] <= KRAM_GAP_HI) continue;                 /* ниже 1 ГБ уже отображено загрузчиком */
        if ((seg[k][0] >> 30) > (uint64_t)kram_pdpt_last) continue;
        uint64_t e = seg[k][1];
        if (e > ((uint64_t)(kram_pdpt_last + 1) << 30)) e = (uint64_t)(kram_pdpt_last + 1) << 30;
        map_ram_identity(seg[k][0], e);
    }

    printk("[OK] Memory Manager: %d range(s), %p - %p, mem_map %d KB\n", m, lo, hi, (int)(bytes / 1024));
    printk("[OK] Free Physical Pages: %d (%d MB free for allocation)\n",
           (int)free_pages, (int)(free_pages * PAGE_SIZE / (1024 * 1024)));
    if (limit < KRAM_TOP && hi >= limit)
        printk("[WARN] Memory: limited to %d MB by the page map\n", (int)(limit >> 20));
}

/* Один непрерывный участок [PAGE_ALIGN(max(_end, reserve_end)), limit) -- запасной путь без карты памяти */
void mem_init(uint64_t reserve_end, uint64_t limit)
{
    uint64_t base = (uint64_t)_end;
    if (reserve_end > base) base = reserve_end;
    if (limit == 0) limit = HIGH_MEMORY;
    uint64_t r[1][2] = { { PAGE_ALIGN(base), limit } };
    mem_init_ranges((const uint64_t (*)[2])r, 1);
}

uint64_t get_free_page(void)
{
    uint64_t flags = spin_lock_irqsave(&mem_lock);
    for (uint32_t i = scan_hint; i < paging_pages; i++) {
        if (mem_map[i] == 0) {
            mem_map[i] = 1;
            scan_hint = i + 1;
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

    for (uint32_t i = scan_hint; i <= paging_pages - count; i++) {
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
        if (mem_map[index] == 0 && index < scan_hint) scan_hint = index;
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

    /* PML4[0] -> персональная PDPT процесса. Бит U нужен на всех уровнях, чтобы
     * пользователь вообще мог обращаться к страницам под PDPT[1], [2] и т. д. */
    pml4[0] = pdpt_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;

    /* PDPT[0] -> общая таблица ядра pd_table (0..1 ГБ identity map).
     * БЕЗ бита U: ядро недоступно из Ring 3 (раньше было 0x87 -- любая
     * пользовательская программа могла читать и портить память ядра). */
    pdpt[0] = (uint64_t)pd_table | PTE_PRESENT | PTE_WRITABLE;

    /* Устройства ядра (Local APIC 0xFEE00000, MMIO NVMe/AHCI в PDPT[3]) --
     * общие для всех процессов, только supervisor. */
    extern char pdpt_table[];
    uint64_t *boot_pdpt = (uint64_t *)pdpt_table;
    for (int i = KMMIO_PDPT_FIRST; i <= kram_pdpt_last; i++) pdpt[i] = boot_pdpt[i];

    extern char pml4_table[];
    pml4[511] = ((uint64_t *)pml4_table)[511];

    return pml4_phys;
}

/* Страница PTE принадлежит пулу? Тогда у неё есть счётчик ссылок */
static inline int page_in_pool(uint64_t phys)
{
    return phys >= low_mem && phys < high_mem;
}

/* Обход пользовательских PTE: cb вызывается для каждой присутствующей
 * пользовательской страницы. PML4[0] -> только PDPT[1..511] без [3] (ядро/MMIO). */
typedef int (*pte_cb)(uint64_t virt, uint64_t *pte, void *arg);

static int walk_user(uint64_t *pml4, pte_cb cb, void *arg, int free_tables)
{
    for (uint64_t i4 = 0; i4 < 256; i4++) {          /* нижняя половина адресов */
        if (!(pml4[i4] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)(pml4[i4] & 0x000FFFFFFFFFF000ULL);
        for (uint64_t i3 = (i4 == 0 ? 1 : 0); i3 < 512; i3++) {
            if (i4 == 0 && i3 != 1) continue;        /* кроме 1..2 ГБ -- ядро и MMIO */
            if (!(pdpt[i3] & PTE_PRESENT)) continue;
            uint64_t *pd = (uint64_t *)(pdpt[i3] & 0x000FFFFFFFFFF000ULL);
            for (uint64_t i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PTE_PRESENT)) continue;
                uint64_t *pt = (uint64_t *)(pd[i2] & 0x000FFFFFFFFFF000ULL);
                for (uint64_t i1 = 0; i1 < 512; i1++) {
                    if (!(pt[i1] & PTE_PRESENT)) continue;
                    uint64_t virt = (i4 << 39) | (i3 << 30) | (i2 << 21) | (i1 << 12);
                    if (cb(virt, &pt[i1], arg) < 0) return -1;
                }
                if (free_tables) free_page((uint64_t)pt);
            }
            if (free_tables) free_page((uint64_t)pd);
        }
        if (free_tables && i4 != 0) free_page((uint64_t)pdpt);
    }
    return 0;
}

struct cow_ctx { uint64_t *child; };

static int cow_cb(uint64_t virt, uint64_t *pte, void *arg)
{
    struct cow_ctx *c = (struct cow_ctx *)arg;
    uint64_t phys = *pte & 0x000FFFFFFFFFF000ULL;
    uint64_t flags = *pte & 0xFFF;

    /* Copy-On-Write: записываемую страницу помечаем PTE_COW и закрываем на запись
     * у обоих процессов. Уже только читаемые страницы просто разделяются. */
    if (flags & PTE_WRITABLE) {
        flags = (flags & ~PTE_WRITABLE) | PTE_COW;
        *pte = phys | flags;
        invlpg(virt);
    }
    if (page_in_pool(phys)) {
        mem_map[(phys - low_mem) / PAGE_SIZE]++;
    }
    if (map_page(c->child, virt, phys, flags) < 0) return -1;
    return 0;
}

uint64_t copy_process_pml4(uint64_t parent_pml4)
{
    uint64_t child_pml4_phys = create_process_pml4();
    if (!child_pml4_phys) return 0;
    if (!parent_pml4) return child_pml4_phys;

    struct cow_ctx ctx = { (uint64_t *)child_pml4_phys };
    if (walk_user((uint64_t *)parent_pml4, cow_cb, &ctx, 0) < 0) {
        free_process_pml4(child_pml4_phys);
        return 0;
    }
    return child_pml4_phys;
}

static int free_cb(uint64_t virt, uint64_t *pte, void *arg)
{
    (void)virt; (void)arg;
    free_page(*pte & 0x000FFFFFFFFFF000ULL);
    *pte = 0;
    return 0;
}

void free_process_pml4(uint64_t pml4_phys)
{
    if (!pml4_phys) return;
    uint64_t *pml4 = (uint64_t *)pml4_phys;

    walk_user(pml4, free_cb, NULL, 1);

    /* PDPT[0] процесса (персональная; pd_table внутри неё общая -- не трогаем) */
    if (pml4[0] & PTE_PRESENT) {
        free_page(pml4[0] & 0x000FFFFFFFFFF000ULL);
    }
    free_page(pml4_phys);
}

/* Снять отображение одной пользовательской страницы (освободить кадр) */
int unmap_user_page(uint64_t *pml4, uint64_t virt)
{
    uint64_t i4 = (virt >> 39) & 0x1FF, i3 = (virt >> 30) & 0x1FF;
    uint64_t i2 = (virt >> 21) & 0x1FF, i1 = (virt >> 12) & 0x1FF;
    if (!(pml4[i4] & PTE_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t *)(pml4[i4] & 0x000FFFFFFFFFF000ULL);
    if (!(pdpt[i3] & PTE_PRESENT)) return 0;
    uint64_t *pd = (uint64_t *)(pdpt[i3] & 0x000FFFFFFFFFF000ULL);
    if (!(pd[i2] & PTE_PRESENT)) return 0;
    uint64_t *pt = (uint64_t *)(pd[i2] & 0x000FFFFFFFFFF000ULL);
    if (!(pt[i1] & PTE_PRESENT)) return 0;
    free_page(pt[i1] & 0x000FFFFFFFFFF000ULL);
    pt[i1] = 0;
    invlpg(virt);
    return 1;
}

/* Найти PTE пользовательской страницы (NULL, если таблиц нет) */
uint64_t *lookup_pte(uint64_t *pml4, uint64_t virt)
{
    uint64_t i4 = (virt >> 39) & 0x1FF, i3 = (virt >> 30) & 0x1FF;
    uint64_t i2 = (virt >> 21) & 0x1FF, i1 = (virt >> 12) & 0x1FF;
    if (!(pml4[i4] & PTE_PRESENT)) return NULL;
    uint64_t *pdpt = (uint64_t *)(pml4[i4] & 0x000FFFFFFFFFF000ULL);
    if (!(pdpt[i3] & PTE_PRESENT)) return NULL;
    uint64_t *pd = (uint64_t *)(pdpt[i3] & 0x000FFFFFFFFFF000ULL);
    if (!(pd[i2] & PTE_PRESENT)) return NULL;
    if (pd[i2] & (1ULL << 7)) return NULL;           /* 2 МБ страница ядра */
    uint64_t *pt = (uint64_t *)(pd[i2] & 0x000FFFFFFFFFF000ULL);
    return &pt[i1];
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
    return total_pages;
}

int do_wp_page(uint64_t *pte, uint64_t addr)
{
    uint64_t old_page = *pte & 0x000FFFFFFFFFF000ULL;
    uint32_t idx = page_in_pool(old_page) ? (old_page - low_mem) / PAGE_SIZE : 0;
    uint64_t keep = (*pte & 0xFFF) & ~PTE_COW;

    /* Если страницу больше никто не делит -- просто возвращаем право записи */
    if (page_in_pool(old_page) && mem_map[idx] <= 1) {
        *pte = old_page | keep | PTE_WRITABLE | PTE_PRESENT;
        invlpg(addr);
        return 0;
    }

    /* Иначе приватная копия страницы для пишущего процесса */
    uint64_t new_page = get_free_page();
    if (!new_page) return -1;

    memcpy((void *)new_page, (void *)old_page, PAGE_SIZE);
    if (page_in_pool(old_page) && mem_map[idx] > 0) {
        mem_map[idx]--;
        if (mem_map[idx] == 0 && idx < scan_hint) scan_hint = idx;
    }

    *pte = new_page | keep | PTE_WRITABLE | PTE_PRESENT;
    invlpg(addr);
    return 0;
}

/* Page Fault: возвращает 0, если ошибка устранена (COW или ленивое выделение).
 * Иначе -1 -- вызывающий завершит процесс (SIGSEGV) или остановит ядро. */
int do_page_fault(struct trap_frame *tf)
{
    uint64_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

    if (cr2 < USER_VA_MIN || cr2 >= USER_VA_MAX) return -1;

    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *pml4 = (uint64_t *)(cr3 & ~0xFFFULL);
    uint64_t err = tf->error_code;

    uint64_t *pte = lookup_pte(pml4, cr2);
    if ((err & 1) && pte && (*pte & PTE_PRESENT)) {
        /* Защитное нарушение на присутствующей странице: допустим только COW */
        if ((err & 2) && (*pte & PTE_COW)) {
            return do_wp_page(pte, cr2);
        }
        return -1;
    }

    /* Страница отсутствует: ленивое выделение по VMA */
    extern struct task_struct *current;
    return vma_handle_fault(current, cr2, err);
}

void *mmio_map(uint64_t phys, uint64_t size, uint64_t pte_flags)
{
    uint64_t off = phys & (PAGE_SIZE - 1);
    uint64_t base = phys - off;
    uint64_t pages = (size + off + PAGE_SIZE - 1) / PAGE_SIZE;
    int identity = mmio_window_ok(phys, size);
    uint64_t va = identity ? base : kio_next;

    for (uint64_t i = 0; i < pages; i++) {
        /* ниже 1 ГБ MMIO уже покрыт identity-map ядра */
        if (identity && base + i * PAGE_SIZE < 0x40000000ULL) continue;
        if (map_page(NULL, va + i * PAGE_SIZE, base + i * PAGE_SIZE, pte_flags) < 0) return NULL;
    }
    if (!identity) kio_next += pages * PAGE_SIZE;
    return (void *)(va + off);
}
