# MASTER PROMPT: ПРОДОЛЖЕНИЕ РАЗРАБОТКИ LINUX 0.01 ДЛЯ x86_64

## 1. О ПРОЕКТЕ
Мы разрабатываем операционную систему "Linux 0.01 (x86_64 Edition)" — полное переосмысление исторического ядра Линуса Торвальдса Linux 0.01 (1991) для 64-битного режима Long Mode архитектуры AMD64/x86_64.
Целевая платформа сборки: Arch Linux, компилятор GCC (флаги freestanding), эмулятор QEMU (qemu-system-x86_64).
Мы пишем ядро с нуля, сохраняя дух, имена функций и структуру каталогов Linux 0.01, но заменяя 32-битные рудименты (TSS ljmp, LDT, fs-сегменты, 2-уровневый пейджинг) на современные 64-битные стандарты.

## 2. КРИТИЧЕСКИЕ ПРАВИЛА РАЗРАБОТКИ
1. Для существующих файлов выдавать только точечные измененные блоки/строки с окружающим контекстом (diff-формат) без пропуска кода внутри этих блоков. Новые файлы выдавать целиком и самодостаточно.
2. Продолжать нумерацию этапов последовательно: следующий этап — ЭТАП 52 (Stage 52).
3. Компиляция ведется со строгими флагами:
   CFLAGS = -Wall -Wextra -O2 -m64 -mcmodel=kernel -ffreestanding -mgeneral-regs-only -fno-stack-protector -fno-pie -no-pie -mno-red-zone -nostdinc -Iinclude
   USER_CFLAGS = -Wall -Wextra -O2 -m64 -ffreestanding -nostdinc -fno-stack-protector -fno-pie -no-pie -mno-red-zone -Iuser
   Запрещены XMM/SSE регистры в ядре (-mgeneral-regs-only).
   Запрещена стандартная библиотека C (-nostdinc, используется наша lib/string.c и user/ulibc.c).
4. Запуск QEMU выполняется с флагом:
   qemu-system-x86_64 -smp 2 -m 256M -kernel Image -initrd rootfs.tar -drive file=disk.img,format=raw,index=0,media=disk -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 -serial mon:stdio
5. Привилегии:
   Ring 0: CS=0x08, DS/SS=0x10.
   Ring 3 (User Space): CS=0x23 (0x20|3), DS/SS=0x1B (0x18|3).
   TSS: 16-байтовый дескриптор в GDT (селектор 0x28), поле TSS.rsp0 указывает на вершину ядерного стека текущего процесса для возврата из Ring 3.
6. Системные вызовы поддерживаются двумя путями:
   - Быстрый путь: MSR syscall / sysretq (STAR, LSTAR, SFMASK) через ассемблерный шлюз syscall_entry.S.
   - Классический путь: int $0x80 через шлюз прерывания IDT с DPL=3.

