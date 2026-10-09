/*
 * kernel/xhci.c -- Драйвер xHCI (USB 3.x host controller) и загрузочной
 * USB HID клавиатуры (Stage 48).
 *
 * Работает без прерываний (polling): события читаются из Event Ring
 * либо синхронно при инициализации, либо из таймерного прерывания
 * (usb_kbd_poll). Все DMA-структуры лежат ниже 1 ГБ (identity map),
 * поэтому физический адрес == виртуальному.
 *
 * Поддерживается: корневые порты, Enable Slot / Address Device /
 * Evaluate Context / Configure Endpoint, control-передачи по EP0,
 * Interrupt IN endpoint boot-клавиатуры. Хабы пока НЕ поддерживаются.
 */
#include <linux/xhci.h>
#include <linux/pci.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/string.h>
#include <asm/io.h>

/* ------------------------------------------------------------------ */
/* Константы                                                          */
/* ------------------------------------------------------------------ */
#define RING_TRBS        256            /* 256 * 16 = 4096 байт = страница */

/* Типы TRB */
#define TRB_NORMAL        1
#define TRB_SETUP         2
#define TRB_DATA          3
#define TRB_STATUS        4
#define TRB_LINK          6
#define TRB_CMD_ENABLE_SLOT   9
#define TRB_CMD_ADDRESS_DEV   11
#define TRB_CMD_CONFIG_EP     12
#define TRB_CMD_EVAL_CTX      13
#define TRB_EV_TRANSFER       32
#define TRB_EV_CMD_COMPLETE   33
#define TRB_EV_PORT_CHANGE    34

#define TRB_C        (1U << 0)   /* Cycle */
#define TRB_TC       (1U << 1)   /* Toggle Cycle (Link) */
#define TRB_ISP      (1U << 2)   /* Interrupt on Short Packet */
#define TRB_IOC      (1U << 5)   /* Interrupt On Completion */
#define TRB_IDT      (1U << 6)   /* Immediate Data */
#define TRB_TYPE(t)  ((uint32_t)(t) << 10)

#define CC_SUCCESS        1
#define CC_SHORT_PACKET  13

/* PORTSC */
#define PORTSC_CCS   (1U << 0)
#define PORTSC_PED   (1U << 1)
#define PORTSC_PR    (1U << 4)
#define PORTSC_PP    (1U << 9)
#define PORTSC_CSC   (1U << 17)
#define PORTSC_PEC   (1U << 18)
#define PORTSC_WRC   (1U << 19)
#define PORTSC_OCC   (1U << 20)
#define PORTSC_PRC   (1U << 21)
#define PORTSC_PLC   (1U << 22)
#define PORTSC_CEC   (1U << 23)
#define PORTSC_CHANGE_BITS (PORTSC_CSC | PORTSC_PEC | PORTSC_WRC | PORTSC_OCC | \
                            PORTSC_PRC | PORTSC_PLC | PORTSC_CEC)
/* Биты, которые надо сохранять при записи (RW): PP, PIC, wake enable */
#define PORTSC_PRESERVE 0x0E00C200U

struct trb {
    volatile uint32_t d0, d1, d2, d3;
};

struct ring {
    struct trb *trbs;
    uint32_t    enq;
    uint32_t    cycle;
};

struct xhci_state {
    int      ready;               /* клавиатура настроена, можно опрашивать */
    volatile uint8_t  *cap;
    volatile uint8_t  *op;
    volatile uint8_t  *rt;
    volatile uint32_t *db;
    uint32_t ctx_size;            /* 32 или 64 байта */
    uint32_t max_slots, max_ports;

    uint64_t *dcbaa;
    struct ring cmd;
    struct trb *evt;
    uint32_t evt_deq;
    uint32_t evt_cycle;

    /* Найденная клавиатура */
    uint32_t kbd_slot;
    uint32_t kbd_dci;
    struct ring kbd_ring;
    volatile uint8_t *kbd_buf;    /* 8 байт отчёта HID */
};

static struct xhci_state X;
static struct xhci_info  x_info;

