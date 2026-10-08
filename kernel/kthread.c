#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/syscall.h>
#include <linux/tty.h>
#include <linux/string.h>

void mutex_init(mutex_t *m)
{
    spin_lock_init(&m->lock);
    m->locked = 0;
    m->owner = NULL;
    m->waiters = 0;
}

void mutex_lock(mutex_t *m)
{
    while (1) {
        spin_lock(&m->lock);
        if (!m->locked) {
            m->locked = 1;
            m->owner = current;
            spin_unlock(&m->lock);
            return;
        }
        m->waiters++;
        current->state = TASK_INTERRUPTIBLE;
        spin_unlock(&m->lock);

        schedule();

        spin_lock(&m->lock);
        m->waiters--;
        spin_unlock(&m->lock);
    }
}

int mutex_trylock(mutex_t *m)
{
    spin_lock(&m->lock);
    if (!m->locked) {
        m->locked = 1;
        m->owner = current;
        spin_unlock(&m->lock);
        return 1;
    }
    spin_unlock(&m->lock);
    return 0;
}

void mutex_unlock(mutex_t *m)
{
    spin_lock(&m->lock);
    m->locked = 0;
    m->owner = NULL;
    spin_unlock(&m->lock);

    /* Будим ожидающие процессы */
    for (int i = 1; i < NR_TASKS; i++) {
        if (task[i] && task[i]->state == TASK_INTERRUPTIBLE) {
            task[i]->state = TASK_RUNNING;
        }
    }
}

static void kthread_bootstrap(void)
{
    __asm__ volatile ("sti");

    if (current && current->kthread_fn) {
        current->kthread_fn(current->kthread_arg);
    }

    sys_exit(0);
}

int kthread_create(int (*fn)(void *), void *arg, const char *name)
{
    int pid = task_create(kthread_bootstrap, 15);
    if (pid < 0) return -1;

    task[pid]->kthread_fn = fn;
    task[pid]->kthread_arg = arg;

    if (name) {
        strncpy(task[pid]->name, name, sizeof(task[pid]->name) - 1);
        task[pid]->name[sizeof(task[pid]->name) - 1] = '\0';
    } else {
        strcpy(task[pid]->name, "kthread");
    }

    printk("[OK] Kernel Thread '%s' created (PID %d)\n", task[pid]->name, pid);
    return pid;
}
