#ifndef _LINUX_KEYBOARD_H
#define _LINUX_KEYBOARD_H

#include <linux/types.h>

void keyboard_init(void);
void keyboard_handler(void);
void check_serial_events(void);
char keyboard_getchar(void);

/* Ввод от USB HID (этап 48): символы и Ctrl+C попадают в тот же буфер, что и PS/2 */
void keyboard_inject_char(char c);
void keyboard_inject_sigint(void);

#endif