/* Буфер дескрипторов: страница из аллокатора (не пересекает границу 64 КБ) */
static uint8_t *ctl_buf;

const struct xhci_info *xhci_get_info(void) { return &x_info; }

/* ------------------------------------------------------------------ */
/* Низкоуровневые помощники                                           */
/* ------------------------------------------------------------------ */
static inline void mb(void) { __asm__ volatile ("mfence" : : : "memory"); }

static inline uint32_t rd32(volatile uint8_t *base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}
static inline void wr32(volatile uint8_t *base, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(base + off) = v;
}

/* Задержка ~1 мкс: запись в порт 0x80 занимает фиксированное время шины */
static void udelay(uint32_t us)
{
    while (us--) outb(0, 0x80);
}
static void mdelay(uint32_t ms)
{
    while (ms--) udelay(1000);
}

static void *dma_page(void)
{
    return (void *)get_free_page();      /* уже обнулена, выровнена на 4 КБ */
}

static void ring_init(struct ring *r)
{
    r->trbs  = (struct trb *)dma_page();
    r->enq   = 0;
    r->cycle = 1;
}

/* Кладёт TRB в кольцо; бит Cycle ставится последним. Link-TRB обрабатывается тут же. */
static void ring_push(struct ring *r, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t ctrl)
{
    struct trb *t = &r->trbs[r->enq];
    t->d0 = d0; t->d1 = d1; t->d2 = d2;
    mb();
    t->d3 = ctrl | (r->cycle ? TRB_C : 0);
    r->enq++;

    if (r->enq == RING_TRBS - 1) {
        struct trb *l = &r->trbs[RING_TRBS - 1];
        uint64_t base = (uint64_t)r->trbs;
        l->d0 = (uint32_t)base;
        l->d1 = (uint32_t)(base >> 32);
        l->d2 = 0;
        mb();
        l->d3 = TRB_TYPE(TRB_LINK) | TRB_TC | (r->cycle ? TRB_C : 0);
        r->enq = 0;
        r->cycle ^= 1;
    }
}

static inline void ring_doorbell(uint32_t slot, uint32_t target)
{
    mb();
    X.db[slot] = target;
}

/* Забирает одно событие из Event Ring (0 -- нет события) */
static int event_get(struct trb *out)
{
    struct trb *t = &X.evt[X.evt_deq];
    uint32_t c = t->d3;
    if ((c & TRB_C) != X.evt_cycle) return 0;
    mb();
    out->d0 = t->d0; out->d1 = t->d1; out->d2 = t->d2; out->d3 = c;

    X.evt_deq++;
    if (X.evt_deq == RING_TRBS) {
        X.evt_deq = 0;
        X.evt_cycle ^= 1;
    }
    /* ERDP (интеррапер 0): адрес следующего TRB + бит EHB (3) */
    uint64_t erdp = (uint64_t)&X.evt[X.evt_deq] | (1U << 3);
    wr32(X.rt, 0x38, (uint32_t)erdp);
    wr32(X.rt, 0x3C, (uint32_t)(erdp >> 32));
    return 1;
}

/* Ждёт событие нужного типа; остальные события (Port Status Change) отбрасываются */
static int event_wait(uint32_t type, uint32_t slot_filter, struct trb *out, uint32_t timeout_ms)
{
    for (uint32_t i = 0; i < timeout_ms * 20; i++) {
        struct trb e;
        while (event_get(&e)) {
            if (((e.d3 >> 10) & 0x3F) == type &&
                (slot_filter == 0 || ((e.d3 >> 24) & 0xFF) == slot_filter)) {
                *out = e;
                return 0;
            }
        }
        udelay(50);
    }
    return -1;
}

/* Команда: возвращает код завершения (CC) или -1 по таймауту */
static int command(uint32_t d0, uint32_t d1, uint32_t ctrl, uint32_t *slot_out)
{
    ring_push(&X.cmd, d0, d1, 0, ctrl);
    ring_doorbell(0, 0);
    struct trb e;
    if (event_wait(TRB_EV_CMD_COMPLETE, 0, &e, 500) < 0) return -1;
    if (slot_out) *slot_out = (e.d3 >> 24) & 0xFF;
    return (int)((e.d2 >> 24) & 0xFF);
}

