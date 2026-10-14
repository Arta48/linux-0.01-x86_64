#include <linux/tty.h>
#include <linux/multiboot.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <asm/io.h>
#include <stdarg.h>

extern struct mb_info mbi;

static uint32_t *fb = NULL;
static uint32_t fb_width = 0;
static uint32_t fb_height = 0;
static uint32_t fb_pitch = 0;

static int cursor_x = 0;
static int cursor_y = 0;
static uint32_t current_fg = 0xFFFFFF; /* Белый, как в обычной tty */
static uint32_t current_bg = 0x000000; /* Черный фон */

/* Минималистичный системный шрифт 8x8 (ASCII 32-127) */
static const uint8_t font8x8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},
    {0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00}, {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00},
    {0x18,0x7E,0x60,0x3C,0x06,0x7E,0x18,0x00}, {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00},
    {0x38,0x6C,0x68,0x76,0xDC,0xCC,0x76,0x00}, {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00},
    {0x3C,0x66,0x6E,0x7E,0x76,0x66,0x3C,0x00}, {0x18,0x38,0x58,0x18,0x18,0x18,0x7E,0x00},
    {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00}, {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00},
    {0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x00}, {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00},
    {0x3C,0x66,0x60,0x7C,0x66,0x66,0x3C,0x00}, {0x7E,0x66,0x06,0x0C,0x18,0x18,0x18,0x00},
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, {0x3C,0x66,0x66,0x3E,0x06,0x66,0x3C,0x00},
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30},
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00}, {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00},
    {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00}, {0x3C,0x66,0x0C,0x18,0x18,0x00,0x18,0x00},
    {0x3C,0x66,0x6E,0x6E,0x60,0x66,0x3C,0x00}, {0x3C,0x66,0x66,0x7E,0x66,0x66,0x66,0x00},
    {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00},
    {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, {0x7E,0x60,0x60,0x78,0x60,0x60,0x7E,0x00},
    {0x7E,0x60,0x60,0x78,0x60,0x60,0x60,0x00}, {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3E,0x00},
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00}, {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00},
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00},
    {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00}, {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, {0x3C,0x66,0x66,0x66,0x6A,0x6C,0x36,0x00},
    {0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00}, {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00},
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
    {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, {0x63,0x63,0x6B,0x7F,0x77,0x63,0x63,0x00},
    {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00},
    {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}, {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
    {0x80,0xC0,0x60,0x30,0x18,0x0C,0x06,0x00}, {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
    {0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00},
    {0x30,0x18,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00},
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00}, {0x00,0x00,0x3C,0x60,0x60,0x66,0x3C,0x00},
    {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00}, {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00},
    {0x1C,0x30,0x7C,0x30,0x30,0x30,0x30,0x00}, {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x3C},
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00}, {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},
    {0x0C,0x00,0x1C,0x0C,0x0C,0x0C,0x0C,0x38}, {0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00},
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, {0x00,0x00,0x66,0x7F,0x7F,0x6B,0x63,0x00},
    {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00}, {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00},
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60}, {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06},
    {0x00,0x00,0x7C,0x66,0x60,0x60,0x60,0x00}, {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00},
    {0x30,0x30,0x7C,0x30,0x30,0x34,0x18,0x00}, {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00},
    {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00}, {0x00,0x00,0x63,0x6B,0x7F,0x77,0x63,0x00},
    {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00}, {0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C},
    {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00}, {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00},
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00},
    {0x3B,0x6E,0x00,0x00,0x00,0x00,0x00,0x00}
};

static void draw_char(char c, int cx, int cy, uint32_t fg, uint32_t bg)
{
    if (!fb) return;
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = font8x8[c - 32];

    for (int y = 0; y < 8; y++) {
        uint32_t *pixel_row = fb + ((cy * 8 + y) * (fb_pitch / 4)) + (cx * 8);
        uint8_t row_data = glyph[y];
        for (int x = 0; x < 8; x++) {
            pixel_row[x] = (row_data & (0x80 >> x)) ? fg : bg;
        }
    }
}

