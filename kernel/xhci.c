/*
 * kernel/xhci.c -- Драйвер xHCI (USB 3.x host controller), USB-хабов и
 * загрузочной USB HID клавиатуры (Stage 48).
 *
 * - Работает без прерываний (polling): события читаются из Event Ring либо
 *   синхронно при ожидании команд, либо из таймерного прерывания (usb_kbd_poll).
 * - Все DMA-структуры лежат ниже 1 ГБ (identity map): физический == виртуальный.
 * - Поддерживаются: несколько контроллеров, корневые порты, USB 2.0 хабы
 *   (до 5 уровней), горячее подключение/отключение (поток ядра), несколько
 *   клавиатур, светодиоды NumLock/CapsLock/ScrollLock.
 * - Синхронизация: на время операций с контроллером (перечисление, LED) поток
 *   берёт c->lock; опрос из IRQ0 в это время пропускается (одно CPU), а
 *   ожидающие функции сами разбирают события, включая отчёты клавиатур.
 */
#include <linux/xhci.h>
#include <linux/usb_hid.h>
#include <linux/pci.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/string.h>
#include <linux/kthread.h>
#include <asm/io.h>

/* ------------------------------------------------------------------ */
/* Константы                                                          */
/* ------------------------------------------------------------------ */
#define RING_TRBS        256            /* 256 * 16 = 4096 байт = страница */
#define MAX_CTRL         4
#define MAX_DEVS         48
#define MAX_PORTS        32
#define MAX_HUB_DEPTH    5

/* Типы TRB */
#define TRB_NORMAL        1
#define TRB_SETUP         2
#define TRB_DATA          3
#define TRB_STATUS        4
#define TRB_LINK          6
#define TRB_CMD_ENABLE_SLOT   9
#define TRB_CMD_DISABLE_SLOT  10
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

struct usb_dev;

struct xhci_ctrl {
    int      used;
    int      running;
    uint8_t  bus, slot, func;
    uint64_t mmio;
    uint16_t hci_version;
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
    uint8_t *buf;                 /* DMA-буфер для control-передач (страница) */

    volatile int lock;            /* поток ядра работает с контроллером */

    /* Завершения, на которые ждут синхронные функции */
    volatile int cmd_done;
    volatile uint32_t cmd_cc, cmd_slot;
    volatile int xfer_done;
    volatile uint32_t xfer_slot, xfer_dci, xfer_cc;

    struct usb_dev *slots[33];            /* slot id -> устройство */
    struct usb_dev *port_dev[MAX_PORTS + 1];
    uint8_t port_fail[MAX_PORTS + 1];
};

#define KIND_UNKNOWN 0
#define KIND_KEYBOARD 1
#define KIND_HUB 2

struct usb_dev {
    int used;
    struct xhci_ctrl *c;
    struct usb_dev *parent;
    struct usb_dev *child[16];
    uint32_t parent_port;
    uint32_t slot, speed, root_port;
    uint32_t route, depth;
    uint32_t tt_slot, tt_port;
    struct ring ep0;
    void *dev_ctx;
    void *in_ctx;
    uint16_t vid, pid;
    uint8_t  dclass;
    uint8_t  kind;
    uint8_t  failed;
    char     ifaces[40];       /* "3/1/1 3/1/2": класс/подкласс/протокол */
    /* USB hub */
    uint32_t hub_ports;
    uint8_t  hub_fail[16];
    /* Кандидаты HID-интерфейсов (не boot-клавиатура/мышь): для report-protocol */
    struct { uint8_t iface, ep, interval; uint16_t mps, rlen; } cand[4];
    uint8_t  ncand;
    uint8_t  configured;
    /* Клавиатура */
    uint8_t  report_mode;      /* 1: report protocol (разбор по дескриптору) */
    struct hid_layout lay;
    uint32_t klen;             /* длина запроса Interrupt IN */
    uint8_t  iface;
    uint32_t dci;
    struct ring kring;
    volatile uint8_t *kbuf;
    struct hid_state hid;
    uint32_t kerrors;
    volatile int kdead;
};

static struct xhci_ctrl C[MAX_CTRL];
static int nctrl;
static struct usb_dev DEVS[MAX_DEVS];
static struct xhci_info x_info;
static int n_keyboards;

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

static void ctrl_lock(struct xhci_ctrl *c)
{
    uint64_t fl;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(fl) : : "memory");
    c->lock = 1;
    if (fl & 0x200) __asm__ volatile ("sti" : : : "memory");
}

