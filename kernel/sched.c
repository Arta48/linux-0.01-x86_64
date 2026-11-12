#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/mm.h>
#include <linux/gdt.h>
#include <linux/string.h>

extern volatile uint64_t jiffies;

static char idle_kstack[KSTACK_SIZE] __attribute__((aligned(4096)));

static struct files_struct init_files;

static struct task_struct init_task = {
        .rsp = 0,
        .cr3 = 0,
        .state = TASK_RUNNING,
        .counter = 15,
        .priority = 15,
        .pid = 0,
        .father = 0,
        .exit_code = 0,
        .tgid = 0,
        .cwd = "/",
        .signal = 0,
        .alarm = 0
};

struct task_struct *current = &init_task;
struct task_struct *task[NR_TASKS] = { &init_task, };

uint64_t kernel_current_stack = (uint64_t)idle_kstack + KSTACK_SIZE;

extern void switch_to(struct task_struct *prev, struct task_struct *next);

void sched_init(void)
{
    for (int i = 1; i < NR_TASKS; i++) {
        task[i] = NULL;
    }
    for (int s = 0; s < 32; s++) {
        init_task.sig_fn[s] = (uint64_t)SIG_DFL;
    }
    init_task.files = &init_files;
    fpu_save_current();
    init_files.users = init_files.refs = 1;
    init_task.mm = &init_task.own_mm;
    init_task.own_mm.start_brk = init_task.own_mm.brk = HEAP_START_VIRT;
    init_task.kstack = (uint64_t)idle_kstack;
    kernel_current_stack = (uint64_t)idle_kstack + KSTACK_SIZE;
    set_tss_stack(kernel_current_stack);
    printk("[OK] Scheduler Initialized: Task 0 (Idle) is active\n");
}

struct task_struct *alloc_task(void)
{
    uint64_t page = get_free_pages(TASK_PAGES);
    if (!page) return NULL;
    struct task_struct *t = (struct task_struct *)page;
    memset(t, 0, TASK_PAGES * PAGE_SIZE);
    t->kstack = get_free_pages(KSTACK_PAGES);
    if (!t->kstack) {
        free_pages(page, TASK_PAGES);
        return NULL;
    }
    t->files = files_alloc();
    if (!t->files) {
        free_pages(t->kstack, KSTACK_PAGES);
        free_pages(page, TASK_PAGES);
        return NULL;
    }
    t->mm = &t->own_mm;
    fpu_init_state(t->fpu);
    return t;
}

struct files_struct *files_alloc(void)
{
    uint64_t pg = get_free_pages(FILES_PAGES);
    if (!pg) return NULL;
    struct files_struct *f = (struct files_struct *)pg;
    memset(f, 0, FILES_PAGES * PAGE_SIZE);
    f->users = f->refs = 1;
    return f;
}

void files_put(struct files_struct *f)
{
    if (f && --f->refs <= 0) free_pages((uint64_t)f, FILES_PAGES);
}

/* Освобождает задачи-потоки в состоянии ZOMBIE (их память и адресное пространство принадлежат лидеру) */
void reap_zombie_threads(void)
{
    for (int i = 1; i < NR_TASKS; i++) {
        struct task_struct *t = task[i];
        if (t && t != current && t->is_thread && t->state == TASK_ZOMBIE) {
            task[i] = NULL;
            free_task(t);
        }
    }
}

void free_task(struct task_struct *t)
{
    if (!t) return;
    if (t->kstack) free_pages(t->kstack, KSTACK_PAGES);
    files_put(t->files);
    free_pages((uint64_t)t, TASK_PAGES);
}

