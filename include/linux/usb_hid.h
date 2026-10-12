#ifndef _LINUX_USB_HID_H
#define _LINUX_USB_HID_H

#include <linux/types.h>

/* Состояние одной USB-клавиатуры (предыдущий отчёт + автоповтор) */
struct hid_state {
    uint8_t  prev[6];
    uint8_t  rep_key;
    uint8_t  rep_mods;
    uint32_t rep_cnt;
};

/* Раскладка Input-отчёта клавиатуры, полученная из HID report descriptor.
 * Нужна для клавиатур без boot-протокола (NKRO, Report ID, длинные отчёты). */
#define HID_MAX_BITMAPS 3
struct hid_layout {
    uint8_t  valid;          /* найдены поля клавиш (Usage Page 0x07) */
    uint8_t  rid;            /* Report ID входного отчёта (0 = без ID) */
    uint16_t bytes;          /* полная длина входного отчёта, включая байт ID */
    int16_t  mod_off;        /* битовое смещение 8 модификаторов (-1 = нет) */
    int16_t  arr_off;        /* битовое смещение массива кодов клавиш (-1 = нет) */
    uint8_t  arr_count;      /* число 8-битных кодов в массиве */
    uint8_t  nbitmaps;       /* битовые карты клавиш (NKRO) */
    struct { int16_t off; uint8_t usage_min; uint8_t usage_max; uint16_t count; } bm[HID_MAX_BITMAPS];
    uint8_t  led_ok;         /* есть Output-отчёт со светодиодами (Usage Page 0x08) */
    uint8_t  led_rid;        /* Report ID выходного отчёта светодиодов */
};

/* Разбор report descriptor; возвращает 1, если это клавиатура с клавишами */
int hid_parse_report_desc(const uint8_t *desc, uint32_t len, struct hid_layout *lay);

/* Приводит входной отчёт к виду boot-отчёта [mods, 0, k1..k6]; 0 -- отчёт чужой */
int hid_to_boot_report(const struct hid_layout *lay, const volatile uint8_t *rep, uint32_t len,
                       uint8_t out[8]);

void usb_hid_register(struct hid_state *s, int on);
void usb_hid_report(struct hid_state *s, const uint8_t *r);
void usb_hid_tick(void);

/* Состояние светодиодов (бит0 NumLock, бит1 CapsLock, бит2 ScrollLock) */
uint8_t usb_hid_leds(void);
int     usb_hid_leds_dirty(void);   /* возвращает 1 и сбрасывает флаг, если LED изменились */

#endif