/* Текстовый буфер в ОЗУ. Видеопамять UEFI GOP часто write-combining/uncached:
 * чтение из неё (memmove при прокрутке) занимает секунды на строку и выглядит
 * как медленная "перезапись по кругу". Поэтому прокрутка делается в ОЗУ, а в
 * видеопамять только пишем, и только изменившиеся символы.
 * Ячейка: младший байт -- символ, старший -- атрибуты (бит 0 = инверсия). */
#define CON_MAX_COLS 320
#define CON_MAX_ROWS 160
#define ATTR_REV   0x100
#define SHOWN_NONE 0xFFFF
static uint16_t cells[CON_MAX_ROWS][CON_MAX_COLS]; /* что должно быть на экране */
static uint16_t shown[CON_MAX_ROWS][CON_MAX_COLS]; /* что реально нарисовано     */
static int con_cols = 0;
static int con_rows = 0;
static uint16_t cur_attr = 0;       /* текущие атрибуты вывода (ESC[..m) */
static int cursor_drawn = 0;        /* курсор сейчас нарисован на экране */

static void draw_cell(uint16_t v, int cx, int cy)
{
    uint32_t fg = current_fg, bg = current_bg;
    if (v & ATTR_REV) { fg = current_bg; bg = current_fg; }
    draw_char((char)(v & 0xFF), cx, cy, fg, bg);
}

static void put_cell(char c, int cx, int cy)
{
    if (cx < 0 || cy < 0 || cx >= con_cols || cy >= con_rows) return;
    uint16_t v = (uint16_t)((unsigned char)c | cur_attr);
    cells[cy][cx] = v;
    if (shown[cy][cx] != v) {
        shown[cy][cx] = v;
        draw_cell(v, cx, cy);
    }
}

/* Заполнить ячейки пробелами (обычный фон): стирание строки/экрана */
static void erase_cells(int y, int x0, int x1)
{
    if (y < 0 || y >= con_rows) return;
    if (x0 < 0) x0 = 0;
    if (x1 > con_cols) x1 = con_cols;
    for (int x = x0; x < x1; x++) {
        cells[y][x] = ' ';
        if (shown[y][x] != ' ') {
            shown[y][x] = ' ';
            draw_cell(' ', x, y);
        }
    }
}

/* Курсор: инвертированная ячейка. Убираем перед любым выводом, рисуем после. */
static void cursor_hide(void)
{
    if (!cursor_drawn) return;
    cursor_drawn = 0;
    if (cursor_x < con_cols && cursor_y < con_rows) {
        uint16_t v = cells[cursor_y][cursor_x];
        shown[cursor_y][cursor_x] = v;
        draw_cell(v, cursor_x, cursor_y);
    }
}

static void cursor_show(void)
{
    if (cursor_x >= con_cols || cursor_y >= con_rows) return;
    uint16_t v = cells[cursor_y][cursor_x] ^ ATTR_REV;
    shown[cursor_y][cursor_x] = SHOWN_NONE;  /* при любом обновлении перерисуется */
    draw_cell(v, cursor_x, cursor_y);
    cursor_drawn = 1;
}

static void fb_scroll(void)
{
    if (!fb) return;
    for (int r = 0; r < con_rows - 1; r++)
        for (int x = 0; x < con_cols; x++)
            cells[r][x] = cells[r + 1][x];
    for (int x = 0; x < con_cols; x++)
        cells[con_rows - 1][x] = ' ';

    for (int r = 0; r < con_rows; r++)
        for (int x = 0; x < con_cols; x++)
            if (cells[r][x] != shown[r][x]) {
                shown[r][x] = cells[r][x];
                draw_cell(cells[r][x], x, r);
            }
    cursor_y--;
}

static void fb_clear(void)
{
    if (!fb) return;
    uint32_t total_pixels = (fb_pitch / 4) * fb_height;
    for (uint32_t i = 0; i < total_pixels; i++) {
        fb[i] = current_bg;
    }
    for (int r = 0; r < CON_MAX_ROWS; r++)
        for (int x = 0; x < CON_MAX_COLS; x++)
            cells[r][x] = shown[r][x] = ' ';
    cursor_x = 0;
    cursor_y = 0;
    cursor_drawn = 0;
}