## 3. СТРУКТУРА КАТАЛОГОВ
.
├── boot/
│   ├── boot.S          # Multiboot + точка входа uefi_entry, пейджинг, переход в 64-бит
│   ├── linker.ld       # Скрипт компоновщика: elf32-i386 с 64-битным кодом, база 1 МБ
│   ├── uefi.c          # Загрузчик BOOTX64.EFI: GOP, /Image и /rootfs.tar с ESP, ExitBootServices
│   └── uefi_jump.S     # Прыжок в uefi_entry ядра
├── tests/linux/        # Проверки Linux ABI (abi_*.c, build.sh собирает их gcc хоста)
├── tools/
│   ├── import_host.sh  # Копирование программ хоста (bash, coreutils, fastfetch) и их библиотек в rootfs
│   └── mkefi.c         # Упаковка flat-binary загрузчика в PE32+ (.efi)
├── fs/
│   ├── buffer.c        # 64-битный Buffer Cache (1024B блоки, хэш-таблица, LRU)
│   ├── minix.c         # Дисковая файловая система Minix v1 на /dev/hda (/mnt)
│   ├── pipe.c          # Кольцевые каналы IPC (sys_pipe, pipe_read, pipe_write)
│   └── ramfs.c         # VFS, делегирование путей /mnt в Minix FS, TarFS
├── include/
│   ├── asm/io.h        # inb, outb, inw, outw, inl, outl, insw, outsw
│   └── linux/
│       ├── acpi.h      # ACPI: выключение/перезагрузка ПК, sys_reboot, root_task_pid (Этап 48)
│       ├── nic.h       # struct nic_ops (Этап 51)
│       ├── blkdev.h    # Блочные устройства, разделы, интерфейс драйверов накопителей (Этап 49)
│       ├── buffer.h    # Структуры buffer_head и прототипы Buffer Cache
│       ├── e1000.h     # Регистры и дескрипторы RX/TX сетевой карты Intel 82540EM
│       ├── fs.h        # VFS структуры, файловые дескрипторы, exec_header
│       ├── gdt.h       # 64-битная GDT, селекторы, TSS структура
│       ├── hdreg.h     # Регистры и команды ATA/IDE PIO LBA28
│       ├── ip.h        # Структуры заголовков ARP, IPv4, ICMP, UDP и контрольные суммы
│       ├── usb_hid.h   # hid_state, hid_layout, разбор HID report descriptor (Этап 48)
│       ├── keyboard.h  # PS/2 драйвер, опрос COM1, буфер, стрелки ANSI, inject от USB
│       ├── kthread.h   # Прототип kthread_create для создания потоков ядра
│       ├── minix_fs.h  # Minix v1 структуры: super_block, inode, dir_entry
│       ├── mm.h        # Аллокатор фреймов, CR3 per-process таблицы, COW
│       ├── multiboot.h # Multiboot структуры и tar_header
│       ├── mutex.h     # Спящие мьютексы ядра mutex_t
│       ├── net.h       # Канальный уровень Ethernet (L2)
│       ├── pci.h       # Конфигурационное пространство PCI (порты 0xCF8 / 0xCFC)
│       ├── sched.h     # task_struct, task_union, приоритеты Линуса
│       ├── signal.h    # Сигналы SIGINT, SIGKILL, SIGTERM, SIG_DFL
│       ├── smp.h       # Local APIC структуры и прототипы многоядерности
│       ├── spinlock.h  # Атомарные аппаратные спинлоки spinlock_t (xchgl)
│       ├── stat.h      # struct stat, S_IFREG, S_IFDIR
│       ├── string.h    # memcpy, memset, memcmp, strcmp, strncmp, strcpy, strncpy, strlen
│       ├── syscall.h   # Номера __NR_* и прототип syscall_dispatcher
│       ├── tcp.h
│       ├── time.h      # struct tm, kernel_mktime, CMOS RTC
│       ├── traps.h     # 16-байтовые дескрипторы IDT, trap_frame
│       ├── tty.h       # console_init, console_putc, printk
│       ├── types.h     # uint64_t, int64_t, uint32_t, NULL
│       └── utsname.h   # struct utsname для uname
├── init/
│   └── main.c          # Точка входа ядра: старт подсистем и запуск /bin/init в Ring 3
├── kernel/
│   ├── asm.S           # 64-битные заглушки прерываний ISR0..19, IRQ0, IRQ1, int 0x80
│   ├── console.c       # VGA 0xB8000 + COM1, аппаратный VT100/ANSI парсер, printk c %02x
│   ├── linux_abi.c     # Linux x86_64 ABI: syscalls, сигналы, tty, getdents64 (Этап 51)
│   ├── nic.c           # Слой nic_ops, выбор сетевой карты (Этап 51)
│   ├── rtl8169.c       # Драйвер Realtek RTL8168/8111/8169 (Этап 51)
│   ├── e1000.c         # 64-битный драйвер гигабитной сетевой карты Intel 82540EM
│   ├── fork.c          # Клонирование процессов sys_fork с COW-разделением памяти
│   ├── gdt.c           # Настройка 64-битной GDT и дескриптора TSS
│   ├── blk.c           # Блочный слой: диски, разделы GPT/MBR, права записи, /proc/disks (Этап 49)
│   ├── ahci.c          # Драйвер SATA AHCI, polling (Этап 50)
│   ├── nvme.c          # Драйвер NVMe SSD, polling (Этап 49)
│   ├── hd.c            # 64-битный ATA PIO драйвер жесткого диска (0x1F0..0x1F7)
│   ├── keyboard.c      # Скан-коды PS/2, стрелки ANSI, COM1, keyboard_inject_char() для USB
│   ├── kthread.c       # Потоки ядра (kthread_create) и спящие мьютексы
│   ├── net.c           # Канальный уровень Ethernet (отправка/прием кадров L2)
│   ├── net_stack.c     # Сетевой стек ядра: протоколы ARP, IPv4 (10.0.2.15), ICMP, UDP
│   ├── pci.c           # Сканер шины PCI через 32-битные порты 0xCF8 / 0xCFC
│   ├── acpi.c          # ACPI: разбор FADT/_S5, выключение и перезагрузка ПК (Этап 48)
│   ├── hid_parse.c     # Разбор HID report descriptor (клавиатуры без boot-протокола) (Этап 48)
│   ├── usb_kbd.c       # USB HID Usage -> ASCII, Ctrl+C, автоповтор, LED (Этап 48)
│   ├── xhci.c          # Драйвер xHCI: несколько контроллеров, хабы USB 2.0/3.x, hotplug, клавиатуры (boot и report protocol), /proc/usb (Этап 48)
│   ├── sched.c         # Планировщик Линуса (decay priority), переключение CR3
│   ├── smp.c           # Local APIC драйвер (0xFEE00000), протокол INIT-SIPI-SIPI
│   ├── switch.S        # Программное переключение контекста switch_to и ret_from_fork
│   ├── syscall.c       # Диспетчер системных вызовов, sys_sync, sys_execve
│   ├── syscall_entry.S # Аппаратный шлюз syscall/sysretq
│   ├── time.c          # CMOS RTC опрос (порты 0x70/0x71, BCD декодирование), mktime
│   ├── trampoline.S    # 16-битный трамплин 0x8000 для холодного старта ядер AP
│   └── traps.c         # Настройка IDT, перехватчик Page Fault для COW, net_poll на таймере
├── lib/
│   └── string.c        # Быстрые строковые функции на rep movsb / rep stosb, strcpy, strncpy
├── user/
│   ├── crt0.S          # Точка входа Ring 3 (_start -> main -> exit)
│   ├── init.c          # /bin/init: запускает /bin/bash, по его завершении выключает ПК
│   ├── poweroff.c      # /bin/poweroff, /bin/halt, /bin/reboot (по argv[0])
│   ├── lspci.c lsblk.c usbinfo.c smpinfo.c netinfo.c  # диагностика железа и сети
│   ├── ulibc.c         # Реализация ulibc: syscalls, printf, malloc, free, sync
│   ├── ulibc.h         # Заголовочный файл автономной библиотеки ulibc
│   └── user.ld         # Скрипт компоновки пользовательских бинарников с exec_header
└── Makefile            # Сборка ядра, init, rootfs.tar (HOST_PKGS), BOOTX64.EFI, образов; цели run, run-usb, run-uefi