/* ------------------------------------------------------------------ */
/* Контексты                                                          */
/* ------------------------------------------------------------------ */
static inline uint32_t *ctx_at(void *base, uint32_t index)
{
    return (uint32_t *)((uint8_t *)base + (uint64_t)index * X.ctx_size);
}

struct usb_dev {
    uint32_t slot;
    uint32_t speed;
    uint32_t port;
    struct ring ep0;
    void *dev_ctx;
    void *in_ctx;
};
static struct usb_dev D;

/* Control-передача по EP0. dir_in: 1 = данные от устройства. Возвращает 0 при успехе */
static int control_xfer(uint8_t bm, uint8_t req, uint16_t val, uint16_t idx,
                        uint16_t len, void *buf, int dir_in)
{
    uint32_t trt = len ? (dir_in ? 3 : 2) : 0;

    ring_push(&D.ep0,
              (uint32_t)bm | ((uint32_t)req << 8) | ((uint32_t)val << 16),
              (uint32_t)idx | ((uint32_t)len << 16),
              8,
              TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));
    if (len) {
        uint64_t p = (uint64_t)buf;
        ring_push(&D.ep0, (uint32_t)p, (uint32_t)(p >> 32), len,
                  TRB_TYPE(TRB_DATA) | (dir_in ? (1U << 16) : 0));
    }
    /* Status stage: направление противоположно данным (для no-data -- IN) */
    ring_push(&D.ep0, 0, 0, 0,
              TRB_TYPE(TRB_STATUS) | TRB_IOC | ((len && dir_in) ? 0 : (1U << 16)));
    ring_doorbell(D.slot, 1);

    struct trb e;
    if (event_wait(TRB_EV_TRANSFER, D.slot, &e, 1000) < 0) return -1;
    uint32_t cc = (e.d2 >> 24) & 0xFF;
    return (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) ? 0 : -(int)cc;
}

static int get_descriptor(uint8_t type, uint8_t index, uint16_t len, void *buf)
{
    memset(buf, 0, len);
    return control_xfer(0x80, 6, (uint16_t)((type << 8) | index), 0, len, buf, 1);
}

/* ------------------------------------------------------------------ */
/* Инициализация контроллера                                          */
/* ------------------------------------------------------------------ */
static void bios_handoff(uint32_t hccparams1)
{
    uint32_t off = ((hccparams1 >> 16) & 0xFFFF) * 4;
    for (int guard = 0; off && guard < 64; guard++) {
        uint32_t cap = rd32(X.cap, off);
        if ((cap & 0xFF) == 1) {            /* USB Legacy Support */
            if (cap & (1U << 16)) {         /* BIOS Owned */
                wr32(X.cap, off, cap | (1U << 24));   /* OS Owned */
                for (int t = 0; t < 1000; t++) {
                    if (!(rd32(X.cap, off) & (1U << 16))) break;
                    mdelay(1);
                }
                if (rd32(X.cap, off) & (1U << 16))
                    printk("[USB] BIOS did not release xHCI (continuing anyway)\n");
                else
                    printk("[OK] USB: xHCI ownership taken from BIOS/SMM\n");
            }
            /* Отключаем SMI от xHCI и сбрасываем события (как в Linux) */
            uint32_t ctl = rd32(X.cap, off + 4);
            ctl &= (0x3U << 1) | (0xFFU << 5) | (0x7U << 17);
            ctl |= (0x7U << 29);
            wr32(X.cap, off + 4, ctl);
        }
        uint32_t next = (cap >> 8) & 0xFF;
        if (!next) break;
        off += next * 4;
    }
}

