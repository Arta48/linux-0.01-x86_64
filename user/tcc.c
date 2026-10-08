#include "ulibc.h"

#define MAX_SRC_SIZE    (64 * 1024)
#define MAX_CODE_SIZE   (64 * 1024)
#define MAX_DATA_SIZE   (32 * 1024)
#define MAX_SYMS        512
#define MAX_RELOCS      512

/* Базовый адрес в виртуальной памяти по соглашению Linux 0.01 x86_64 */
#define BASE_VIRT_ADDR  0x60000000ULL
#define EXEC_MAGIC_VAL  0x4C494E5553303031ULL /* "LINUS001" */

enum token_type {
    TOK_EOF = 0,
    TOK_NUM,
    TOK_STR,
    TOK_ID,
    TOK_INT,
    TOK_CHAR,
    TOK_VOID,
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_FOR,
    TOK_RETURN,
    TOK_SIZEOF,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_ASSIGN,
    TOK_EQ,
    TOK_NE,
    TOK_LT,
    TOK_LE,
    TOK_GT,
    TOK_GE,
    TOK_ANDAND,
    TOK_OROR,
    TOK_AMP,
    TOK_PIPE,
    TOK_CARET,
    TOK_EXCL,
    TOK_TILDE,
    TOK_PLUSPLUS,
    TOK_MINUSMINUS,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_SEMI,
    TOK_COMMA
};

struct symbol {
    char name[32];
    int is_global;
    int is_func;
    int offset;
};

struct reloc {
    int code_pos;
    char target_name[32];
};

struct str_reloc {
    int code_pos;
    int data_offset;
};

static char src[MAX_SRC_SIZE];
static int  src_idx = 0;
static int  line_num = 1;

static int  tok;
static int64_t tok_num;
static char tok_str[256];
static char tok_id[32];

static uint8_t code[MAX_CODE_SIZE];
static int     code_idx = 0;

static uint8_t data[MAX_DATA_SIZE];
static int     data_idx = 0;

static struct symbol symtab[MAX_SYMS];
static int           sym_count = 0;

static struct symbol locals[MAX_SYMS];
static int           local_count = 0;
static int           local_stack_offset = 0;

static struct reloc  relocs[MAX_RELOCS];
static int           reloc_count = 0;

static struct str_reloc str_relocs[MAX_RELOCS];
static int              str_reloc_count = 0;

static void error(const char *msg)
{
    printf("[TCC ERROR] Line %d: %s\n", line_num, msg);
    exit(1);
}

/* Генерация байтов машинного кода x86_64 */
static inline void emit1(uint8_t b)
{
    if (code_idx >= MAX_CODE_SIZE) error("Code buffer overflow");
    code[code_idx++] = b;
}

static inline void emit4(uint32_t dw)
{
    emit1((uint8_t)(dw & 0xFF));
    emit1((uint8_t)((dw >> 8) & 0xFF));
    emit1((uint8_t)((dw >> 16) & 0xFF));
    emit1((uint8_t)((dw >> 24) & 0xFF));
}

static inline void emit8(uint64_t qw)
{
    emit4((uint32_t)(qw & 0xFFFFFFFF));
    emit4((uint32_t)(qw >> 32));
}

static inline void fixup4(int pos, uint32_t dw)
{
    code[pos + 0] = (uint8_t)(dw & 0xFF);
    code[pos + 1] = (uint8_t)((dw >> 8) & 0xFF);
    code[pos + 2] = (uint8_t)((dw >> 16) & 0xFF);
    code[pos + 3] = (uint8_t)((dw >> 24) & 0xFF);
}

static inline void fixup8(int pos, uint64_t qw)
{
    fixup4(pos, (uint32_t)(qw & 0xFFFFFFFF));
    fixup4(pos + 4, (uint32_t)(qw >> 32));
}

/* Лексический анализатор */
static void next_char(void)
{
    if (src[src_idx] == '\n') line_num++;
    src_idx++;
}

