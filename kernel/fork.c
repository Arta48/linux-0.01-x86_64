#include <linux/sched.h>
#include <linux/traps.h>
#include <linux/mm.h>
#include <linux/tty.h>

extern void ret_from_fork(void);

static void *memcpy(void *dest, const void *src, uint64_t n)
{
    char *d = dest;
    const char *s = src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}

int64_t sys_fork(struct trap_frame *tf)
{
    int new_pid = -1;
    for (int i = 1; i < NR_TASKS; i++) {
        if (!task[i]) {
            new_pid = i;
            break;
        }
    }
    if (new_pid == -1) {
        printk("[FORK] Process table full!\n");
        return -1;
    }

    /* 1. Выделяем страницу памяти под task_union дочернего процесса */
    uint64_t child_kpage = get_free_page();
    if (!child_kpage) {
        return -1;
    }

    union task_union *child_union = (union task_union *)child_kpage;
    struct task_struct *child = &child_union->task;

    /* Копируем метаданные родителя */
    *child = *current;
    child->pid = new_pid;
    child->state = TASK_RUNNING;
    child->counter = child->priority;

    /* 2. Копируем страницу пользовательского стека родителя */
    uint64_t parent_user_stack_page = tf->rsp & ~0xFFFULL;
    uint64_t child_user_stack_page = get_free_page();
    if (!child_user_stack_page) {
        free_page(child_kpage);
        return -1;
    }
    memcpy((void *)child_user_stack_page, (void *)parent_user_stack_page, PAGE_SIZE);

    int64_t stack_offset = child_user_stack_page - parent_user_stack_page;

    /* 3. Размещаем кадр trap_frame в верхней части ядерного стека ребенка */
    uint64_t kstack_top = child_kpage + PAGE_SIZE;
    struct trap_frame *child_tf = (struct trap_frame *)(kstack_top - sizeof(struct trap_frame));
    *child_tf = *tf;

    /* В дочернем процессе fork() возвращает 0! */
    child_tf->rax = 0;

    /* Смещаем стек пользователя ребенка на его собственную страницу памяти */
    child_tf->rsp = tf->rsp + stack_offset;
    child_tf->rbp = tf->rbp + stack_offset;

    /* 4. Формируем кадр для switch_to: адрес ret_from_fork и регистры */
    uint64_t *sp = (uint64_t *)child_tf;

    *(--sp) = (uint64_t)ret_from_fork; /* Адрес возврата по инструкции ret */
    *(--sp) = 0x202ULL;                /* RFLAGS (IF=1) */
    *(--sp) = 0;                       /* RBX */
    *(--sp) = 0;                       /* RBP */
    *(--sp) = 0;                       /* R12 */
    *(--sp) = 0;                       /* R13 */
    *(--sp) = 0;                       /* R14 */
    *(--sp) = 0;                       /* R15 */

    child->rsp = (uint64_t)sp;
    task[new_pid] = child;

    /* Родителю возвращаем PID созданного дочернего процесса */
    return new_pid;
}
