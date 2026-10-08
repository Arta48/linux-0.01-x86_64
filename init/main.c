#include <linux/tty.h>
#include <linux/traps.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/gdt.h>
#include <linux/syscall.h>
#include <linux/fs.h>
#include <linux/time.h>
#include <linux/multiboot.h>
#include <linux/hdreg.h>

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

void main(uint64_t mb_magic, uint64_t mb_info_addr)
{
    console_init();

    printk("==============================================\n");
    printk("   Linux 0.01 (x86_64 Edition) Booting...    \n");
    printk("==============================================\n\n");

    uint64_t initrd_start = 0;
    uint64_t initrd_end = 0;

    if (mb_magic == MULTIBOOT_BOOTLOADER_MAGIC && mb_info_addr != 0) {
        struct mb_info *mbi = (struct mb_info *)mb_info_addr;
        if ((mbi->flags & MB_FLAG_MODS) && mbi->mods_count > 0 && mbi->mods_addr != 0) {
            struct mb_module *mod = (struct mb_module *)((uint64_t)mbi->mods_addr);
            initrd_start = (uint64_t)mod->mod_start;
            initrd_end   = (uint64_t)mod->mod_end;
            printk("[OK] Multiboot Initrd Module: %p - %p (%d KB)\n",
                   initrd_start, initrd_end, (int)((initrd_end - initrd_start) / 1024));
        }
    }

    time_init();
    gdt_init();
    trap_init();
    syscall_init();
    mem_init(initrd_end);
    sched_init();
    ide_init();
    fs_init();

    if (initrd_start && initrd_end) {
        tarfs_mount(initrd_start, initrd_end);
    }

    task_create(user_trampoline, 10);

    __asm__ volatile ("sti");
    printk("[OK] System Initialized. Launching /bin/sh via Task 1...\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