int task_create(void (*fn)(void), long priority)
{
    int i;
    for (i = 1; i < NR_TASKS; i++) {
        if (!task[i]) break;
    }
    if (i == NR_TASKS) return -1;

    struct task_struct *nt = alloc_task();
    if (!nt) return -1;
    nt->state = TASK_RUNNING;
    nt->priority = priority;
    nt->counter = priority;
    nt->pid = i;
    nt->tgid = i;
    nt->father = 0;
    nt->exit_code = 0;
    nt->mm->start_brk = HEAP_START_VIRT;
    nt->mm->brk = HEAP_START_VIRT;

    /* Создаем персональное дерево каталогов страниц для задачи */
    uint64_t task_cr3 = create_process_pml4();
    if (!task_cr3) {
        free_task(nt);
        return -1;
    }
    nt->cr3 = task_cr3;

    nt->signal = 0;
    nt->alarm = 0;
    nt->cwd[0] = '/';
    nt->cwd[1] = '\0';

    for (int s = 0; s < 32; s++) {
        nt->sig_fn[s] = (uint64_t)SIG_DFL;
    }

    for (int fd = 0; fd < NR_OPEN; fd++) {
        nt->files->f[fd].in_use = 0;
        nt->files->f[fd].pipe = NULL;
    }

    uint64_t stack_top = nt->kstack + KSTACK_SIZE - 8;
    uint64_t *sp = (uint64_t *)stack_top;

    *(--sp) = (uint64_t)fn;
    *(--sp) = 0x202ULL;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;

    nt->rsp = (uint64_t)sp;
    task[i] = nt;

    return i;
}

void send_signal(struct task_struct *t, int sig)
{
    if (!t || sig <= 0 || sig >= 32) return;
    t->signal |= (1U << sig);
    if (t->state == TASK_INTERRUPTIBLE) {
        t->state = TASK_RUNNING; /* Будим процесс при получении сигнала */
    }
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

        kernel_current_stack = current->kstack + KSTACK_SIZE;
        set_tss_stack(kernel_current_stack);

        /* TLS Linux-процессов (arch_prctl): база FS -- часть контекста задачи */
        if (current->fs_base != prev->fs_base) {
            uint64_t v = current->fs_base;
            __asm__ volatile ("wrmsr" : : "c"(0xC0000100U), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
        }

        __asm__ volatile ("fxsave64 %0" : "=m"(*(struct { uint8_t b[512]; } *)prev->fpu) : : "memory");
        __asm__ volatile ("fxrstor64 %0" : : "m"(*(struct { uint8_t b[512]; } *)current->fpu) : "memory");
        switch_to(prev, task[next]);
    }
}

/* Состояние FPU/SSE после сброса: FCW=0x37F, MXCSR=0x1F80 (все исключения замаскированы) */
void fpu_init_state(uint8_t *area)
{
    memset(area, 0, 512);
    *(uint16_t *)(area + 0) = 0x37F;
    *(uint32_t *)(area + 24) = 0x1F80;
    *(uint32_t *)(area + 28) = 0xFFFF;
}

/* Сохранить регистры текущей задачи в её task_struct (перед копированием при fork/clone) */
void fpu_save_current(void)
{
    __asm__ volatile ("fxsave64 %0" : "=m"(*(struct { uint8_t b[512]; } *)current->fpu) : : "memory");
}

/* execve: новая программа стартует с чистым состоянием FPU */
void fpu_load_default(void)
{
    fpu_init_state(current->fpu);
    __asm__ volatile ("fxrstor64 %0" : : "m"(*(struct { uint8_t b[512]; } *)current->fpu) : "memory");
}

void do_timer(void)
{
    /* Проверяем пробуждение спящих задач */
    for (int i = 1; i < NR_TASKS; i++) {
        if (task[i] && task[i]->state == TASK_INTERRUPTIBLE) {
            if (task[i]->alarm && jiffies >= task[i]->alarm) {
                task[i]->state = TASK_RUNNING;
                task[i]->alarm = 0;
            }
            if (task[i]->futex_addr && task[i]->futex_deadline && jiffies >= task[i]->futex_deadline)
                task[i]->state = TASK_RUNNING;      /* futex_addr остаётся: ожидающий увидит таймаут */
        }
    }

    if (--current->counter > 0)
        return;

    current->counter = 0;
    schedule();
}

/* Ожидание без блокировки (nanosleep, tty): уступить CPU другим готовым задачам, иначе hlt до прерывания */
void sched_yield_sleep(void)
{
    int other = 0;
    for (int i = 1; i < NR_TASKS; i++) {
        if (task[i] && task[i] != current && task[i]->state == TASK_RUNNING) { other = 1; break; }
    }
    __asm__ volatile ("sti");
    if (other) {
        current->counter = 0;
        schedule();
    } else {
        __asm__ volatile ("hlt");
    }
}
