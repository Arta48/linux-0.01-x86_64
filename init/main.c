#include <linux/tty.h>
#include <linux/traps.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/gdt.h>
#include <linux/syscall.h>
#include <linux/fs.h>
#include <linux/minix_fs.h>
#include <linux/time.h>
#include <linux/multiboot.h>
#include <linux/hdreg.h>
#include <linux/kthread.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/net.h>
#include <linux/pci.h>
#include <linux/xhci.h>
#include <asm/io.h>

/* Фоновый поток ядра */
static int kthread_heartbeat(void *arg)
{
    (void)arg;
    for (;;) {
        for (volatile uint64_t k = 0; k < 200000000ULL; k++) {
            __asm__ volatile ("pause");
        }
    }
    return 0;
}

extern void enter_user_mode(uint64_t entry_point, uint64_t user_stack);

static inline int64_t sys_execve_user(const char *path, char **argv, char **envp)
{
    int64_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(11), "b"((uint64_t)path), "c"((uint64_t)argv), "d"((uint64_t)envp) : "memory");
    return ret;
}

static inline void sys_exit_user(int status)
{
    __asm__ volatile ("syscall" : : "a"(1), "D"(status) : "rcx", "r11", "memory");
    for (;;);
}

/* Task 1: Первичный процесс инициализации (Ring 3) */
void init_process(void)
{
    strcpy(current->name, "init");
    char *argv[] = { "/bin/sh", NULL };
    char *envp[] = { "HOME=/", "PATH=/bin", NULL };

    /* Запуск автономного бинарного шелла /bin/sh в Ring 3 */
    sys_execve_user("/bin/sh", argv, envp);

    /* Если запуск не удался */
    sys_exit_user(1);
}

void user_trampoline(void)
{
    uint64_t stack_page = get_free_pages(8); /* 32 КБ непрерывного стека */
    uint64_t user_stack = stack_page + (8 * PAGE_SIZE) - 16;
    current->user_stack_page = stack_page;
    enter_user_mode((uint64_t)init_process, user_stack);
}

struct mb_info mbi;

void main(uint64_t mb_magic, uint64_t mb_info_addr)
{
    /* COM1 Инициализация для ранних логов (до console_init) */
    outb(0x00, 0x3F8 + 1); outb(0x80, 0x3F8 + 3); outb(0x03, 0x3F8 + 0);
    outb(0x00, 0x3F8 + 1); outb(0x03, 0x3F8 + 3); outb(0xC7, 0x3F8 + 2); outb(0x0B, 0x3F8 + 4);

    if (mb_magic == MULTIBOOT_BOOTLOADER_MAGIC && mb_info_addr != 0) {
        memcpy(&mbi, (void *)mb_info_addr, sizeof(struct mb_info));
    } else {
        memset(&mbi, 0, sizeof(struct mb_info));
    }

    uint64_t initrd_start = 0;
    uint64_t initrd_end = 0;

    if ((mbi.flags & MB_FLAG_MODS) && mbi.mods_count > 0 && mbi.mods_addr != 0) {
        struct mb_module *mod = (struct mb_module *)((uint64_t)mbi.mods_addr);
        initrd_start = (uint64_t)mod->mod_start;
        initrd_end   = (uint64_t)mod->mod_end;
    }

    /* Резервируем initrd и страницу с mb_info (её кладёт UEFI-загрузчик сразу
     * за initrd) и берём границу памяти от загрузчика, если она проверена. */
    uint64_t reserve_end = initrd_end;
    uint64_t mem_limit = HIGH_MEMORY;
    if ((mbi.flags & MB_FLAG_UEFI) && (mbi.flags & MB_FLAG_MEM)) {
        if (mb_info_addr + PAGE_SIZE > reserve_end) {
            reserve_end = mb_info_addr + PAGE_SIZE;
        }
        mem_limit = 0x100000ULL + (uint64_t)mbi.mem_upper * 1024ULL;
    }

    /* Инициализируем память ДО консоли, чтобы работал маппинг фреймбуфера! */
    mem_init(reserve_end, mem_limit);

    console_init();

    printk("==============================================\n");
    printk("   Linux 0.01 (x86_64 Edition) Booting...    \n");
    printk("==============================================\n\n");

    if (initrd_start && initrd_end) {
        printk("[OK] Multiboot Initrd Module: %p - %p (%d KB)\n",
                initrd_start, initrd_end, (int)((initrd_end - initrd_start) / 1024));
    }

    time_init();
    gdt_init();
    trap_init();
    syscall_init();
    sched_init();
    ide_init();
    fs_init();
    minix_init();
    pci_scan_all();

    /* Этап 48: собственный драйвер xHCI + USB HID клавиатура (BIOS handoff:
     * прошивка перестаёт эмулировать PS/2, управление берёт ядро) */
    xhci_init();

    /* На реальном железе (UEFI) пока работаем на одном ядре: все AP стартовали
     * бы на одном общем стеке, а трамплин 0x8000 занят прошивкой. */
    smp_init((mbi.flags & MB_FLAG_UEFI) ? 1 : 0);
    net_init();
    strcpy(current->name, "idle");

    /* Запуск фонового потока ядра */
    kthread_create(kthread_heartbeat, NULL, "kworker");

    if (initrd_start && initrd_end) {
        tarfs_mount(initrd_start, initrd_end);
    }

    int init_pid = task_create(user_trampoline, 10);

    __asm__ volatile ("sti");
    printk("[OK] System Initialized. Launching /bin/sh via Task %d...\n", init_pid);

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
