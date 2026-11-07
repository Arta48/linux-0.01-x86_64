/*
 * mm/vma.c -- области виртуальной памяти процесса (VMA), ленивое выделение
 * страниц (anonymous mmap, куча, стек) и mprotect/munmap.
 *
 * Список VMA живёт на одной странице (t->mm->vmas), отсортирован по start.
 */
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/string.h>

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* Адрес пользовательской памяти? (не ядро и не окно MMIO 3..4 ГБ) */
int user_range_ok(uint64_t addr, uint64_t len)
{
    if (len > USER_VA_MAX) return 0;
    uint64_t end = addr + len;
    if (end < addr) return 0;
    /* Старые процессы: 1..2 ГБ */
    if (addr >= USER_VA_MIN && end <= 0x80000000ULL) return 1;
    /* Linux-ABI: от 512 ГБ до границы пользовательского адресного пространства */
    if (addr >= USER_NEW_BASE && end <= USER_VA_MAX) return 1;
    return 0;
}

/* Доступен ли диапазон процессу: каждая страница должна лежать в VMA с нужными правами */
int kernel_buf_access = 0;      /* 1: адреса -- буфер ядра (sendfile), проверка VMA не нужна */

int user_access_ok(uint64_t addr, uint64_t len, int write)
{
    if (len == 0) return 1;
    if (kernel_buf_access) return 1;
    if (!user_range_ok(addr, len)) return 0;
    for (uint64_t pg = addr & ~(PAGE_SIZE - 1); pg < addr + len; pg += PAGE_SIZE) {
        struct vma *v = vma_find(current, pg);
        if (!v) return 0;
        if (!(v->prot & (VM_READ | VM_WRITE | VM_EXEC))) return 0;
        if (write && !(v->prot & VM_WRITE)) return 0;
    }
    return 1;
}

/* Права VMA -> флаги PTE. Без R/W/X (PROT_NONE) страница недоступна из Ring 3. */
uint64_t prot_to_pte(uint32_t prot)
{
    uint64_t f = PTE_PRESENT;
    if (prot & (VM_READ | VM_WRITE | VM_EXEC)) f |= PTE_USER;
    if (prot & VM_WRITE) f |= PTE_WRITABLE;
    return f;
}

static struct vma *vma_table(struct task_struct *t, int create)
{
    if (!t->mm->vmas && create) {
        uint64_t pg = get_free_pages(VMA_PAGES);
        if (!pg) return NULL;
        t->mm->vmas = (struct vma *)pg;
        t->mm->nvma = 0;
    }
    return t->mm->vmas;
}

int vma_add(struct task_struct *t, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags)
{
    start &= ~(PAGE_SIZE - 1);
    end = PAGE_ALIGN(end);
    if (end <= start) return -1;
    struct vma *v = vma_table(t, 1);
    if (!v || t->mm->nvma >= (int)MAX_VMAS) return -1;

    int i = t->mm->nvma;
    while (i > 0 && v[i - 1].start > start) {
        v[i] = v[i - 1];
        i--;
    }
    v[i].start = start;
    v[i].end = end;
    v[i].prot = prot;
    v[i].flags = flags;
    t->mm->nvma++;
    return 0;
}

struct vma *vma_find(struct task_struct *t, uint64_t addr)
{
    if (!t->mm->vmas) return NULL;
    for (int i = 0; i < t->mm->nvma; i++) {
        if (addr >= t->mm->vmas[i].start && addr < t->mm->vmas[i].end) return &t->mm->vmas[i];
    }
    return NULL;
}

static void vma_remove_index(struct task_struct *t, int i)
{
    for (int k = i; k < t->mm->nvma - 1; k++) t->mm->vmas[k] = t->mm->vmas[k + 1];
    t->mm->nvma--;
}

/* Разрезать VMA по адресу at (start < at < end) на две */
static int vma_split(struct task_struct *t, int i, uint64_t at)
{
    struct vma *v = t->mm->vmas;
    if (t->mm->nvma >= (int)MAX_VMAS) return -1;
    for (int k = t->mm->nvma; k > i + 1; k--) v[k] = v[k - 1];
    v[i + 1] = v[i];
    v[i].end = at;
    v[i + 1].start = at;
    t->mm->nvma++;
    return 0;
}

