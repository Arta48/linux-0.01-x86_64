#include <linux/time.h>
#include <linux/tty.h>
#include <asm/io.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

#define MINUTE 60
#define HOUR   (60 * MINUTE)
#define DAY    (24 * HOUR)
#define YEAR   (365 * DAY)

extern volatile uint64_t jiffies;
uint64_t startup_time = 0;

static const int month[12] = {
    0,
    DAY * (31),
    DAY * (31 + 29),
    DAY * (31 + 29 + 31),
    DAY * (31 + 29 + 31 + 30),
    DAY * (31 + 29 + 31 + 30 + 31),
    DAY * (31 + 29 + 31 + 30 + 31 + 30),
    DAY * (31 + 29 + 31 + 30 + 31 + 30 + 31),
    DAY * (31 + 29 + 31 + 30 + 31 + 30 + 31 + 31),
    DAY * (31 + 29 + 31 + 30 + 31 + 30 + 31 + 31 + 30),
    DAY * (31 + 29 + 31 + 30 + 31 + 30 + 31 + 31 + 30 + 31),
    DAY * (31 + 29 + 31 + 30 + 31 + 30 + 31 + 31 + 30 + 31 + 30)
};

static inline uint8_t cmos_read(uint8_t reg)
{
    outb(0x80 | reg, CMOS_ADDR);
    return inb(CMOS_DATA);
}

#define BCD_TO_BIN(val) ((val) = ((val) & 15) + ((val) >> 4) * 10)

uint64_t kernel_mktime(struct tm *tm)
{
    uint64_t res;
    int year = tm->tm_year - 1970;

    res = YEAR * year + DAY * ((year + 1) / 4);
    res += month[tm->tm_mon];
    if (tm->tm_mon > 1 && ((year + 2) % 4))
        res -= DAY;
    res += DAY * (tm->tm_mday - 1);
    res += HOUR * tm->tm_hour;
    res += MINUTE * tm->tm_min;
    res += tm->tm_sec;
    return res;
}

void time_init(void)
{
    struct tm t;

    /* Ждем, если микросхема CMOS обновляет регистры прямо сейчас (бит 7 в регистре 0x0A) */
    while (cmos_read(0x0A) & 0x80);

    /* Двойное чтение для защиты от перехода секунды */
    do {
        t.tm_sec  = cmos_read(0);
        t.tm_min  = cmos_read(2);
        t.tm_hour = cmos_read(4);
        t.tm_mday = cmos_read(7);
        t.tm_mon  = cmos_read(8);
        t.tm_year = cmos_read(9);
    } while (t.tm_sec != cmos_read(0));

    uint8_t regb = cmos_read(0x0B);
    if (!(regb & 0x04)) {
        /* Декодируем BCD ДО любых арифметических операций! */
        BCD_TO_BIN(t.tm_sec);
        BCD_TO_BIN(t.tm_min);
        BCD_TO_BIN(t.tm_hour);
        BCD_TO_BIN(t.tm_mday);
        BCD_TO_BIN(t.tm_mon);
        BCD_TO_BIN(t.tm_year);
    }

    /* Месяцы 0..11 для алгоритма mktime */
    t.tm_mon -= 1;

    if (t.tm_year < 70) {
        t.tm_year += 2000;
    } else {
        t.tm_year += 1900;
    }

    startup_time = kernel_mktime(&t);
    printk("[OK] CMOS Real-Time Clock Initialized (Epoch: %d)\n", (int)startup_time);
}

uint64_t get_current_time(void)
{
    return startup_time + (jiffies / 100);
}
