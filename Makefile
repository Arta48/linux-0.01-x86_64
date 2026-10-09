CC = gcc
CFLAGS = -Wall -Wextra -O2 -m64 -mcmodel=kernel -ffreestanding \
         -mgeneral-regs-only -fno-stack-protector -fno-pie -no-pie \
         -mno-red-zone -nostdinc -Iinclude

USER_CFLAGS = -Wall -Wextra -O2 -m64 -ffreestanding -nostdinc \
              -fno-stack-protector -fno-pie -no-pie -mno-red-zone -Iuser -Iinclude

UEFI_CFLAGS = -Wall -Wextra -O2 -m64 -ffreestanding -fshort-wchar \
              -fno-stack-protector -fPIC -mno-red-zone -nostdinc -Iinclude \
              -fcf-protection=none -fno-asynchronous-unwind-tables -Wa,-mx86-used-note=no

LD = ld
LDFLAGS = -n -T boot/linker.ld -static --no-warn-rwx-segments

OBJS = boot/boot.o init/main.o kernel/console.o kernel/asm.o \
       kernel/traps.o mm/memory.o kernel/switch.o kernel/sched.o \
       kernel/gdt.o kernel/syscall.o kernel/keyboard.o kernel/fork.o \
       kernel/syscall_entry.o kernel/time.o kernel/hd.o kernel/smp.o kernel/trampoline.o \
       kernel/kthread.o kernel/pci.o kernel/xhci.o kernel/e1000.o kernel/net.o kernel/net_stack.o \
       fs/buffer.o fs/minix.o fs/ramfs.o fs/pipe.o lib/string.o

USER_BINARIES = rootfs/bin/sh rootfs/bin/hello rootfs/bin/calc rootfs/bin/test_ulibc \
                rootfs/bin/nano rootfs/bin/hdtest rootfs/bin/mintest rootfs/bin/cowtest \
                rootfs/bin/smpinfo rootfs/bin/threadtest rootfs/bin/nettest rootfs/bin/ping \
                rootfs/bin/httpd rootfs/bin/tcc rootfs/bin/cc rootfs/bin/elfhello \
                rootfs/bin/dltest rootfs/lib/libmath.so rootfs/bin/lspci rootfs/bin/usbinfo

all: Image rootfs.tar disk.img usbdisk.img BOOTX64.EFI liveusb.img

boot/boot.o: boot/boot.S
	$(CC) $(CFLAGS) -c boot/boot.S -o boot/boot.o

kernel/asm.o: kernel/asm.S
	$(CC) $(CFLAGS) -c kernel/asm.S -o kernel/asm.o

kernel/switch.o: kernel/switch.S
	$(CC) $(CFLAGS) -c kernel/switch.S -o kernel/switch.o

kernel/gdt.o: kernel/gdt.c
	$(CC) $(CFLAGS) -c kernel/gdt.c -o kernel/gdt.o

kernel/syscall.o: kernel/syscall.c
	$(CC) $(CFLAGS) -c kernel/syscall.c -o kernel/syscall.o

kernel/syscall_entry.o: kernel/syscall_entry.S
	$(CC) $(CFLAGS) -c kernel/syscall_entry.S -o kernel/syscall_entry.o

kernel/keyboard.o: kernel/keyboard.c
	$(CC) $(CFLAGS) -c kernel/keyboard.c -o kernel/keyboard.o

kernel/fork.o: kernel/fork.c
	$(CC) $(CFLAGS) -c kernel/fork.c -o kernel/fork.o

kernel/time.o: kernel/time.c
	$(CC) $(CFLAGS) -c kernel/time.c -o kernel/time.o

kernel/hd.o: kernel/hd.c
	$(CC) $(CFLAGS) -c kernel/hd.c -o kernel/hd.o

fs/buffer.o: fs/buffer.c
	$(CC) $(CFLAGS) -c fs/buffer.c -o fs/buffer.o

fs/minix.o: fs/minix.c
	$(CC) $(CFLAGS) -c fs/minix.c -o fs/minix.o

fs/ramfs.o: fs/ramfs.c
	$(CC) $(CFLAGS) -c fs/ramfs.c -o fs/ramfs.o

fs/pipe.o: fs/pipe.c
	$(CC) $(CFLAGS) -c fs/pipe.c -o fs/pipe.o

