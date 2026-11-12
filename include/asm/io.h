#ifndef _ASM_IO_H
#define _ASM_IO_H

static inline void outb(unsigned char value, unsigned short port)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline unsigned char inb(unsigned short port)
{
    unsigned char value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(unsigned short value, unsigned short port)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline unsigned short inw(unsigned short port)
{
    unsigned short value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void insw(unsigned short port, void *addr, unsigned long count)
{
    __asm__ volatile (
        "cld\n\t"
        "rep insw"
        : "+D"(addr), "+c"(count)
        : "d"(port)
        : "memory"
    );
}

static inline void outsw(unsigned short port, const void *addr, unsigned long count)
{
    __asm__ volatile (
        "cld\n\t"
        "rep outsw"
        : "+S"(addr), "+c"(count)
        : "d"(port)
        : "memory"
    );
}

static inline void outl(unsigned int value, unsigned short port)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline unsigned int inl(unsigned short port)
{
    unsigned int value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

#endif
