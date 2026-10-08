#ifndef _LINUX_MUTEX_H
#define _LINUX_MUTEX_H

#include <linux/spinlock.h>

struct task_struct;

typedef struct {
    spinlock_t lock;
    volatile int locked;
    struct task_struct *owner;
    int waiters;
} mutex_t;

#define MUTEX_INIT { .lock = SPINLOCK_INIT, .locked = 0, .owner = NULL, .waiters = 0 }

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
int  mutex_trylock(mutex_t *m);
void mutex_unlock(mutex_t *m);

#endif
