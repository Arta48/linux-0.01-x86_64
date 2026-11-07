#include <linux/sched.h>
#include <linux/sock.h>
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

    reap_zombie_threads();
    fpu_save_current();

    struct task_struct *child = alloc_task();
    if (!child) {
        return -1;
    }
    uint64_t child_kstack = child->kstack;
    struct files_struct *child_files = child->files;

    *child = *current;
    child->kstack = child_kstack;
    child->files = child_files;
    memcpy(child_files, current->files, sizeof(struct files_struct));
    child_files->users = child_files->refs = 1;
    child->own_mm = *current->mm;
    child->mm = &child->own_mm;
    child->tgid = new_pid;
    child->is_thread = 0;
    child->clear_child_tid = 0;
    child->futex_addr = 0;
    child->kstack = child_kstack;
    child->pid = new_pid;
    child->father = current->tgid;
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
        if (child->files->f[fd].in_use && child->files->f[fd].type == FILE_TYPE_PIPE && child->files->f[fd].pipe) {
            child->files->f[fd].pipe->ref_count++;
            if (child->files->f[fd].mode == 1) child->files->f[fd].pipe->readers++;
            if (child->files->f[fd].mode == 2) child->files->f[fd].pipe->writers++;
        }
        if (child->files->f[fd].in_use && child->files->f[fd].type == FILE_TYPE_SOCKET)
            sock_ref(child->files->f[fd].sock_id);
    }

    /* Клонируем персональное адресное пространство страниц (CR3) ребенка */
    uint64_t child_cr3 = copy_process_pml4(current->cr3);
    if (!child_cr3) {
        free_task(child);
        return -1;
    }
    child->cr3 = child_cr3;

    /* Области памяти: копия списка VMA (страницы уже разделены через COW) */
    if (vma_clone(child, current) < 0) {
        free_process_pml4(child_cr3);
        free_task(child);
        return -1;
    }

    uint64_t kstack_top = child->kstack + KSTACK_SIZE;
    struct trap_frame *child_tf = (struct trap_frame *)(kstack_top - sizeof(struct trap_frame));
    *child_tf = *tf;

    child_tf->rax = 0;      /* стек и rbp те же: страницы разделены через COW */

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

/* ---- clone(CLONE_THREAD): поток в том же адресном пространстве (Linux ABI) ---- */
#define CLONE_VM             0x00000100
#define CLONE_FILES          0x00000400
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

int64_t sys_clone_thread(struct trap_frame *tf, uint64_t flags, uint64_t stack,
                         uint64_t ptid, uint64_t ctid, uint64_t tls)
{
    if (!(flags & CLONE_VM) || !(flags & CLONE_FILES)) return -22;   /* -EINVAL */
    if (!stack) return -22;

    reap_zombie_threads();
    fpu_save_current();

    int new_pid = -1;
    for (int i = 1; i < NR_TASKS; i++) {
        if (!task[i]) { new_pid = i; break; }
    }
    if (new_pid == -1) return -11;                                   /* -EAGAIN */

    struct task_struct *child = alloc_task();
    if (!child) return -12;                                          /* -ENOMEM */
    uint64_t child_kstack = child->kstack;
    struct files_struct *own_files = child->files;

    *child = *current;
    free_pages((uint64_t)own_files, FILES_PAGES);                    /* своя таблица не нужна: общая */
    child->kstack = child_kstack;
    child->files = current->files;
    current->files->users++;
    current->files->refs++;
    child->mm = current->mm;
    child->cr3 = current->cr3;
    child->pid = new_pid;
    child->tgid = current->tgid;
    child->is_thread = 1;
    child->exit_code = 0;
    child->state = TASK_RUNNING;
    child->counter = child->priority;
    child->signal = 0;
    child->alarm = 0;
    child->futex_addr = 0;
    child->futex_deadline = 0;
    child->clear_child_tid = (flags & CLONE_CHILD_CLEARTID) ? ctid : 0;
    if (flags & CLONE_SETTLS) child->fs_base = tls;

    if ((flags & CLONE_PARENT_SETTID) && ptid && user_access_ok(ptid, 4, 1)) *(uint32_t *)ptid = (uint32_t)new_pid;
    if ((flags & CLONE_CHILD_SETTID) && ctid && user_access_ok(ctid, 4, 1)) *(uint32_t *)ctid = (uint32_t)new_pid;

    uint64_t kstack_top = child->kstack + KSTACK_SIZE;
    struct trap_frame *child_tf = (struct trap_frame *)(kstack_top - sizeof(struct trap_frame));
    *child_tf = *tf;
    child_tf->rax = 0;
    child_tf->rsp = stack;

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