static int controller_start(void)
{
    if (!ctl_buf) ctl_buf = (uint8_t *)dma_page();
    uint32_t hcs1 = rd32(X.cap, 4);
    uint32_t hcs2 = rd32(X.cap, 8);
    uint32_t hcc1 = rd32(X.cap, 0x10);
    X.max_slots = hcs1 & 0xFF;
    X.max_ports = (hcs1 >> 24) & 0xFF;
    X.ctx_size  = (hcc1 & (1U << 2)) ? 64 : 32;
    uint32_t caplen = X.cap[0];
    X.op = X.cap + caplen;
    X.rt = X.cap + (rd32(X.cap, 0x18) & ~0x1FU);
    X.db = (volatile uint32_t *)(X.cap + (rd32(X.cap, 0x14) & ~0x3U));

    bios_handoff(hcc1);

    /* Остановка и сброс контроллера */
    wr32(X.op, 0x00, rd32(X.op, 0x00) & ~1U);
    for (int t = 0; t < 100 && !(rd32(X.op, 0x04) & 1U); t++) mdelay(1);
    wr32(X.op, 0x00, rd32(X.op, 0x00) | (1U << 1));            /* HCRST */
    for (int t = 0; t < 500 && (rd32(X.op, 0x00) & (1U << 1)); t++) mdelay(1);
    for (int t = 0; t < 500 && (rd32(X.op, 0x04) & (1U << 11)); t++) mdelay(1);
    if (rd32(X.op, 0x04) & (1U << 11)) {
        printk("[USB] xHCI: controller not ready after reset\n");
        return -1;
    }

    if (X.max_slots > 32) X.max_slots = 32;
    wr32(X.op, 0x38, X.max_slots);                              /* CONFIG */

    /* DCBAA + scratchpad */
    X.dcbaa = (uint64_t *)dma_page();
    uint32_t spb = ((hcs2 >> 21) & 0x1F) << 5 | ((hcs2 >> 27) & 0x1F);
    if (spb) {
        uint64_t *sp = (uint64_t *)dma_page();
        if (spb > 512) spb = 512;
        for (uint32_t i = 0; i < spb; i++) sp[i] = (uint64_t)dma_page();
        X.dcbaa[0] = (uint64_t)sp;
    }
    wr32(X.op, 0x30, (uint32_t)(uint64_t)X.dcbaa);
    wr32(X.op, 0x34, (uint32_t)((uint64_t)X.dcbaa >> 32));

    /* Command ring */
    ring_init(&X.cmd);
    uint64_t crcr = (uint64_t)X.cmd.trbs | 1;                   /* RCS = 1 */
    wr32(X.op, 0x18, (uint32_t)crcr);
    wr32(X.op, 0x1C, (uint32_t)(crcr >> 32));

    /* Event ring: один сегмент */
    X.evt = (struct trb *)dma_page();
    X.evt_deq = 0;
    X.evt_cycle = 1;
    uint64_t *erst = (uint64_t *)dma_page();
    erst[0] = (uint64_t)X.evt;
    erst[1] = RING_TRBS;
    wr32(X.rt, 0x28, 1);                                        /* ERSTSZ */
    wr32(X.rt, 0x38, (uint32_t)(uint64_t)X.evt);                /* ERDP */
    wr32(X.rt, 0x3C, (uint32_t)((uint64_t)X.evt >> 32));
    wr32(X.rt, 0x30, (uint32_t)(uint64_t)erst);                 /* ERSTBA (последним) */
    wr32(X.rt, 0x34, (uint32_t)((uint64_t)erst >> 32));

    /* Запуск (INTE = 0: работаем опросом) */
    wr32(X.op, 0x00, 1U);
    for (int t = 0; t < 100 && (rd32(X.op, 0x04) & 1U); t++) mdelay(1);
    if (rd32(X.op, 0x04) & 1U) {
        printk("[USB] xHCI: controller failed to start\n");
        return -1;
    }
    mdelay(20);
    return 0;
}