int vma_unmap(struct task_struct *t, uint64_t start, uint64_t end)
{
    start &= ~(PAGE_SIZE - 1);
    end = PAGE_ALIGN(end);
    if (end <= start || !t->mm->vmas) return 0;

    for (int i = 0; i < t->mm->nvma; ) {
        struct vma *v = &t->mm->vmas[i];
        if (v->end <= start || v->start >= end) { i++; continue; }
        if (v->start < start && v->end > end) {         /* дыра внутри */
            if (vma_split(t, i, start) < 0) return -1;
            continue;                                   /* повторим для правой половины */
        }
        if (v->start < start) { v->end = start; i++; continue; }
        if (v->end > end)     { v->start = end; i++; continue; }
        vma_remove_index(t, i);                         /* VMA целиком внутри */
    }

    uint64_t *pml4 = (uint64_t *)t->cr3;
    if (pml4) {
        for (uint64_t va = start; va < end; va += PAGE_SIZE) unmap_user_page(pml4, va);
    }
    return 0;
}

int vma_protect(struct task_struct *t, uint64_t start, uint64_t end, uint32_t prot)
{
    start &= ~(PAGE_SIZE - 1);
    end = PAGE_ALIGN(end);
    if (end <= start) return 0;

    for (int i = 0; i < t->mm->nvma; i++) {
        struct vma *v = &t->mm->vmas[i];
        if (v->end <= start || v->start >= end) continue;
        if (v->start < start) {
            if (vma_split(t, i, start) < 0) return -1;
            continue;
        }
        if (v->end > end) {
            if (vma_split(t, i, end) < 0) return -1;
        }
        t->mm->vmas[i].prot = prot;
    }

    uint64_t *pml4 = (uint64_t *)t->cr3;
    if (!pml4) return 0;
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t *pte = lookup_pte(pml4, va);
        if (!pte || !(*pte & PTE_PRESENT)) continue;
        uint64_t f = *pte & 0xFFF;
        f &= ~(PTE_USER | PTE_WRITABLE);
        if (prot & (VM_READ | VM_WRITE | VM_EXEC)) f |= PTE_USER;
        /* Страницу под COW оставляем закрытой на запись -- откроет обработчик */
        if ((prot & VM_WRITE) && !(f & PTE_COW)) f |= PTE_WRITABLE;
        *pte = (*pte & PTE_ADDR_MASK) | f;
        __asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
    }
    return 0;
}

/* Свободный диапазон для mmap: простое наращивание от LINUX_MMAP_BASE */
uint64_t vma_find_free(struct task_struct *t, uint64_t len)
{
    len = PAGE_ALIGN(len);
    uint64_t base = t->mm->mmap_cur ? t->mm->mmap_cur : LINUX_MMAP_BASE;
    t->mm->mmap_cur = base + len + PAGE_SIZE;        /* зазор-ловушка между областями */
    return base;
}

int vma_clone(struct task_struct *child, struct task_struct *parent)
{
    child->mm->vmas = NULL;
    child->mm->nvma = 0;
    if (!parent->mm->vmas) return 0;
    uint64_t pg = get_free_pages(VMA_PAGES);
    if (!pg) return -1;
    memcpy((void *)pg, parent->mm->vmas, VMA_PAGES * PAGE_SIZE);
    child->mm->vmas = (struct vma *)pg;
    child->mm->nvma = parent->mm->nvma;
    return 0;
}

void vma_clear(struct task_struct *t)
{
    if (t->mm->vmas) free_pages((uint64_t)t->mm->vmas, VMA_PAGES);
    t->mm->vmas = NULL;
    t->mm->nvma = 0;
}

/* Ленивое выделение страницы. err -- код ошибки #PF. 0 = исправлено. */
int vma_handle_fault(struct task_struct *t, uint64_t addr, uint64_t err)
{
    struct vma *v = vma_find(t, addr);
    if (!v || !(v->flags & VM_ANON)) return -1;
    if ((err & 2) && !(v->prot & VM_WRITE)) return -1;
    if ((err & 16) && !(v->prot & VM_EXEC)) return -1;
    if (!(err & 2) && !(err & 16) && !(v->prot & (VM_READ | VM_WRITE | VM_EXEC))) return -1;

    uint64_t phys = get_free_page();        /* уже обнулена */
    if (!phys) return -1;
    uint64_t flags = prot_to_pte(v->prot) | PTE_USER | PTE_WRITABLE;
    if (!(v->prot & VM_WRITE)) flags &= ~PTE_WRITABLE;
    if (map_page((uint64_t *)t->cr3, addr & ~(PAGE_SIZE - 1), phys, flags) < 0) {
        free_page(phys);
        return -1;
    }
    return 0;
}
