/*
 * Разбор HID report descriptor для клавиатур без boot-протокола.
 * Поддерживается: Report ID, массив кодов клавиш (Usage Page 0x07, 8 бит),
 * 8 модификаторов (Usage 0xE0..0xE7), битовые карты (NKRO), LED (Usage Page 0x08).
 */
#include <linux/usb_hid.h>

#define MAX_IDS 8

int hid_parse_report_desc(const uint8_t *d, uint32_t len, struct hid_layout *lay)
{
    uint32_t id_list[MAX_IDS];
    uint32_t id_pos[MAX_IDS];
    int nids = 0;

    uint32_t page = 0, rsize = 0, rcount = 0, rid = 0;
    uint32_t umin = 0, umax = 0;
    int has_range = 0;
    int kbd_ids_set = 0;

    lay->valid = 0; lay->rid = 0; lay->bytes = 0;
    lay->mod_off = -1; lay->arr_off = -1; lay->arr_count = 0; lay->nbitmaps = 0;
    lay->led_ok = 0; lay->led_rid = 0;

    for (uint32_t i = 0; i < len; ) {
        uint8_t pfx = d[i];
        uint32_t size = pfx & 3, type = (pfx >> 2) & 3, tag = pfx >> 4;
        if (pfx == 0xFE) {                       /* long item: пропускаем */
            if (i + 2 >= len) break;
            i += 3 + d[i + 1];
            continue;
        }
        if (size == 3) size = 4;
        if (i + 1 + size > len) break;
        uint32_t v = 0;
        for (uint32_t b = 0; b < size; b++) v |= (uint32_t)d[i + 1 + b] << (8 * b);
        i += 1 + size;

        if (type == 1) {                         /* Global */
            if (tag == 0) page = v;
            else if (tag == 7) rsize = v;
            else if (tag == 8) rid = v;
            else if (tag == 9) rcount = v;
        } else if (type == 2) {                  /* Local */
            if (tag == 1) { umin = v; has_range = 1; }
            else if (tag == 2) { umax = v; has_range = 1; }
        } else if (type == 0) {                  /* Main */
            if (tag == 8) {                      /* Input */
                int pi = -1;
                for (int k = 0; k < nids; k++) if (id_list[k] == rid) pi = k;
                if (pi < 0 && nids < MAX_IDS) {
                    id_list[nids] = rid; id_pos[nids] = 0; pi = nids++;
                }
                if (pi >= 0) {
                    uint32_t pos = id_pos[pi];
                    int is_const = v & 1, is_var = (v >> 1) & 1;
                    /* Клавиатурные поля берём только из первого отчёта, где они есть */
                    if (!is_const && page == 7 && (!kbd_ids_set || lay->rid == rid) && pos < 32000) {
                        if (is_var && rsize == 1 && has_range && umin == 0xE0 && umax == 0xE7 && rcount == 8) {
                            lay->mod_off = (int16_t)pos; lay->rid = (uint8_t)rid; kbd_ids_set = 1; lay->valid = 1;
                        } else if (is_var && rsize == 1 && has_range && lay->nbitmaps < HID_MAX_BITMAPS) {
                            int n = lay->nbitmaps++;
                            lay->bm[n].off = (int16_t)pos;
                            lay->bm[n].usage_min = (uint8_t)umin;
                            lay->bm[n].usage_max = (uint8_t)umax;
                            lay->bm[n].count = (uint16_t)rcount;
                            lay->rid = (uint8_t)rid; kbd_ids_set = 1; lay->valid = 1;
                        } else if (!is_var && rsize == 8 && lay->arr_off < 0) {
                            lay->arr_off = (int16_t)pos;
                            lay->arr_count = (uint8_t)(rcount > 32 ? 32 : rcount);
                            lay->rid = (uint8_t)rid; kbd_ids_set = 1; lay->valid = 1;
                        }
                    }
                    id_pos[pi] = pos + rsize * rcount;
                }
            } else if (tag == 9) {               /* Output */
                if (page == 8 && !lay->led_ok) { lay->led_ok = 1; lay->led_rid = (uint8_t)rid; }
            }
            umin = umax = 0; has_range = 0;      /* локальное состояние сбрасывается */
        }
    }

    if (!lay->valid) return 0;
    for (int k = 0; k < nids; k++) {
        if (id_list[k] == lay->rid) {
            uint32_t bits = id_pos[k];
            lay->bytes = (uint16_t)((bits + 7) / 8 + (lay->rid ? 1 : 0));
        }
    }
    if (lay->bytes == 0 || lay->bytes > 64) return 0;
    return 1;
}

static uint8_t get_bit(const volatile uint8_t *p, uint32_t off)
{
    return (p[off >> 3] >> (off & 7)) & 1;
}

static uint8_t get_byte(const volatile uint8_t *p, uint32_t off)
{
    uint32_t i = off >> 3, s = off & 7;
    if (s == 0) return p[i];
    return (uint8_t)((p[i] >> s) | (p[i + 1] << (8 - s)));
}

int hid_to_boot_report(const struct hid_layout *lay, const volatile uint8_t *rep, uint32_t len,
                       uint8_t out[8])
{
    const volatile uint8_t *p = rep;
    if (len < lay->bytes) return 0;
    if (lay->rid) {
        if (p[0] != lay->rid) return 0;
        p++;
    }
    for (int i = 0; i < 8; i++) out[i] = 0;
    int n = 0;

    if (lay->mod_off >= 0) out[0] = get_byte(p, (uint32_t)lay->mod_off);

    if (lay->arr_off >= 0) {
        for (uint32_t i = 0; i < lay->arr_count; i++) {
            uint8_t k = get_byte(p, (uint32_t)lay->arr_off + 8 * i);
            if (k == 1) { out[2] = 1; return 1; }          /* ErrorRollOver */
            if (k >= 4 && n < 6) out[2 + n++] = k;
        }
    }
    for (int b = 0; b < lay->nbitmaps; b++) {
        for (uint32_t i = 0; i < lay->bm[b].count; i++) {
            if (!get_bit(p, (uint32_t)lay->bm[b].off + i)) continue;
            uint32_t u = (uint32_t)lay->bm[b].usage_min + i;
            if (u >= 0xE0 && u <= 0xE7) out[0] |= (uint8_t)(1U << (u - 0xE0));
            else if (u >= 4 && u < 0xE0 && n < 6) out[2 + n++] = (uint8_t)u;
        }
    }
    return 1;
}
