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
#include <linux/acpi.h>
#include <linux/blkdev.h>
#include <asm/io.h>

/* Фоновый поток ядра */
extern int64_t sys_execve_ex(const char *filename, char **argv, char **envp, struct trap_frame *tf, int from_kernel);
extern void launch_user_frame(struct trap_frame *tf);

/* Task 3: первый пользовательский процесс. Загружаем /bin/init прямо из ядра и
 * уходим в Ring 3 через iretq по готовому кадру (раньше в Ring 3 выполнялась
 * функция ядра -- это невозможно при защите ядра от пользователя). */
void user_trampoline(void)
{
    struct trap_frame tf;
    memset(&tf, 0, sizeof(tf));
    tf.cs = 0x23;
    tf.ss = 0x1B;
    tf.rflags = 0x202;

    strcpy(current->name, "init");
    printk("[INIT] Task %d started, loading /bin/init...\n", (int)current->pid);
    char *argv[] = { "/bin/init", NULL };
    char *envp[] = { "HOME=/", "PATH=/bin", "TERM=linux", NULL };

    if (sys_execve_ex("/bin/init", argv, envp, &tf, 1) < 0) {
        printk("[INIT] cannot execute /bin/init\n");
        sys_exit(1);
    }
    printk("[INIT] /bin/init loaded: entering Ring 3 rip=%p rsp=%p\n", tf.rip, tf.rsp);
    launch_user_frame(&tf);
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
    if ((mbi.flags & MB_FLAG_UEFI) && (mbi.flags & MB_FLAG_MEM) && mb_info_addr + PAGE_SIZE > reserve_end) {
        reserve_end = mb_info_addr + PAGE_SIZE;
    }
    if (mbi.flags & MB_FLAG_MEM) {          /* размер ОЗУ сообщает загрузчик (UEFI-загрузчик или multiboot) */
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
    pci_scan_all();
    storage_init();       /* ATA/IDE, NVMe, разделы GPT/MBR */
    fs_init();
    minix_init();
    acpi_init((mbi.flags & MB_FLAG_UEFI) ? (uint64_t)mbi.config_table : 0);

    /* Этап 48: собственный драйвер xHCI + USB HID клавиатура (BIOS handoff:
     * прошивка перестаёт эмулировать PS/2, управление берёт ядро) */
    xhci_init();

    /* SMP: AP стартуют по INIT/SIPI с отдельным стеком каждый; трамплин на
     * UEFI лежит на странице, выделенной загрузчиком (mbi.smp_trampoline). */
    smp_init(0);
    net_init();
    strcpy(current->name, "idle");

    /* Запуск фонового потока ядра */
    usb_start_thread();

    if (initrd_start && initrd_end) {
        tarfs_mount(initrd_start, initrd_end);
    }

    int init_pid = task_create(user_trampoline, 10);
    root_task_pid = init_pid;

    __asm__ volatile ("sti");
    printk("[OK] System Initialized. Launching /bin/init via Task %d...\n", init_pid);
    timer_selftest();

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