static void next_tok(void)
{
    while (src[src_idx]) {
        while (src[src_idx] == ' ' || src[src_idx] == '\t' || src[src_idx] == '\r' || src[src_idx] == '\n') {
            next_char();
        }

        /* Пропуск однострочных комментариев */
        if (src[src_idx] == '/' && src[src_idx + 1] == '/') {
            while (src[src_idx] && src[src_idx] != '\n') next_char();
            continue;
        }

        /* Пропуск многострочных комментариев */
        if (src[src_idx] == '/' && src[src_idx + 1] == '*') {
            next_char(); next_char();
            while (src[src_idx] && !(src[src_idx] == '*' && src[src_idx + 1] == '/')) {
                next_char();
            }
            if (src[src_idx]) { next_char(); next_char(); }
            continue;
        }

        break;
    }

    if (!src[src_idx]) {
        tok = TOK_EOF;
        return;
    }

    char c = src[src_idx];

    /* Числовые константы */
    if (c >= '0' && c <= '9') {
        int64_t val = 0;
        if (c == '0' && (src[src_idx + 1] == 'x' || src[src_idx + 1] == 'X')) {
            next_char(); next_char();
            while (1) {
                char ch = src[src_idx];
                if (ch >= '0' && ch <= '9') { val = (val << 4) + (ch - '0'); next_char(); }
                else if (ch >= 'a' && ch <= 'f') { val = (val << 4) + (ch - 'a' + 10); next_char(); }
                else if (ch >= 'A' && ch <= 'F') { val = (val << 4) + (ch - 'A' + 10); next_char(); }
                else break;
            }
        } else {
            while (src[src_idx] >= '0' && src[src_idx] <= '9') {
                val = val * 10 + (src[src_idx] - '0');
                next_char();
            }
        }
        tok = TOK_NUM;
        tok_num = val;
        return;
    }

    /* Строковые литералы */
    if (c == '"') {
        next_char();
        int si = 0;
        while (src[src_idx] && src[src_idx] != '"') {
            char sc = src[src_idx];
            if (sc == '\\') {
                next_char();
                sc = src[src_idx];
                if (sc == 'n') sc = '\n';
                else if (sc == 't') sc = '\t';
                else if (sc == 'r') sc = '\r';
                else if (sc == '0') sc = '\0';
            }
            if (si < 255) tok_str[si++] = sc;
            next_char();
        }
        tok_str[si] = '\0';
        if (src[src_idx] == '"') next_char();
        tok = TOK_STR;
        return;
    }

    /* Символьные литералы */
    if (c == '\'') {
        next_char();
        char sc = src[src_idx];
        if (sc == '\\') {
            next_char();
            sc = src[src_idx];
            if (sc == 'n') sc = '\n';
            else if (sc == 't') sc = '\t';
            else if (sc == '0') sc = '\0';
        }
        next_char();
        if (src[src_idx] == '\'') next_char();
        tok = TOK_NUM;
        tok_num = (unsigned char)sc;
        return;
    }

    /* Идентификаторы и ключевые слова */
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        int ii = 0;
        while ((src[src_idx] >= 'a' && src[src_idx] <= 'z') ||
            (src[src_idx] >= 'A' && src[src_idx] <= 'Z') ||
            (src[src_idx] >= '0' && src[src_idx] <= '9') || src[src_idx] == '_') {
            if (ii < 31) tok_id[ii++] = src[src_idx];
            next_char();
            }
            tok_id[ii] = '\0';

        if (strcmp(tok_id, "int") == 0)    { tok = TOK_INT; return; }
        if (strcmp(tok_id, "char") == 0)   { tok = TOK_CHAR; return; }
        if (strcmp(tok_id, "void") == 0)   { tok = TOK_VOID; return; }
        if (strcmp(tok_id, "if") == 0)     { tok = TOK_IF; return; }
        if (strcmp(tok_id, "else") == 0)   { tok = TOK_ELSE; return; }
        if (strcmp(tok_id, "while") == 0)  { tok = TOK_WHILE; return; }
        if (strcmp(tok_id, "for") == 0)    { tok = TOK_FOR; return; }
        if (strcmp(tok_id, "return") == 0) { tok = TOK_RETURN; return; }
        if (strcmp(tok_id, "sizeof") == 0) { tok = TOK_SIZEOF; return; }

        tok = TOK_ID;
        return;
    }

    /* Операторы */
    next_char();
    switch (c) {
        case '+':
            if (src[src_idx] == '+') { next_char(); tok = TOK_PLUSPLUS; return; }
            tok = TOK_PLUS; return;
        case '-':
            if (src[src_idx] == '-') { next_char(); tok = TOK_MINUSMINUS; return; }
            tok = TOK_MINUS; return;
        case '*': tok = TOK_STAR; return;
        case '/': tok = TOK_SLASH; return;
        case '%': tok = TOK_PERCENT; return;
        case '=':
            if (src[src_idx] == '=') { next_char(); tok = TOK_EQ; return; }
            tok = TOK_ASSIGN; return;
        case '!':
            if (src[src_idx] == '=') { next_char(); tok = TOK_NE; return; }
            tok = TOK_EXCL; return;
        case '<':
            if (src[src_idx] == '=') { next_char(); tok = TOK_LE; return; }
            tok = TOK_LT; return;
        case '>':
            if (src[src_idx] == '=') { next_char(); tok = TOK_GE; return; }
            tok = TOK_GT; return;
        case '&':
            if (src[src_idx] == '&') { next_char(); tok = TOK_ANDAND; return; }
            tok = TOK_AMP; return;
        case '|':
            if (src[src_idx] == '|') { next_char(); tok = TOK_OROR; return; }
            tok = TOK_PIPE; return;
        case '^': tok = TOK_CARET; return;
        case '~': tok = TOK_TILDE; return;
        case '(': tok = TOK_LPAREN; return;
        case ')': tok = TOK_RPAREN; return;
        case '{': tok = TOK_LBRACE; return;
        case '}': tok = TOK_RBRACE; return;
        case '[': tok = TOK_LBRACKET; return;
        case ']': tok = TOK_RBRACKET; return;
        case ';': tok = TOK_SEMI; return;
        case ',': tok = TOK_COMMA; return;
        default:  error("Unknown character token");
    }
}

