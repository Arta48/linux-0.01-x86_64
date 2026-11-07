#include <linux/nic.h>
#include <linux/tty.h>
#include <linux/string.h>
#include <asm/io.h>

static const struct nic_ops *cur_nic = NULL;
static uint64_t tsc_khz = 0;

unsigned long irq_save(void)
{
    unsigned long f;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) : : "memory");
    return f;
}

void irq_restore(unsigned long f)
{
    __asm__ volatile ("pushq %0; popfq" : : "r"(f) : "memory", "cc");
}

static inline uint64_t rdtsc64(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* PIT канал 2, режим 0: 10 мс = 11932 тактов 1193182 Гц. Работает при cli. */
void tsc_calibrate(void)
{
    if (tsc_khz) return;
    uint8_t p61 = inb(0x61);
    outb((p61 & ~0x02) | 0x01, 0x61);       /* gate вкл, динамик выкл */
    outb(0xB0, 0x43);                       /* ch2, lobyte/hibyte, mode 0 */
    outb(11932 & 0xFF, 0x42);
    outb(11932 >> 8, 0x42);
    uint64_t t0 = rdtsc64();
    int guard = 0;
    while (!(inb(0x61) & 0x20) && ++guard < 10000000) { }
    uint64_t t1 = rdtsc64();
    outb(p61, 0x61);
    uint64_t khz = (t1 - t0) / 10;          /* тактов за 10 мс -> кГц */
    if (guard >= 10000000 || khz < 100000 || khz > 10000000) khz = 2000000;
    tsc_khz = khz;
}

uint64_t tsc_get_khz(void)
{
    if (!tsc_khz) tsc_calibrate();
    return tsc_khz;
}

uint64_t time_us(void)
{
    if (!tsc_khz) tsc_calibrate();
    return rdtsc64() * 1000 / tsc_khz;
}

void udelay(uint32_t us)
{
    uint64_t end = time_us() + us;
    while (time_us() < end) __asm__ volatile ("pause");
}

void nic_register(const struct nic_ops *ops)
{
    cur_nic = ops;
}

int nic_init(void)
{
    tsc_calibrate();
    /* Realtek первой: на реальных ПК он встречается чаще всего */
    if (rtl8169_init() == 0) return 0;
    if (e1000_init() == 0) return 0;
    printk("[INFO] No supported Ethernet NIC (Realtek RTL81xx / Intel e1000): networking disabled\n");
    return -1;
}

int nic_present(void) { return cur_nic != NULL; }
const char *nic_name(void) { return cur_nic ? cur_nic->name : "none"; }

void nic_get_mac(uint8_t out_mac[6])
{
    if (cur_nic) memcpy(out_mac, cur_nic->mac, 6);
    else memset(out_mac, 0, 6);
}

int nic_send(const void *data, uint16_t len)
{
    if (!cur_nic) return -1;
    unsigned long f = irq_save();
    int r = cur_nic->send(data, len);
    irq_restore(f);
    return r;
}

int nic_recv(void *buf, uint16_t max_len)
{
    if (!cur_nic) return 0;
    unsigned long f = irq_save();
    int r = cur_nic->recv(buf, max_len);
    irq_restore(f);
    return r;
}

int nic_link(uint32_t *speed, int *full)
{
    if (!cur_nic || !cur_nic->link) return 0;
    return cur_nic->link(speed, full);
}