## 4. СТАТУС ПРОЕКТА К ТЕКУЩЕМУ МОМЕНТУ (ЭТАП 51 ЗАВЕРШЕН)
- Этапы 1–51 реализованы. Этап 47: исправлена загрузка по UEFI на реальном ПК и консоль на фреймбуфере. Этап 48: драйвер xHCI + USB HID клавиатуры, ACPI. Этап 49: блочный слой + NVMe. Этап 50: SATA AHCI.
- Загрузчик BOOTX64.EFI: резервирует память через AllocateAddress (ядро строго по 0x100000, initrd сразу за ядром, затем страница mb_info), размеры файлов через EFI_FILE_PROTOCOL.GetInfo, никаких статических буферов (образ — flat binary без .bss), карта памяти в страницах UEFI; PE-секция RWX; ядру передаётся MB_FLAG_UEFI | MB_FLAG_MEM (mem_upper = граница непрерывной свободной области < 1 ГБ), MB_FLAG_FB (фреймбуфер). Ошибки загрузчика печатаются на экран и система останавливается.
- Ядро: mem_init(reserve_end, limit) с защитой от underflow; на UEFI-загрузке smp_init(1) (только BSP); COM1 с таймаутом; буферный кэш использует BH_FREE_DEV=0xFFFF; Minix форматирует только чистый (нулевой) диск; исключения Ring 3 завершают процесс (код 139).
- Этап 48: kernel/xhci.c (xHCI: handoff, кольца TRB, slot/endpoint, control и interrupt передачи, опрос из IRQ0 через usb_kbd_poll) и kernel/usb_kbd.c (HID Usage -> ASCII, автоповтор). Ввод попадает в общий буфер keyboard.c через keyboard_inject_char()/keyboard_inject_sigint(). xhci_init() вызывается из main() после pci_scan_all().
- Этап 48 (доработка): include/linux/usb_hid.h (hid_state на клавиатуру, LED), kernel/xhci.c поддерживает до 4 контроллеров, дерево устройств (usb_dev, до 48), USB 2.0-хабы, hotplug и обновление LED в потоке usb_thread (usb_start_thread() из main()), /proc/usb (fs/ramfs.c, индекс 17) читается /bin/usbinfo. traps.c/asm.S: шлюзы IDT 34–47, спурьёзные IRQ7/IRQ15 без EOI. Блокировка контроллера c->lock; ISR-опрос пропускает занятый контроллер.
- Этап 48 (ACPI и USB-доработка): kernel/acpi.c + include/linux/acpi.h (разбор RSDP/RSDT/XSDT/FADT, \_S5 из DSDT/SSDT; acpi_poweroff()/acpi_reboot(); sys_reboot() = syscall 88; root_task_pid -- завершение корневого шелла выключает ПК). boot/uefi.c передаёт адрес RSDP в mb_info.config_table (при MB_FLAG_UEFI; на BIOS/multiboot RSDP ищется в EBDA и 0xE0000..0xFFFFF). Таблицы ACPI выше 1 ГБ отображаются map_page() в acpi_init() (до запуска процессов), поэтому acpi_poweroff() использует только порты ввода-вывода. kernel/hid_parse.c: hid_parse_report_desc()/hid_to_boot_report() приводят отчёты клавиатур без boot-протокола (Report ID, NKRO) к 8-байтовому boot-формату. xhci.c: USB 3.x хабы (SET_HUB_DEPTH, дескриптор 0x2A, BH reset), кандидаты HID-интерфейсов usb_dev.cand[]. fs/minix.c: без ATA-диска /mnt не монтируется (INFO, не ошибка).
- Этап 49: include/linux/blkdev.h + kernel/blk.c (блочный слой: таблица BLK_MAX=40 устройств, dev 0 = IDE hda всегда занят, секторы 512 Б, blk_rw() проверяет границы и флаг writable; разделы GPT/MBR как отдельные dev с именами nvme0n1p2/sda1; blk_find_minix() выбирает раздел с валидным суперблоком Minix v1 (диски с таблицей разделов целиком не рассматриваются); blk_flush_all() вызывается из minix_sync(); storage_init() вызывается из main() после pci_scan_all(), до fs_init()). kernel/nvme.c: драйвер NVMe (polling, Admin+IO очереди по 16, bounce-буфер 8 КБ, BAR должен лежать в <1 ГБ или 3..4 ГБ -- общая часть адресных пространств процессов). fs/buffer.c -> blk_read/blk_write (блок = 2 сектора, номер dev = индекс блочного слоя). fs/minix.c: minix_init() монтирует hda (как раньше, с автоформатом только чистого диска) либо найденный раздел Minix на NVMe/SATA (никогда не форматирует; blk_set_writable() только для него). fs/ramfs.c: /dev/<имя> для всех дисков и разделов (только чтение, кроме hda), /proc/disks (индекс 18). lseek (syscall 19), /bin/lsblk, /bin/sectdump.
- Этап 50: kernel/ahci.c (AHCI, polling; MAX_HBA=4, MAX_PORTS=12; слот команды 0; один общий bounce-буфер 64 КБ на контроллер; ABAR должен лежать в <1 ГБ или 3..4 ГБ; диски sda, sdb, ... регистрируются через blk_add_disk(), storage_init() вызывает nvme_init() и ahci_init() до разбора разделов). Проверка на хосте: программная модель AHCI (LBA48 и LBA28), GPT, монтирование Minix, запись 300 КБ и проверка `fsck.minix`.
- Консоль (kernel/console.c): текст хранится в ОЗУ (cells/shown), прокрутка не читает видеопамять — чтение VRAM (write-combining) на реальном ПК занимало секунды на строку и выглядело как «перезапись вывода по кругу».
- Консоль понимает ANSI/VT100 (CSI): H/f, A/B/C/D, G, d, J, K, m (0, 7, 27 — инверсия), s/u, табуляцию; видимый курсор-блок. Нужно для nano и `clear` на экране; в COM1 байты уходят как есть.
- Проверка этапа 49 (хост, без железа): разбор GPT и поиск Minix, чтение/запись Minix-образа, созданного `mkfs.minix -1 -n 14`, и проверка результата `fsck.minix`; драйвер NVMe прогнан на программной модели контроллера (очереди, фазовый бит, PRP, создание очередей). На реальном NVMe не проверялся.
- Исправлено на этапе 48: sysret на AMD давал SS с RPL=0 -> #GP(0x18) на iretq; STAR[63:48] теперь 0x13. Консоль по умолчанию белая (current_fg = 0xFFFFFF). MAX_PCI_DEVS = 128.
- Проверено пользователем на Ryzen 5 3600 (по фото экрана): UEFI-загрузка, 2 контроллера xHCI, 2 USB-клавиатуры, шелл. Автором патчей (в среде нет QEMU/OVMF) проверялась только сборка, а парсер HID -- модульным тестом на хосте. Не проверено на железе: выключение/перезагрузка через ACPI, USB 3.x хабы, клавиатуры без boot-протокола. В QEMU: `make run-usb`.
- Этап 51 (память): ядро supervisor-only identity-map 0–1 ГБ, CR0.WP=1; общее MMIO-окно 2–64 ГБ (PDPT[2..63], страницы PD копируются в каждый PML4) и ioremap `mmio_map(phys,size,flags)` в PML4[511] (`KIO_BASE`); пользовательские страницы 1–2 ГБ (образы по 0x40000000) и от 512 ГБ (mmap-база 0x400000000000, стек Linux до 0x7FFFFFFFF000); COW через бит 9 PTE; VMA на задачу, ленивые страницы; ядерные стеки 16 КБ; `alloc_task()` обнуляет task_struct; вход `syscall` сохраняет полный trap_frame, возврат через iretq; первый процесс стартует через `user_trampoline()`. Фреймбуфер — через `mmio_map` (гипотеза причины зависания ноутбука на «Launching /bin/init»).
- Этап 51 (сеть): `include/linux/nic.h`, `kernel/nic.c` (struct nic_ops), `kernel/rtl8169.c` (RTL8168/8111/8169, порядок инициализации как в r8169: reset, unlock Cfg9346, адреса дескрипторов, RxMaxSize, CR=TE|RE, TxConfig, RxConfig, lock), `kernel/e1000.c`; TSC калибруется по каналу 2 PIT; ARP-кэш, DHCP (при отсутствии ответа — запасной 10.0.2.15/24 gw 10.0.2.2), `net_ping`, `sys_netctl` (syscall 100: INFO/PING/DHCP/SLEEP), `net_poll` также из IRQ0 (флаг занятости). QEMU без slirp: тест через Python-эмулятор LAN (`-netdev stream`).
- Этап 51 (SMP): `kernel/trampoline.S` перемещаемый (%ebx = база из CS<<4), `boot/uefi.c` выделяет нижнюю страницу (mbi.smp_trampoline), `kernel/acpi.c` `acpi_madt_cpus()`, `kernel/smp.c` последовательные INIT/SIPI по APIC ID, стек 16 КБ на AP, AP паркуются `cli; hlt` (планировщик на одном CPU). `smp_init(0)` для всех загрузок; `make SMP=0` отключает (`-DSMP_ENABLE`).
- Этап 51 (Linux ABI): `kernel/linux_abi.c`; ELF64 с `e_ident[7]==0x66` -> `current->linux_abi=1` -> `linux_syscall()` с номерами Linux x86_64; бинарники — статический musl, non-PIE, база 0x40000000. Сигналы доставляются при выходе из syscall через `lx_sigframe` на пользовательском стеке; TLS: arch_prctl + wrmsr FS_BASE при переключении контекста; tty: каноническая дисциплина строки с эхом (`TIOCGPGRP` возвращает pgrp вызывающего, если передней группы нет; Ctrl+C -> SIGINT только передней группе, иначе всем Linux-процессам); getdents64 через `sys_list`; NR_OPEN=64. Старые номера syscalls LINUS001-программ сохранены. Потоки: `struct mm_struct` и `struct files_struct` (общие у потоков, `task_struct` 1 страница, таблица fd 2 страницы), `sys_clone_thread()` в fork.c, futex по паре (cr3, адрес) в linux_abi.c, `exit_group`/`kill_other_threads`, потоки-зомби освобождаются в `reap_zombie_threads()`/waitpid лидера. Реализованы statfs/fstatfs/statx/sendfile. Не реализовано: CLONE_VM без CLONE_THREAD, clone3, robust futex.
- Этап 51 (обычные программы Linux): `kernel/exec.c` — загрузчик ELF: `ET_DYN` (база 0x555555554000, интерпретатор 0x7FFFF7DC0000), `PT_INTERP`/ld-linux, auxv (AT_PHDR, AT_BASE, AT_ENTRY, AT_RANDOM, AT_HWCAP2...), `#!`-скрипты (глубина до 4), Linux ABI определяется по `EI_OSABI==0x66`, `ET_DYN` или наличию интерпретатора; FXSAVE/FXRSTOR на задачу (`task_struct.fpu`). Пути-алиасы в `resolve_path` (fs/ramfs.c): /usr/bin, /usr/sbin, /sbin, /usr/local/* -> /bin; /usr/lib, /usr/lib64, /lib64, /lib/x86_64-linux-gnu -> /lib. `tools/import_host.sh` копирует программы хоста и их `ldd`-зависимости; `make HOST_PKGS="..."` (имена пакетов, `pkg:` в скрипте: pacman -Qlq / dpkg -L / rpm -ql; в образ идут все исполняемые файлы пакета из bin/sbin); terminfo копируется в /lib/terminfo (пути /usr/share/terminfo и /etc/terminfo отображаются туда) (нет на хосте — пропускается). Первый процесс: ядро запускает `/bin/init` (user/init.c), тот делает fork+exec `/bin/bash -l` (PATH=/bin, TERM=linux), после завершения bash — sync и reboot(0) (выключение). Linux-syscalls, которые glibc-программы требуют для coreutils: xattr (188–199 -> EOPNOTSUPP), сокеты (41–55, 288 -> EAFNOSUPPORT), fadvise64 (221), sched_getaffinity (204, все ЦП), ioctl TCGETS2/TCSETS2 (новая glibc проверяет isatty через них). /proc: cpuinfo (реальные МГц), meminfo, uptime, version, display, mounts, self/mountinfo, pci, usb, disks; /etc/mtab, /etc/group. Сеть доступна только через собственный syscall `netctl` (сокеты Linux не реализованы). Лимиты: MAX_FILES=8192, образ с coreutils ~16 МБ. Проверки: `tests/linux/build.sh` (abi_basic, abi_fork, abi_signal_pipe, abi_thread). Сборка: `make EXTRA_CFLAGS=...` добавляет флаги ядра; зависимости заголовков ведёт `-MMD`.
- Этап 51 (UEFI): если за ядром (1 МБ) не хватает места под initrd (`AllocateAddress` не удался), `boot/uefi.c` берёт самый большой участок EfiConventionalMemory ниже 1 ГБ, кладёт initrd в его начало, mb_info — сразу за ним; память между ядром и initrd не используется.
- Этап 51 (сокеты и сеть): `kernel/sock.c` (таблица 64 сокетов; TCP клиент/сервер с окном 64 КБ, RTO по SRTT/RTTVAR, быстрая повторная передача, go-back-N, TIME_WAIT; UDP; ICMP raw и SOCK_DGRAM/IPPROTO_ICMP; AF_UNIX socketpair), `kernel/net_stack.c` (ip_send_ex, очередь loopback, net_arp_prepare перед отправкой, net_poll под irq_save + sock_timer), Linux-вызовы 41–55 и 288 в linux_abi.c, `fd_pollmask` (poll/select для tty, pipe, сокетов), /etc/resolv.conf и /etc/hosts — генераторы в ramfs, `import_host.sh` копирует CA-сертификаты и локаль C.UTF-8, init задаёт LANG=C.UTF-8; таблица VMA — 4 страницы; собственный ping и NETCTL_PING/SLEEP удалены (используется ping из iputils с хоста). Тест: tap-интерфейс 10.0.2.2/24 (+ алиас 10.0.2.3 как DNS), host http.server/https/DNS-ответчик. Известно: передача через pipe медленная (~100 КБ/с, ожидание в pipe_read/write через schedule()); пробовал sched_yield_sleep — стало хуже.
- Этап 51 (util-linux): список util-linux (cal col colrm column dmesg findmnt flock getopt hexdump look mcookie more mountpoint namei renice rev setsid taskset uuidgen whereis); mount/login/su ждут PAM. sysfs: fs/sysfs.c (PCI, блочные устройства, cpu), символические ссылки только под /sys (ramfs is_link, fs_readlink); lspci/lsblk/lscpu — оригинальные, собственные удалены; lx_fault_signal (traps.c) доставляет SIGSEGV/SIGFPE/SIGILL. mm/memory.c: mem_init_ranges(uint64 [][2]) -- mem_map динамический (в первом большом участке <1 ГБ), ОЗУ >=2 ГБ identity в PDPT[2..kram_pdpt_last] (2 МБ), 1–2 ГБ пропущено; scan_hint ускоряет get_free_page; multiboot строит диапазоны из mmap (MB_FLAG_MMAP); UEFI-загрузчик передаёт 64-битные диапазоны (initrd и mb_info по-прежнему <1 ГБ). MAX_CPUS 254 (xAPIC), sched_getaffinity отдаёт все ядра, /proc/cpuinfo растёт под ядра, маски sysfs в формате Linux. Не проверено: UEFI-путь с ОЗУ >1 ГБ на железе (в QEMU проверен multiboot 256M..6G, 32 CPU). Makefile: HOST_PKGS (pkg:имя в import_host.sh). Makefile: только HOST_PKGS (HOST_APPS и списки по категориям удалены); import_host.sh делает /bin/sh копией bash. Не включены (нужны /proc/<pid>, netlink): procps (ps/top/free), iproute2 (ip/ss). console.c: UTF-8 декодируется (utf8_feed) только для экрана, ячейка = индекс глyфа (12 бит) + атрибуты; шрифт 8x16 из GNU Unifont генерирует tools/mkfont.py (unifont.otf, PIL) в include/linux/font16.h; printk поддерживает ширину/ноль (%02x). lsblk RO=1 у дисков до монтирования minix — так задумано (blk_set_writable). check_serial_events (keyboard.c): при LSR==0xFF UART нет — опрос отключается, иначе вечный цикл в IRQ0 на ноутбуке. UEFI: список свободной памяти в mb_info (MB_FLAG_RANGES). Журнал ядра: kernel/console.c (klog, 16 КБ), syscall syslog (103) для dmesg. Добавлены eventfd/eventfd2, capget/capset, getresuid/getresgid (без них curl 8.21 и bash 5.3 с glibc 2.42 не работали).
- Этап 51 (чистка): удалены собственный `sh`, `nano`, `tcc`, `httpd`, `calc`, `hello`, тестовые и отладочные программы, BusyBox, dash, `bb_alias`/`bb_applets.h`, `/etc/init.sh`, `compile.sh`, отладочный вывод (`LX_TRACE`, сообщения о процессах/задачах). Консоль: `\b` только сдвигает курсор (как в настоящем терминале), клавиши Delete/Insert/PgUp/PgDn выдают `ESC[n~`; /etc/passwd в стандартном 7-польном формате (shell). Сделано позже в этом же этапе: сокеты (kernel/sock.c, include/linux/sock.h), см. ниже. Не сделано: /sys (для настоящих lspci/lsblk); gcc. Консоль: цвета SGR (16 цветов, bold) — проверен только вывод в COM1, на экране не проверено глазами.
- Правило: после изменения заголовков обязателен `make clean` (в Makefile нет зависимостей от .h; устаревшая сборка давала мусор в task_struct и ложные «баги» fork/сигналов).
- Проверка этапа 51: в QEMU (multiboot и UEFI/OVMF, 4 CPU) проходят тесты Linux ABI (fork, wait, сигналы, pipe, файлы, tty, потоки pthread, futex, statfs, sendfile), bash 5.2 (glibc, динамически) и GNU coreutils 9.4 интерактивно (ls, cp, cat, sort, wc, df, id, md5sum, пайпы, фоновые задачи, `exit` выключает ПК). fastfetch не проверялся: в среде разработки нет его бинарника. НЕ проверено на реальном железе: исправление зависания ноутбука, драйвер Realtek, DHCP в настоящей сети, SMP, CR0.WP и весь Linux ABI.
- Этап 51 (ноутбук ECHIPS): по фото зависание наступало до входа в Ring 3 (нет «[INIT] /bin/init loaded»), вероятно IRQ0 не доходил до CPU. Меры: LINT0=ExtINT, опрос i8042 из тика, `timer_selftest()` с запасным LAPIC timer. Проверено только в QEMU (в том числе с намеренно замаскированным IRQ0).
- Известные проблемы: планировщик работает на одном CPU (AP паркуются); нет установки на диск (этап 52).

## 5. СЛЕДУЮЩИЕ ПЛАНИРУЕМЫЕ ЭТАПЫ (НАЧИНАЯ С ЭТАПА 52):
* **ЭТАП 52: Готовый дистрибутив и программа установки (Installer):**
  Сборка гибридного LiveUSB/ISO образа с возможностью как работы без установки из ОЗУ, так и установки на диск.
