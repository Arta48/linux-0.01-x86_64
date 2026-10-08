#ifndef _LINUX_SCHED_H
#define _LINUX_SCHED_H

#include <linux/types.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/signal.h>

#define NR_TASKS 64

#define TASK_RUNNING         0
#define TASK_INTERRUPTIBLE   1
#define TASK_UNINTERRUPTIBLE 2
#define TASK_ZOMBIE          3
#define TASK_STOPPED         4

#define HEAP_START_VIRT 0x40000000ULL

struct task_struct {
    uint64_t rsp;              /* Смещение 0: стек ядра */
    uint64_t cr3;              /* Смещение 8: CR3 */
    long state;
    long counter;
    long priority;
    long pid;
    long father;
    int exit_code;
    uint64_t user_stack_page;
    uint64_t start_brk;
    uint64_t brk;
    char cwd[64];
    uint16_t uid;              /* Идентификатор пользователя */
    uint16_t euid;             /* Эффективный UID */
    uint16_t gid;              /* Идентификатор группы */
    uint16_t egid;             /* Эффективный GID */
    uint32_t signal;
    uint64_t sig_fn[32];
    uint64_t alarm;
    int (*kthread_fn)(void *);
    void *kthread_arg;
    char name[32];
    struct file filp[NR_OPEN];
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
void send_signal(struct task_struct *t, int sig);

#endif