lib/string.o: lib/string.c
	$(CC) $(CFLAGS) -c lib/string.c -o lib/string.o

init/main.o: init/main.c
	$(CC) $(CFLAGS) -c init/main.c -o init/main.o

kernel/console.o: kernel/console.c
	$(CC) $(CFLAGS) -c kernel/console.c -o kernel/console.o

kernel/traps.o: kernel/traps.c
	$(CC) $(CFLAGS) -c kernel/traps.c -o kernel/traps.o

kernel/sched.o: kernel/sched.c
	$(CC) $(CFLAGS) -c kernel/sched.c -o kernel/sched.o

mm/memory.o: mm/memory.c
	$(CC) $(CFLAGS) -c mm/memory.c -o mm/memory.o

Image: $(OBJS)
	$(LD) $(LDFLAGS) -o Image $(OBJS)

user/crt0.o: user/crt0.S
	$(CC) $(USER_CFLAGS) -c user/crt0.S -o user/crt0.o

user/ulibc.o: user/ulibc.c user/ulibc.h
	$(CC) $(USER_CFLAGS) -c user/ulibc.c -o user/ulibc.o

user/sh.o: user/sh.c user/ulibc.h
	$(CC) $(USER_CFLAGS) -c user/sh.c -o user/sh.o

user/hello.o: user/hello.c user/ulibc.h
	$(CC) $(USER_CFLAGS) -c user/hello.c -o user/hello.o

user/calc.o: user/calc.c user/ulibc.h
	$(CC) $(USER_CFLAGS) -c user/calc.c -o user/calc.o

user/test_ulibc.o: user/test_ulibc.c user/ulibc.h
	$(CC) $(USER_CFLAGS) -c user/test_ulibc.c -o user/test_ulibc.o

user/nano.o: user/nano.c user/ulibc.h
	$(CC) $(USER_CFLAGS) -c user/nano.c -o user/nano.o

user/hdtest.o: user/hdtest.c user/ulibc.h
	$(CC) $(USER_CFLAGS) -c user/hdtest.c -o user/hdtest.o

user/mintest.o: user/mintest.c user/ulibc.h
	$(CC) $(USER_CFLAGS) -c user/mintest.c -o user/mintest.o

rootfs/bin/sh: user/crt0.o user/sh.o user/ulibc.o user/user.ld
	@mkdir -p rootfs/bin
	$(LD) -T user/user.ld -static user/crt0.o user/sh.o user/ulibc.o -o rootfs/bin/sh

rootfs/bin/hello: user/crt0.o user/hello.o user/ulibc.o user/user.ld
	@mkdir -p rootfs/bin
	$(LD) -T user/user.ld -static user/crt0.o user/hello.o user/ulibc.o -o rootfs/bin/hello

rootfs/bin/calc: user/crt0.o user/calc.o user/ulibc.o user/user.ld
	@mkdir -p rootfs/bin
	$(LD) -T user/user.ld -static user/crt0.o user/calc.o user/ulibc.o -o rootfs/bin/calc

rootfs/bin/test_ulibc: user/crt0.o user/test_ulibc.o user/ulibc.o user/user.ld
	@mkdir -p rootfs/bin
	$(LD) -T user/user.ld -static user/crt0.o user/test_ulibc.o user/ulibc.o -o rootfs/bin/test_ulibc

rootfs/bin/nano: user/crt0.o user/nano.o user/ulibc.o user/user.ld
	@mkdir -p rootfs/bin
	$(LD) -T user/user.ld -static user/crt0.o user/nano.o user/ulibc.o -o rootfs/bin/nano

rootfs/bin/hdtest: user/crt0.o user/hdtest.o user/ulibc.o user/user.ld
	@mkdir -p rootfs/bin
	$(LD) -T user/user.ld -static user/crt0.o user/hdtest.o user/ulibc.o -o rootfs/bin/hdtest

rootfs/bin/mintest: user/crt0.o user/mintest.o user/ulibc.o user/user.ld
	@mkdir -p rootfs/bin
	$(LD) -T user/user.ld -static user/crt0.o user/mintest.o user/ulibc.o -o rootfs/bin/mintest

user/cowtest.o: user/cowtest.c user/ulibc.h
	$(CC) $(USER_CFLAGS) -c user/cowtest.c -o user/cowtest.o