static struct symbol *find_local(const char *name)
{
    for (int i = 0; i < local_count; i++) {
        if (strcmp(locals[i].name, name) == 0) return &locals[i];
    }
    return NULL;
}

static struct symbol *find_global(const char *name)
{
    for (int i = 0; i < sym_count; i++) {
        if (strcmp(symtab[i].name, name) == 0) return &symtab[i];
    }
    return NULL;
}

/* Встроенные функции среды выполнения (Standard Builtin Runtime) */
static void emit_runtime(void)
{
    /* 1. putchar(char c): dil -> sys_write(1, &c, 1) */
    symtab[sym_count].is_func = 1;
    symtab[sym_count].offset = code_idx;
    strcpy(symtab[sym_count++].name, "putchar");

    emit1(0x55);                         /* push %rbp */
    emit1(0x48); emit1(0x89); emit1(0xe5); /* mov %rsp, %rbp */
    emit1(0x48); emit1(0x83); emit1(0xec); emit1(0x10); /* sub $16, %rsp */
    emit1(0x40); emit1(0x88); emit1(0x7d); emit1(0xff); /* mov %dil, -1(%rbp) */
    emit1(0xb8); emit4(4);              /* mov $4, %eax (sys_write) */
    emit1(0xbf); emit4(1);              /* mov $1, %edi (stdout) */
    emit1(0x48); emit1(0x8d); emit1(0x75); emit1(0xff); /* lea -1(%rbp), %rsi */
    emit1(0xba); emit4(1);              /* mov $1, %edx (1 byte) */
    emit1(0x0f); emit1(0x05);            /* syscall */
    emit1(0xc9);                         /* leave */
    emit1(0xc3);                         /* ret */

    /* 2. print(char *str): rdi -> sys_write(1, str, strlen) */
    symtab[sym_count].is_func = 1;
    symtab[sym_count].offset = code_idx;
    strcpy(symtab[sym_count++].name, "print");

    emit1(0x55);                         /* push %rbp */
    emit1(0x48); emit1(0x89); emit1(0xe5); /* mov %rsp, %rbp */
    emit1(0x53);                         /* push %rbx */
    emit1(0x48); emit1(0x89); emit1(0xfb); /* mov %rdi, %rbx */
    emit1(0x48); emit1(0x31); emit1(0xd2); /* xor %rdx, %rdx */
    int len_loop = code_idx;
    emit1(0x80); emit1(0x3c); emit1(0x13); emit1(0x00); /* cmpb $0, (%rbx,%rdx) */
    emit1(0x74); int jmp_len_done = code_idx; emit1(0); /* je done */
    emit1(0x48); emit1(0xff); emit1(0xc2); /* inc %rdx */
    emit1(0xeb); emit1((uint8_t)(len_loop - (code_idx + 1))); /* jmp len_loop */
    code[jmp_len_done] = (uint8_t)(code_idx - (jmp_len_done + 1));
    /* done */
    emit1(0x48); emit1(0x89); emit1(0xde); /* mov %rbx, %rsi */
    emit1(0xbf); emit4(1);              /* mov $1, %edi (stdout) */
    emit1(0xb8); emit4(4);              /* mov $4, %eax */
    emit1(0x0f); emit1(0x05);            /* syscall */
    emit1(0x5b);                         /* pop %rbx */
    emit1(0x5d);                         /* pop %rbp */
    emit1(0xc3);                         /* ret */

    /* 3. print_num(long n): вывод целых чисел через стек */
    symtab[sym_count].is_func = 1;
    symtab[sym_count].offset = code_idx;
    strcpy(symtab[sym_count++].name, "print_num");

    emit1(0x55);                         /* push %rbp */
    emit1(0x48); emit1(0x89); emit1(0xe5); /* mov %rsp, %rbp */
    emit1(0x48); emit1(0x83); emit1(0xec); emit1(0x40); /* sub $64, %rsp */
    emit1(0x48); emit1(0x89); emit1(0x7d); emit1(0xf8); /* mov %rdi, -8(%rbp) */

    /* Если n == 0, печатаем '0' */
    emit1(0x48); emit1(0x83); emit1(0x7d); emit1(0xf8); emit1(0x00); /* cmpq $0, -8(%rbp) */
    emit1(0x75); int jmp_not_zero = code_idx; emit1(0); /* jne not_zero */
    emit1(0xbf); emit4('0');
    emit1(0xe8); emit4((uint32_t)(symtab[0].offset - (code_idx + 4))); /* call putchar */
    emit1(0xc9); emit1(0xc3);            /* leave; ret */

    code[jmp_not_zero] = (uint8_t)(code_idx - (jmp_not_zero + 1));
    /* Если n < 0, выводим '-' и инвертируем */
    emit1(0x48); emit1(0x83); emit1(0x7d); emit1(0xf8); emit1(0x00); /* cmpq $0, -8(%rbp) */
    emit1(0x79); int jmp_not_neg = code_idx; emit1(0); /* jge not_neg */
    emit1(0xbf); emit4('-');
    emit1(0xe8); emit4((uint32_t)(symtab[0].offset - (code_idx + 4))); /* call putchar */
    emit1(0x48); emit1(0xf7); emit1(0x5d); emit1(0xf8); /* negq -8(%rbp) */

    code[jmp_not_neg] = (uint8_t)(code_idx - (jmp_not_neg + 1));
    emit1(0xc7); emit1(0x45); emit1(0xf4); emit4(0); /* movl $0, -12(%rbp) count = 0 */

    int loop_extract = code_idx;
    emit1(0x48); emit1(0x83); emit1(0x7d); emit1(0xf8); emit1(0x00); /* cmpq $0, -8(%rbp) */
    emit1(0x74); int jmp_extract_done = code_idx; emit1(0); /* je extract_done */

    emit1(0x48); emit1(0x8b); emit1(0x45); emit1(0xf8); /* mov -8(%rbp), %rax */
    emit1(0x48); emit1(0x31); emit1(0xd2);             /* xor %rdx, %rdx */
    emit1(0xb9); emit4(10);                             /* mov $10, %ecx */
    emit1(0x48); emit1(0xf7); emit1(0xf1);             /* div %rcx */
    emit1(0x48); emit1(0x89); emit1(0x45); emit1(0xf8); /* mov %rax, -8(%rbp) */

    emit1(0x8b); emit1(0x4d); emit1(0xf4);             /* mov -12(%rbp), %ecx */
    emit1(0x88); emit1(0x54); emit1(0x0d); emit1(0xc0); /* mov %dl, -64(%rbp,%rcx) */
    emit1(0xff); emit1(0x45); emit1(0xf4);             /* incl -12(%rbp) */
    emit1(0xeb); emit1((uint8_t)(loop_extract - (code_idx + 1))); /* jmp loop_extract */

    code[jmp_extract_done] = (uint8_t)(code_idx - (jmp_extract_done + 1));

    int loop_print = code_idx;
    emit1(0x83); emit1(0x7d); emit1(0xf4); emit1(0x00); /* cmpl $0, -12(%rbp) */
    emit1(0x7e); int jmp_print_done = code_idx; emit1(0); /* jle print_done */
    emit1(0xff); emit1(0x4d); emit1(0xf4);             /* decl -12(%rbp) */
    emit1(0x8b); emit1(0x4d); emit1(0xf4);             /* mov -12(%rbp), %ecx */
    emit1(0x0f); emit1(0xb6); emit1(0x7c); emit1(0x0d); emit1(0xc0); /* movzbl -64(%rbp,%rcx), %edi */
    emit1(0x83); emit1(0xc7); emit1('0');              /* add $'0', %edi */
    emit1(0xe8); emit4((uint32_t)(symtab[0].offset - (code_idx + 4))); /* call putchar */
    emit1(0xeb); emit1((uint8_t)(loop_print - (code_idx + 1))); /* jmp loop_print */

    code[jmp_print_done] = (uint8_t)(code_idx - (jmp_print_done + 1));
    emit1(0xc9); emit1(0xc3);            /* leave; ret */

    /* 4. exit(int code): dil -> sys_exit */
    symtab[sym_count].is_func = 1;
    symtab[sym_count].offset = code_idx;
    strcpy(symtab[sym_count++].name, "exit");
    emit1(0xb8); emit4(1);              /* mov $1, %eax (sys_exit) */
    emit1(0x0f); emit1(0x05);            /* syscall */
    emit1(0xc3);

    /* 5. syscall(nr, a1, a2, a3) */
    symtab[sym_count].is_func = 1;
    symtab[sym_count].offset = code_idx;
    strcpy(symtab[sym_count++].name, "syscall");
    emit1(0x48); emit1(0x89); emit1(0xf8); /* mov %rdi, %rax */
    emit1(0x48); emit1(0x89); emit1(0xf7); /* mov %rsi, %rdi */
    emit1(0x48); emit1(0x89); emit1(0xd6); /* mov %rdx, %rsi */
    emit1(0x48); emit1(0x89); emit1(0xca); /* mov %rcx, %rdx */
    emit1(0x0f); emit1(0x05);            /* syscall */
    emit1(0xc3);
}

