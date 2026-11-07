#ifndef _LINUX_TTY_H
#define _LINUX_TTY_H

void console_init(void);
void console_putc(char c);
int klog_read(char *dst, int max);
void printk(const char *fmt, ...);

#endif
