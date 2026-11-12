/*
 * kernel/exec.c -- execve: загрузка бинарников LINUS001 и ELF64 в новое
 * адресное пространство, сборка стека (argv/envp/auxv), VMA, brk.
 *
 * Linux-ABI процессы получают стек в соответствии с System V AMD64 ABI и
 * вектор auxv. Linux ABI включается, если у ELF метка EI_OSABI = 0x66, либо
 * он позиционно-независимый (ET_DYN, как все современные дистрибутивные
 * бинарники), либо имеет PT_INTERP (динамическая компоновка: загружается
 * ld.so, например ld-linux-x86-64.so.2 из glibc). Скрипты "#!" запускаются
 * через интерпретатор.
 */
#include <linux/syscall.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/elf.h>
#include <linux/string.h>
#include <linux/tty.h>

#define ELFOSABI_LINUXABI 0x66   /* метка: бинарник собран под Linux ABI (tools/mark_abi) */

#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_BASE    7
#define AT_FLAGS   8
#define AT_ENTRY   9
#define AT_UID     11
#define AT_EUID    12
#define AT_GID     13
#define AT_EGID    14
#define AT_PLATFORM 15
#define AT_HWCAP   16
#define AT_HWCAP2  26
#define AT_CLKTCK  17
#define AT_SECURE  23
#define AT_RANDOM  25
#define AT_EXECFN  31

#define DYN_MAIN_BASE   0x555555554000ULL   /* база ET_DYN-программы */
#define DYN_INTERP_BASE 0x7FFFF7DC0000ULL   /* база ld.so */
#define EXEC_BUF_PAGES 32
#define EXEC_MAX_STR   1024            /* argc + envc */
#define EXEC_STR_AREA  (EXEC_BUF_PAGES * PAGE_SIZE - EXEC_MAX_STR * 4)

extern int64_t sys_close(int fd);
/* закрыть O_CLOEXEC, подготовить fd 0..2 (реализация в linux_abi.c) */
__attribute__((weak)) void linux_fd_exec_cleanup(void) { }

struct exec_args {
    char *base;            /* выделенный буфер */
    uint32_t *off;         /* смещения строк */
    char *str;             /* область строк */
    uint64_t used;
    int argc, envc;
};

/* Безопасно ли читать указатель пользователя */
static int uptr_ok(const void *p, uint64_t len, int from_kernel)
{
    if (from_kernel) return 1;
    return user_access_ok((uint64_t)p, len, 0);
}

static int add_string(struct exec_args *a, const char *s, int from_kernel)
{
    if (a->argc + a->envc >= EXEC_MAX_STR) return -1;
    uint64_t start = a->used;
    int first = 1;
    for (;;) {
        if (first || ((uint64_t)s & (PAGE_SIZE - 1)) == 0) {
            if (!uptr_ok(s, 1, from_kernel)) return -1;   /* проверка при входе на новую страницу */
            first = 0;
        }
        if (a->used + 1 >= EXEC_STR_AREA) return -1;
        char c = *s++;
        a->str[a->used++] = c;
        if (c == '\0') break;
    }
    a->off[a->argc + a->envc] = (uint32_t)start;
    return 0;
}

static int collect_args(struct exec_args *a, const char *filename, char **argv, char **envp, int from_kernel, int env_kernel)
{
    a->argc = a->envc = 0;
    a->used = 0;

    if (argv) {
        for (int i = 0; ; i++) {
            if (!uptr_ok(&argv[i], sizeof(char *), from_kernel)) return -1;
            if (!argv[i]) break;
            if (add_string(a, argv[i], from_kernel) < 0) return -1;
            a->argc++;
        }
    }
    if (a->argc == 0) {
        if (add_string(a, filename, 1) < 0) return -1;
        a->argc = 1;
    }
    if (envp) {
        for (int i = 0; ; i++) {
            if (!uptr_ok(&envp[i], sizeof(char *), env_kernel)) return -1;
            if (!envp[i]) break;
            if (add_string(a, envp[i], env_kernel) < 0) return -1;
            a->envc++;
        }
    }
    return 0;
}

static int map_user_pages(uint64_t *pml4, uint64_t va_start, uint64_t npages, uint64_t flags)
{
    for (uint64_t p = 0; p < npages; p++) {
        uint64_t va = va_start + p * PAGE_SIZE;
        uint64_t *pte = lookup_pte(pml4, va);
        if (pte && (*pte & PTE_PRESENT)) continue;
        uint64_t phys = get_free_page();
        if (!phys) return -1;
        if (map_page(pml4, va, phys, flags) < 0) { free_page(phys); return -1; }
    }
    return 0;
}

