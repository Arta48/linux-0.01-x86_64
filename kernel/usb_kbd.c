/*
 * kernel/usb_kbd.c -- Преобразование отчётов USB HID Boot Keyboard в символы
 * (Stage 48). Отчёт: [0]=модификаторы, [1]=reserved, [2..7]=коды клавиш (Usage ID).
 * Состояние (предыдущий отчёт, автоповтор) хранится отдельно для каждой клавиатуры,
 * CapsLock/NumLock/ScrollLock -- общие для всех.
 */
#include <linux/usb_hid.h>
#include <linux/keyboard.h>

#define MOD_LCTRL   (1U << 0)
#define MOD_LSHIFT  (1U << 1)
#define MOD_RCTRL   (1U << 4)
#define MOD_RSHIFT  (1U << 5)

#define REPEAT_DELAY_TICKS  50   /* 500 мс при 100 Гц */
#define REPEAT_RATE_TICKS    3   /* ~33 символа/с */

#define MAX_KBDS 16

/* Usage ID 0x04..0x38 (a..z, 1..0, Enter, Esc, BS, Tab, Space, знаки) */
static const char usage_plain[0x39] = {
    [0x04] = 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
               'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    [0x1E] = '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    [0x28] = '\n', 27, '\b', '\t', ' ', '-', '=', '[', ']', '\\', '\\',
               ';', '\'', '`', ',', '.', '/',
};

static const char usage_shift[0x39] = {
    [0x04] = 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
               'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    [0x1E] = '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    [0x28] = '\n', 27, '\b', '\t', ' ', '_', '+', '{', '}', '|', '|',
               ':', '"', '~', '<', '>', '?',
};

static struct hid_state *kbds[MAX_KBDS];
static uint8_t caps_lock;
static uint8_t num_lock = 1;
static uint8_t scroll_lock;
static volatile uint8_t leds_dirty = 1;   /* при первом подключении включаем NumLock */

void usb_hid_register(struct hid_state *s, int on)
{
    for (int i = 0; i < MAX_KBDS; i++) {
        if (on && !kbds[i]) { kbds[i] = s; return; }
        if (!on && kbds[i] == s) { kbds[i] = 0; return; }
    }
}

uint8_t usb_hid_leds(void)
{
    return (uint8_t)((num_lock ? 1 : 0) | (caps_lock ? 2 : 0) | (scroll_lock ? 4 : 0));
}

int usb_hid_leds_dirty(void)
{
    if (!leds_dirty) return 0;
    leds_dirty = 0;
    return 1;
}

static void esc_seq(char c)
{
    keyboard_inject_char('\033');
    keyboard_inject_char('[');
    keyboard_inject_char(c);
}

static void emit_key(uint8_t key, uint8_t mods)
{
    int shift = (mods & (MOD_LSHIFT | MOD_RSHIFT)) != 0;
    int ctrl  = (mods & (MOD_LCTRL | MOD_RCTRL)) != 0;

    if (key >= 0x04 && key <= 0x1D) {                   /* буквы */
        if (ctrl) {
            if (key == 0x06) keyboard_inject_sigint();  /* Ctrl+C */
            else keyboard_inject_char((char)(key - 0x04 + 1));
            return;
        }
        if (caps_lock) shift = !shift;
        keyboard_inject_char(shift ? usage_shift[key] : usage_plain[key]);
        return;
    }
    if (key < sizeof(usage_plain)) {
        char c = shift ? usage_shift[key] : usage_plain[key];
        if (c) keyboard_inject_char(c);
        return;
    }
    switch (key) {
        case 0x4C: keyboard_inject_char('\b'); break;   /* Delete -> Backspace */
        case 0x4F: esc_seq('C'); break;                 /* Right */
        case 0x50: esc_seq('D'); break;                 /* Left  */
        case 0x51: esc_seq('B'); break;                 /* Down  */
        case 0x52: esc_seq('A'); break;                 /* Up    */
        case 0x4A: esc_seq('H'); break;                 /* Home  */
        case 0x4D: esc_seq('F'); break;                 /* End   */
        /* Цифровой блок */
        case 0x54: keyboard_inject_char('/'); break;
        case 0x55: keyboard_inject_char('*'); break;
        case 0x56: keyboard_inject_char('-'); break;
        case 0x57: keyboard_inject_char('+'); break;
        case 0x58: keyboard_inject_char('\n'); break;
        case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D:
        case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63:
            if (num_lock) {
                static const char digits[] = "1234567890.";
                keyboard_inject_char(digits[key - 0x59]);
            } else {
                switch (key) {
                    case 0x59: esc_seq('F'); break;     /* 1 -> End   */
                    case 0x5A: esc_seq('B'); break;     /* 2 -> Down  */
                    case 0x5C: esc_seq('D'); break;     /* 4 -> Left  */
                    case 0x5E: esc_seq('C'); break;     /* 6 -> Right */
                    case 0x5F: esc_seq('H'); break;     /* 7 -> Home  */
                    case 0x60: esc_seq('A'); break;     /* 8 -> Up    */
                    case 0x63: keyboard_inject_char('\b'); break; /* . -> Del */
                    default: break;
                }
            }
            break;
        default: break;
    }
}

void usb_hid_report(struct hid_state *s, const uint8_t *r)
{
    uint8_t mods = r[0];

    /* Rollover error (все клавиши = 0x01) -- игнорируем */
    if (r[2] == 0x01) return;

    int newest = 0;
    for (int i = 0; i < 6; i++) {
        uint8_t k = r[2 + i];
        if (!k) continue;
        int was_down = 0;
        for (int j = 0; j < 6; j++) if (s->prev[j] == k) was_down = 1;
        if (was_down) continue;

        if (k == 0x39) { caps_lock ^= 1; leds_dirty = 1; continue; }    /* CapsLock   */
        if (k == 0x53) { num_lock ^= 1; leds_dirty = 1; continue; }     /* NumLock    */
        if (k == 0x47) { scroll_lock ^= 1; leds_dirty = 1; continue; }  /* ScrollLock */
        emit_key(k, mods);
        newest = k;
    }

    if (newest) {
        s->rep_key = (uint8_t)newest;
        s->rep_mods = mods;
        s->rep_cnt = 0;
    } else {
        /* Автоповтор прекращается, если удерживаемая клавиша отпущена */
        int still = 0;
        for (int i = 0; i < 6; i++) if (r[2 + i] == s->rep_key) still = 1;
        if (!still) s->rep_key = 0;
    }
    for (int i = 0; i < 6; i++) s->prev[i] = r[2 + i];
}

/* Вызывается каждые 10 мс: программный автоповтор удерживаемых клавиш */
void usb_hid_tick(void)
{
    for (int i = 0; i < MAX_KBDS; i++) {
        struct hid_state *s = kbds[i];
        if (!s || !s->rep_key) continue;
        s->rep_cnt++;
        if (s->rep_cnt >= REPEAT_DELAY_TICKS &&
            ((s->rep_cnt - REPEAT_DELAY_TICKS) % REPEAT_RATE_TICKS) == 0) {
            emit_key(s->rep_key, s->rep_mods);
        }
    }
}
