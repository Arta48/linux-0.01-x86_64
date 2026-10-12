/*
 * ACPI: минимальная поддержка для выключения и перезагрузки ПК.
 *
 * Все данные из таблиц ACPI читаются один раз в acpi_init() (пока активны
 * загрузочные таблицы страниц), после чего acpi_poweroff()/acpi_reboot()
 * обращаются только к портам ввода-вывода и работают из любого процесса.
 */
#include <linux/acpi.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/string.h>
#include <linux/minix_fs.h>
#include <linux/fs.h>
#include <asm/io.h>

int root_task_pid = -1;

#define IDMAP_LIMIT (1ULL << 30)

static uint16_t pm1a_cnt, pm1b_cnt;
static uint16_t reset_port;
static uint8_t  reset_val;
static int      have_reset, have_s5, have_fadt;
static uint8_t  slp_typa, slp_typb;
static uint32_t smi_cmd;
static uint8_t  acpi_enable_val;

static uint8_t  rd8(uint64_t a)  { return *(volatile uint8_t *)a; }
static uint16_t rd16(uint64_t a) { __asm__ ("" : "+r"(a)); return *(volatile uint16_t *)a; }
static uint32_t rd32(uint64_t a) { return *(volatile uint32_t *)a; }
static uint64_t rd64(uint64_t a) { return *(volatile uint64_t *)a; }

/* Выше 1 ГБ identity-map ядра нет: отображаем страницы таблиц ACPI по мере надобности */
static void acpi_map(uint64_t phys, uint64_t len)
{
    uint64_t p = phys & ~(PAGE_SIZE - 1);
    uint64_t e = phys + len;
    for (; p < e; p += PAGE_SIZE) {
        if (p >= IDMAP_LIMIT) map_page(NULL, p, p, PTE_WRITABLE);
    }
}

static int sig_is(uint64_t a, const char *s)
{
    for (int i = 0; i < 4; i++) if (rd8(a + i) != (uint8_t)s[i]) return 0;
    return 1;
}

/* Возвращает длину таблицы (после отображения всей таблицы) или 0 */
static uint32_t table_len(uint64_t t)
{
    acpi_map(t, 8);
    uint32_t len = rd32(t + 4);
    if (len < 36 || len > (16u << 20)) return 0;
    acpi_map(t, len);
    return len;
}

/* Ищет объект \_S5 и достаёт SLP_TYPa/SLP_TYPb */
static int find_s5(uint64_t t, uint32_t len)
{
    for (uint32_t i = 36; i + 8 < len; i++) {
        if (rd8(t + i) == '_' && rd8(t + i + 1) == 'S' && rd8(t + i + 2) == '5' &&
            rd8(t + i + 3) == '_' && rd8(t + i + 4) == 0x12) {
            /* перед именем: NameOp (0x08), возможно с '\\' */
            uint8_t prev = i >= 1 ? rd8(t + i - 1) : 0;
            uint8_t prev2 = i >= 2 ? rd8(t + i - 2) : 0;
            if (prev != 0x08 && !(prev == '\\' && prev2 == 0x08)) continue;

            uint64_t j = t + i + 5;
            j += ((rd8(j) >> 6) & 3) + 1;   /* PkgLength */
            j++;                            /* NumElements */
            if (rd8(j) == 0x0A) j++;
            slp_typa = rd8(j++);
            if (rd8(j) == 0x0A) j++;
            slp_typb = rd8(j);
            return 1;
        }
    }
    return 0;
}

static uint64_t find_rsdp_bios(void)
{
    uint64_t ebda = (uint64_t)rd16(0x40E) << 4;
    uint64_t ranges[2][2] = { { ebda, ebda + 1024 }, { 0xE0000, 0x100000 } };
    for (int r = 0; r < 2; r++) {
        if (r == 0 && (ebda < 0x80000 || ebda >= 0xA0000)) continue;
        for (uint64_t a = ranges[r][0]; a + 20 <= ranges[r][1]; a += 16) {
            if (rd8(a) == 'R' && rd8(a + 1) == 'S' && rd8(a + 2) == 'D' && rd8(a + 3) == ' ' &&
                rd8(a + 4) == 'P' && rd8(a + 5) == 'T' && rd8(a + 6) == 'R' && rd8(a + 7) == ' ') {
                uint8_t sum = 0;
                for (int i = 0; i < 20; i++) sum += rd8(a + i);
                if (sum == 0) return a;
            }
        }
    }
    return 0;
}

