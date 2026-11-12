CC = gcc
SMP ?= 1
CFLAGS = -DSMP_ENABLE=$(SMP) -Wall -Wextra -O2 -m64 -mcmodel=kernel -ffreestanding \
         -mgeneral-regs-only -fno-stack-protector -fno-pie -no-pie \
         -mno-red-zone -nostdinc -Iinclude $(EXTRA_CFLAGS)

USER_CFLAGS = -Wall -Wextra -O2 -m64 -ffreestanding -nostdinc \
              -fno-stack-protector -fno-pie -no-pie -mno-red-zone -Iuser -Iinclude

UEFI_CFLAGS = -Wall -Wextra -O2 -m64 -ffreestanding -fshort-wchar \
              -fno-stack-protector -fPIC -mno-red-zone -nostdinc -Iinclude \
              -fcf-protection=none -fno-asynchronous-unwind-tables -Wa,-mx86-used-note=no

LD = ld
LDFLAGS = -n -T boot/linker.ld -static --no-warn-rwx-segments

DEPFLAGS = -MMD -MP

OBJS = boot/boot.o init/main.o kernel/console.o kernel/asm.o \
       kernel/traps.o mm/memory.o kernel/switch.o kernel/sched.o \
       kernel/gdt.o kernel/syscall.o kernel/keyboard.o kernel/fork.o \
       kernel/syscall_entry.o kernel/time.o kernel/hd.o kernel/smp.o kernel/trampoline.o \
       kernel/kthread.o kernel/pci.o kernel/blk.o kernel/nvme.o kernel/ahci.o kernel/acpi.o kernel/xhci.o kernel/usb_kbd.o kernel/hid_parse.o kernel/e1000.o kernel/nic.o kernel/rtl8169.o kernel/net.o kernel/net_stack.o kernel/sock.o \
       fs/buffer.o fs/minix.o fs/ramfs.o fs/sysfs.o fs/pipe.o lib/string.o kernel/exec.o mm/vma.o kernel/linux_abi.o

# Программы ядра (родные, на ulibc): init и диагностика железа. Всё остальное -- обычные
# программы Linux (GNU coreutils, bash, fastfetch), которые копируются с хоста пакетами (HOST_PKGS).
USER_BINARIES = rootfs/bin/init rootfs/bin/poweroff rootfs/bin/reboot rootfs/bin/halt \
                rootfs/bin/usbinfo rootfs/bin/smpinfo \
                rootfs/bin/netinfo

# Программы Linux копируются с хоста пакетами целиком: все исполняемые файлы пакета (по `pacman -Ql`;
# на Debian/Fedora -- dpkg/rpm, имена пакетов там свои) и все нужные им библиотеки (через ldd).
# Свой набор: make HOST_PKGS="coreutils htop". Не установленные на хосте пакеты пропускаются.
#   bash coreutils util-linux ncurses grep sed gawk findutils diffutils tar gzip bzip2 xz less which patch -- оболочка и
#   обычный набор команд; pciutils (lspci) curl iputils (ping) nano fastfetch -- как есть.
HOST_PKGS ?= bash coreutils util-linux ncurses grep sed gawk findutils diffutils tar gzip bzip2 xz less which patch pciutils curl iputils nano fastfetch

all: Image rootfs.tar BOOTX64.EFI liveusb.img disk.img usbdisk.img

%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

Image: $(OBJS)
	$(LD) $(LDFLAGS) -o Image $(OBJS)

user/%.o: user/%.c user/ulibc.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/crt0.o: user/crt0.S
	$(CC) $(USER_CFLAGS) -c $< -o $@

rootfs/bin/%: user/crt0.o user/%.o user/ulibc.o user/user.ld
	@mkdir -p rootfs/bin
	$(LD) -T user/user.ld -static user/crt0.o user/$*.o user/ulibc.o -o $@

rootfs/bin/reboot rootfs/bin/halt: rootfs/bin/poweroff
	cp $< $@

tools/mkefi: tools/mkefi.c
	gcc -O2 tools/mkefi.c -o tools/mkefi