void console_init(void)
{
    /* Инициализация фреймбуфера из параметров UEFI */
    if ((mbi.flags & MB_FLAG_FB) && mbi.framebuffer_addr != 0 &&
        mbi.framebuffer_width != 0 && mbi.framebuffer_height != 0 &&
        mbi.framebuffer_bpp == 32) {
        fb_width = mbi.framebuffer_width;
        fb_height = mbi.framebuffer_height;
        fb_pitch = mbi.framebuffer_pitch;
        con_cols = fb_width / 8;
        con_rows = fb_height / 8;
        if (con_cols > CON_MAX_COLS) con_cols = CON_MAX_COLS;
        if (con_rows > CON_MAX_ROWS) con_rows = CON_MAX_ROWS;

        /* Проецируем физическую видеопамять в ядро */
        uint64_t fb_phys = mbi.framebuffer_addr;
        uint64_t fb_size = fb_height * fb_pitch;
        uint64_t pages = (fb_size + PAGE_SIZE - 1) / PAGE_SIZE;

        for (uint64_t p = 0; p < pages; p++) {
            map_page(NULL, fb_phys + (p * PAGE_SIZE), fb_phys + (p * PAGE_SIZE), PTE_WRITABLE);
        }
        fb = (uint32_t *)fb_phys;
        fb_clear();
    }
}

/* COM1 с ограниченным ожиданием: на реальном ПК UART может отсутствовать
 * (порт читается как 0x00/0xFF) -- бесконечный цикл вешал систему. */
static void serial_out(char c)
{
    for (int t = 0; t < 20000; t++) {
        uint8_t st = inb(0x3F8 + 5);
        if (st == 0xFF) return;          /* нет UART */
        if (st & 0x20) { outb(c, 0x3F8); return; }
    }
}

/* ---- Разбор ANSI/VT100 (CSI) для экрана: nano, clear и т.п. ---- */
static int esc_state = 0;           /* 0 текст, 1 после ESC, 2 внутри CSI */
static int esc_par[6];
static int esc_have[6];
static int esc_np = 0;
static int esc_priv = 0;
static int saved_x = 0, saved_y = 0;

static int esc_p(int i, int def)
{
    return (i <= esc_np && esc_have[i] && esc_par[i] > 0) ? esc_par[i] : def;
}

static void clampcur(void)
{
    if (cursor_x < 0) cursor_x = 0;
    if (cursor_x >= con_cols) cursor_x = con_cols - 1;
    if (cursor_y < 0) cursor_y = 0;
    if (cursor_y >= con_rows) cursor_y = con_rows - 1;
}

static void csi_exec(char f)
{
    if (esc_priv) return;           /* ESC[?25l и т.п. -- игнорируем */
    switch (f) {
    case 'H': case 'f':
        cursor_y = esc_p(0, 1) - 1;
        cursor_x = esc_p(1, 1) - 1;
        clampcur();
        break;
    case 'A': cursor_y -= esc_p(0, 1); clampcur(); break;
    case 'B': cursor_y += esc_p(0, 1); clampcur(); break;
    case 'C': cursor_x += esc_p(0, 1); clampcur(); break;
    case 'D': cursor_x -= esc_p(0, 1); clampcur(); break;
    case 'G': cursor_x = esc_p(0, 1) - 1; clampcur(); break;
    case 'd': cursor_y = esc_p(0, 1) - 1; clampcur(); break;
    case 'K': {
        int m = esc_have[0] ? esc_par[0] : 0;
        if (m == 0)      erase_cells(cursor_y, cursor_x, con_cols);
        else if (m == 1) erase_cells(cursor_y, 0, cursor_x + 1);
        else             erase_cells(cursor_y, 0, con_cols);
        break;
    }
    case 'J': {
        int m = esc_have[0] ? esc_par[0] : 0;
        if (m == 0) {
            erase_cells(cursor_y, cursor_x, con_cols);
            for (int y = cursor_y + 1; y < con_rows; y++) erase_cells(y, 0, con_cols);
        } else if (m == 1) {
            for (int y = 0; y < cursor_y; y++) erase_cells(y, 0, con_cols);
            erase_cells(cursor_y, 0, cursor_x + 1);
        } else {
            for (int y = 0; y < con_rows; y++) erase_cells(y, 0, con_cols);
        }
        break;
    }
    case 'm':
        for (int i = 0; i <= esc_np; i++) {
            int v = esc_have[i] ? esc_par[i] : 0;
            if (v == 0 || v == 27) cur_attr = 0;
            else if (v == 7) cur_attr = ATTR_REV;
            /* остальные атрибуты (цвета, жирный) пока не поддерживаются */
        }
        break;
    case 's': saved_x = cursor_x; saved_y = cursor_y; break;
    case 'u': cursor_x = saved_x; cursor_y = saved_y; clampcur(); break;
    default: break;
    }
}