/* Сброс порта; возвращает скорость (1..4) или 0 если устройства нет */
static int port_reset(uint32_t port)
{
    uint32_t off = 0x400 + (port - 1) * 0x10;
    uint32_t sc = rd32(X.op, off);

    if (!(sc & PORTSC_PP)) {
        wr32(X.op, off, (sc & PORTSC_PRESERVE) | PORTSC_PP);
        mdelay(20);
        sc = rd32(X.op, off);
    }
    if (!(sc & PORTSC_CCS)) return 0;

    /* Сбрасываем накопленные флаги изменений */
    wr32(X.op, off, (sc & PORTSC_PRESERVE) | (sc & PORTSC_CHANGE_BITS));

    if (!(sc & PORTSC_PED)) {
        /* USB 2.0: порт включается только после reset (USB 3.0 уже в U0) */
        wr32(X.op, off, (rd32(X.op, off) & PORTSC_PRESERVE) | PORTSC_PR);
        for (int t = 0; t < 500; t++) {
            sc = rd32(X.op, off);
            if (sc & PORTSC_PRC) break;
            mdelay(1);
        }
        wr32(X.op, off, (sc & PORTSC_PRESERVE) | PORTSC_PRC | PORTSC_CSC | PORTSC_PEC);
        mdelay(20);
    }
    sc = rd32(X.op, off);
    if (!(sc & PORTSC_PED) || !(sc & PORTSC_CCS)) return 0;
    return (int)((sc >> 10) & 0xF);
}

static uint32_t ep0_default_mps(uint32_t speed)
{
    switch (speed) {
        case 4:  return 512;   /* SuperSpeed */
        case 3:  return 64;    /* HighSpeed  */
        default: return 8;     /* Low / Full -- уточняется по дескриптору */
    }
}

static void fill_slot_ctx(uint32_t *slot, uint32_t entries)
{
    slot[0] = (D.speed << 20) | (entries << 27);
    slot[1] = (D.port << 16);
}

static void fill_ep_ctx(uint32_t *ep, uint32_t type, uint32_t mps, uint32_t interval,
                        struct ring *r, uint32_t avg_len, uint32_t max_esit)
{
    ep[0] = interval << 16;
    ep[1] = (3U << 1) | (type << 3) | (mps << 16);               /* CErr = 3 */
    uint64_t deq = (uint64_t)r->trbs | 1;                        /* DCS = 1 */
    ep[2] = (uint32_t)deq;
    ep[3] = (uint32_t)(deq >> 32);
    ep[4] = avg_len | (max_esit << 16);
}

/* Поднимает слот для устройства на порту. Возвращает 0 при успехе */
static int device_address(uint32_t port, uint32_t speed)
{
    memset(&D, 0, sizeof(D));
    D.port = port;
    D.speed = speed;

    uint32_t slot = 0;
    int cc = command(0, 0, TRB_TYPE(TRB_CMD_ENABLE_SLOT), &slot);
    if (cc != CC_SUCCESS || !slot) {
        printk("[USB] Port %d: Enable Slot failed (cc=%d)\n", (int)port, (long)cc);
        return -1;
    }
    D.slot = slot;
    D.dev_ctx = dma_page();
    D.in_ctx  = dma_page();
    ring_init(&D.ep0);
    X.dcbaa[slot] = (uint64_t)D.dev_ctx;

    uint32_t *ic = (uint32_t *)D.in_ctx;
    ic[1] = 3;                                   /* Add: Slot + EP0 */
    fill_slot_ctx(ctx_at(D.in_ctx, 1), 1);
    fill_ep_ctx(ctx_at(D.in_ctx, 2), 4, ep0_default_mps(speed), 0, &D.ep0, 8, 0);

    uint64_t p = (uint64_t)D.in_ctx;
    cc = command((uint32_t)p, (uint32_t)(p >> 32), TRB_TYPE(TRB_CMD_ADDRESS_DEV) | (slot << 24), NULL);
    if (cc != CC_SUCCESS) {
        printk("[USB] Port %d: Address Device failed (cc=%d)\n", (int)port, (long)cc);
        return -1;
    }
    return 0;
}

