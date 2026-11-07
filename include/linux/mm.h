#ifndef _LINUX_MM_H
#define _LINUX_MM_H

#include <linux/types.h>

#define PAGE_SIZE     4096ULL
#define PAGE_ALIGN(a) (((a) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_COW       (1ULL << 9)   /* свободный бит ОС: страница была записываемой до fork() */

/* ---- Раскладка виртуальной памяти ----
 * 0 .. 1 ГБ        identity-map ядра (только supervisor, бит U сброшен)
 * 2 .. 64 ГБ       окно MMIO устройств и фреймбуфера (identity, только supervisor).
 *                  Каталоги страниц этого окна общие для ВСЕХ процессов
 *                  (создаются при старте, см. kernel_mmio_init), иначе любое
 *                  обращение ядра к устройству/экрану из процесса давало #PF.
 * 1 .. 2 ГБ        память старых (LINUS001) процессов: куча 0x40000000,
 *                  код 0x60000000, стек у 0x7FFF0000
 *                  Linux-ABI процессы: образ статического ELF линкуется по
 *                  0x40000000 (модель кода small требует адресов < 2 ГБ), brk за образом
 * 512 ГБ .. 128 ТБ mmap с 0x400000000000, стек Linux-ABI у 0x7FFFFFFFF000  */
#define USER_VA_MIN        0x40000000ULL
#define USER_VA_MAX        0x00007FFFFFFFF000ULL
#define USER_NEW_BASE      0x0000008000000000ULL   /* 512 ГБ: начало PML4[1] */
#define KMMIO_MIN          0x80000000ULL
#define KMMIO_MAX          0x1000000000ULL         /* 64 ГБ */
#define KMMIO_PDPT_FIRST   2
#define KMMIO_PDPT_LAST    63
#define LEGACY_STACK_TOP   0x7FFF0000ULL
#define LEGACY_STACK_PAGES 16
#define LINUX_STACK_TOP    0x00007FFFFFFFF000ULL
#define LINUX_STACK_MAX    (8ULL * 1024 * 1024)
#define LINUX_MMAP_BASE    0x0000400000000000ULL

#define VM_READ      1
#define VM_WRITE     2
#define VM_EXEC      4
#define VM_ANON      0x10   /* лениво заполняется нулями */
#define VM_STACK     0x20
#define VM_HEAP      0x40
#define VM_FIXED_MAP 0x80   /* страницы отображены сразу (образ ELF) */

struct vma {
    uint64_t start, end;    /* [start, end), выровнены по странице */
    uint32_t prot;          /* VM_READ|VM_WRITE|VM_EXEC */
    uint32_t flags;         /* VM_ANON, VM_STACK, VM_HEAP, ... */
};
#define VMA_PAGES 4          /* таблица областей: 4 страницы (curl с зависимостями даёт >200 областей) */
#define MAX_VMAS (VMA_PAGES * PAGE_SIZE / sizeof(struct vma))

struct task_struct;
int  user_range_ok(uint64_t addr, uint64_t len);
int  user_access_ok(uint64_t addr, uint64_t len, int write);
extern int kernel_buf_access;
int  vma_add(struct task_struct *t, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags);
struct vma *vma_find(struct task_struct *t, uint64_t addr);
int  vma_unmap(struct task_struct *t, uint64_t start, uint64_t end);
int  vma_protect(struct task_struct *t, uint64_t start, uint64_t end, uint32_t prot);
uint64_t vma_find_free(struct task_struct *t, uint64_t len);
int  vma_clone(struct task_struct *child, struct task_struct *parent);
void vma_clear(struct task_struct *t);
int  vma_handle_fault(struct task_struct *t, uint64_t addr, uint64_t err);
uint64_t prot_to_pte(uint32_t prot);

#define HIGH_MEMORY   (128ULL * 1024 * 1024)
/* Максимум, который покрывает identity-map ядра (1 ГБ) */
#define HIGH_MEMORY_MAX (1024ULL * 1024 * 1024)

void mem_init(uint64_t reserve_end, uint64_t limit);
void kernel_mmio_init(void);
/* Отобразить MMIO-регион для ядра; адрес виден из любого процесса. Возвращает
 * указатель (identity для окна 2..64 ГБ, иначе виртуальный адрес в PML4[511]) */
void *mmio_map(uint64_t phys, uint64_t size, uint64_t pte_flags);
#define KIO_BASE 0xFFFFFF8000000000ULL

/* Можно ли обращаться к MMIO-региону из любого процесса (identity-окно ядра) */
static inline int mmio_window_ok(uint64_t phys, uint64_t len)
{
    return (phys + len <= 0x40000000ULL) || (phys >= KMMIO_MIN && phys + len <= KMMIO_MAX);
}
uint64_t get_free_page(void);
uint64_t get_free_pages(uint32_t count);
void free_page(uint64_t addr);
void free_pages(uint64_t addr, uint32_t count);

int map_page(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
int unmap_user_page(uint64_t *pml4, uint64_t virt);
uint64_t *lookup_pte(uint64_t *pml4, uint64_t virt);

uint64_t create_process_pml4(void);
uint64_t copy_process_pml4(uint64_t parent_pml4);
void free_process_pml4(uint64_t pml4_phys);

uint32_t get_free_pages_count(void);
uint32_t get_total_pages_count(void);

struct trap_frame;
int do_page_fault(struct trap_frame *tf);
int do_wp_page(uint64_t *pte, uint64_t addr);

#endif