/* Парсер выражений */
static void parse_expr(void);

static void parse_primary(void)
{
    if (tok == TOK_NUM) {
        emit1(0x48); emit1(0xb8); emit8((uint64_t)tok_num); /* mov $num, %rax */
        next_tok();
    } else if (tok == TOK_STR) {
        int d_off = data_idx;
        int slen = strlen(tok_str) + 1;
        memcpy(data + data_idx, tok_str, slen);
        data_idx += slen;

        emit1(0x48); emit1(0xb8); /* mov $imm64, %rax */
        str_relocs[str_reloc_count].code_pos = code_idx;
        str_relocs[str_reloc_count].data_offset = d_off;
        str_reloc_count++;
        emit8(0); /* Заглушка: будет скорректирована точным адресом при финализации */
        next_tok();
    } else if (tok == TOK_ID) {
        char name[32];
        strcpy(name, tok_id);
        next_tok();

        /* Вызов функции: name(...) */
        if (tok == TOK_LPAREN) {
            next_tok();
            int arg_count = 0;
            while (tok != TOK_RPAREN) {
                parse_expr();
                emit1(0x50); /* push %rax */
                arg_count++;
                if (tok == TOK_COMMA) next_tok();
                else break;
            }
            if (tok != TOK_RPAREN) error("Expected ')' in function call");
            next_tok();

            /* Загружаем аргументы в регистры ABI System V */
            if (arg_count >= 6) { emit1(0x41); emit1(0x59); } /* pop %r9 */
                if (arg_count >= 5) { emit1(0x41); emit1(0x58); } /* pop %r8 */
                    if (arg_count >= 4) { emit1(0x59); }             /* pop %rcx */
                        if (arg_count >= 3) { emit1(0x5a); }             /* pop %rdx */
                            if (arg_count >= 2) { emit1(0x5e); }             /* pop %rsi */
                                if (arg_count >= 1) { emit1(0x5f); }             /* pop %rdi */

                                    struct symbol *fn = find_global(name);
                                    if (fn && fn->is_func) {
                                        emit1(0xe8); emit4((uint32_t)(fn->offset - (code_idx + 4)));
                                    } else {
                                        emit1(0xe8);
                                        relocs[reloc_count].code_pos = code_idx;
                                        strcpy(relocs[reloc_count++].target_name, name);
                                        emit4(0);
                                    }
                                    return;
        }

        /* Доступ к переменной */
        struct symbol *sym = find_local(name);
        if (sym) {
            emit1(0x48); emit1(0x8b); emit1(0x85); emit4((uint32_t)sym->offset);
        } else {
            error("Undefined variable");
        }
    } else if (tok == TOK_LPAREN) {
        next_tok();
        parse_expr();
        if (tok != TOK_RPAREN) error("Expected ')'");
        next_tok();
    } else {
        error("Unexpected token in primary expression");
    }
}