boot/uefi.o: boot/uefi.c include/uefi.h
	$(CC) $(UEFI_CFLAGS) -c boot/uefi.c -o boot/uefi.o

boot/uefi_jump.o: boot/uefi_jump.S
	$(CC) $(UEFI_CFLAGS) -c boot/uefi_jump.S -o boot/uefi_jump.o

BOOTX64.EFI: tools/mkefi boot/uefi.o boot/uefi_jump.o
	$(LD) -n -e efi_main -Ttext 0x1000 --oformat binary boot/uefi.o boot/uefi_jump.o -o boot/uefi.bin
	./tools/mkefi boot/uefi.bin BOOTX64.EFI

rootfs.tar: $(USER_BINARIES) tools/import_host.sh
	@mkdir -p rootfs/etc rootfs/home rootfs/usr/share/hwdata rootfs/etc/ssl rootfs/mnt rootfs/lib
	@if [ -n "$(HOST_PKGS)" ]; then ./tools/import_host.sh rootfs $(addprefix pkg:,$(HOST_PKGS)); fi
	tar --format=ustar -cf rootfs.tar -C rootfs .

liveusb.img: BOOTX64.EFI Image rootfs.tar
	@rm -f liveusb.img
	@SZ=$$(( ($$(stat -c %s rootfs.tar) + $$(stat -c %s Image) ) / 1048576 + 24 )); \
	 qemu-img create -f raw liveusb.img $${SZ}M >/dev/null 2>&1 || dd if=/dev/zero of=liveusb.img bs=1M count=$$SZ status=none
	@mkfs.vfat -F 32 -n "LINUX_EFI" liveusb.img >/dev/null
	@mmd -i liveusb.img ::/EFI ::/EFI/BOOT
	@mcopy -i liveusb.img BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	@mcopy -i liveusb.img Image ::/Image
	@mcopy -i liveusb.img rootfs.tar ::/rootfs.tar
	@echo "[OK] liveusb.img: EFI/BOOT/BOOTX64.EFI, Image, rootfs.tar"

disk.img:
	@qemu-img create -f raw disk.img 32M 2>/dev/null || dd if=/dev/zero of=disk.img bs=1M count=32 2>/dev/null

usbdisk.img:
	@qemu-img create -f raw usbdisk.img 16M 2>/dev/null || dd if=/dev/zero of=usbdisk.img bs=1M count=16 2>/dev/null

QEMU_NET = -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56

run: Image rootfs.tar disk.img usbdisk.img
	qemu-system-x86_64 -smp 2 -m 256M -kernel Image -initrd rootfs.tar \
	-drive file=disk.img,format=raw,index=0,media=disk \
	-device qemu-xhci,id=xhci -drive file=usbdisk.img,format=raw,if=none,id=usb0 -device usb-storage,bus=xhci.0,drive=usb0 \
	$(QEMU_NET) -serial mon:stdio

# то же, но с USB HID клавиатурой на xHCI
run-usb: Image rootfs.tar disk.img
	qemu-system-x86_64 -smp 2 -m 256M -kernel Image -initrd rootfs.tar \
	-drive file=disk.img,format=raw,index=0,media=disk \
	-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 $(QEMU_NET) -serial mon:stdio

run-uefi: liveusb.img disk.img
	qemu-system-x86_64 -bios /usr/share/edk2/x64/OVMF.4m.fd -smp 2 -m 256M \
	-drive file=disk.img,format=raw,index=0,media=disk \
	-device qemu-xhci,id=xhci -drive file=liveusb.img,format=raw,if=none,id=uefiboot -device usb-storage,bus=xhci.0,drive=uefiboot \
	$(QEMU_NET) -serial mon:stdio

clean:
	rm -rf $(OBJS) $(OBJS:.o=.d) Image rootfs.tar rootfs/bin rootfs/lib rootfs/home rootfs/usr/share/hwdata rootfs/etc/ssl user/*.o disk.img usbdisk.img \
	       BOOTX64.EFI liveusb.img tools/mkefi boot/uefi*.o boot/uefi.bin

-include $(OBJS:.o=.d)
