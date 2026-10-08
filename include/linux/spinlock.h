#ifndef _LINUX_SPINLOCK_H
#define _LINUX_SPINLOCK_H

#include <linux/types.h>

typedef struct {
    volatile uint32_t val;
} spinlock_t;

#define SPINLOCK_INIT { .val = 0 }

static inline void spin_lock_init(spinlock_t *lock)
{
    lock->val = 0;
}

static inline void spin_lock(spinlock_t *lock)
{
    while (__sync_lock_test_and_set(&lock->val, 1)) {
        while (lock->val) {
            __asm__ volatile ("pause");
        }
    }
}

static inline int spin_trylock(spinlock_t *lock)
{
    return (__sync_lock_test_and_set(&lock->val, 1) == 0);
}

static inline void spin_unlock(spinlock_t *lock)
{
    __sync_lock_release(&lock->val);
}

static inline uint64_t spin_lock_irqsave(spinlock_t *lock)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    spin_lock(lock);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags)
{
    spin_unlock(lock);
    __asm__ volatile ("pushq %0; popfq" : : "r"(flags) : "memory");
}

#endif