static void parse_mul(void)
{
    parse_primary();
    while (tok == TOK_STAR || tok == TOK_SLASH || tok == TOK_PERCENT) {
        int op = tok;
        emit1(0x50); /* push %rax */
        next_tok();
        parse_primary();

        if (op == TOK_STAR) {
            emit1(0x59); /* pop %rcx (left) */
            emit1(0x48); emit1(0x0f); emit1(0xaf); emit1(0xc1); /* imul %rcx, %rax */
        } else if (op == TOK_SLASH || op == TOK_PERCENT) {
            emit1(0x48); emit1(0x89); emit1(0xc1); /* mov %rax, %rcx */
            emit1(0x58);                         /* pop left into %rax */
            emit1(0x48); emit1(0x99);            /* cqo */
            emit1(0x48); emit1(0xf7); emit1(0xf9); /* idiv %rcx */
            if (op == TOK_PERCENT) {
                emit1(0x48); emit1(0x89); emit1(0xd0); /* mov %rdx, %rax */
            }
        }
    }
}

static void parse_add(void)
{
    parse_mul();
    while (tok == TOK_PLUS || tok == TOK_MINUS) {
        int op = tok;
        emit1(0x50); /* push %rax (left) */
        next_tok();
        parse_mul();  /* right in %rax */
        emit1(0x59); /* pop %rcx (left) */

        if (op == TOK_PLUS) {
            emit1(0x48); emit1(0x01); emit1(0xc8); /* add %rcx, %rax */
        } else {
            emit1(0x48); emit1(0x29); emit1(0xc1); /* sub %rax, %rcx */
            emit1(0x48); emit1(0x89); emit1(0xc8); /* mov %rcx, %rax */
        }
    }
}

