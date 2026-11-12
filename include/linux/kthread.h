#ifndef _LINUX_KTHREAD_H
#define _LINUX_KTHREAD_H

#include <linux/types.h>

int kthread_create(int (*fn)(void *), void *arg, const char *name);

#endif
