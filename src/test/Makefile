CC = i686-elf-gcc
AS = nasm
LD = i686-elf-ld

CFLAGS = -march=i386 -ffreestanding -nostdlib -O2 -Wall -Wextra
ASFLAGS = -f elf32

all: kernel.bin

boot.o: boot.asm
	$(AS) $(ASFLAGS) boot.asm -o boot.o

kernel.o: kernel.c
	$(CC) $(CFLAGS) -c kernel.c -o kernel.o

kernel.bin: boot.o kernel.o linker.ld
	$(LD) -T linker.ld -o kernel.bin boot.o kernel.o
	@echo "Build OK — run with: qemu-system-i386 -kernel kernel.bin"

run: kernel.bin
	qemu-system-i386 -kernel kernel.bin

clean:
	rm -f *.o kernel.bin

.PHONY: all run clean