static void parse_rel(void)
{
    parse_add();
    while (tok == TOK_LT || tok == TOK_LE || tok == TOK_GT || tok == TOK_GE || tok == TOK_EQ || tok == TOK_NE) {
        int op = tok;
        emit1(0x50); /* push left */
        next_tok();
        parse_add(); /* right in %rax */
        emit1(0x59); /* pop left in %rcx */

        emit1(0x48); emit1(0x39); emit1(0xc1); /* cmp %rax, %rcx */
        if (op == TOK_EQ)      { emit1(0x0f); emit1(0x94); emit1(0xc0); } /* sete */
            else if (op == TOK_NE) { emit1(0x0f); emit1(0x95); emit1(0xc0); } /* setne */
                else if (op == TOK_LT) { emit1(0x0f); emit1(0x9c); emit1(0xc0); } /* setl */
                    else if (op == TOK_LE) { emit1(0x0f); emit1(0x9e); emit1(0xc0); } /* setle */
                        else if (op == TOK_GT) { emit1(0x0f); emit1(0x9f); emit1(0xc0); } /* setg */
                            else if (op == TOK_GE) { emit1(0x0f); emit1(0x9d); emit1(0xc0); } /* setge */
                                emit1(0x48); emit1(0x0f); emit1(0xb6); emit1(0xc0); /* movzbq %al, %rax */
    }
}

static void parse_expr(void)
{
    if (tok == TOK_ID) {
        char name[32];
        strcpy(name, tok_id);
        int saved_idx = src_idx;
        int saved_tok = tok;
        int saved_line = line_num;

        next_tok();
        if (tok == TOK_ASSIGN) {
            next_tok();
            parse_expr();
            struct symbol *sym = find_local(name);
            if (!sym) error("Assign to undeclared local variable");
            emit1(0x48); emit1(0x89); emit1(0x85); emit4((uint32_t)sym->offset);
            return;
        } else if (tok == TOK_PLUSPLUS) {
            next_tok();
            struct symbol *sym = find_local(name);
            if (!sym) error("Variable not found");
            emit1(0x48); emit1(0x8b); emit1(0x85); emit4((uint32_t)sym->offset);
            emit1(0x48); emit1(0xff); emit1(0xc0); /* inc %rax */
            emit1(0x48); emit1(0x89); emit1(0x85); emit4((uint32_t)sym->offset);
            return;
        }

        /* Откат токена */
        src_idx = saved_idx;
        tok = saved_tok;
        line_num = saved_line;
        strcpy(tok_id, name);
    }

    parse_rel();
}

static void parse_stmt(void);

static void parse_block(void)
{
    if (tok != TOK_LBRACE) error("Expected '{'");
    next_tok();
    while (tok != TOK_RBRACE && tok != TOK_EOF) {
        parse_stmt();
    }
    if (tok != TOK_RBRACE) error("Expected '}'");
    next_tok();
}