/* Физический адрес (= адрес ядра) отображённой пользовательской страницы */
static uint8_t *user_page_kva(uint64_t *pml4, uint64_t va)
{
    uint64_t *pte = lookup_pte(pml4, va);
    if (!pte || !(*pte & PTE_PRESENT)) return NULL;
    return (uint8_t *)((*pte & 0x000FFFFFFFFFF000ULL) | (va & (PAGE_SIZE - 1)));
}

/* Записать байты в стек пользователя через физические адреса (страницы уже отображены) */
static int stack_write(uint64_t *pml4, uint64_t va, const void *src, uint64_t len)
{
    const uint8_t *s = (const uint8_t *)src;
    while (len) {
        uint8_t *k = user_page_kva(pml4, va);
        if (!k) return -1;
        uint64_t chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));
        if (chunk > len) chunk = len;
        memcpy(k, s, chunk);
        va += chunk; s += chunk; len -= chunk;
    }
    return 0;
}

static int stack_write64(uint64_t *pml4, uint64_t va, uint64_t v)
{
    return stack_write(pml4, va, &v, 8);
}

/* Описание ELF-образа (основная программа или интерпретатор) */
struct elf_img {
    const uint8_t *data;
    uint64_t size;
    const Elf64_Ehdr *eh;
    uint64_t bias;            /* смещение загрузки (0 для ET_EXEC) */
    uint64_t max_vaddr;       /* конец образа (с учётом bias) */
    uint64_t phdr_va;         /* адрес таблицы заголовков в памяти (с bias), 0 если неизвестен */
    uint64_t entry;           /* точка входа (с bias) */
    char interp[96];          /* PT_INTERP, "" если нет */
};

static int elf_is_x86_64(const uint8_t *d, uint64_t size)
{
    const Elf64_Ehdr *e = (const Elf64_Ehdr *)d;
    return size >= sizeof(Elf64_Ehdr) &&
           e->e_ident[0] == ELFMAG0 && e->e_ident[1] == ELFMAG1 &&
           e->e_ident[2] == ELFMAG2 && e->e_ident[3] == ELFMAG3 &&
           e->e_ident[4] == ELFCLASS64 && e->e_machine == EM_X86_64 &&
           (e->e_type == ET_EXEC || e->e_type == ET_DYN);
}

/* Проверка заголовков до того, как будет уничтожено старое адресное пространство */
static int elf_validate(struct elf_img *im, const uint8_t *data, uint64_t size, uint64_t bias)
{
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;
    if (!elf_is_x86_64(data, size)) return -1;
    if (eh->e_phentsize != sizeof(Elf64_Phdr) || eh->e_phnum == 0 || eh->e_phnum > 64 ||
        eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) > size) return -1;
    im->data = data; im->size = size; im->eh = eh;
    im->bias = (eh->e_type == ET_DYN) ? bias : 0;
    im->max_vaddr = 0; im->phdr_va = 0; im->interp[0] = 0;
    int nload = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(data + eh->e_phoff + i * sizeof(Elf64_Phdr));
        if (ph->p_type == PT_INTERP) {
            if (ph->p_filesz == 0 || ph->p_filesz >= sizeof(im->interp) ||
                ph->p_offset > size || ph->p_filesz > size - ph->p_offset) return -1;
            memcpy(im->interp, data + ph->p_offset, ph->p_filesz);
            im->interp[ph->p_filesz] = 0;
            continue;
        }
        if (ph->p_type != PT_LOAD) continue;
        nload++;
        if (ph->p_filesz > ph->p_memsz) return -1;
        if (ph->p_offset > size || ph->p_filesz > size - ph->p_offset) return -1;
        uint64_t va = ph->p_vaddr + im->bias;
        if (va < ph->p_vaddr) return -1;
        if (!user_range_ok(va, ph->p_memsz ? ph->p_memsz : 1)) return -1;
        uint64_t e = va + ph->p_memsz;
        if (e > im->max_vaddr) im->max_vaddr = e;
        if (eh->e_phoff >= ph->p_offset && eh->e_phoff < ph->p_offset + ph->p_filesz)
            im->phdr_va = va + (eh->e_phoff - ph->p_offset);
    }
    if (!nload) return -1;
    im->entry = eh->e_entry + im->bias;
    if (!user_range_ok(im->entry, 1)) return -1;
    return 0;
}

