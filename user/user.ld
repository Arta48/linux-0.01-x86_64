OUTPUT_FORMAT("binary")
ENTRY(_start)

SECTIONS
{
    . = 0x60000000;

    .header : {
        QUAD(0x4C494E5553303031); /* EXEC_MAGIC ("LINUS001") */
        QUAD(_start);              /* Точка входа в программу */
        QUAD(_end - 0x60000000);   /* Полный размер бинарника */
    }

    .text : ALIGN(16) {
        *(.text.entry)
        *(.text*)
    }

    .rodata : ALIGN(16) {
        *(.rodata*)
    }

    .data : ALIGN(16) {
        *(.data*)
    }

    .bss : ALIGN(16) {
        *(.bss*)
        *(COMMON)
    }

    . = ALIGN(16);
    _end = .;
}