static void esc_feed(char c)
{
    if (esc_state == 1) {
        if (c == '[') {
            esc_state = 2; esc_np = 0; esc_priv = 0;
            for (int i = 0; i < 6; i++) { esc_par[i] = 0; esc_have[i] = 0; }
        } else {
            esc_state = 0;          /* другие ESC-последовательности пропускаем */
        }
        return;
    }
    if (c >= '0' && c <= '9') {
        esc_par[esc_np] = esc_par[esc_np] * 10 + (c - '0');
        esc_have[esc_np] = 1;
    } else if (c == ';') {
        if (esc_np < 5) esc_np++;
    } else if (c == '?' || c == '>' || c == '=') {
        esc_priv = 1;
    } else if (c >= 0x40 && c <= 0x7E) {
        csi_exec(c);
        esc_state = 0;
    } else if (c < 0x20 || c >= 0x7F) {
        esc_state = 0;              /* мусор: выходим из последовательности */
    }
}

void console_putc(char c)
{
    if (c == '\b' || c == 127) {
        /* В терминале QEMU (COM1): возврат влево, пробел, возврат влево */
        serial_out('\b');
        serial_out(' ');
        serial_out('\b');

        /* На экране (Framebuffer) */
        if (fb && cursor_x > 0) {
            cursor_hide();
            cursor_x--;
            uint16_t keep = cur_attr; cur_attr = 0;
            put_cell(' ', cursor_x, cursor_y);
            cur_attr = keep;
            cursor_show();
        }
        return;
    }

    /* Пишем в COM1 (терминал QEMU сам разбирает ESC-последовательности) */
    serial_out(c);

    if (!fb) return; /* Если фреймбуфера нет (BIOS режим), ничего не рисуем на экран */

    cursor_hide();

    int max_cols = con_cols;
    int max_rows = con_rows;

    if (esc_state) {
        esc_feed(c);
    } else if (c == 27) {
        esc_state = 1;
    } else if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\t') {
        int nx = (cursor_x + 8) & ~7;
        while (cursor_x < nx && cursor_x < max_cols - 1) {
            put_cell(' ', cursor_x, cursor_y);
            cursor_x++;
        }
    } else if ((unsigned char)c >= 32) {
        put_cell(c, cursor_x, cursor_y);
        cursor_x++;
        if (cursor_x >= max_cols) {
            cursor_x = 0;
            cursor_y++;
        }
    }

    if (cursor_y >= max_rows) {
        fb_scroll();
    }
    cursor_show();
}

static void print_num(unsigned long n, int base)
{
    char buf[65];
    static const char digits[] = "0123456789ABCDEF";
    int i = 0;
    if (n == 0) { console_putc('0'); return; }
    while (n > 0) { buf[i++] = digits[n % base]; n /= base; }
    while (--i >= 0) console_putc(buf[i]);
}

void printk(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') { console_putc(*p); continue; }
        p++;
        switch (*p) {
            case 'c': console_putc((char)va_arg(args, int)); break;
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                while (*s) console_putc(*s++);
                break;
            }
            case 'd': {
                long d = va_arg(args, long);
                if (d < 0) { console_putc('-'); d = -d; }
                print_num(d, 10);
                break;
            }
            case 'x': print_num(va_arg(args, unsigned long), 16); break;
            case 'p': console_putc('0'); console_putc('x'); print_num(va_arg(args, unsigned long), 16); break;
            case '%': console_putc('%'); break;
            default: console_putc(*p); break;
        }
    }
    va_end(args);
}
