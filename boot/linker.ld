ENTRY(_start)

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386:x86-64)

SECTIONS
{
    . = 1M;

    .boot :
    {
        KEEP(*(.multiboot))
    }

    .text : ALIGN(4K)
    {
        *(.text*)
    }

    .rodata : ALIGN(4K)
    {
        *(.rodata*)
    }

    .data : ALIGN(4K)
    {
        *(.data*)
    }

    .bss : ALIGN(4K)
    {
        *(COMMON)
        *(.bss*)
    }

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.comment)
    }
}
