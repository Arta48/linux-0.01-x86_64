CC = gcc
CFLAGS = -Wall -Wextra -O2 -m64 -mcmodel=kernel -ffreestanding \
         -mgeneral-regs-only -fno-stack-protector -fno-pie -no-pie \
         -mno-red-zone -nostdinc -Iinclude

LD = ld
LDFLAGS = -n -T boot/linker.ld -static --no-warn-rwx-segments

OBJS = boot/boot.o init/main.o kernel/console.o kernel/asm.o \
       kernel/traps.o mm/memory.o kernel/switch.o kernel/sched.o \
       kernel/gdt.o kernel/syscall.o

all: Image

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

run: Image
	qemu-system-x86_64 -m 128M -kernel Image -serial stdio

clean:
	rm -f $(OBJS) Image