static int update_ep0_mps(uint32_t mps)
{
    memset(D.in_ctx, 0, 4096);
    uint32_t *ic = (uint32_t *)D.in_ctx;
    ic[1] = 2;                                   /* Add: EP0 */
    uint32_t *ep = ctx_at(D.in_ctx, 2);
    ep[1] = (3U << 1) | (4U << 3) | (mps << 16);
    uint64_t p = (uint64_t)D.in_ctx;
    return command((uint32_t)p, (uint32_t)(p >> 32), TRB_TYPE(TRB_CMD_EVAL_CTX) | (D.slot << 24), NULL);
}

/* Разбор конфигурационного дескриптора: ищем Boot Keyboard (class 3, sub 1, proto 1) */
static int find_boot_keyboard(const uint8_t *cfg, uint32_t total,
                              uint8_t *iface, uint8_t *ep_addr, uint16_t *mps, uint8_t *interval)
{
    int in_kbd = 0;
    for (uint32_t i = 0; i + 2 <= total && cfg[i] >= 2; i += cfg[i]) {
        uint8_t type = cfg[i + 1];
        if (type == 4 && i + 9 <= total) {
            in_kbd = (cfg[i + 5] == 3 && cfg[i + 6] == 1 && cfg[i + 7] == 1);
            if (in_kbd) *iface = cfg[i + 2];
        } else if (type == 5 && in_kbd && i + 7 <= total) {
            if ((cfg[i + 2] & 0x80) && (cfg[i + 3] & 3) == 3) {   /* Interrupt IN */
                *ep_addr = cfg[i + 2];
                *mps = (uint16_t)(cfg[i + 4] | (cfg[i + 5] << 8)) & 0x7FF;
                *interval = cfg[i + 6];
                return 0;
            }
        }
    }
    return -1;
}

static uint32_t xhci_interval(uint32_t speed, uint32_t b_interval)
{
    if (b_interval == 0) b_interval = 1;
    if (speed == 3 || speed == 4) {              /* HS/SS: 2^(n-1) * 125 мкс */
        uint32_t v = b_interval - 1;
        return v > 15 ? 15 : v;
    }
    /* LS/FS: b_interval в мс -> экспонента в единицах 125 мкс (3..10) */
    uint32_t units = b_interval * 8, e = 0;
    while ((2U << e) <= units) e++;
    if (e < 3) e = 3;
    if (e > 10) e = 10;
    return e;
}

