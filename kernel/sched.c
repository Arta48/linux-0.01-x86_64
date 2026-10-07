#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/mm.h>

/* Процесс 0 (Task 0 / Idle) */
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

extern void switch_to(struct task_struct *prev, struct task_struct *next);

void sched_init(void)
{
    for (int i = 1; i < NR_TASKS; i++) {
        task[i] = NULL;
    }
    printk("[OK] Scheduler Initialized: Task 0 (Idle) is active\n");
}

/* Создание нового фонового потока ядра */
int task_create(void (*fn)(void), long priority)
{
    int i;
    for (i = 1; i < NR_TASKS; i++) {
        if (!task[i]) break;
    }
    if (i == NR_TASKS) return -1; /* Нет свободных слотов */

        /* Выделяем страницу памяти под task_union */
        uint64_t page = get_free_page();
    if (!page) return -1;

    union task_union *u = (union task_union *)page;
    u->task.state = TASK_RUNNING;
    u->task.priority = priority;
    u->task.counter = priority;
    u->task.pid = i;
    u->task.cr3 = 0; /* Разделяет пространство ядра */

    /*
     * Формируем начальный стек процесса:
     * Выравниваем вершину стека с учетом требований x86_64 ABI (RSP % 16 == 8 при входе в C-функцию)
     */
    uint64_t stack_top = page + PAGE_SIZE - 8;
    uint64_t *sp = (uint64_t *)stack_top;

    *(--sp) = (uint64_t)fn;    /* RIP для инструкции ret */
    *(--sp) = 0x202ULL;        /* RFLAGS: прерывания включены (IF=1) */
    *(--sp) = 0;               /* RBX */
    *(--sp) = 0;               /* RBP */
    *(--sp) = 0;               /* R12 */
    *(--sp) = 0;               /* R13 */
    *(--sp) = 0;               /* R14 */
    *(--sp) = 0;               /* R15 */

    u->task.rsp = (uint64_t)sp;
    task[i] = &(u->task);

    printk("[OK] Created Task PID %d (Priority %d, Stack %p)\n", i, priority, sp);
    return i;
}

/* Классический алгоритм Торвальдса из Linux 0.01 */
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
        /* Если у всех активных задач counter == 0, пересчитываем приоритеты */
        for (p = &task[NR_TASKS - 1]; p >= &task[0]; --p) {
            if (*p) {
                (*p)->counter = ((*p)->counter >> 1) + (*p)->priority;
            }
        }
    }

    if (current != task[next]) {
        struct task_struct *prev = current;
        current = task[next];
        switch_to(prev, task[next]);
    }
}

/* Вызывается таймером PIT (100 раз в секунду) */
void do_timer(void)
{
    if (--current->counter > 0)
        return;

    current->counter = 0;
    schedule();
}
