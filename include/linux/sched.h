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

/* Ядерный стек каждой задачи: 4 страницы (16 КБ). Раньше task_struct и стек
 * делили одну страницу, и на стек оставалось ~2.7 КБ -- вложенные прерывания
 * поверх системного вызова с printk/VFS могли его переполнить. */
#define KSTACK_PAGES 4
#define KSTACK_SIZE  (KSTACK_PAGES * PAGE_SIZE)
#define TASK_PAGES   1   /* task_struct выделяется отдельно */
#define FILES_PAGES  2   /* таблица файлов (NR_OPEN дескрипторов), общая у потоков */

/* Адресное пространство: общее у потоков одного процесса (CLONE_VM) */
struct mm_struct {
    struct vma *vmas;          /* Области виртуальной памяти (VMA_PAGES страниц) */
    int nvma;
    uint64_t mmap_cur;         /* Следующий свободный адрес для mmap */
    uint64_t start_brk;
    uint64_t brk;
};

/* Таблица файловых дескрипторов: общая у потоков (CLONE_FILES) */
struct files_struct {
    int users;                 /* живые задачи (при 0 дескрипторы закрываются) */
    int refs;                  /* все ссылки, включая зомби (при 0 страница освобождается) */
    struct file f[NR_OPEN];
};

struct task_struct {
    uint64_t rsp;              /* Смещение 0: стек ядра */
    uint64_t cr3;              /* Смещение 8: CR3 */
    long state;
    long counter;
    long priority;
    long pid;
    long father;
    int exit_code;
    uint64_t kstack;           /* Базовый адрес ядерного стека (KSTACK_SIZE) */
    struct mm_struct *mm;      /* &own_mm или mm лидера группы потоков */
    int linux_abi;             /* 1 = процесс использует Linux x86_64 ABI */
    uint64_t fs_base;          /* TLS (arch_prctl) */
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
    /* --- Linux ABI --- */
    uint64_t sigmask;          /* заблокированные сигналы (бит n = сигнал n) */
    struct {
        uint64_t handler, flags, restorer, mask;
    } lx_sa[32];
    uint64_t cloexec;          /* бит fd = O_CLOEXEC */
    int pgid, sid;
    uint32_t umask;
    uint64_t sleep_until;      /* для nanosleep */
    char name[32];
    struct files_struct *files;
    struct mm_struct own_mm;
    uint8_t fpu[512] __attribute__((aligned(16)));   /* FXSAVE: x87/SSE состояние задачи */
    /* --- потоки и futex --- */
    long tgid;                 /* PID процесса (у лидера равен pid) */
    int is_thread;             /* 1 = не лидер группы: не виден wait4 родителя */
    uint64_t clear_child_tid;  /* CLONE_CHILD_CLEARTID / set_tid_address */
    uint64_t futex_addr;       /* != 0: задача ждёт на этом адресе */
    uint64_t futex_cr3;
    uint32_t futex_bitset;
    uint64_t futex_deadline;   /* jiffies, 0 = без таймаута */
};

_Static_assert(sizeof(struct task_struct) <= TASK_PAGES * PAGE_SIZE, "task_struct не помещается в TASK_PAGES");
_Static_assert(sizeof(struct files_struct) <= FILES_PAGES * PAGE_SIZE, "files_struct не помещается в FILES_PAGES");

struct task_struct *alloc_task(void);
struct files_struct *files_alloc(void);
void files_put(struct files_struct *f);
void reap_zombie_threads(void);
void sched_yield_sleep(void);
void fpu_init_state(uint8_t *area);
void fpu_save_current(void);
void fpu_load_default(void);
void free_task(struct task_struct *t);

extern struct task_struct *task[NR_TASKS];
extern struct task_struct *current;

void sched_init(void);
void schedule(void);
void do_timer(void);

int task_create(void (*fn)(void), long priority);
void send_signal(struct task_struct *t, int sig);

#endif
