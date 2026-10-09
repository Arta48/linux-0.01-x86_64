#ifndef _LINUX_SMP_H
#define _LINUX_SMP_H

#include <linux/types.h>

#define MAX_CPUS         8
#define LAPIC_DEFAULT_BASE 0xFEE00000ULL

/* Смещения регистров Local APIC */
#define LAPIC_ID         0x0020
#define LAPIC_VERSION    0x0030
#define LAPIC_TPR        0x0080
#define LAPIC_EOI        0x00B0
#define LAPIC_LDR        0x00D0
#define LAPIC_DFR        0x00E0
#define LAPIC_SVR        0x00F0
#define LAPIC_ESR        0x0280
#define LAPIC_ICR_LOW    0x0300
#define LAPIC_ICR_HIGH   0x0310
#define LAPIC_TIMER      0x0320
#define LAPIC_TICR       0x0380
#define LAPIC_TCCR       0x0390
#define LAPIC_TDCR       0x03E0

struct cpu_info {
    uint32_t id;
    volatile int online;
    uint64_t kstack;
};

extern struct cpu_info cpus[MAX_CPUS];
extern volatile int smp_num_cpus;

void smp_init(int bsp_only);
void lapic_init(void);
uint32_t smp_get_cpu_id(void);
void ap_startup(void);

#endif