rootfs/bin/cowtest: user/crt0.o user/cowtest.o user/ulibc.o user/user.ld
	@mkdir -p rootfs/bin
	$(LD) -T user/user.ld -static user/crt0.o user/cowtest.o user/ulibc.o -o rootfs/bin/cowtest

kernel/smp.o: kernel/smp.c
	$(CC) $(CFLAGS) -c kernel/smp.c -o kernel/smp.o

kernel/trampoline.o: kernel/trampoline.S
	$(CC) $(CFLAGS) -c kernel/trampoline.S -o kernel/trampoline.o

user/smpinfo.o: user/smpinfo.c user/ulibc.h
	$(CC) $(USER_CFLAGS) -c user/smpinfo.c -o user/smpinfo.o

rootfs/bin/smpinfo: user/crt0.o user/smpinfo.o user/ulibc.o user/user.ld
	@mkdir -p rootfs/bin
	$(LD) -T user/user.ld -static user/crt0.o user/smpinfo.o user/ulibc.o -o rootfs/bin/smpinfo

kernel/kthread.o: kernel/kthread.c
	$(CC) $(CFLAGS) -c kernel/kthread.c -o kernel/kthread.o

user/threadtest.o: user/threadtest.c user/ulibc.h
	$(CC) $(USER_CFLAGS) -c user/threadtest.c -o user/threadtest.o

rootfs/bin/threadtest: user/crt0.o user/threadtest.o user/ulibc.o user/user.ld
	@mkdir -p rootfs/bin
	$(LD) -T user/user.ld -static user/crt0.o user/threadtest.o user/ulibc.o -o rootfs/bin/threadtest

kernel/pci.o: kernel/pci.c
	$(CC) $(CFLAGS) -c kernel/pci.c -o kernel/pci.o

kernel/xhci.o: kernel/xhci.c
	$(CC) $(CFLAGS) -c kernel/xhci.c -o kernel/xhci.o

kernel/e1000.o: kernel/e1000.c
	$(CC) $(CFLAGS) -c kernel/e1000.c -o kernel/e1000.o

kernel/net.o: kernel/net.c
	$(CC) $(CFLAGS) -c kernel/net.c -o kernel/net.o

user/nettest.o: user/nettest.c user/ulibc.h
	$(CC) $(USER_CFLAGS) -c user/nettest.c -o user/nettest.o

rootfs/bin/nettest: user/crt0.o user/nettest.o user/ulibc.o user/user.ld
	@mkdir -p rootfs/bin
	$(LD) -T user/user.ld -static user/crt0.o user/nettest.o user/ulibc.o -o rootfs/bin/nettest

kernel/net_stack.o: kernel/net_stack.c
	$(CC) $(CFLAGS) -c kernel/net_stack.c -o kernel/net_stack.o

user/ping.o: user/ping.c user/ulibc.h
	$(CC) $(USER_CFLAGS) -c user/ping.c -o user/ping.o

rootfs/bin/ping: user/crt0.o user/ping.o user/ulibc.o user/user.ld
	@mkdir -p rootfs/bin
	$(LD) -T user/user.ld -static user/crt0.o user/ping.o user/ulibc.o -o rootfs/bin/ping

user/httpd.o: user/httpd.c user/ulibc.h
	$(CC) $(USER_CFLAGS) -c user/httpd.c -o user/httpd.o

rootfs/bin/httpd: user/crt0.o user/httpd.o user/ulibc.o user/user.ld
	@mkdir -p rootfs/bin
	$(LD) -T user/user.ld -static user/crt0.o user/httpd.o user/ulibc.o -o rootfs/bin/httpd

user/tcc.o: user/tcc.c user/ulibc.h
	$(CC) $(USER_CFLAGS) -c user/tcc.c -o user/tcc.o

rootfs/bin/tcc: user/crt0.o user/tcc.o user/ulibc.o user/user.ld
	@mkdir -p rootfs/bin
	$(LD) -T user/user.ld -static user/crt0.o user/tcc.o user/ulibc.o -o rootfs/bin/tcc

rootfs/bin/cc: rootfs/bin/tcc
	@mkdir -p rootfs/bin
	cp rootfs/bin/tcc rootfs/bin/cc

user/elfhello.o: user/elfhello.c user/ulibc.h
	$(CC) $(USER_CFLAGS) -c user/elfhello.c -o user/elfhello.o

