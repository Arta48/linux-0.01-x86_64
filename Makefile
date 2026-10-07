CC = gcc
CFLAGS = -Wall -Wextra -O2 -m64 -mcmodel=kernel -ffreestanding \
         -fno-stack-protector -fno-pie -no-pie -mno-red-zone \
         -nostdinc -Iinclude

LD = ld
LDFLAGS = -n -T boot/linker.ld -static --no-warn-rwx-segments

OBJS = boot/boot.o init/main.o kernel/console.o

all: Image

boot/boot.o: boot/boot.S
	$(CC) $(CFLAGS) -c boot/boot.S -o boot/boot.o

init/main.o: init/main.c
	$(CC) $(CFLAGS) -c init/main.c -o init/main.o

kernel/console.o: kernel/console.c
	$(CC) $(CFLAGS) -c kernel/console.c -o kernel/console.o

Image: $(OBJS)
	$(LD) $(LDFLAGS) -o Image $(OBJS)

run: Image
	qemu-system-x86_64 -kernel Image -serial stdio

clean:
	rm -f $(OBJS) Image
