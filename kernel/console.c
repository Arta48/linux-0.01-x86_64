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
static unsigned char current_attr = 0x07; /* Серый на черном */

/* Состояния парсера ANSI: 0 - текст, 1 - ESC, 2 - CSI '[' */
static int ansi_state = 0;
#define MAX_ANSI_PAR 4
static int ansi_par[MAX_ANSI_PAR];
static int ansi_par_idx = 0;

static void serial_init(void)
{
    outb(0x00, COM1 + 1);
    outb(0x80, COM1 + 3);
    outb(0x03, COM1 + 0);
    outb(0x00, COM1 + 1);
    outb(0x03, COM1 + 3);
    outb(0xC7, COM1 + 2);
    outb(0x0B, COM1 + 4);
}

static void serial_putc(char c)
{
    while ((inb(COM1 + 5) & 0x20) == 0);
    outb(c, COM1);
}

void console_init(void)
{
    serial_init();
    current_attr = 0x07;
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga[i] = (current_attr << 8) | ' ';
    }
    cursor_x = 0;
    cursor_y = 0;
    ansi_state = 0;
}

static void clamp_cursor(void)
{
    if (cursor_x < 0) cursor_x = 0;
    if (cursor_x >= VGA_WIDTH) cursor_x = VGA_WIDTH - 1;
    if (cursor_y < 0) cursor_y = 0;
    if (cursor_y >= VGA_HEIGHT) cursor_y = VGA_HEIGHT - 1;
}

static void scroll(void)
{
    for (int i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++) {
        vga[i] = vga[i + VGA_WIDTH];
    }
    for (int i = (VGA_HEIGHT - 1) * VGA_WIDTH; i < VGA_HEIGHT * VGA_WIDTH; i++) {
        vga[i] = (current_attr << 8) | ' ';
    }
    cursor_y = VGA_HEIGHT - 1;
}

static void vga_clear_screen(void)
{
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga[i] = (current_attr << 8) | ' ';
    }
    cursor_x = 0;
    cursor_y = 0;
}

static void vga_clear_line_end(void)
{
    for (int x = cursor_x; x < VGA_WIDTH; x++) {
        vga[cursor_y * VGA_WIDTH + x] = (current_attr << 8) | ' ';
    }
}

void console_putc(char c)
{
    /* Правильное затирание символа при Backspace */
    if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            vga[cursor_y * VGA_WIDTH + cursor_x] = (current_attr << 8) | ' ';
        }
        /* В терминале: возврат влево, затирание пробелом, повторный возврат влево */
        serial_putc('\b');
        serial_putc(' ');
        serial_putc('\b');
        return;
    }

    /* Передаем символ в COM1 для терминала */
    serial_putc(c);

    /* Конечный автомат парсера ANSI для VGA */
    if (ansi_state == 0) {
        if (c == '\033') {
            ansi_state = 1;
            return;
        }

        if (c == '\f' || c == 12) {
            vga_clear_screen();
            return;
        }

        if (c == '\n') {
            cursor_x = 0;
            cursor_y++;
        } else if (c == '\r') {
            cursor_x = 0;
        } else if (c == '\t') {
            cursor_x = (cursor_x + 8) & ~7;
            if (cursor_x >= VGA_WIDTH) {
                cursor_x = 0;
                cursor_y++;
            }
        } else if ((unsigned char)c >= 32) {
            vga[cursor_y * VGA_WIDTH + cursor_x] = (current_attr << 8) | c;
            cursor_x++;
            if (cursor_x >= VGA_WIDTH) {
                cursor_x = 0;
                cursor_y++;
            }
        }

        if (cursor_y >= VGA_HEIGHT) {
            scroll();
        }
        return;
    }

    if (ansi_state == 1) {
        if (c == '[') {
            ansi_state = 2;
            ansi_par_idx = 0;
            for (int i = 0; i < MAX_ANSI_PAR; i++) ansi_par[i] = 0;
            return;
        }
        ansi_state = 0;
        return;
    }

    if (ansi_state == 2) {
        if (c >= '0' && c <= '9') {
            ansi_par[ansi_par_idx] = ansi_par[ansi_par_idx] * 10 + (c - '0');
            return;
        }
        if (c == ';') {
            if (ansi_par_idx < MAX_ANSI_PAR - 1) {
                ansi_par_idx++;
            }
            return;
        }

        ansi_state = 0;
        switch (c) {
            case 'H':
            case 'f': {
                int r = ansi_par[0] ? ansi_par[0] - 1 : 0;
                int col = ansi_par[1] ? ansi_par[1] - 1 : 0;
                cursor_y = r;
                cursor_x = col;
                clamp_cursor();
                break;
            }
            case 'J': {
                if (ansi_par[0] == 2 || ansi_par[0] == 0) {
                    vga_clear_screen();
                }
                break;
            }
            case 'K': {
                vga_clear_line_end();
                break;
            }
            case 'A': {
                int count = ansi_par[0] ? ansi_par[0] : 1;
                cursor_y -= count;
                clamp_cursor();
                break;
            }
            case 'B': {
                int count = ansi_par[0] ? ansi_par[0] : 1;
                cursor_y += count;
                clamp_cursor();
                break;
            }
            case 'C': {
                int count = ansi_par[0] ? ansi_par[0] : 1;
                cursor_x += count;
                clamp_cursor();
                break;
            }
            case 'D': {
                int count = ansi_par[0] ? ansi_par[0] : 1;
                cursor_x -= count;
                clamp_cursor();
                break;
            }
            case 'm': {
                for (int i = 0; i <= ansi_par_idx; i++) {
                    int p = ansi_par[i];
                    if (p == 0) current_attr = 0x07;
                    else if (p == 1) current_attr = 0x0F;
                    else if (p == 7) current_attr = 0x70;
                }
                break;
            }
            default:
                break;
        }
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
            case 'c': {
                char c = (char)va_arg(args, int);
                console_putc(c);
                break;
            }
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