int acpi_init(uint64_t rsdp)
{
    if (!rsdp) rsdp = find_rsdp_bios();
    if (!rsdp) {
        printk("[WARN] ACPI: RSDP not found, power off via ACPI unavailable\n");
        return -1;
    }
    acpi_map(rsdp, 36);
    if (rd8(rsdp) != 'R' || rd8(rsdp + 1) != 'S' || rd8(rsdp + 2) != 'D') {
        printk("[WARN] ACPI: bad RSDP signature\n");
        return -1;
    }

    uint64_t root; int esz;
    if (rd8(rsdp + 15) >= 2 && rd64(rsdp + 24)) { root = rd64(rsdp + 24); esz = 8; }
    else { root = rd32(rsdp + 16); esz = 4; }

    uint32_t rlen = table_len(root);
    if (!rlen) { printk("[WARN] ACPI: bad RSDT/XSDT\n"); return -1; }
    uint32_t n = (rlen - 36) / esz;

    uint64_t dsdt = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t t = esz == 8 ? rd64(root + 36 + (uint64_t)i * 8) : rd32(root + 36 + (uint64_t)i * 4);
        uint32_t len = table_len(t);
        if (!len || !sig_is(t, "FACP")) continue;

        have_fadt = 1;
        dsdt = rd32(t + 40);
        if (len >= 148 && rd64(t + 140)) dsdt = rd64(t + 140);
        smi_cmd = rd32(t + 48);
        acpi_enable_val = rd8(t + 52);
        pm1a_cnt = (uint16_t)rd32(t + 64);
        pm1b_cnt = (uint16_t)rd32(t + 68);
        if (!pm1a_cnt && len >= 184 && rd8(t + 172) == 1) pm1a_cnt = (uint16_t)rd64(t + 176);
        uint32_t flags = rd32(t + 112);
        if ((flags & (1u << 10)) && len >= 129 && rd8(t + 116) == 1 && rd64(t + 120)) {
            reset_port = (uint16_t)rd64(t + 120);
            reset_val = rd8(t + 128);
            have_reset = 1;
        }
        break;
    }
    if (!have_fadt) { printk("[WARN] ACPI: FADT not found\n"); return -1; }

    if (dsdt) {
        uint32_t dl = table_len(dsdt);
        if (dl) have_s5 = find_s5(dsdt, dl);
    }
    for (uint32_t i = 0; i < n && !have_s5; i++) {
        uint64_t t = esz == 8 ? rd64(root + 36 + (uint64_t)i * 8) : rd32(root + 36 + (uint64_t)i * 4);
        uint32_t len = table_len(t);
        if (len && sig_is(t, "SSDT")) have_s5 = find_s5(t, len);
    }

    printk("[OK] ACPI: PM1a=%p, S5 sleep type %s, reset register %s\n",
           (uint64_t)pm1a_cnt, have_s5 ? "found" : "NOT found", have_reset ? "present" : "absent");
    return 0;
}

static void halt_forever(void)
{
    for (;;) __asm__ volatile ("cli; hlt");
}

static void spin(uint32_t units)
{
    /* Прерывания выключены, jiffies не идёт: грубая задержка (~0.3 с на единицу) */
    for (uint32_t u = 0; u < units; u++)
        for (volatile uint64_t d = 0; d < 100000000ULL; d++) {}
}

void acpi_poweroff(void)
{
    __asm__ volatile ("cli");

    printk("[ACPI] Power off: PM1a=%p PM1b=%p SLP_TYPa=%d SLP_TYPb=%d S5=%d SCI_EN=%d\n",
           (uint64_t)pm1a_cnt, (uint64_t)pm1b_cnt, (long)slp_typa, (long)slp_typb,
           (long)have_s5, (long)(pm1a_cnt ? (inw(pm1a_cnt) & 1) : -1));

    if (have_fadt && pm1a_cnt && have_s5) {
        /* Если ACPI ещё не включён (SCI_EN = 0), просим прошивку включить его */
        if (smi_cmd && acpi_enable_val && !(inw(pm1a_cnt) & 1)) {
            outb(acpi_enable_val, (uint16_t)smi_cmd);
            for (int i = 0; i < 300 && !(inw(pm1a_cnt) & 1); i++) {
                for (volatile int d = 0; d < 100000; d++) {}
            }
        }
        /* Сначала SLP_TYP, затем SLP_EN отдельной записью (так требует часть чипсетов) */
        uint16_t v = (uint16_t)((inw(pm1a_cnt) & ~0x3C00) | (slp_typa << 10));
        outw(v, pm1a_cnt);
        outw((uint16_t)(v | (1 << 13)), pm1a_cnt);
        if (pm1b_cnt) {
            uint16_t w = (uint16_t)((inw(pm1b_cnt) & ~0x3C00) | (slp_typb << 10));
            outw(w, pm1b_cnt);
            outw((uint16_t)(w | (1 << 13)), pm1b_cnt);
        }
        spin(5);
    }

    /* Известные порты эмуляторов: QEMU (PIIX/ICH9), Bochs, VirtualBox/Cloud Hypervisor */
    outw(0x2000, 0x604);
    outw(0x2000, 0xB004);
    outw(0x3400, 0x4004);
    spin(2);

    printk("\n[ACPI] Power off did NOT work (PM1a_CNT now %p). Rebooting in ~10 s...\n",
           (uint64_t)(pm1a_cnt ? inw(pm1a_cnt) : 0));
    spin(30);
    acpi_reboot();
    halt_forever();
}

void acpi_reboot(void)
{
    __asm__ volatile ("cli");

    if (have_reset) {
        outb(reset_val, reset_port);
        for (volatile uint64_t d = 0; d < 100000000ULL; d++) {}
    }

    /* Контроллер клавиатуры 8042: импульс на линию RESET */
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) {}
    outb(0xFE, 0x64);
    for (volatile uint64_t d = 0; d < 100000000ULL; d++) {}

    /* Регистр сброса чипсета */
    outb(0x02, 0xCF9);
    outb(0x06, 0xCF9);
    for (volatile uint64_t d = 0; d < 100000000ULL; d++) {}
    outb(0x0E, 0xCF9);
    for (volatile uint64_t d = 0; d < 100000000ULL; d++) {}

    /* Последнее средство: тройная ошибка (пустая IDT + прерывание) */
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = { 0, 0 };
    __asm__ volatile ("lidt %0; int3" : : "m"(null_idt));
    halt_forever();
}

int64_t sys_reboot(int cmd)
{
    minix_sync();
    if (cmd == 1) {
        printk("\n[SYSTEM] Rebooting...\n");
        acpi_reboot();
    } else if (cmd == 2) {
        printk("\n[SYSTEM] System halted.\n");
        halt_forever();
    } else {
        printk("\n[SYSTEM] Powering off...\n");
        acpi_poweroff();
    }
    return 0;
}
