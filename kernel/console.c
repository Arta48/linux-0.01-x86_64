#include <linux/tty.h>
#include <asm/io.h>
#include <stdarg.h>

#define VGA_BUFFER 0xB8000
#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define COM1       0x3F8

static unsigned short *vga = (unsigned short *)VGA_BUFFER;
static int cursor_x = 0;
static int cursor_y = 0;

/* Инициализация COM-порта для вывода в терминал */
static void serial_init(void)
{
    outb(0x00, COM1 + 1);    /* Отключаем прерывания */
    outb(0x80, COM1 + 3);    /* Включаем DLAB (установка битрейта) */
    outb(0x03, COM1 + 0);    /* Делитель 3 (38400 бод) */
    outb(0x00, COM1 + 1);
    outb(0x03, COM1 + 3);    /* 8 бит, без четности, 1 стоп-бит */
    outb(0xC7, COM1 + 2);    /* Включаем FIFO */
    outb(0x0B, COM1 + 4);    /* Включаем IRQ */
}

static void serial_putc(char c)
{
    while ((inb(COM1 + 5) & 0x20) == 0);
    outb(c, COM1);
}

void console_init(void)
{
    serial_init();
    /* Очистка экрана */
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga[i] = (0x07 << 8) | ' ';
    }
    cursor_x = 0;
    cursor_y = 0;
}

static void console_putc(char c)
{
    serial_putc(c); /* Дублируем в серийный порт */

    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\r') {
        cursor_x = 0;
    } else {
        vga[cursor_y * VGA_WIDTH + cursor_x] = (0x0A << 8) | c; /* Светло-зеленый текст */
        cursor_x++;
        if (cursor_x >= VGA_WIDTH) {
            cursor_x = 0;
            cursor_y++;
        }
    }

    if (cursor_y >= VGA_HEIGHT) {
        /* Простая прокрутка экрана */
        for (int i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++) {
            vga[i] = vga[i + VGA_WIDTH];
        }
        for (int i = (VGA_HEIGHT - 1) * VGA_WIDTH; i < VGA_HEIGHT * VGA_WIDTH; i++) {
            vga[i] = (0x07 << 8) | ' ';
        }
        cursor_y = VGA_HEIGHT - 1;
    }
}

static void print_num(unsigned long n, int base)
{
    char buf[65];
    static const char digits[] = "0123456789ABCDEF";
    int i = 0;

    if (n == 0) {
        console_putc('0');
        return;
    }
    while (n > 0) {
        buf[i++] = digits[n % base];
        n /= base;
    }
    while (--i >= 0) {
        console_putc(buf[i]);
    }
}

void printk(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            console_putc(*p);
            continue;
        }
        p++;
        switch (*p) {
            case 's': {
                const char *s = va_arg(args, const char *);
                while (*s) console_putc(*s++);
                break;
            }
            case 'd': {
                long d = va_arg(args, long);
                if (d < 0) {
                    console_putc('-');
                    d = -d;
                }
                print_num(d, 10);
                break;
            }
            case 'x':
            case 'p': {
                console_putc('0');
                console_putc('x');
                print_num(va_arg(args, unsigned long), 16);
                break;
            }
            case '%':
                console_putc('%');
                break;
            default:
                console_putc(*p);
                break;
        }
    }
    va_end(args);
}
