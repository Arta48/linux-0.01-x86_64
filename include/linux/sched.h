#ifndef _LINUX_SCHED_H
#define _LINUX_SCHED_H

#include <linux/types.h>
#include <linux/mm.h>
#include <linux/fs.h>

#define NR_TASKS 64

#define TASK_RUNNING         0
#define TASK_INTERRUPTIBLE   1
#define TASK_UNINTERRUPTIBLE 2
#define TASK_ZOMBIE          3
#define TASK_STOPPED         4

struct task_struct {
    uint64_t rsp;          /* Смещение 0: стек ядра */
    uint64_t cr3;          /* Смещение 8: CR3 */
    long state;
    long counter;
    long priority;
    long pid;
    struct file filp[NR_OPEN]; /* Таблица открытых файлов процесса */
};

union task_union {
    struct task_struct task;
    char stack[PAGE_SIZE];
};

extern struct task_struct *task[NR_TASKS];
extern struct task_struct *current;

void sched_init(void);
void schedule(void);
void do_timer(void);

int task_create(void (*fn)(void), long priority);

#endif
