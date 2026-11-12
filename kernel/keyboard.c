#include <linux/keyboard.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <asm/io.h>

#define KBD_DATA_PORT 0x60
#define COM1_PORT     0x3F8
#define BUFFER_SIZE   256

static char kbd_buffer[BUFFER_SIZE];
static volatile uint32_t kbd_head = 0;
static volatile uint32_t kbd_tail = 0;
static int shift_pressed = 0;
static int ctrl_pressed = 0;
static int ext_scancode = 0;

static const char kbd_map[128] = {
    0,   27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0,
    '*',
    0,
    ' ',
    0,
};

static const char kbd_shift_map[128] = {
    0,   27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '\"', '~',
    0,
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0,
    '*',
    0,
    ' ',
    0,
};

void keyboard_init(void)
{
    kbd_head = 0;
    kbd_tail = 0;
    shift_pressed = 0;
    ctrl_pressed = 0;
    ext_scancode = 0;
}

static void kbd_push_char(char c)
{
    uint32_t next = (kbd_head + 1) % BUFFER_SIZE;
    if (next != kbd_tail) {
        kbd_buffer[kbd_head] = c;
        kbd_head = next;
    }
}

extern void tty_sigint(void);
void check_serial_events(void);

int keyboard_pending(void)
{
    check_serial_events();
    return kbd_head != kbd_tail;
}

void keyboard_inject_char(char c)
{
    kbd_push_char(c);
}

void keyboard_inject_sigint(void)
{
    if (current && current->pid > 0) {
        tty_sigint();
    }
}

/* Читает байт контроллера i8042, только если он действительно готов (иначе дубли при опросе + IRQ1).
 * 0xFF в статусе -- контроллера нет. Байты мыши (бит 5) пропускаются. */
static int i8042_read(uint8_t *out)
{
    uint8_t st = inb(0x64);
    if (st == 0xFF || !(st & 1)) return 0;
    uint8_t b = inb(KBD_DATA_PORT);
    if (st & 0x20) return -1;
    *out = b;
    return 1;
}

static void keyboard_scancode(uint8_t scancode);

void keyboard_handler(void)
{
    uint8_t sc;
    for (int n = 0; n < 16; n++) {
        int r = i8042_read(&sc);
        if (r == 0) break;
        if (r > 0) keyboard_scancode(sc);
    }
}

/* Опрос из тика: клавиатура работает и там, где IRQ1 не доходит до CPU */
void i8042_poll(void)
{
    keyboard_handler();
}

static void keyboard_scancode(uint8_t scancode)
{

    if (scancode == 0xE0) {
        ext_scancode = 1;
        return;
    }

    if (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; return; }
    if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0; return; }

    if (scancode == 0x1D) { ctrl_pressed = 1; return; }
    if (scancode == 0x9D) { ctrl_pressed = 0; return; }

    /* Обработка расширенных клавиш (стрелки, Home, End) в виде ANSI-последовательностей */
    if (ext_scancode) {
        ext_scancode = 0;
        if (scancode & 0x80) return; /* Отпускание */

            if (scancode == 0x48) { /* Up Arrow */
                kbd_push_char('\033'); kbd_push_char('['); kbd_push_char('A');
                return;
            }
            if (scancode == 0x50) { /* Down Arrow */
                kbd_push_char('\033'); kbd_push_char('['); kbd_push_char('B');
                return;
            }
            if (scancode == 0x4D) { /* Right Arrow */
                kbd_push_char('\033'); kbd_push_char('['); kbd_push_char('C');
                return;
            }
            if (scancode == 0x4B) { /* Left Arrow */
                kbd_push_char('\033'); kbd_push_char('['); kbd_push_char('D');
                return;
            }
            if (scancode == 0x53) { /* Delete */
                kbd_push_char('\033'); kbd_push_char('['); kbd_push_char('3'); kbd_push_char('~');
                return;
            }
            if (scancode == 0x52) { /* Insert */
                kbd_push_char('\033'); kbd_push_char('['); kbd_push_char('2'); kbd_push_char('~');
                return;
            }
            if (scancode == 0x49) { /* Page Up */
                kbd_push_char('\033'); kbd_push_char('['); kbd_push_char('5'); kbd_push_char('~');
                return;
            }
            if (scancode == 0x51) { /* Page Down */
                kbd_push_char('\033'); kbd_push_char('['); kbd_push_char('6'); kbd_push_char('~');
                return;
            }
            if (scancode == 0x47) { /* Home */
                kbd_push_char('\033'); kbd_push_char('['); kbd_push_char('H');
                return;
            }
            if (scancode == 0x4F) { /* End */
                kbd_push_char('\033'); kbd_push_char('['); kbd_push_char('F');
                return;
            }
            return;
    }

    /* Сигнал SIGINT при Ctrl+C на PS/2 */
    if (ctrl_pressed && scancode == 0x2E) {
        if (current && current->pid > 0) {
            tty_sigint();
        }
        return;
    }

    if (scancode & 0x80) return;

    /* Горячие клавиши Ctrl+X, Ctrl+O, Ctrl+K, Ctrl+U для nano */
    if (ctrl_pressed) {
        char base = kbd_map[scancode];
        if (base >= 'a' && base <= 'z') {
            kbd_push_char((char)(base - 'a' + 1));
            return;
        }
    }

    char c = shift_pressed ? kbd_shift_map[scancode] : kbd_map[scancode];
    if (c != 0) {
        kbd_push_char(c);
    }
}

void check_serial_events(void)
{
    /* Нет UART (ноутбуки): шина отдаёт 0xFF, "данные готовы" был бы всегда -- вечный цикл */
    static int no_uart;
    if (no_uart) return;
    uint8_t lsr = inb(COM1_PORT + 5);
    if (lsr == 0xFF) { no_uart = 1; return; }
    for (int n = 0; n < 64 && (lsr & 0x01); n++, lsr = inb(COM1_PORT + 5)) {
        char c = (char)inb(COM1_PORT);

        if (c == 3) { /* Ctrl+C */
            if (current && current->pid > 1) {
                tty_sigint();
                continue;
            }
        }

        if (c == '\r') c = '\n';
        if (c == 127)  c = '\b';

        kbd_push_char(c);
    }
}

char keyboard_getchar(void)
{
    check_serial_events();

    if (kbd_head != kbd_tail) {
        char c = kbd_buffer[kbd_tail];
        kbd_tail = (kbd_tail + 1) % BUFFER_SIZE;
        return c;
    }
    return 0;
}