/* Отобразить PT_LOAD сегменты образа в новое адресное пространство; 0 = успех */
static int elf_load(uint64_t *pml4, const struct elf_img *im)
{
    const Elf64_Ehdr *eh = im->eh;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(im->data + eh->e_phoff + i * sizeof(Elf64_Phdr));
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        uint64_t vaddr = ph->p_vaddr + im->bias;
        uint64_t page_start = vaddr & ~(PAGE_SIZE - 1);
        uint64_t page_end = PAGE_ALIGN(vaddr + ph->p_memsz);
        uint64_t pflags = PTE_USER | ((ph->p_flags & PF_W) ? PTE_WRITABLE : 0);

        for (uint64_t va = page_start; va < page_end; va += PAGE_SIZE) {
            uint64_t *pte = lookup_pte(pml4, va);
            if (pte && (*pte & PTE_PRESENT)) {
                if (ph->p_flags & PF_W) *pte |= PTE_WRITABLE;   /* страница делится с соседним сегментом */
            } else {
                uint64_t phys = get_free_page();
                if (!phys || map_page(pml4, va, phys, pflags) < 0) return -1;
            }
        }

        /* Копируем содержимое файла постранично через физические адреса */
        uint64_t src_off = ph->p_offset, dst = vaddr, left = ph->p_filesz;
        while (left) {
            uint8_t *k = user_page_kva(pml4, dst);
            uint64_t chunk = PAGE_SIZE - (dst & (PAGE_SIZE - 1));
            if (chunk > left) chunk = left;
            memcpy(k, im->data + src_off, chunk);
            dst += chunk; src_off += chunk; left -= chunk;
        }

        uint32_t prot = ((ph->p_flags & PF_R) ? VM_READ : 0) | ((ph->p_flags & PF_W) ? VM_WRITE : 0) |
                        ((ph->p_flags & PF_X) ? VM_EXEC : 0);
        vma_add(current, page_start, page_end, prot ? prot : VM_READ, VM_FIXED_MAP);
    }
    return 0;
}

/* "#!интерпретатор [аргумент]": собрать новый argv в страницу ядра и запустить интерпретатор */
static int64_t execve_inner(const char *filename, char **argv, char **envp, struct trap_frame *tf,
                            int from_kernel, int depth);

static int64_t exec_shebang(const char *filename, const char *data, uint64_t file_size, char **argv,
                            char **envp, struct trap_frame *tf, int from_kernel, int depth)
{
    if (depth >= 4) return -1;
    char line[130];
    uint64_t n = 0;
    while (n < sizeof(line) - 1 && n + 2 < file_size && data[2 + n] != '\n' && data[2 + n] != '\r') {
        line[n] = data[2 + n];
        n++;
    }
    line[n] = 0;
    char *interp = line;
    while (*interp == ' ' || *interp == '\t') interp++;
    if (!*interp) return -1;
    char *arg = interp;
    while (*arg && *arg != ' ' && *arg != '\t') arg++;
    if (*arg) {
        *arg++ = 0;
        while (*arg == ' ' || *arg == '\t') arg++;
        char *e = arg + strlen(arg);
        while (e > arg && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    }

    uint64_t pg = get_free_page();
    if (!pg) return -1;
    char **nav = (char **)pg;                  /* 64 указателей */
    char *buf = (char *)pg + 64 * sizeof(char *);
    uint64_t cap = PAGE_SIZE - 64 * sizeof(char *), used = 0;
    int ac = 0, fail = 0;
    #define PUSHSTR(src) do { uint64_t l_ = strlen(src) + 1; \
        if (ac >= 62 || used + l_ > cap) { fail = 1; break; } \
        memcpy(buf + used, (src), l_); nav[ac++] = buf + used; used += l_; } while (0)
    do {
        PUSHSTR(interp);
        if (*arg) PUSHSTR(arg);
        PUSHSTR(filename);
        if (argv) {
            for (int i = 1; !fail; i++) {
                if (!uptr_ok(&argv[i], sizeof(char *), from_kernel)) { fail = 1; break; }
                if (!argv[i]) break;
                const char *str = argv[i];
                uint64_t l = 0;
                for (;;) {
                    if ((l & (PAGE_SIZE - 1)) == 0 && !uptr_ok(str + l, 1, from_kernel)) { fail = 1; break; }
                    if (str[l++] == 0) break;
                    if (l > cap) { fail = 1; break; }
                }
                if (fail) break;
                PUSHSTR(str);
            }
        }
    } while (0);
    #undef PUSHSTR
    int64_t r = -1;
    if (!fail) {
        nav[ac] = NULL;
        /* argv теперь в ядре; envp остаётся пользовательским (проверяется как раньше) */
        r = execve_inner(interp, nav, envp, tf, from_kernel | 2, depth + 1);
    }
    free_page(pg);
    return r;
}

