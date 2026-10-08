#include <linux/sched.h>
#include <linux/traps.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/string.h>
#include <linux/fs.h>

extern void ret_from_fork(void);

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

    uint64_t child_kpage = get_free_page();
    if (!child_kpage) {
        return -1;
    }

    union task_union *child_union = (union task_union *)child_kpage;
    struct task_struct *child = &child_union->task;

    *child = *current;
    child->pid = new_pid;
    child->father = current->pid;
    child->exit_code = 0;
    child->state = TASK_RUNNING;
    child->counter = child->priority;
    child->signal = 0;
    child->alarm = 0;
    memcpy(child->cwd, current->cwd, sizeof(current->cwd));

    for (int s = 0; s < 32; s++) {
        child->sig_fn[s] = current->sig_fn[s];
    }

    for (int fd = 0; fd < NR_OPEN; fd++) {
        if (child->filp[fd].in_use && child->filp[fd].type == FILE_TYPE_PIPE && child->filp[fd].pipe) {
            child->filp[fd].pipe->ref_count++;
            if (child->filp[fd].mode == 1) child->filp[fd].pipe->readers++;
            if (child->filp[fd].mode == 2) child->filp[fd].pipe->writers++;
        }
    }

    /* Выделяем непрерывные 32 КБ для стека ребенка */
    uint64_t parent_user_stack_page = current->user_stack_page;
    if (!parent_user_stack_page) {
        parent_user_stack_page = tf->rsp & ~0xFFFULL;
    }

    uint64_t child_user_stack_page = get_free_pages(8);
    if (!child_user_stack_page) {
        free_page(child_kpage);
        return -1;
    }
    memcpy((void *)child_user_stack_page, (void *)parent_user_stack_page, 8 * PAGE_SIZE);

    child->user_stack_page = child_user_stack_page;
    int64_t stack_offset = child_user_stack_page - parent_user_stack_page;

    uint64_t kstack_top = child_kpage + PAGE_SIZE;
    struct trap_frame *child_tf = (struct trap_frame *)(kstack_top - sizeof(struct trap_frame));
    *child_tf = *tf;

    child_tf->rax = 0;
    child_tf->rsp = tf->rsp + stack_offset;
    child_tf->rbp = tf->rbp + stack_offset;

    uint64_t *sp = (uint64_t *)child_tf;

    *(--sp) = (uint64_t)ret_from_fork;
    *(--sp) = 0x202ULL;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;

    child->rsp = (uint64_t)sp;
    task[new_pid] = child;

    return new_pid;
}
