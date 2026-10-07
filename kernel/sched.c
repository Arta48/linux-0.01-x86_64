#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/mm.h>
#include <linux/gdt.h>

static union task_union init_task = {
    .task = {
        .rsp = 0,
        .cr3 = 0,
        .state = TASK_RUNNING,
        .counter = 15,
        .priority = 15,
        .pid = 0
    }
};

struct task_struct *current = &(init_task.task);
struct task_struct *task[NR_TASKS] = { &(init_task.task), };

/* Глобальный указатель на стек ядра текущего процесса для инструкции syscall */
uint64_t kernel_current_stack = (uint64_t)&init_task + PAGE_SIZE;

extern void switch_to(struct task_struct *prev, struct task_struct *next);

void sched_init(void)
{
    for (int i = 1; i < NR_TASKS; i++) {
        task[i] = NULL;
    }
    kernel_current_stack = (uint64_t)&init_task + PAGE_SIZE;
    set_tss_stack(kernel_current_stack);
    printk("[OK] Scheduler Initialized: Task 0 (Idle) is active\n");
}

int task_create(void (*fn)(void), long priority)
{
    int i;
    for (i = 1; i < NR_TASKS; i++) {
        if (!task[i]) break;
    }
    if (i == NR_TASKS) return -1;

    uint64_t page = get_free_page();
    if (!page) return -1;

    union task_union *u = (union task_union *)page;
    u->task.state = TASK_RUNNING;
    u->task.priority = priority;
    u->task.counter = priority;
    u->task.pid = i;
    u->task.cr3 = 0;

    uint64_t stack_top = page + PAGE_SIZE - 8;
    uint64_t *sp = (uint64_t *)stack_top;

    *(--sp) = (uint64_t)fn;
    *(--sp) = 0x202ULL;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;

    u->task.rsp = (uint64_t)sp;
    task[i] = &(u->task);

    printk("[OK] Created Task PID %d (Priority %d)\n", i, priority);
    return i;
}

void schedule(void)
{
    int i, next, c;
    struct task_struct **p;

    while (1) {
        c = -1;
        next = 0;
        i = NR_TASKS;
        p = &task[NR_TASKS];
        while (--i) {
            if (!*--p)
                continue;
            if ((*p)->state == TASK_RUNNING && (*p)->counter > c) {
                c = (*p)->counter;
                next = i;
            }
        }
        if (c) break;
        for (p = &task[NR_TASKS - 1]; p >= &task[0]; --p) {
            if (*p) {
                (*p)->counter = ((*p)->counter >> 1) + (*p)->priority;
            }
        }
    }

    if (current != task[next]) {
        struct task_struct *prev = current;
        current = task[next];

        /* Обновляем вершину стека ядра как для TSS (int 0x80), так и для MSR (syscall) */
        kernel_current_stack = (uint64_t)current + PAGE_SIZE;
        set_tss_stack(kernel_current_stack);

        switch_to(prev, task[next]);
    }
}

void do_timer(void)
{
    if (--current->counter > 0)
        return;

    current->counter = 0;
    schedule();
}