rootfs/bin/elfhello: user/crt0.o user/elfhello.o user/ulibc.o
	@mkdir -p rootfs/bin
	$(LD) -m elf_x86_64 -static user/crt0.o user/elfhello.o user/ulibc.o -o rootfs/bin/elfhello

rootfs/lib/libmath.so: user/libmath.c user/ulibc.h
	@mkdir -p rootfs/lib
	gcc -Wall -Wextra -O2 -m64 -ffreestanding -nostdinc -fno-stack-protector -fPIC -shared -mno-red-zone -Iuser -Iinclude user/libmath.c -o rootfs/lib/libmath.so

user/dltest.o: user/dltest.c user/ulibc.h
	$(CC) $(USER_CFLAGS) -c user/dltest.c -o user/dltest.o

rootfs/bin/dltest: user/crt0.o user/dltest.o user/ulibc.o user/user.ld
	@mkdir -p rootfs/bin
	$(LD) -T user/user.ld -static user/crt0.o user/dltest.o user/ulibc.o -o rootfs/bin/dltest

user/lspci.o: user/lspci.c user/ulibc.h
	$(CC) $(USER_CFLAGS) -c user/lspci.c -o user/lspci.o

rootfs/bin/lspci: user/crt0.o user/lspci.o user/ulibc.o user/user.ld
	@mkdir -p rootfs/bin
	$(LD) -T user/user.ld -static user/crt0.o user/lspci.o user/ulibc.o -o rootfs/bin/lspci

user/usbinfo.o: user/usbinfo.c user/ulibc.h
	$(CC) $(USER_CFLAGS) -c user/usbinfo.c -o user/usbinfo.o

rootfs/bin/usbinfo: user/crt0.o user/usbinfo.o user/ulibc.o user/user.ld
	@mkdir -p rootfs/bin
	$(LD) -T user/user.ld -static user/crt0.o user/usbinfo.o user/ulibc.o -o rootfs/bin/usbinfo

tools/mkefi: tools/mkefi.c
	gcc -O2 tools/mkefi.c -o tools/mkefi

tools/mkesp: tools/mkesp.c
	gcc -O2 tools/mkesp.c -o tools/mkesp

boot/uefi.o: boot/uefi.c include/uefi.h
	$(CC) $(UEFI_CFLAGS) -c boot/uefi.c -o boot/uefi.o

boot/uefi_jump.o: boot/uefi_jump.S
	$(CC) $(UEFI_CFLAGS) -c boot/uefi_jump.S -o boot/uefi_jump.o

BOOTX64.EFI: tools/mkefi boot/uefi.o boot/uefi_jump.o
	$(LD) -n -e efi_main -Ttext 0x1000 --oformat binary boot/uefi.o boot/uefi_jump.o -o boot/uefi.bin
	./tools/mkefi boot/uefi.bin BOOTX64.EFI

liveusb.img: BOOTX64.EFI Image rootfs.tar
	@rm -f liveusb.img
	@qemu-img create -f raw liveusb.img 64M >/dev/null 2>&1 || dd if=/dev/zero of=liveusb.img bs=1M count=64 status=none
	@mkfs.vfat -F 32 -n "LINUX_EFI" liveusb.img >/dev/null
	@mmd -i liveusb.img ::/EFI ::/EFI/BOOT
	@mcopy -i liveusb.img BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	@mcopy -i liveusb.img Image ::/Image
	@mcopy -i liveusb.img rootfs.tar ::/rootfs.tar
	@echo "[OK] liveusb.img generated with complete EFI/BOOT/BOOTX64.EFI structure"

disk.img:
	@if [ ! -f disk.img ]; then \
		qemu-img create -f raw disk.img 32M 2>/dev/null || dd if=/dev/zero of=disk.img bs=1M count=32 2>/dev/null; \
	fi

usbdisk.img:
	@if [ ! -f usbdisk.img ]; then \
		qemu-img create -f raw usbdisk.img 16M 2>/dev/null || dd if=/dev/zero of=usbdisk.img bs=1M count=16 2>/dev/null; \
	fi

