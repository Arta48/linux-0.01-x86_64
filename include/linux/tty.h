#ifndef _LINUX_TTY_H
#define _LINUX_TTY_H

void console_init(void);
void console_putc(char c);
void printk(const char *fmt, ...);

#endif