static int64_t execve_inner(const char *filename, char **argv, char **envp, struct trap_frame *tf,
                            int from_kernel, int depth)
{
    int argv_kernel = from_kernel & 3 ? 1 : 0;       /* бит 0: всё из ядра; бит 1: argv уже в ядре */
    int env_kernel = from_kernel & 1;
    if (!tf || !filename) return -1;

    uint64_t file_size = 0;
    const char *data = fs_get_file_data(filename, &file_size);
    if (!data || file_size < 16) return -1;

    if (data[0] == '#' && data[1] == '!')
        return exec_shebang(filename, data, file_size, argv, envp, tf, from_kernel & 1, depth);

    const struct exec_header *hdr = (const struct exec_header *)data;
    int is_linus001 = (file_size >= sizeof(struct exec_header) && hdr->magic == EXEC_MAGIC);
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;
    int is_elf64 = elf_is_x86_64((const uint8_t *)data, file_size);
    if (!is_linus001 && !is_elf64) return -1;

    /* ---- Проверяем ELF ДО того, как уничтожим старое адресное пространство ---- */
    struct elf_img main_img, interp_img;
    int has_interp = 0;
    uint64_t elf_max_vaddr = 0, phdr_va = 0;
    int linux_abi = 0;
    if (is_elf64) {
        if (elf_validate(&main_img, (const uint8_t *)data, file_size, DYN_MAIN_BASE) < 0) return -1;
        elf_max_vaddr = main_img.max_vaddr;
        phdr_va = main_img.phdr_va;
        linux_abi = ehdr->e_ident[7] == ELFOSABI_LINUXABI || ehdr->e_type == ET_DYN || main_img.interp[0];
        if (main_img.interp[0]) {
            uint64_t isz = 0;
            const char *idata = fs_get_file_data(main_img.interp, &isz);
            if (!idata || elf_validate(&interp_img, (const uint8_t *)idata, isz, DYN_INTERP_BASE) < 0) {
                printk("[EXEC] interpreter %s not found or invalid\n", main_img.interp);
                return -1;
            }
            if (interp_img.interp[0]) return -1;
            has_interp = 1;
        }
    }

    if (current->linux_abi && !current->is_thread) kill_other_threads(0);

    /* ---- argv / envp -> буфер ядра (пока ещё жива старая память) ---- */
    uint64_t bufpage = get_free_pages(EXEC_BUF_PAGES);
    if (!bufpage) return -1;
    struct exec_args ea;
    ea.base = (char *)bufpage;
    ea.off = (uint32_t *)bufpage;
    ea.str = (char *)bufpage + EXEC_MAX_STR * 4;
    if (collect_args(&ea, filename, argv, envp, argv_kernel, env_kernel) < 0) {
        free_pages(bufpage, EXEC_BUF_PAGES);
        return -1;
    }
    char execfn[96];
    {
        uint64_t l = strlen(filename);
        if (l > sizeof(execfn) - 1) l = sizeof(execfn) - 1;
        memcpy(execfn, filename, l);
        execfn[l] = 0;
    }

    /* ---- Новое адресное пространство ---- */
    uint64_t old_pml4 = current->cr3;
    uint64_t new_pml4 = create_process_pml4();
    if (!new_pml4) { free_pages(bufpage, EXEC_BUF_PAGES); return -1; }

    current->cr3 = new_pml4;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(new_pml4) : "memory");
    if (old_pml4) free_process_pml4(old_pml4);
    vma_clear(current);

    uint64_t entry_point = 0;
    uint64_t max_vaddr = HEAP_START_VIRT;
    uint64_t *pml4 = (uint64_t *)new_pml4;
    int fail = 0;

    if (is_linus001) {
        uint64_t total_mem_size = hdr->text_size;
        if (total_mem_size < file_size) total_mem_size = file_size;
        if (total_mem_size > 16 * 1024 * 1024) total_mem_size = 16 * 1024 * 1024;
        uint64_t total_pages = (total_mem_size + PAGE_SIZE - 1) / PAGE_SIZE;

        if (map_user_pages(pml4, USER_TEXT_BASE, total_pages, PTE_USER | PTE_WRITABLE) < 0) fail = 1;
        if (!fail) {
            for (uint64_t p = 0; p < total_pages; p++) {
                uint64_t off = p * PAGE_SIZE;
                if (off >= file_size) break;
                uint64_t n = file_size - off;
                if (n > PAGE_SIZE) n = PAGE_SIZE;
                memcpy(user_page_kva(pml4, USER_TEXT_BASE + off), data + off, n);
            }
            vma_add(current, USER_TEXT_BASE, USER_TEXT_BASE + total_pages * PAGE_SIZE,
                    VM_READ | VM_WRITE | VM_EXEC, VM_FIXED_MAP);
            entry_point = hdr->entry;
            max_vaddr = USER_TEXT_BASE + total_pages * PAGE_SIZE;
        }
    } else {
        entry_point = main_img.entry;
        if (elf_load(pml4, &main_img) < 0) fail = 1;
        if (!fail && has_interp) {
            if (elf_load(pml4, &interp_img) < 0) fail = 1;
            entry_point = interp_img.entry;
        }
        max_vaddr = elf_max_vaddr;
    }

    /* ---- Стек ---- */
    uint64_t user_rsp = 0, argv_ptr = 0, envp_ptr = 0;
    uint64_t stack_top = linux_abi ? LINUX_STACK_TOP : LEGACY_STACK_TOP;

    if (!fail) {
        int argc = ea.argc, envc = ea.envc;
        uint64_t strbytes = ea.used + strlen(execfn) + 1 + 16 + 8;     /* + random + platform */
        uint64_t naux = 20;
        uint64_t vecbytes = 8 * (1 + (argc + 1) + (envc + 1)) + 16 * naux;
        uint64_t need = strbytes + vecbytes + 256;
        uint64_t stack_pages = LEGACY_STACK_PAGES;
        if (need / PAGE_SIZE + 2 > stack_pages) stack_pages = need / PAGE_SIZE + 2;

        if (map_user_pages(pml4, stack_top - stack_pages * PAGE_SIZE, stack_pages,
                           PTE_USER | PTE_WRITABLE) < 0) fail = 1;

        if (!fail) {
            if (linux_abi) {
                vma_add(current, stack_top - LINUX_STACK_MAX, stack_top, VM_READ | VM_WRITE, VM_ANON | VM_STACK);
            } else {
                vma_add(current, stack_top - stack_pages * PAGE_SIZE, stack_top, VM_READ | VM_WRITE, VM_ANON | VM_STACK);
            }

            uint64_t sp = stack_top - 16;
            /* строки argv/envp */
            uint64_t area = (ea.used + 15) & ~15ULL;
            uint64_t execfn_va = 0, plat_va = 0, rand_va = 0;
            if (linux_abi) {
                sp -= 16; rand_va = sp;
                static const char rnd[16] = {0x4c,0x69,0x6e,0x75,0x73,0x30,0x30,0x31,0x5f,0x72,0x6e,0x64,0x73,0x65,0x65,0x64};
                stack_write(pml4, rand_va, rnd, 16);
                sp -= 16; plat_va = sp;
                stack_write(pml4, plat_va, "x86_64", 7);
                uint64_t l = (strlen(execfn) + 16) & ~15ULL;
                sp -= l; execfn_va = sp;
                stack_write(pml4, execfn_va, execfn, strlen(execfn) + 1);
            }
            sp -= area;
            uint64_t str_va = sp;
            stack_write(pml4, str_va, ea.str, ea.used);

            if (linux_abi) {
                uint64_t aux[2 * 20];
                int n = 0;
                #define AUX(t, v) do { aux[n++] = (t); aux[n++] = (v); } while (0)
                if (is_elf64) {
                    AUX(AT_PHDR, phdr_va);
                    AUX(AT_PHENT, sizeof(Elf64_Phdr));
                    AUX(AT_PHNUM, ehdr->e_phnum);
                }
                AUX(AT_PAGESZ, PAGE_SIZE);
                AUX(AT_BASE, has_interp ? interp_img.bias : 0);
                AUX(AT_FLAGS, 0);
                AUX(AT_ENTRY, is_elf64 ? main_img.entry : entry_point);
                AUX(AT_UID, current->uid);
                AUX(AT_EUID, current->euid);
                AUX(AT_GID, current->gid);
                AUX(AT_EGID, current->egid);
                AUX(AT_SECURE, 0);
                AUX(AT_HWCAP, 0);
                AUX(AT_HWCAP2, 0);
                AUX(AT_CLKTCK, 100);
                AUX(AT_PLATFORM, plat_va);
                AUX(AT_RANDOM, rand_va);
                AUX(AT_EXECFN, execfn_va);
                AUX(AT_NULL, 0);
                #undef AUX

                uint64_t vec = 8 * (1 + (argc + 1) + (envc + 1)) + 8 * (uint64_t)n;
                sp -= vec;
                sp &= ~15ULL;
                uint64_t p = sp;
                stack_write64(pml4, p, (uint64_t)argc); p += 8;
                argv_ptr = p;
                for (int i = 0; i < argc; i++) { stack_write64(pml4, p, str_va + ea.off[i]); p += 8; }
                stack_write64(pml4, p, 0); p += 8;
                envp_ptr = p;
                for (int i = 0; i < envc; i++) { stack_write64(pml4, p, str_va + ea.off[argc + i]); p += 8; }
                stack_write64(pml4, p, 0); p += 8;
                stack_write(pml4, p, aux, 8 * (uint64_t)n);
                user_rsp = sp;
            } else {
                /* Старый формат: [argc][argv...][NULL][envp...][NULL]; rdi=argc, rsi=argv */
                uint64_t vec = 8 * (1 + (argc + 1) + (envc + 1));
                sp -= vec;
                sp &= ~15ULL;
                uint64_t p = sp;
                stack_write64(pml4, p, (uint64_t)argc); p += 8;
                argv_ptr = p;
                for (int i = 0; i < argc; i++) { stack_write64(pml4, p, str_va + ea.off[i]); p += 8; }
                stack_write64(pml4, p, 0); p += 8;
                envp_ptr = p;
                for (int i = 0; i < envc; i++) { stack_write64(pml4, p, str_va + ea.off[argc + i]); p += 8; }
                stack_write64(pml4, p, 0);
                user_rsp = sp;
                if ((user_rsp % 16) == 0) user_rsp -= 8;   /* как раньше: как будто выполнен call */
            }
        }
    }

    free_pages(bufpage, EXEC_BUF_PAGES);

    if (fail) {
        printk("[EXEC] out of memory loading %s\n", execfn);
        sys_exit(128 + 9);
        return -1;
    }

    /* ---- Куча ---- */
    current->mm->start_brk = (max_vaddr > HEAP_START_VIRT) ? PAGE_ALIGN(max_vaddr) : HEAP_START_VIRT;
    current->mm->brk = current->mm->start_brk;
    current->mm->mmap_cur = 0;
    current->linux_abi = linux_abi;
    current->fs_base = 0;

    /* Обработчики сигналов сбрасываются; закрытие fd (>=3 как раньше; Linux ABI -- только O_CLOEXEC) */
    for (int s = 0; s < 32; s++) {
        if (current->sig_fn[s] != (uint64_t)SIG_IGN) current->sig_fn[s] = (uint64_t)SIG_DFL;
    }
    current->fs_base = 0;
    if (linux_abi) {
        linux_fd_exec_cleanup();
    } else {
        for (int i = 3; i < NR_OPEN; i++) {
            if (current->files->f[i].in_use) sys_close(i);
        }
    }

    fpu_load_default();
    tf->cs = 0x23;
    tf->ss = 0x1B;
    tf->rip = entry_point;
    tf->rsp = user_rsp;
    tf->rbp = linux_abi ? 0 : user_rsp;
    tf->rax = 0;
    tf->rbx = tf->rcx = tf->r8 = tf->r9 = tf->r10 = tf->r11 = 0;
    tf->r12 = tf->r13 = tf->r14 = tf->r15 = 0;
    tf->rdi = linux_abi ? 0 : (uint64_t)ea.argc;
    tf->rsi = linux_abi ? 0 : argv_ptr;
    tf->rdx = linux_abi ? 0 : envp_ptr;
    tf->rflags = 0x202;
    return 0;
}

int64_t sys_execve_ex(const char *filename, char **argv, char **envp, struct trap_frame *tf, int from_kernel)
{
    return execve_inner(filename, argv, envp, tf, from_kernel ? 1 : 0, 0);
}

int64_t sys_execve(const char *filename, char **argv, char **envp, struct trap_frame *tf)
{
    return sys_execve_ex(filename, argv, envp, tf, 0);
}
