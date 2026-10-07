#ifndef _LINUX_KEYBOARD_H
#define _LINUX_KEYBOARD_H

#include <linux/types.h>

void keyboard_init(void);
void keyboard_handler(void);
char keyboard_getchar(void);

#endif