static void parse_stmt(void)
{
    if (tok == TOK_INT || tok == TOK_CHAR || tok == TOK_VOID) {
        next_tok();
        while (1) {
            if (tok != TOK_ID) error("Expected identifier in variable declaration");
            char name[32];
            strcpy(name, tok_id);
            next_tok();

            local_stack_offset -= 8;
            strcpy(locals[local_count].name, name);
            locals[local_count].offset = local_stack_offset;
            locals[local_count].is_global = 0;
            locals[local_count].is_func = 0;
            local_count++;

            if (tok == TOK_ASSIGN) {
                next_tok();
                parse_expr();
                emit1(0x48); emit1(0x89); emit1(0x85); emit4((uint32_t)local_stack_offset);
            }

            if (tok == TOK_COMMA) next_tok();
            else break;
        }
        if (tok != TOK_SEMI) error("Expected ';' after declaration");
        next_tok();
    } else if (tok == TOK_IF) {
        next_tok();
        if (tok != TOK_LPAREN) error("Expected '(' after if");
        next_tok();
        parse_expr();
        if (tok != TOK_RPAREN) error("Expected ')' after if condition");
        next_tok();

        emit1(0x48); emit1(0x85); emit1(0xc0); /* test %rax, %rax */
        emit1(0x0f); emit1(0x84); int jmp_else = code_idx; emit4(0); /* jz else */

        parse_stmt();

        if (tok == TOK_ELSE) {
            next_tok();
            emit1(0xe9); int jmp_end = code_idx; emit4(0); /* jmp end */
            fixup4(jmp_else, (uint32_t)(code_idx - (jmp_else + 4)));
            parse_stmt();
            fixup4(jmp_end, (uint32_t)(code_idx - (jmp_end + 4)));
        } else {
            fixup4(jmp_else, (uint32_t)(code_idx - (jmp_else + 4)));
        }
    } else if (tok == TOK_WHILE) {
        next_tok();
        int loop_start = code_idx;
        if (tok != TOK_LPAREN) error("Expected '(' after while");
        next_tok();
        parse_expr();
        if (tok != TOK_RPAREN) error("Expected ')' after while condition");
        next_tok();

        emit1(0x48); emit1(0x85); emit1(0xc0); /* test %rax, %rax */
        emit1(0x0f); emit1(0x84); int jmp_exit = code_idx; emit4(0); /* jz exit */

        parse_stmt();
        emit1(0xe9); emit4((uint32_t)(loop_start - (code_idx + 4))); /* jmp loop_start */
        fixup4(jmp_exit, (uint32_t)(code_idx - (jmp_exit + 4)));
    } else if (tok == TOK_FOR) {
        next_tok();
        if (tok != TOK_LPAREN) error("Expected '(' after for");
        next_tok();
        if (tok != TOK_SEMI) { parse_expr(); }
        if (tok != TOK_SEMI) error("Expected ';' in for");
        next_tok();

        int cond_start = code_idx;
        int jmp_exit = -1;
        if (tok != TOK_SEMI) {
            parse_expr();
            emit1(0x48); emit1(0x85); emit1(0xc0);
            emit1(0x0f); emit1(0x84); jmp_exit = code_idx; emit4(0);
        }
        if (tok != TOK_SEMI) error("Expected ';' in for");
        next_tok();

        uint8_t step_code[512];
        int step_len = 0;
        int orig_code_idx = code_idx;
        if (tok != TOK_RPAREN) {
            parse_expr();
            step_len = code_idx - orig_code_idx;
            memcpy(step_code, code + orig_code_idx, step_len);
            code_idx = orig_code_idx;
        }
        if (tok != TOK_RPAREN) error("Expected ')' in for");
        next_tok();

        parse_stmt();

        for (int si = 0; si < step_len; si++) emit1(step_code[si]);
        emit1(0xe9); emit4((uint32_t)(cond_start - (code_idx + 4)));

        if (jmp_exit != -1) {
            fixup4(jmp_exit, (uint32_t)(code_idx - (jmp_exit + 4)));
        }
    } else if (tok == TOK_RETURN) {
        next_tok();
        if (tok != TOK_SEMI) {
            parse_expr();
        }
        if (tok != TOK_SEMI) error("Expected ';' after return");
        next_tok();
        emit1(0xc9); /* leave */
        emit1(0xc3); /* ret */
    } else if (tok == TOK_LBRACE) {
        parse_block();
    } else {
        parse_expr();
        if (tok == TOK_SEMI) next_tok();
    }
}