/* Полная настройка устройства; возвращает 1 если это клавиатура и она запущена */
static int setup_keyboard(uint32_t port, uint32_t speed)
{
    if (device_address(port, speed) < 0) return 0;

    /* Первые 8 байт дескриптора -> реальный размер пакета EP0 */
    if (get_descriptor(1, 0, 8, ctl_buf) < 0) {
        printk("[USB] Port %d: cannot read device descriptor\n", (int)port);
        return 0;
    }
    uint32_t mps = ctl_buf[7];
    if (ctl_buf[3] >= 3) mps = 1U << mps;        /* bcdUSB >= 3.0: степень двойки */
    if (mps && mps != ep0_default_mps(speed)) {
        if (update_ep0_mps(mps) != CC_SUCCESS)
            printk("[USB] Port %d: Evaluate Context failed\n", (int)port);
    }
    if (get_descriptor(1, 0, 18, ctl_buf) < 0) return 0;
    uint8_t dclass = ctl_buf[4];
    uint16_t vid = (uint16_t)(ctl_buf[8] | (ctl_buf[9] << 8));
    uint16_t pid = (uint16_t)(ctl_buf[10] | (ctl_buf[11] << 8));
    printk("[OK] USB Port %d: device %x:%x, class %d, speed %d\n",
           (int)port, vid, pid, dclass, (int)speed);
    if (dclass == 9) {
        printk("[USB] Port %d: USB hub -- not supported yet\n", (int)port);
        return 0;
    }

    /* Конфигурация: сначала заголовок, затем целиком */
    if (get_descriptor(2, 0, 9, ctl_buf) < 0) return 0;
    uint32_t total = ctl_buf[2] | (ctl_buf[3] << 8);
    if (total > sizeof(ctl_buf)) total = sizeof(ctl_buf);
    if (get_descriptor(2, 0, (uint16_t)total, ctl_buf) < 0) return 0;

    uint8_t iface = 0, ep_addr = 0, interval = 0;
    uint16_t ep_mps = 0;
    if (find_boot_keyboard(ctl_buf, total, &iface, &ep_addr, &ep_mps, &interval) < 0)
        return 0;                                /* не клавиатура */
    uint8_t cfg_value = ctl_buf[5];

    printk("[OK] USB HID Boot Keyboard found (iface %d, ep 0x%x, mps %d)\n",
           iface, ep_addr, ep_mps);

    if (control_xfer(0x00, 9, cfg_value, 0, 0, NULL, 0) < 0) {       /* SET_CONFIGURATION */
        printk("[USB] SET_CONFIGURATION failed\n");
        return 0;
    }
    control_xfer(0x21, 0x0B, 0, iface, 0, NULL, 0);                  /* SET_PROTOCOL: boot */
    control_xfer(0x21, 0x0A, 0, iface, 0, NULL, 0);                  /* SET_IDLE: 0 (только изменения) */

    /* Configure Endpoint для Interrupt IN */
    uint32_t dci = (uint32_t)(ep_addr & 0x0F) * 2 + 1;
    ring_init(&X.kbd_ring);
    X.kbd_buf = (volatile uint8_t *)dma_page();

    memset(D.in_ctx, 0, 4096);
    uint32_t *ic = (uint32_t *)D.in_ctx;
    ic[1] = 1 | (1U << dci);                                          /* Add: Slot + EP */
    fill_slot_ctx(ctx_at(D.in_ctx, 1), dci);
    fill_ep_ctx(ctx_at(D.in_ctx, 1 + dci), 7, ep_mps, xhci_interval(speed, interval),
                &X.kbd_ring, 8, ep_mps);

    uint64_t p = (uint64_t)D.in_ctx;
    int cc = command((uint32_t)p, (uint32_t)(p >> 32),
                     TRB_TYPE(TRB_CMD_CONFIG_EP) | (D.slot << 24), NULL);
    if (cc != CC_SUCCESS) {
        printk("[USB] Configure Endpoint failed (cc=%d)\n", (long)cc);
        return 0;
    }

    X.kbd_slot = D.slot;
    X.kbd_dci = dci;

    /* Первый запрос отчёта */
    uint64_t b = (uint64_t)X.kbd_buf;
    ring_push(&X.kbd_ring, (uint32_t)b, (uint32_t)(b >> 32), 8,
              TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    ring_doorbell(X.kbd_slot, X.kbd_dci);
    return 1;
}

/* Инициализация одного контроллера; возвращает 1 если найдена клавиатура */
static int init_controller(const struct pci_device *pd)
{
    uint32_t bar0 = pci_read_dword(pd->bus, pd->slot, pd->func, 0x10);
    uint64_t addr = bar0 & ~0xFULL;
    if ((bar0 & 0x6) == 0x4)
        addr |= (uint64_t)pci_read_dword(pd->bus, pd->slot, pd->func, 0x14) << 32;
    if (!addr) {
        printk("[USB] xHCI %d:%d has no BAR\n", pd->bus, pd->slot);
        return 0;
    }

    /* Memory Space + Bus Master */
    uint16_t cmd = pci_read_word(pd->bus, pd->slot, pd->func, 0x04);
    pci_write_word(pd->bus, pd->slot, pd->func, 0x04, (uint16_t)(cmd | 0x6));

    /* MMIO вне identity-map ядра (1 ГБ): мапим 128 КБ как uncacheable */
    if (addr >= 0x40000000ULL) {
        for (uint64_t pg = 0; pg < 32; pg++)
            map_page(NULL, addr + pg * PAGE_SIZE, addr + pg * PAGE_SIZE,
                     PTE_WRITABLE | (3ULL << 3));
    }
    X.cap = (volatile uint8_t *)addr;

    x_info.mmio_base = addr;
    x_info.cap_length = X.cap[0];
    x_info.hci_version = (uint16_t)(rd32(X.cap, 0) >> 16);
    printk("[OK] USB: xHCI controller %d:%d, MMIO %p, v%x\n",
           pd->bus, pd->slot, addr, x_info.hci_version);

    memset(&X, 0, sizeof(X));
    X.cap = (volatile uint8_t *)addr;
    if (controller_start() < 0) return 0;

    x_info.present = 1;
    x_info.max_slots = (uint8_t)X.max_slots;
    x_info.max_ports = (uint8_t)(X.max_ports > 16 ? 16 : X.max_ports);
    x_info.active_ports_count = 0;

    /* Некоторые контроллеры стартуют с выключенным питанием портов (PPC) */
    for (uint32_t port = 1; port <= X.max_ports; port++) {
        uint32_t off = 0x400 + (port - 1) * 0x10;
        uint32_t sc = rd32(X.op, off);
        if (!(sc & PORTSC_PP)) wr32(X.op, off, (sc & PORTSC_PRESERVE) | PORTSC_PP);
    }
    mdelay(120);                       /* debounce + link training USB 3.0 */

    for (uint32_t port = 1; port <= X.max_ports; port++) {
        uint32_t sc = rd32(X.op, 0x400 + (port - 1) * 0x10);
        if (port <= 16) {
            x_info.ports[port - 1].port_num = (int)port;
            x_info.ports[port - 1].connected = (sc & 1);
            x_info.ports[port - 1].speed = (int)((sc >> 10) & 0xF);
            x_info.ports[port - 1].raw_status = sc;
        }
        if (!(sc & PORTSC_CCS)) continue;
        x_info.active_ports_count++;

        int speed = port_reset(port);
        if (!speed) {
            printk("[USB] Port %d: device present but port did not enable\n", (int)port);
            continue;
        }
        if (setup_keyboard(port, (uint32_t)speed)) {
            X.ready = 1;
            return 1;
        }
    }
    return 0;
}

int xhci_init(void)
{
    memset(&x_info, 0, sizeof(x_info));
    int found = 0;
    int n = pci_get_device_count();

    for (int i = 0; i < n && !found; i++) {
        const struct pci_device *pd = pci_get_device(i);
        if (!pd || pd->class_code != 0x0C || pd->subclass != 0x03 || pd->prog_if != 0x30)
            continue;
        found = init_controller(pd);
    }

    if (found)
        printk("[OK] USB keyboard active (xHCI polling, boot protocol)\n");
    else if (!x_info.present)
        printk("[WARN] xHCI USB Controller not found (PS/2 keyboard only)\n");
    else
        printk("[WARN] No USB HID boot keyboard found on xHCI root ports\n");
    return found;
}

/* ------------------------------------------------------------------ */
/* Опрос из таймерного прерывания (каждые 10 мс)                      */
/* ------------------------------------------------------------------ */
extern void usb_hid_report(const uint8_t *r);
extern void usb_hid_tick(void);

void usb_kbd_poll(void)
{
    if (!X.ready) return;

    struct trb e;
    while (event_get(&e)) {
        if (((e.d3 >> 10) & 0x3F) != TRB_EV_TRANSFER) continue;
        if (((e.d3 >> 24) & 0xFF) != X.kbd_slot) continue;
        if (((e.d3 >> 16) & 0x1F) != X.kbd_dci) continue;

        uint32_t cc = (e.d2 >> 24) & 0xFF;
        if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) {
            uint8_t rep[8];
            for (int i = 0; i < 8; i++) rep[i] = X.kbd_buf[i];
            usb_hid_report(rep);
        }
        /* Запрашиваем следующий отчёт */
        uint64_t b = (uint64_t)X.kbd_buf;
        ring_push(&X.kbd_ring, (uint32_t)b, (uint32_t)(b >> 32), 8,
                  TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
        ring_doorbell(X.kbd_slot, X.kbd_dci);
    }
    usb_hid_tick();
}