rootfs.tar: $(USER_BINARIES)
	@mkdir -p rootfs/etc rootfs/home rootfs/scripts rootfs/mnt rootfs/lib
	@echo "Hello from external rootfs.tar mounted via Multiboot Initrd!" > rootfs/home/initrd_test.txt
	@echo "#!/bin/sh" > rootfs/scripts/welcome.sh
	@echo "echo === Executing /scripts/welcome.sh from TarFS ===" >> rootfs/scripts/welcome.sh
	@echo "uname -a" >> rootfs/scripts/welcome.sh
	@echo "echo Initrd TarFS is fully operational!" >> rootfs/scripts/welcome.sh
	@echo "/* Демонстрационная программа на Си для тестирования /bin/tcc */" > rootfs/home/prime.c
	@echo "int is_prime(int n) {" >> rootfs/home/prime.c
	@echo "    if (n <= 1) return 0;" >> rootfs/home/prime.c
	@echo "    int i;" >> rootfs/home/prime.c
	@echo "    for (i = 2; i * i <= n; i++) {" >> rootfs/home/prime.c
	@echo "        if (n % i == 0) return 0;" >> rootfs/home/prime.c
	@echo "    }" >> rootfs/home/prime.c
	@echo "    return 1;" >> rootfs/home/prime.c
	@echo "}" >> rootfs/home/prime.c
	@echo "" >> rootfs/home/prime.c
	@echo "int main() {" >> rootfs/home/prime.c
	@echo "    print(\"=== Calculating Prime Numbers in Ring 3 via Self-Hosting TCC ===\\n\");" >> rootfs/home/prime.c
	@echo "    int count = 0;" >> rootfs/home/prime.c
	@echo "    int i;" >> rootfs/home/prime.c
	@echo "    for (i = 2; i <= 50; i++) {" >> rootfs/home/prime.c
	@echo "        if (is_prime(i)) {" >> rootfs/home/prime.c
	@echo "            print(\"Prime: \");" >> rootfs/home/prime.c
	@echo "            print_num(i);" >> rootfs/home/prime.c
	@echo "            print(\"\\n\");" >> rootfs/home/prime.c
	@echo "            count++;" >> rootfs/home/prime.c
	@echo "        }" >> rootfs/home/prime.c
	@echo "    }" >> rootfs/home/prime.c
	@echo "    print(\"Total primes found: \");" >> rootfs/home/prime.c
	@echo "    print_num(count);" >> rootfs/home/prime.c
	@echo "    print(\"\\n\");" >> rootfs/home/prime.c
	@echo "    return 0;" >> rootfs/home/prime.c
	@echo "}" >> rootfs/home/prime.c
	tar --format=ustar -cf rootfs.tar -C rootfs .

run: Image rootfs.tar disk.img usbdisk.img
	qemu-system-x86_64 -smp 2 -m 128M -kernel Image -initrd rootfs.tar \
	-drive file=disk.img,format=raw,index=0,media=disk \
	-device qemu-xhci,id=xhci -drive file=usbdisk.img,format=raw,if=none,id=usb0 -device usb-storage,bus=xhci.0,drive=usb0 \
	-netdev user,id=net0,hostfwd=tcp::8080-:80 \
	-device e1000,netdev=net0,mac=52:54:00:12:34:56 -serial mon:stdio

run-tap: Image rootfs.tar disk.img usbdisk.img
	qemu-system-x86_64 -smp 2 -m 128M -kernel Image -initrd rootfs.tar \
	-drive file=disk.img,format=raw,index=0,media=disk \
	-device qemu-xhci,id=xhci -drive file=usbdisk.img,format=raw,if=none,id=usb0 -device usb-storage,bus=xhci.0,drive=usb0 \
	-netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
	-device e1000,netdev=net0,mac=52:54:00:12:34:56 -serial mon:stdio

run-uefi: Image rootfs.tar disk.img usbdisk.img liveusb.img
	qemu-system-x86_64 -bios /usr/share/edk2/x64/OVMF.4m.fd -smp 2 -m 128M \
	-drive file=disk.img,format=raw,index=0,media=disk \
	-device qemu-xhci,id=xhci -drive file=liveusb.img,format=raw,if=none,id=uefiboot -device usb-storage,bus=xhci.0,drive=uefiboot \
	-netdev user,id=net0,hostfwd=tcp::8080-:80 \
	-device e1000,netdev=net0,mac=52:54:00:12:34:56 -serial mon:stdio

clean:
	rm -rf $(OBJS) Image rootfs.tar rootfs user/*.o disk.img usbdisk.img BOOTX64.EFI liveusb.img tools/mkefi tools/mkesp boot/uefi*.o boot/uefi.bin
