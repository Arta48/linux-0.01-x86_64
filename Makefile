CC = gcc
CFLAGS = -Wall -Wextra -O2 -m64 -mcmodel=kernel -ffreestanding \
         -mgeneral-regs-only -fno-stack-protector -fno-pie -no-pie \
         -mno-red-zone -nostdinc -Iinclude

LD = ld
LDFLAGS = -n -T boot/linker.ld -static --no-warn-rwx-segments

OBJS = boot/boot.o init/main.o kernel/console.o kernel/asm.o \
       kernel/traps.o mm/memory.o kernel/switch.o kernel/sched.o \
       kernel/gdt.o kernel/syscall.o kernel/keyboard.o kernel/fork.o \
       kernel/syscall_entry.o kernel/time.o fs/ramfs.o fs/pipe.o lib/string.o

all: Image rootfs.tar

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

rootfs.tar:
	@mkdir -p rootfs/etc rootfs/bin rootfs/home rootfs/scripts
	@echo "Hello from external rootfs.tar mounted via Multiboot Initrd!" > rootfs/home/initrd_test.txt
	@echo "#!/bin/sh" > rootfs/scripts/welcome.sh
	@echo "echo === Executing /scripts/welcome.sh from TarFS ===" >> rootfs/scripts/welcome.sh
	@echo "uname -a" >> rootfs/scripts/welcome.sh
	@echo "echo Initrd TarFS is fully operational!" >> rootfs/scripts/welcome.sh
	@echo "#!/bin/sh" > rootfs/scripts/test_control.sh
	@echo "echo === 1. Testing Conditionals (if / then / else / fi) ===" >> rootfs/scripts/test_control.sh
	@echo "VAL=10" >> rootfs/scripts/test_control.sh
	@echo "if [ \$$VAL -gt 5 ]" >> rootfs/scripts/test_control.sh
	@echo "then" >> rootfs/scripts/test_control.sh
	@echo "    echo [PASS] VAL is greater than 5" >> rootfs/scripts/test_control.sh
	@echo "else" >> rootfs/scripts/test_control.sh
	@echo "    echo [FAIL] VAL is not greater than 5" >> rootfs/scripts/test_control.sh
	@echo "fi" >> rootfs/scripts/test_control.sh
	@echo "if [ -f /etc/passwd ]" >> rootfs/scripts/test_control.sh
	@echo "then" >> rootfs/scripts/test_control.sh
	@echo "    echo [PASS] /etc/passwd exists" >> rootfs/scripts/test_control.sh
	@echo "fi" >> rootfs/scripts/test_control.sh
	@echo "if [ -d /nonexistent ]" >> rootfs/scripts/test_control.sh
	@echo "then" >> rootfs/scripts/test_control.sh
	@echo "    echo [FAIL] Directory should not exist" >> rootfs/scripts/test_control.sh
	@echo "else" >> rootfs/scripts/test_control.sh
	@echo "    echo [PASS] /nonexistent correctly detected as absent" >> rootfs/scripts/test_control.sh
	@echo "fi" >> rootfs/scripts/test_control.sh
	@echo "echo === 2. Testing For Loop ===" >> rootfs/scripts/test_control.sh
	@echo "for item in alpha beta gamma" >> rootfs/scripts/test_control.sh
	@echo "do" >> rootfs/scripts/test_control.sh
	@echo "    echo Item: \$$item" >> rootfs/scripts/test_control.sh
	@echo "done" >> rootfs/scripts/test_control.sh
	@echo "echo === 3. Testing While Loop and let ===" >> rootfs/scripts/test_control.sh
	@echo "NUM=1" >> rootfs/scripts/test_control.sh
	@echo "while [ \$$NUM -le 3 ]" >> rootfs/scripts/test_control.sh
	@echo "do" >> rootfs/scripts/test_control.sh
	@echo "    echo Loop step: \$$NUM" >> rootfs/scripts/test_control.sh
	@echo "    let NUM = \$$NUM + 1" >> rootfs/scripts/test_control.sh
	@echo "done" >> rootfs/scripts/test_control.sh
	@echo "echo === All Stage 30 control tests passed! ===" >> rootfs/scripts/test_control.sh
	tar --format=ustar -cf rootfs.tar -C rootfs .

run: Image rootfs.tar
	qemu-system-x86_64 -m 128M -kernel Image -initrd rootfs.tar -serial mon:stdio

clean:
	rm -rf $(OBJS) Image rootfs.tar rootfs
