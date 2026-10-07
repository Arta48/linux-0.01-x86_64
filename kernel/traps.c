#include <linux/traps.h>
#include <linux/tty.h>
#include <asm/io.h>

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define PIT_CH0   0x40
#define PIT_CMD   0x43
#define HZ        100

static struct idt_entry idt[256];
static struct idt_ptr   idtr;

volatile uint64_t jiffies = 0;

extern void isr0();  extern void isr1();  extern void isr2();  extern void isr3();
extern void isr4();  extern void isr5();  extern void isr6();  extern void isr7();
extern void isr8();  extern void isr9();  extern void isr10(); extern void isr11();
extern void isr12(); extern void isr13(); extern void isr14(); extern void isr15();
extern void isr16(); extern void isr17(); extern void isr18(); extern void isr19();
extern void isr32(); extern void isr33();

static void set_idt_gate(int n, uint64_t handler)
{
    idt[n].offset_low  = handler & 0xFFFF;
    idt[n].selector    = 0x08;
    idt[n].ist         = 0;
    idt[n].type_attr   = 0x8E;
    idt[n].offset_mid  = (handler >> 16) & 0xFFFF;
    idt[n].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[n].zero        = 0;
}

static void pic_remap(void)
{
    outb(0x11, PIC1_CMD);
    outb(0x11, PIC2_CMD);

    outb(0x20, PIC1_DATA);
    outb(0x28, PIC2_DATA);

    outb(0x04, PIC1_DATA);
    outb(0x02, PIC2_DATA);

    outb(0x01, PIC1_DATA);
    outb(0x01, PIC2_DATA);

    outb(0xFE, PIC1_DATA);
    outb(0xFF, PIC2_DATA);
}

static void timer_init(void)
{
    uint16_t divisor = 1193180 / HZ;
    outb(0x36, PIT_CMD);
    outb(divisor & 0xFF, PIT_CH0);
    outb((divisor >> 8) & 0xFF, PIT_CH0);
}

void trap_init(void)
{
    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)&idt;

    set_idt_gate(0,  (uint64_t)isr0);   set_idt_gate(1,  (uint64_t)isr1);
    set_idt_gate(2,  (uint64_t)isr2);   set_idt_gate(3,  (uint64_t)isr3);
    set_idt_gate(4,  (uint64_t)isr4);   set_idt_gate(5,  (uint64_t)isr5);
    set_idt_gate(6,  (uint64_t)isr6);   set_idt_gate(7,  (uint64_t)isr7);
    set_idt_gate(8,  (uint64_t)isr8);   set_idt_gate(9,  (uint64_t)isr9);
    set_idt_gate(10, (uint64_t)isr10);  set_idt_gate(11, (uint64_t)isr11);
    set_idt_gate(12, (uint64_t)isr12);  set_idt_gate(13, (uint64_t)isr13);
    set_idt_gate(14, (uint64_t)isr14);  set_idt_gate(15, (uint64_t)isr15);
    set_idt_gate(16, (uint64_t)isr16);  set_idt_gate(17, (uint64_t)isr17);
    set_idt_gate(18, (uint64_t)isr18);  set_idt_gate(19, (uint64_t)isr19);

    set_idt_gate(32, (uint64_t)isr32);

    pic_remap();
    timer_init();

    __asm__ volatile ("lidt %0" : : "m"(idtr));
}

static const char *exceptions[] = {
    "Divide Error", "Debug", "NMI", "Breakpoint",
    "Overflow", "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Overrun", "Invalid TSS", "Segment Not Present",
    "Stack-Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
    "x87 FPU Error", "Alignment Check", "Machine Check", "SIMD Floating-Point"
};

void isr_handler(struct trap_frame *tf)
{
    /* Аппаратные прерывания IRQ (32..47) */
    if (tf->int_no >= 32 && tf->int_no < 48) {
        if (tf->int_no == 32) {
            jiffies++;
        }
        if (tf->int_no >= 40) {
            outb(0x20, PIC2_CMD);
        }
        outb(0x20, PIC1_CMD);
        return;
    }

    /* Исключения процессора (0..31) */
    __asm__ volatile ("cli");

    if (tf->int_no < 20) {
        printk("\n================ KERNEL PANIC ================\n");
        printk("CPU EXCEPTION #%d: %s\n", tf->int_no, exceptions[tf->int_no]);

        /* Если это Page Fault, считываем CR2 */
        if (tf->int_no == 14) {
            uint64_t cr2;
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            printk("Faulting Linear Address (CR2): %p\n", cr2);
        }

        printk("RIP: %p   CS: 0x%x   RFLAGS: %p\n", tf->rip, tf->cs, tf->rflags);
        printk("RAX: %p   RBX: %p   RCX: %p\n", tf->rax, tf->rbx, tf->rcx);
        printk("RDX: %p   RSI: %p   RDI: %p\n", tf->rdx, tf->rsi, tf->rdi);
        printk("RSP: %p   SS: 0x%x   Error Code: %d\n", tf->rsp, tf->ss, tf->error_code);
        printk("System halted.\n");
        for (;;) {
            __asm__ volatile ("hlt");
        }
    }
}