static void ctrl_unlock(struct xhci_ctrl *c)
{
    mb();
    c->lock = 0;
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

static inline void ring_doorbell(struct xhci_ctrl *c, uint32_t slot, uint32_t target)
{
    mb();
    c->db[slot] = target;
}

/* ------------------------------------------------------------------ */
/* События                                                            */
/* ------------------------------------------------------------------ */
static int event_get(struct xhci_ctrl *c, struct trb *out)
{
    struct trb *t = &c->evt[c->evt_deq];
    uint32_t cw = t->d3;
    if ((cw & TRB_C) != c->evt_cycle) return 0;
    mb();
    out->d0 = t->d0; out->d1 = t->d1; out->d2 = t->d2; out->d3 = cw;

    c->evt_deq++;
    if (c->evt_deq == RING_TRBS) {
        c->evt_deq = 0;
        c->evt_cycle ^= 1;
    }
    /* ERDP (интеррапер 0): адрес следующего TRB + бит EHB (3) */
    uint64_t erdp = (uint64_t)&c->evt[c->evt_deq] | (1U << 3);
    wr32(c->rt, 0x38, (uint32_t)erdp);
    wr32(c->rt, 0x3C, (uint32_t)(erdp >> 32));
    return 1;
}

static void kbd_queue(struct usb_dev *d)
{
    uint64_t b = (uint64_t)d->kbuf;
    ring_push(&d->kring, (uint32_t)b, (uint32_t)(b >> 32), d->klen,
              TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    ring_doorbell(d->c, d->slot, d->dci);
}

static void kbd_event(struct usb_dev *d, uint32_t cc)
{
    if (d->kdead) return;
    if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) {
        uint8_t rep[8];
        d->kerrors = 0;
        if (d->report_mode) {
            if (hid_to_boot_report(&d->lay, d->kbuf, d->klen, rep)) usb_hid_report(&d->hid, rep);
        } else {
            for (int i = 0; i < 8; i++) rep[i] = d->kbuf[i];
            usb_hid_report(&d->hid, rep);
        }
    } else if (++d->kerrors > 64) {
        d->kdead = 1;                    /* поток ядра отключит устройство */
        return;
    }
    kbd_queue(d);
}

/* Разбирает все накопленные события контроллера */
static void ctrl_poll_events(struct xhci_ctrl *c)
{
    struct trb e;
    while (event_get(c, &e)) {
        uint32_t type = (e.d3 >> 10) & 0x3F;
        if (type == TRB_EV_CMD_COMPLETE) {
            c->cmd_cc = (e.d2 >> 24) & 0xFF;
            c->cmd_slot = (e.d3 >> 24) & 0xFF;
            c->cmd_done = 1;
        } else if (type == TRB_EV_TRANSFER) {
            uint32_t slot = (e.d3 >> 24) & 0xFF;
            uint32_t dci = (e.d3 >> 16) & 0x1F;
            uint32_t cc = (e.d2 >> 24) & 0xFF;
            struct usb_dev *d = slot <= 32 ? c->slots[slot] : NULL;
            if (d && d->kind == KIND_KEYBOARD && dci == d->dci) {
                kbd_event(d, cc);
            } else {
                c->xfer_slot = slot; c->xfer_dci = dci; c->xfer_cc = cc;
                c->xfer_done = 1;
            }
        }
        /* Port Status Change и прочее: хот-плаг опрашивается потоком */
    }
}

/* Команда: возвращает код завершения (CC) или -1 по таймауту */
static int command(struct xhci_ctrl *c, uint32_t d0, uint32_t d1, uint32_t ctrl, uint32_t *slot_out)
{
    c->cmd_done = 0;
    ring_push(&c->cmd, d0, d1, 0, ctrl);
    ring_doorbell(c, 0, 0);
    for (uint32_t i = 0; i < 500 * 20; i++) {
        ctrl_poll_events(c);
        if (c->cmd_done) {
            if (slot_out) *slot_out = c->cmd_slot;
            return (int)c->cmd_cc;
        }
        udelay(50);
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Контексты и control-передачи                                       */
/* ------------------------------------------------------------------ */
static inline uint32_t *ctx_at(struct xhci_ctrl *c, void *base, uint32_t index)
{
    return (uint32_t *)((uint8_t *)base + (uint64_t)index * c->ctx_size);
}

/* Control-передача по EP0. dir_in: 1 = данные от устройства. Возвращает 0 при успехе */
static int control_xfer(struct usb_dev *d, uint8_t bm, uint8_t req, uint16_t val,
                        uint16_t idx, uint16_t len, void *buf, int dir_in)
{
    struct xhci_ctrl *c = d->c;
    uint32_t trt = len ? (dir_in ? 3 : 2) : 0;

    c->xfer_done = 0;
    ring_push(&d->ep0,
              (uint32_t)bm | ((uint32_t)req << 8) | ((uint32_t)val << 16),
              (uint32_t)idx | ((uint32_t)len << 16),
              8,
              TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));
    if (len) {
        uint64_t p = (uint64_t)buf;
        ring_push(&d->ep0, (uint32_t)p, (uint32_t)(p >> 32), len,
                  TRB_TYPE(TRB_DATA) | (dir_in ? (1U << 16) : 0));
    }
    /* Status stage: направление противоположно данным (для no-data -- IN) */
    ring_push(&d->ep0, 0, 0, 0,
              TRB_TYPE(TRB_STATUS) | TRB_IOC | ((len && dir_in) ? 0 : (1U << 16)));
    ring_doorbell(c, d->slot, 1);

    for (uint32_t i = 0; i < 1000 * 20; i++) {
        ctrl_poll_events(c);
        if (c->xfer_done && c->xfer_slot == d->slot && c->xfer_dci == 1) {
            uint32_t cc = c->xfer_cc;
            return (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) ? 0 : -(int)cc;
        }
        udelay(50);
    }
    return -1000;
}

static int get_descriptor(struct usb_dev *d, uint8_t type, uint8_t index, uint16_t len)
{
    memset(d->c->buf, 0, len);
    return control_xfer(d, 0x80, 6, (uint16_t)((type << 8) | index), 0, len, d->c->buf, 1);
}

/* ------------------------------------------------------------------ */
/* Инициализация контроллера                                          */
/* ------------------------------------------------------------------ */
static void bios_handoff(struct xhci_ctrl *c, uint32_t hccparams1)
{
    uint32_t off = ((hccparams1 >> 16) & 0xFFFF) * 4;
    for (int guard = 0; off && guard < 64; guard++) {
        uint32_t cap = rd32(c->cap, off);
        if ((cap & 0xFF) == 1) {            /* USB Legacy Support */
            if (cap & (1U << 16)) {         /* BIOS Owned */
                wr32(c->cap, off, cap | (1U << 24));   /* OS Owned */
                for (int t = 0; t < 1000; t++) {
                    if (!(rd32(c->cap, off) & (1U << 16))) break;
                    mdelay(1);
                }
                if (rd32(c->cap, off) & (1U << 16))
                    printk("[USB] BIOS did not release xHCI (continuing anyway)\n");
                else
                    printk("[OK] USB: xHCI ownership taken from BIOS/SMM\n");
            }
            /* Отключаем SMI от xHCI и сбрасываем события (как в Linux) */
            uint32_t ctl = rd32(c->cap, off + 4);
            ctl &= (0x3U << 1) | (0xFFU << 5) | (0x7U << 17);
            ctl |= (0x7U << 29);
            wr32(c->cap, off + 4, ctl);
        }
        uint32_t next = (cap >> 8) & 0xFF;
        if (!next) break;
        off += next * 4;
    }
}

static int controller_start(struct xhci_ctrl *c)
{
    uint32_t hcs1 = rd32(c->cap, 4);
    uint32_t hcs2 = rd32(c->cap, 8);
    uint32_t hcc1 = rd32(c->cap, 0x10);
    c->max_slots = hcs1 & 0xFF;
    c->max_ports = (hcs1 >> 24) & 0xFF;
    if (c->max_ports > MAX_PORTS) c->max_ports = MAX_PORTS;
    c->ctx_size  = (hcc1 & (1U << 2)) ? 64 : 32;
    uint32_t caplen = c->cap[0];
    c->op = c->cap + caplen;
    c->rt = c->cap + (rd32(c->cap, 0x18) & ~0x1FU);
    c->db = (volatile uint32_t *)(c->cap + (rd32(c->cap, 0x14) & ~0x3U));
    c->buf = (uint8_t *)dma_page();

    bios_handoff(c, hcc1);

    /* Остановка и сброс контроллера */
    wr32(c->op, 0x00, rd32(c->op, 0x00) & ~1U);
    for (int t = 0; t < 100 && !(rd32(c->op, 0x04) & 1U); t++) mdelay(1);
    wr32(c->op, 0x00, rd32(c->op, 0x00) | (1U << 1));            /* HCRST */
    for (int t = 0; t < 500 && (rd32(c->op, 0x00) & (1U << 1)); t++) mdelay(1);
    for (int t = 0; t < 500 && (rd32(c->op, 0x04) & (1U << 11)); t++) mdelay(1);
    if (rd32(c->op, 0x04) & (1U << 11)) {
        printk("[USB] xHCI: controller not ready after reset\n");
        return -1;
    }

    if (c->max_slots > 32) c->max_slots = 32;
    wr32(c->op, 0x38, c->max_slots);                             /* CONFIG */

    /* DCBAA + scratchpad */
    c->dcbaa = (uint64_t *)dma_page();
    uint32_t spb = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);
    if (spb) {
        uint64_t *sp = (uint64_t *)dma_page();
        if (spb > 512) spb = 512;
        for (uint32_t i = 0; i < spb; i++) sp[i] = (uint64_t)dma_page();
        c->dcbaa[0] = (uint64_t)sp;
    }
    wr32(c->op, 0x30, (uint32_t)(uint64_t)c->dcbaa);
    wr32(c->op, 0x34, (uint32_t)((uint64_t)c->dcbaa >> 32));

    /* Command ring */
    ring_init(&c->cmd);
    uint64_t crcr = (uint64_t)c->cmd.trbs | 1;                   /* RCS = 1 */
    wr32(c->op, 0x18, (uint32_t)crcr);
    wr32(c->op, 0x1C, (uint32_t)(crcr >> 32));

    /* Event ring: один сегмент */
    c->evt = (struct trb *)dma_page();
    c->evt_deq = 0;
    c->evt_cycle = 1;
    uint64_t *erst = (uint64_t *)dma_page();
    erst[0] = (uint64_t)c->evt;
    erst[1] = RING_TRBS;
    wr32(c->rt, 0x28, 1);                                        /* ERSTSZ */
    wr32(c->rt, 0x38, (uint32_t)(uint64_t)c->evt);               /* ERDP */
    wr32(c->rt, 0x3C, (uint32_t)((uint64_t)c->evt >> 32));
    wr32(c->rt, 0x30, (uint32_t)(uint64_t)erst);                 /* ERSTBA (последним) */
    wr32(c->rt, 0x34, (uint32_t)((uint64_t)erst >> 32));

    /* Запуск (INTE = 0: работаем опросом) */
    wr32(c->op, 0x00, 1U);
    for (int t = 0; t < 100 && (rd32(c->op, 0x04) & 1U); t++) mdelay(1);
    if (rd32(c->op, 0x04) & 1U) {
        printk("[USB] xHCI: controller failed to start\n");
        return -1;
    }
    mdelay(20);
    c->running = 1;
    return 0;
}

/* Сброс корневого порта; возвращает скорость (1..4) или 0 если устройства нет */
static int port_reset(struct xhci_ctrl *c, uint32_t port)
{
    uint32_t off = 0x400 + (port - 1) * 0x10;
    uint32_t sc = rd32(c->op, off);

    if (!(sc & PORTSC_PP)) {
        wr32(c->op, off, (sc & PORTSC_PRESERVE) | PORTSC_PP);
        mdelay(20);
        sc = rd32(c->op, off);
    }
    if (!(sc & PORTSC_CCS)) return 0;

    /* Сбрасываем накопленные флаги изменений */
    wr32(c->op, off, (sc & PORTSC_PRESERVE) | (sc & PORTSC_CHANGE_BITS));

    if (!(sc & PORTSC_PED)) {
        /* USB 2.0: порт включается только после reset (USB 3.0 уже в U0) */
        wr32(c->op, off, (rd32(c->op, off) & PORTSC_PRESERVE) | PORTSC_PR);
        for (int t = 0; t < 500; t++) {
            sc = rd32(c->op, off);
            if (sc & PORTSC_PRC) break;
            mdelay(1);
        }
        wr32(c->op, off, (sc & PORTSC_PRESERVE) | PORTSC_PRC | PORTSC_CSC | PORTSC_PEC);
        mdelay(20);
    }
    sc = rd32(c->op, off);
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

static void fill_slot_ctx(struct usb_dev *d, uint32_t *slot, uint32_t entries)
{
    slot[0] = d->route | (d->speed << 20) | (entries << 27);
    slot[1] = (d->root_port << 16);
    slot[2] = d->tt_slot | (d->tt_port << 8);
    if (d->kind == KIND_HUB) {
        slot[0] |= (1U << 26);                   /* Hub */
        slot[1] |= (d->hub_ports << 24);         /* Number of Ports */
    }
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

static struct usb_dev *dev_alloc(void)
{
    for (int i = 0; i < MAX_DEVS; i++) {
        if (!DEVS[i].used) {
            memset(&DEVS[i], 0, sizeof(DEVS[i]));
            DEVS[i].used = 1;
            return &DEVS[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Удаление устройств                                                 */
/* ------------------------------------------------------------------ */
static void dev_remove(struct usb_dev *d)
{
    struct xhci_ctrl *c = d->c;
    for (int i = 0; i < 16; i++) {
        if (d->child[i]) { dev_remove(d->child[i]); d->child[i] = NULL; }
    }
    if (d->kind == KIND_KEYBOARD) {
        d->kind = KIND_UNKNOWN;
        usb_hid_register(&d->hid, 0);
        if (n_keyboards > 0) n_keyboards--;
    }
    if (d->slot && d->slot <= 32) {
        command(c, 0, 0, TRB_TYPE(TRB_CMD_DISABLE_SLOT) | (d->slot << 24), NULL);
        c->dcbaa[d->slot] = 0;
        c->slots[d->slot] = NULL;
    }
    printk("[USB] Device %x:%x removed\n", (long)d->vid, (long)d->pid);
    d->used = 0;
}

/* ------------------------------------------------------------------ */
/* Перечисление устройства                                            */
/* ------------------------------------------------------------------ */
static int update_ep0_mps(struct usb_dev *d, uint32_t mps)
{
    memset(d->in_ctx, 0, 4096);
    uint32_t *ic = (uint32_t *)d->in_ctx;
    ic[1] = 2;                                   /* Add: EP0 */
    uint32_t *ep = ctx_at(d->c, d->in_ctx, 2);
    ep[1] = (3U << 1) | (4U << 3) | (mps << 16);
    uint64_t p = (uint64_t)d->in_ctx;
    return command(d->c, (uint32_t)p, (uint32_t)(p >> 32),
                   TRB_TYPE(TRB_CMD_EVAL_CTX) | (d->slot << 24), NULL);
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

/* SET_CONFIGURATION (один раз на устройство) */
static int set_config(struct usb_dev *d, uint8_t cfg_value)
{
    if (d->configured) return 0;
    if (control_xfer(d, 0x00, 9, cfg_value, 0, 0, NULL, 0) < 0) {       /* SET_CONFIGURATION */
        printk("[USB] %x:%x SET_CONFIGURATION failed\n", (long)d->vid, (long)d->pid);
        return -1;
    }
    d->configured = 1;
    return 0;
}

/* Настройка interrupt IN endpoint клавиатуры. lay == NULL: boot-протокол */
static int keyboard_start(struct usb_dev *d, uint8_t iface, uint8_t ep_addr, uint16_t ep_mps,
                          uint8_t interval, const struct hid_layout *lay)
{
    struct xhci_ctrl *c = d->c;
    if (lay) {
        control_xfer(d, 0x21, 0x0B, 1, iface, 0, NULL, 0);              /* SET_PROTOCOL: report */
        d->lay = *lay;
        d->report_mode = 1;
        d->klen = lay->bytes > 64 ? 64 : lay->bytes;
    } else {
        control_xfer(d, 0x21, 0x0B, 0, iface, 0, NULL, 0);              /* SET_PROTOCOL: boot */
        d->report_mode = 0;
        d->klen = 8;
    }
    control_xfer(d, 0x21, 0x0A, 0, iface, 0, NULL, 0);                  /* SET_IDLE: 0 (только изменения) */

    uint32_t dci = (uint32_t)(ep_addr & 0x0F) * 2 + 1;
    ring_init(&d->kring);
    d->kbuf = (volatile uint8_t *)dma_page();

    memset(d->in_ctx, 0, 4096);
    uint32_t *ic = (uint32_t *)d->in_ctx;
    ic[1] = 1 | (1U << dci);                                            /* Add: Slot + EP */
    fill_slot_ctx(d, ctx_at(c, d->in_ctx, 1), dci);
    fill_ep_ctx(ctx_at(c, d->in_ctx, 1 + dci), 7, ep_mps, xhci_interval(d->speed, interval),
                &d->kring, d->klen, ep_mps);

    uint64_t p = (uint64_t)d->in_ctx;
    int cc = command(c, (uint32_t)p, (uint32_t)(p >> 32),
                     TRB_TYPE(TRB_CMD_CONFIG_EP) | (d->slot << 24), NULL);
    if (cc != CC_SUCCESS) {
        printk("[USB] %x:%x Configure Endpoint failed (cc=%d)\n", (long)d->vid, (long)d->pid, (long)cc);
        return -1;
    }
    d->iface = iface;
    d->dci = dci;
    d->kind = KIND_KEYBOARD;
    usb_hid_register(&d->hid, 1);
    n_keyboards++;
    kbd_queue(d);
    return 0;
}

/* Клавиатура без boot-протокола: читаем HID report descriptor каждого кандидата */
static int hid_report_keyboard(struct usb_dev *d, uint8_t cfg_value)
{
    struct xhci_ctrl *c = d->c;
    if (set_config(d, cfg_value) < 0) return -1;
    for (int i = 0; i < d->ncand; i++) {
        uint32_t rlen = d->cand[i].rlen;
        if (!d->cand[i].ep || rlen < 4) continue;
        if (rlen > 2048) rlen = 2048;
        memset(c->buf, 0, rlen);
        if (control_xfer(d, 0x81, 6, 0x2200, d->cand[i].iface, (uint16_t)rlen, c->buf, 1) < 0) {
            printk("[USB] %x:%x HID iface %d: cannot read report descriptor\n",
                   (long)d->vid, (long)d->pid, (long)d->cand[i].iface);
            continue;
        }
        struct hid_layout lay;
        if (!hid_parse_report_desc(c->buf, rlen, &lay)) continue;   /* мышь, мультимедиа и т.п. */
        if (keyboard_start(d, d->cand[i].iface, d->cand[i].ep, d->cand[i].mps,
                           d->cand[i].interval, &lay) == 0) {
            printk("[OK] USB HID Report-protocol Keyboard ready (iface %d, ep 0x%x, report %d bytes)\n",
                   (long)d->cand[i].iface, (long)d->cand[i].ep, (long)lay.bytes);
            return 0;
        }
    }
    return -1;
}

static void scan_hub(struct usb_dev *hub);

/* Настройка USB 2.0 хаба: питание портов, поиск устройств */
static int hub_start(struct usb_dev *d, uint8_t cfg_value)
{
    struct xhci_ctrl *c = d->c;
    int ss = (d->speed == 4);
    if (d->depth >= MAX_HUB_DEPTH) {
        printk("[USB] Hub nesting too deep\n");
        return -1;
    }
    if (set_config(d, cfg_value) < 0) return -1;
    memset(c->buf, 0, 16);
    if (ss) {
        /* USB 3.x хаб: SET_HUB_DEPTH, затем SuperSpeed Hub Descriptor (0x2A) */
        control_xfer(d, 0x20, 12, (uint16_t)d->depth, 0, 0, NULL, 0);
    }
    if (control_xfer(d, 0xA0, 6, ss ? 0x2A00 : 0x2900, 0, ss ? 12 : 9, c->buf, 1) < 0) { /* GET_HUB_DESCRIPTOR */
        printk("[USB] Hub descriptor read failed\n");
        return -1;
    }
    uint32_t nports = c->buf[2];
    uint32_t pwr_good = c->buf[5];
    if (nports == 0 || nports > 15) {
        printk("[USB] Hub reports %d ports -- unsupported\n", (long)nports);
        return -1;
    }
    d->kind = KIND_HUB;
    d->hub_ports = nports;

    /* Configure Endpoint: обновляем Slot Context (Hub = 1, число портов) */
    memset(d->in_ctx, 0, 4096);
    uint32_t *ic = (uint32_t *)d->in_ctx;
    ic[1] = 1;                                                           /* Add: Slot */
    fill_slot_ctx(d, ctx_at(c, d->in_ctx, 1), 1);
    uint64_t p = (uint64_t)d->in_ctx;
    int cc = command(c, (uint32_t)p, (uint32_t)(p >> 32),
                     TRB_TYPE(TRB_CMD_CONFIG_EP) | (d->slot << 24), NULL);
    if (cc != CC_SUCCESS) {
        printk("[USB] Hub Configure Endpoint failed (cc=%d)\n", (long)cc);
        d->kind = KIND_UNKNOWN;
        return -1;
    }

    printk("[OK] USB %s Hub: %d ports\n", ss ? "3.x SuperSpeed" : "2.0", (long)nports);
    for (uint32_t pn = 1; pn <= nports; pn++)
        control_xfer(d, 0x23, 3, 8, (uint16_t)pn, 0, NULL, 0);          /* SET_FEATURE PORT_POWER */
    uint32_t wait = pwr_good * 2;
    mdelay(wait < 100 ? 100 : wait);
    scan_hub(d);
    return 0;
}

static struct usb_dev *enumerate_device(struct xhci_ctrl *c, struct usb_dev *parent,
                                        uint32_t port, uint32_t speed);

/* Состояние порта хаба: возвращает 0 и заполняет status/change */
static int hub_port_status(struct usb_dev *hub, uint32_t port, uint32_t *st, uint32_t *ch)
{
    struct xhci_ctrl *c = hub->c;
    if (control_xfer(hub, 0xA3, 0, 0, (uint16_t)port, 4, c->buf, 1) < 0) return -1;
    *st = c->buf[0] | ((uint32_t)c->buf[1] << 8);
    *ch = c->buf[2] | ((uint32_t)c->buf[3] << 8);
    return 0;
}

static void hub_port_check(struct usb_dev *hub, uint32_t port)
{
    uint32_t st, ch;
    if (hub_port_status(hub, port, &st, &ch) < 0) return;

    int ss = (hub->speed == 4);
    if (ch & 1) control_xfer(hub, 0x23, 1, 16, (uint16_t)port, 0, NULL, 0); /* C_PORT_CONNECTION */
    if (ss && (ch & (1U << 5))) control_xfer(hub, 0x23, 1, 29, (uint16_t)port, 0, NULL, 0); /* C_BH_PORT_RESET */
    if (ss && (ch & (1U << 6))) control_xfer(hub, 0x23, 1, 25, (uint16_t)port, 0, NULL, 0); /* C_PORT_LINK_STATE */

    struct usb_dev *child = hub->child[port];
    if (!(st & 1)) {                                   /* ничего не подключено */
        if (child) { dev_remove(child); hub->child[port] = NULL; }
        hub->hub_fail[port] = 0;
        return;
    }
    if (child || hub->hub_fail[port]) return;

    /* Сброс порта хаба */
    if (ss && ((st >> 5) & 0xF) == 6)                                   /* SS.Inactive: нужен warm reset */
        control_xfer(hub, 0x23, 3, 28, (uint16_t)port, 0, NULL, 0);     /* SET_FEATURE BH_PORT_RESET */
    else
        control_xfer(hub, 0x23, 3, 4, (uint16_t)port, 0, NULL, 0);      /* SET_FEATURE PORT_RESET */
    for (int t = 0; t < 100; t++) {
        mdelay(10);
        if (hub_port_status(hub, port, &st, &ch) < 0) break;
        if (ch & ((1U << 4) | (ss ? (1U << 5) : 0))) break;             /* C_PORT_RESET / C_BH_PORT_RESET */
    }
    control_xfer(hub, 0x23, 1, 20, (uint16_t)port, 0, NULL, 0);         /* CLEAR C_PORT_RESET */
    if (ss) control_xfer(hub, 0x23, 1, 29, (uint16_t)port, 0, NULL, 0); /* CLEAR C_BH_PORT_RESET */
    if (hub_port_status(hub, port, &st, &ch) < 0 || !(st & 2)) {
        printk("[USB] Hub port %d: reset failed\n", (long)port);
        hub->hub_fail[port] = 1;
        return;
    }
    uint32_t speed = ss ? 4 : ((st & (1U << 9)) ? 2 : ((st & (1U << 10)) ? 3 : 1));
    mdelay(20);
    struct usb_dev *nd = enumerate_device(hub->c, hub, port, speed);
    if (nd) hub->child[port] = nd; else hub->hub_fail[port] = 1;
}

static void scan_hub(struct usb_dev *hub)
{
    for (uint32_t p = 1; p <= hub->hub_ports && p < 16; p++)
        hub_port_check(hub, p);
}

/* Разбор конфигурационного дескриптора. Возвращает 1, если найден boot keyboard (3/1/1);
 * остальные HID-интерфейсы (кроме мыши) попадают в d->cand для разбора report descriptor */
static int parse_config(struct usb_dev *d, const uint8_t *cfg, uint32_t total,
                        uint8_t *iface, uint8_t *ep_addr, uint16_t *mps, uint8_t *interval)
{
    int in_kbd = 0, found = 0, n = 0, cur = -1;
    char *o = d->ifaces;
    d->ncand = 0;
    for (uint32_t i = 0; i + 2 <= total && cfg[i] >= 2; i += cfg[i]) {
        uint8_t type = cfg[i + 1];
        if (type == 4 && i + 9 <= total) {
            in_kbd = (cfg[i + 5] == 3 && cfg[i + 6] == 1 && cfg[i + 7] == 1);
            if (in_kbd && !found) *iface = cfg[i + 2];
            cur = -1;
            if (cfg[i + 5] == 3 && !in_kbd && cfg[i + 7] != 2 && d->ncand < 4 && cfg[i + 3] == 0) {
                cur = d->ncand++;
                d->cand[cur].iface = cfg[i + 2];
                d->cand[cur].ep = 0;
                d->cand[cur].rlen = 0;
            }
            if (n < 4) {                                  /* "3/1/1 " */
                if (n) *o++ = ' ';
                *o++ = (char)('0' + (cfg[i + 5] % 10));
                *o++ = '/';
                *o++ = (char)('0' + (cfg[i + 6] % 10));
                *o++ = '/';
                *o++ = (char)('0' + (cfg[i + 7] % 10));
                n++;
            }
        } else if (type == 0x21 && cur >= 0 && cfg[i] >= 9) {  /* HID descriptor */
            for (uint32_t k = 6; k + 3 <= cfg[i] && i + k + 3 <= total; k += 3) {
                if (cfg[i + k] == 0x22)
                    d->cand[cur].rlen = (uint16_t)(cfg[i + k + 1] | (cfg[i + k + 2] << 8));
            }
        } else if (type == 5 && i + 7 <= total) {
            if ((cfg[i + 2] & 0x80) && (cfg[i + 3] & 3) == 3) {   /* Interrupt IN */
                if (in_kbd && !found) {
                    *ep_addr = cfg[i + 2];
                    *mps = (uint16_t)(cfg[i + 4] | (cfg[i + 5] << 8)) & 0x7FF;
                    *interval = cfg[i + 6];
                    found = 1;
                } else if (cur >= 0 && !d->cand[cur].ep) {
                    d->cand[cur].ep = cfg[i + 2];
                    d->cand[cur].mps = (uint16_t)(cfg[i + 4] | (cfg[i + 5] << 8)) & 0x7FF;
                    d->cand[cur].interval = cfg[i + 6];
                }
            }
        }
    }
    *o = '\0';
    return found;
}

/* Поднимает слот и настраивает устройство. NULL -- слот получить не удалось */
static struct usb_dev *enumerate_device(struct xhci_ctrl *c, struct usb_dev *parent,
                                        uint32_t port, uint32_t speed)
{
    struct usb_dev *d = dev_alloc();
    if (!d) { printk("[USB] Too many USB devices\n"); return NULL; }
    d->c = c;
    d->parent = parent;
    d->parent_port = port;
    d->speed = speed;
    d->root_port = parent ? parent->root_port : port;
    d->route = parent ? (parent->route | (port << (4 * parent->depth))) : 0;
    d->depth = parent ? parent->depth + 1 : 0;
    /* Transaction Translator: LS/FS устройства за HS-хабом */
    if (speed < 3 && parent) {
        if (parent->speed == 3) { d->tt_slot = parent->slot; d->tt_port = port; }
        else { d->tt_slot = parent->tt_slot; d->tt_port = parent->tt_port; }
    }

    uint32_t slot = 0;
    int cc = command(c, 0, 0, TRB_TYPE(TRB_CMD_ENABLE_SLOT), &slot);
    if (cc != CC_SUCCESS || !slot || slot > 32) {
        printk("[USB] Port %d: Enable Slot failed (cc=%d)\n", (long)port, (long)cc);
        d->used = 0;
        return NULL;
    }
    d->slot = slot;
    c->slots[slot] = d;
    d->dev_ctx = dma_page();
    d->in_ctx  = dma_page();
    ring_init(&d->ep0);
    c->dcbaa[slot] = (uint64_t)d->dev_ctx;

    uint32_t *ic = (uint32_t *)d->in_ctx;
    ic[1] = 3;                                   /* Add: Slot + EP0 */
    fill_slot_ctx(d, ctx_at(c, d->in_ctx, 1), 1);
    fill_ep_ctx(ctx_at(c, d->in_ctx, 2), 4, ep0_default_mps(speed), 0, &d->ep0, 8, 0);

    uint64_t p = (uint64_t)d->in_ctx;
    cc = command(c, (uint32_t)p, (uint32_t)(p >> 32),
                 TRB_TYPE(TRB_CMD_ADDRESS_DEV) | (slot << 24), NULL);
    if (cc != CC_SUCCESS) {
        printk("[USB] Port %d: Address Device failed (cc=%d)\n", (long)port, (long)cc);
        d->failed = 1;
        return d;
    }
    mdelay(10);                                  /* recovery после SET_ADDRESS */

    /* Первые 8 байт дескриптора -> реальный размер пакета EP0 */
    int r = get_descriptor(d, 1, 0, 8);
    if (r < 0) {                                 /* одна повторная попытка */
        mdelay(20);
        r = get_descriptor(d, 1, 0, 8);
    }
    if (r < 0) {
        printk("[USB] Port %d: cannot read device descriptor (err %d)\n", (long)port, (long)r);
        d->failed = 1;
        return d;
    }
    uint32_t mps = c->buf[7];
    if (c->buf[3] >= 3) mps = 1U << mps;         /* bcdUSB >= 3.0: степень двойки */
    if (mps && mps != ep0_default_mps(speed)) {
        if (update_ep0_mps(d, mps) != CC_SUCCESS)
            printk("[USB] Port %d: Evaluate Context failed\n", (long)port);
    }
    if (get_descriptor(d, 1, 0, 18) < 0) {
        printk("[USB] Port %d: cannot read full device descriptor\n", (long)port);
        d->failed = 1;
        return d;
    }
    d->dclass = c->buf[4];
    d->vid = (uint16_t)(c->buf[8] | (c->buf[9] << 8));
    d->pid = (uint16_t)(c->buf[10] | (c->buf[11] << 8));

    /* Конфигурация: сначала заголовок, затем целиком */
    if (get_descriptor(d, 2, 0, 9) < 0) {
        printk("[USB] %x:%x cannot read config descriptor\n", (long)d->vid, (long)d->pid);
        d->failed = 1;
        return d;
    }
    uint32_t total = c->buf[2] | (c->buf[3] << 8);
    if (total > 1024) total = 1024;
    if (total < 9) total = 9;
    if (get_descriptor(d, 2, 0, (uint16_t)total) < 0) {
        printk("[USB] %x:%x cannot read full config descriptor\n", (long)d->vid, (long)d->pid);
        d->failed = 1;
        return d;
    }
    uint8_t cfg_value = c->buf[5];

    uint8_t iface = 0, ep_addr = 0, interval = 0;
    uint16_t ep_mps = 0;
    int is_kbd = parse_config(d, c->buf, total, &iface, &ep_addr, &ep_mps, &interval);

    printk("[OK] USB %d:%d port %d: %x:%x class %d speed %d ifaces [%s]\n",
           (long)c->bus, (long)c->slot, (long)port, (long)d->vid, (long)d->pid,
           (long)d->dclass, (long)speed, d->ifaces);

    if (d->dclass == 9) {
        hub_start(d, cfg_value);
    } else if (is_kbd) {
        if (set_config(d, cfg_value) == 0 &&
            keyboard_start(d, iface, ep_addr, ep_mps, interval, NULL) == 0) {
            printk("[OK] USB HID Boot Keyboard ready (iface %d, ep 0x%x)\n",
                   (long)iface, (long)ep_addr);
        }
    } else if (d->ncand) {
        hid_report_keyboard(d, cfg_value);
    }
    return d;
}

/* ------------------------------------------------------------------ */
/* Корневые порты                                                     */
/* ------------------------------------------------------------------ */
static void root_port_check(struct xhci_ctrl *c, uint32_t port)
{
    uint32_t off = 0x400 + (port - 1) * 0x10;
    uint32_t sc = rd32(c->op, off);
    if (sc & PORTSC_CHANGE_BITS)
        wr32(c->op, off, (sc & PORTSC_PRESERVE) | (sc & PORTSC_CHANGE_BITS));

    if (!(sc & PORTSC_CCS)) {
        if (c->port_dev[port]) { dev_remove(c->port_dev[port]); c->port_dev[port] = NULL; }
        c->port_fail[port] = 0;
        return;
    }
    if (c->port_dev[port] || c->port_fail[port]) return;

    int speed = port_reset(c, port);
    if (!speed) {
        printk("[USB] %d:%d port %d: device present but port did not enable\n",
               (long)c->bus, (long)c->slot, (long)port);
        c->port_fail[port] = 1;
        return;
    }
    struct usb_dev *d = enumerate_device(c, NULL, port, (uint32_t)speed);
    if (d) c->port_dev[port] = d; else c->port_fail[port] = 1;
}

static int init_controller(const struct pci_device *pd)
{
    if (nctrl >= MAX_CTRL) return 0;
    uint32_t bar0 = pci_read_dword(pd->bus, pd->slot, pd->func, 0x10);
    uint64_t addr = bar0 & ~0xFULL;
    if ((bar0 & 0x6) == 0x4)
        addr |= (uint64_t)pci_read_dword(pd->bus, pd->slot, pd->func, 0x14) << 32;
    if (!addr) {
        printk("[USB] xHCI %d:%d has no BAR\n", (long)pd->bus, (long)pd->slot);
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

    struct xhci_ctrl *c = &C[nctrl];
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->bus = pd->bus; c->slot = pd->slot; c->func = pd->func;
    c->mmio = addr;
    c->cap = (volatile uint8_t *)addr;
    c->hci_version = (uint16_t)(rd32(c->cap, 0) >> 16);
    printk("[OK] USB: xHCI controller %d:%d, MMIO %p, v%x\n",
           (long)pd->bus, (long)pd->slot, addr, (long)c->hci_version);

    if (controller_start(c) < 0) { c->used = 0; return 0; }
    nctrl++;

    if (!x_info.present) {
        x_info.present = 1;
        x_info.mmio_base = addr;
        x_info.cap_length = c->cap[0];
        x_info.hci_version = c->hci_version;
        x_info.max_slots = (uint8_t)c->max_slots;
        x_info.max_ports = (uint8_t)(c->max_ports > 16 ? 16 : c->max_ports);
    }

    /* Некоторые контроллеры стартуют с выключенным питанием портов (PPC) */
    for (uint32_t port = 1; port <= c->max_ports; port++) {
        uint32_t off = 0x400 + (port - 1) * 0x10;
        uint32_t sc = rd32(c->op, off);
        if (!(sc & PORTSC_PP)) wr32(c->op, off, (sc & PORTSC_PRESERVE) | PORTSC_PP);
    }
    mdelay(120);                       /* debounce + link training USB 3.0 */

    ctrl_lock(c);
    for (uint32_t port = 1; port <= c->max_ports; port++) {
        uint32_t sc = rd32(c->op, 0x400 + (port - 1) * 0x10);
        if (sc & PORTSC_CCS) x_info.active_ports_count++;
        root_port_check(c, port);
    }
    ctrl_unlock(c);
    return 1;
}

int xhci_init(void)
{
    memset(&x_info, 0, sizeof(x_info));
    nctrl = 0;
    n_keyboards = 0;
    int n = pci_get_device_count();

    for (int i = 0; i < n; i++) {
        const struct pci_device *pd = pci_get_device(i);
        if (!pd || pd->class_code != 0x0C || pd->subclass != 0x03 || pd->prog_if != 0x30)
            continue;
        init_controller(pd);
    }

    if (n_keyboards)
        printk("[OK] USB: %d keyboard(s) active (xHCI polling, boot protocol)\n", (long)n_keyboards);
    else if (!nctrl)
        printk("[WARN] xHCI USB Controller not found (PS/2 keyboard only)\n");
    else
        printk("[WARN] No USB HID boot keyboard found yet (hot-plug is monitored)\n");
    return n_keyboards;
}

/* ------------------------------------------------------------------ */
/* Опрос из таймерного прерывания (каждые 10 мс)                      */
/* ------------------------------------------------------------------ */
void usb_kbd_poll(void)
{
    for (int i = 0; i < nctrl; i++) {
        struct xhci_ctrl *c = &C[i];
        if (c->running && !c->lock) ctrl_poll_events(c);
    }
    usb_hid_tick();
}

/* ------------------------------------------------------------------ */
/* Поток ядра: горячее подключение, хабы, светодиоды                  */
/* ------------------------------------------------------------------ */
extern volatile uint64_t jiffies;

static void leds_update(void)
{
    if (!usb_hid_leds_dirty()) return;
    uint8_t leds = usb_hid_leds();
    for (int i = 0; i < MAX_DEVS; i++) {
        struct usb_dev *d = &DEVS[i];
        if (!d->used || d->kind != KIND_KEYBOARD || d->kdead) continue;
        struct xhci_ctrl *c = d->c;
        ctrl_lock(c);
        if (d->report_mode) {
            if (!d->lay.led_ok) { ctrl_unlock(c); continue; }
            uint32_t n = 0;
            if (d->lay.led_rid) c->buf[n++] = d->lay.led_rid;
            c->buf[n++] = leds;
            control_xfer(d, 0x21, 0x09, (uint16_t)(0x0200 | d->lay.led_rid), d->iface,
                         (uint16_t)n, c->buf, 0);                      /* SET_REPORT (Output) */
        } else {
            c->buf[0] = leds;
            control_xfer(d, 0x21, 0x09, 0x0200, d->iface, 1, c->buf, 0);
        }
        ctrl_unlock(c);
    }
}

static void hubs_scan(struct xhci_ctrl *c)
{
    for (int i = 0; i < MAX_DEVS; i++) {
        struct usb_dev *d = &DEVS[i];
        if (d->used && d->c == c && d->kind == KIND_HUB) scan_hub(d);
    }
}

static int usb_thread(void *arg)
{
    (void)arg;
    uint32_t round = 0;
    for (;;) {
        uint64_t until = jiffies + 50;                 /* 500 мс */
        while (jiffies < until) __asm__ volatile ("hlt");

        leds_update();
        round++;
        for (int i = 0; i < nctrl; i++) {
            struct xhci_ctrl *c = &C[i];
            if (!c->running) continue;
            ctrl_lock(c);
            for (uint32_t port = 1; port <= c->max_ports; port++)
                root_port_check(c, port);
            /* Отключаем клавиатуры, перестающие отвечать */
            for (int k = 0; k < MAX_DEVS; k++) {
                struct usb_dev *d = &DEVS[k];
                if (d->used && d->c == c && d->kdead && d->kind == KIND_KEYBOARD) {
                    d->kind = KIND_UNKNOWN;
                    usb_hid_register(&d->hid, 0);
                    if (n_keyboards > 0) n_keyboards--;
                    printk("[USB] Keyboard %x:%x stopped responding\n", (long)d->vid, (long)d->pid);
                }
            }
            if ((round % 2) == 0) hubs_scan(c);
            ctrl_unlock(c);
        }
    }
    return 0;
}

void usb_start_thread(void)
{
    if (nctrl) kthread_create(usb_thread, NULL, "usbd");
}

/* ------------------------------------------------------------------ */
/* /proc/usb                                                          */
/* ------------------------------------------------------------------ */
static void sb_str(char *b, uint64_t *o, uint64_t max, const char *s)
{
    while (*s && *o < max - 1) b[(*o)++] = *s++;
}

static void sb_dec(char *b, uint64_t *o, uint64_t max, uint32_t v)
{
    char t[12]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && *o < max - 1) b[(*o)++] = t[--n];
}

static void sb_hex(char *b, uint64_t *o, uint64_t max, uint64_t v, int digits)
{
    const char *h = "0123456789abcdef";
    for (int i = (digits - 1) * 4; i >= 0 && *o < max - 1; i -= 4)
        b[(*o)++] = h[(v >> i) & 0xF];
}

void usb_proc_dump(char *buf, uint64_t max_len)
{
    uint64_t o = 0;
    sb_str(buf, &o, max_len, "USB (xHCI) status\n");
    if (!nctrl) {
        sb_str(buf, &o, max_len, "No xHCI controller initialised\n");
        buf[o] = '\0';
        return;
    }
    for (int i = 0; i < nctrl; i++) {
        struct xhci_ctrl *c = &C[i];
        sb_str(buf, &o, max_len, "\nController ");
        sb_dec(buf, &o, max_len, (uint32_t)i);
        sb_str(buf, &o, max_len, ": PCI ");
        sb_hex(buf, &o, max_len, c->bus, 2); sb_str(buf, &o, max_len, ":");
        sb_hex(buf, &o, max_len, c->slot, 2); sb_str(buf, &o, max_len, ".");
        sb_dec(buf, &o, max_len, c->func);
        sb_str(buf, &o, max_len, "  MMIO 0x"); sb_hex(buf, &o, max_len, c->mmio, 8);
        sb_str(buf, &o, max_len, "  xHCI v"); sb_hex(buf, &o, max_len, c->hci_version, 4);
        sb_str(buf, &o, max_len, "  ports "); sb_dec(buf, &o, max_len, c->max_ports);
        sb_str(buf, &o, max_len, "  slots "); sb_dec(buf, &o, max_len, c->max_slots);
        sb_str(buf, &o, max_len, "\n");

        for (uint32_t port = 1; port <= c->max_ports; port++) {
            uint32_t sc = rd32(c->op, 0x400 + (port - 1) * 0x10);
            if (!(sc & PORTSC_CCS)) continue;
            sb_str(buf, &o, max_len, "  root port "); sb_dec(buf, &o, max_len, port);
            sb_str(buf, &o, max_len, ": connected, PORTSC 0x"); sb_hex(buf, &o, max_len, sc, 8);
            sb_str(buf, &o, max_len, "\n");
        }
        for (int k = 0; k < MAX_DEVS; k++) {
            struct usb_dev *d = &DEVS[k];
            if (!d->used || d->c != c) continue;
            static const char *sp[] = { "?", "FS 12M", "LS 1.5M", "HS 480M", "SS 5G" };
            sb_str(buf, &o, max_len, "    slot "); sb_dec(buf, &o, max_len, d->slot);
            sb_str(buf, &o, max_len, d->parent ? " (hub port " : " (root port ");
            sb_dec(buf, &o, max_len, d->parent_port);
            sb_str(buf, &o, max_len, ") ");
            sb_str(buf, &o, max_len, sp[d->speed < 5 ? d->speed : 0]);
            sb_str(buf, &o, max_len, "  ");
            sb_hex(buf, &o, max_len, d->vid, 4); sb_str(buf, &o, max_len, ":");
            sb_hex(buf, &o, max_len, d->pid, 4);
            sb_str(buf, &o, max_len, "  class "); sb_dec(buf, &o, max_len, d->dclass);
            sb_str(buf, &o, max_len, "  ifaces ["); sb_str(buf, &o, max_len, d->ifaces);
            sb_str(buf, &o, max_len, "]  ");
            if (d->failed) sb_str(buf, &o, max_len, "ERROR (enumeration failed)");
            else if (d->kind == KIND_KEYBOARD) sb_str(buf, &o, max_len, d->kdead ? "keyboard (not responding)" : (d->report_mode ? "HID report-protocol keyboard" : "HID boot keyboard"));
            else if (d->kind == KIND_HUB) { sb_str(buf, &o, max_len, d->speed == 4 ? "USB 3.x hub, ports " : "USB 2.0 hub, ports "); sb_dec(buf, &o, max_len, d->hub_ports); }
            else sb_str(buf, &o, max_len, "no driver");
            sb_str(buf, &o, max_len, "\n");
        }
    }
    sb_str(buf, &o, max_len, "\nKeyboards active: ");
    sb_dec(buf, &o, max_len, (uint32_t)n_keyboards);
    sb_str(buf, &o, max_len, "\n");
    buf[o] = '\0';
}