static void parse_function(void)
{
    next_tok();
    if (tok != TOK_ID) error("Expected function name");

    char fn_name[32];
    strcpy(fn_name, tok_id);
    next_tok();

    if (tok != TOK_LPAREN) error("Expected '(' after function name");
    next_tok();

    local_count = 0;
    local_stack_offset = 0;

    int param_count = 0;
    while (tok != TOK_RPAREN && tok != TOK_EOF) {
        if (tok == TOK_INT || tok == TOK_CHAR || tok == TOK_VOID) next_tok();
        while (tok == TOK_STAR) next_tok();

        if (tok == TOK_ID) {
            local_stack_offset -= 8;
            strcpy(locals[local_count].name, tok_id);
            locals[local_count].offset = local_stack_offset;
            locals[local_count].is_global = 0;
            locals[local_count].is_func = 0;
            local_count++;
            param_count++;
            next_tok();
        }
        if (tok == TOK_COMMA) next_tok();
        else break;
    }
    if (tok != TOK_RPAREN) error("Expected ')' in parameter list");
    next_tok();

    symtab[sym_count].is_func = 1;
    symtab[sym_count].offset = code_idx;
    strcpy(symtab[sym_count++].name, fn_name);

    emit1(0x55);                         /* push %rbp */
    emit1(0x48); emit1(0x89); emit1(0xe5); /* mov %rsp, %rbp */
    emit1(0x48); emit1(0x81); emit1(0xec); int stack_patch = code_idx; emit4(0); /* sub $framesize, %rsp */

    if (param_count >= 1) { emit1(0x48); emit1(0x89); emit1(0x7d); emit1((uint8_t)(locals[0].offset)); }
    if (param_count >= 2) { emit1(0x48); emit1(0x89); emit1(0x75); emit1((uint8_t)(locals[1].offset)); }
    if (param_count >= 3) { emit1(0x48); emit1(0x89); emit1(0x55); emit1((uint8_t)(locals[2].offset)); }
    if (param_count >= 4) { emit1(0x48); emit1(0x89); emit1(0x4d); emit1((uint8_t)(locals[3].offset)); }
    if (param_count >= 5) { emit1(0x4c); emit1(0x89); emit1(0x45); emit1((uint8_t)(locals[4].offset)); }
    if (param_count >= 6) { emit1(0x4c); emit1(0x89); emit1(0x4d); emit1((uint8_t)(locals[5].offset)); }

    parse_block();

    emit1(0xc9); /* leave */
    emit1(0xc3); /* ret */

    int frame_size = (-local_stack_offset + 15) & ~15;
    if (frame_size < 32) frame_size = 32;
    fixup4(stack_patch, (uint32_t)frame_size);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: tcc <source.c> [-o output_binary]\n");
        printf("       cc  <source.c> [-o output_binary]\n");
        return 1;
    }

    const char *src_file = argv[1];
    const char *out_file = "a.out";

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_file = argv[i + 1];
            i++;
        }
    }

    printf("[TCC] Compiling %s -> %s (AMD64 Linux 0.01)\n", src_file, out_file);

    int in_fd = open(src_file, O_RDONLY);
    if (in_fd < 0) {
        printf("[TCC] Error: Cannot open input file '%s'\n", src_file);
        return 1;
    }

    int n = read(in_fd, src, sizeof(src) - 1);
    if (n < 0) n = 0;
    src[n] = '\0';
    close(in_fd);

    src_idx = 0;
    line_num = 1;
    code_idx = 0;
    data_idx = 0;
    sym_count = 0;
    reloc_count = 0;
    str_reloc_count = 0;

    /* 1. Генерируем входную точку _start */
    emit1(0x48); emit1(0x83); emit1(0xe4); emit1(0xf0); /* and $-16, %rsp */
    emit1(0xe8); int call_main_pos = code_idx; emit4(0); /* call main */
    emit1(0x48); emit1(0x89); emit1(0xc7);             /* mov %rax, %rdi */
    emit1(0xb8); emit4(1);                             /* mov $1, %eax (sys_exit) */
    emit1(0x0f); emit1(0x05);                           /* syscall */

    /* 2. Генерируем встроенные функции среды (putchar, print, print_num, exit, syscall) */
    emit_runtime();

    /* 3. Парсим функции программы */
    next_tok();
    while (tok != TOK_EOF) {
        parse_function();
    }

    /* 4. Разрешаем вызов main */
    struct symbol *main_sym = find_global("main");
    if (!main_sym) error("Function 'main' not defined in program");
    fixup4(call_main_pos, (uint32_t)(main_sym->offset - (call_main_pos + 4)));

    /* 5. Разрешаем релокации функций */
    for (int i = 0; i < reloc_count; i++) {
        struct symbol *tgt = find_global(relocs[i].target_name);
        if (!tgt) {
            printf("[TCC] Link error: Undefined reference to '%s'\n", relocs[i].target_name);
            return 1;
        }
        fixup4(relocs[i].code_pos, (uint32_t)(tgt->offset - (relocs[i].code_pos + 4)));
    }

    /* 6. Формируем структуру заголовка LINUS001 */
    int total_binary_size = sizeof(struct { uint64_t m, e, s; }) + code_idx + data_idx;

    struct {
        uint64_t magic;
        uint64_t entry;
        uint64_t size;
    } __attribute__((packed)) hdr;

    hdr.magic = EXEC_MAGIC_VAL;
    hdr.entry = BASE_VIRT_ADDR + sizeof(hdr);
    hdr.size  = total_binary_size;

    /* 7. Точное разрешение адресов строковых литералов (Data Base размещается ровно за Code) */
    uint64_t data_base_addr = BASE_VIRT_ADDR + sizeof(hdr) + code_idx;
    for (int i = 0; i < str_reloc_count; i++) {
        uint64_t s_addr = data_base_addr + str_relocs[i].data_offset;
        fixup8(str_relocs[i].code_pos, s_addr);
    }

    /* 8. Запись бинарника на диск / в RamFS */
    int out_fd = open(out_file, O_CREAT | O_WRONLY | O_TRUNC);
    if (out_fd < 0) {
        printf("[TCC] Error: Cannot create output file '%s'\n", out_file);
        return 1;
    }

    write(out_fd, &hdr, sizeof(hdr));
    write(out_fd, code, code_idx);
    if (data_idx > 0) {
        write(out_fd, data, data_idx);
    }
    close(out_fd);

    printf("[TCC] Successfully generated binary: %d bytes (Code: %d, Data: %d)\n",
           total_binary_size, code_idx, data_idx);
    return 0;
}
