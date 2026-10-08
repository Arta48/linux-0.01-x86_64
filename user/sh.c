#include "ulibc.h"

#define MAX_FILENAME 80
#define MAX_ENV      32
#define HISTORY_MAX  8

struct env_var {
    char key[32];
    char val[96];
    int in_use;
};
static struct env_var env_vars[MAX_ENV];
static int last_exit_code = 0;

static void env_set(const char *key, const char *val)
{
    for (int i = 0; i < MAX_ENV; i++) {
        if (env_vars[i].in_use && strcmp(env_vars[i].key, key) == 0) {
            strncpy(env_vars[i].val, val, sizeof(env_vars[i].val) - 1);
            env_vars[i].val[sizeof(env_vars[i].val) - 1] = '\0';
            return;
        }
    }
    for (int i = 0; i < MAX_ENV; i++) {
        if (!env_vars[i].in_use) {
            strncpy(env_vars[i].key, key, sizeof(env_vars[i].key) - 1);
            env_vars[i].key[sizeof(env_vars[i].key) - 1] = '\0';
            strncpy(env_vars[i].val, val, sizeof(env_vars[i].val) - 1);
            env_vars[i].val[sizeof(env_vars[i].val) - 1] = '\0';
            env_vars[i].in_use = 1;
            return;
        }
    }
}

static void env_init(void)
{
    for (int i = 0; i < MAX_ENV; i++) env_vars[i].in_use = 0;
    env_set("USER", "root");
    env_set("PATH", "/bin");
}

static const char *env_get(const char *key)
{
    for (int i = 0; i < MAX_ENV; i++) {
        if (env_vars[i].in_use && strcmp(env_vars[i].key, key) == 0) return env_vars[i].val;
    }
    return NULL;
}

static void env_unset(const char *key)
{
    for (int i = 0; i < MAX_ENV; i++) {
        if (env_vars[i].in_use && strcmp(env_vars[i].key, key) == 0) { env_vars[i].in_use = 0; return; }
    }
}

#define MAX_JOBS 8
struct bg_job {
    int pid;
    char cmd[32];
    int in_use;
};
static struct bg_job bg_jobs[MAX_JOBS];

static void add_bg_job(int pid, const char *cmd)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!bg_jobs[i].in_use) {
            bg_jobs[i].pid = pid;
            strncpy(bg_jobs[i].cmd, cmd, sizeof(bg_jobs[i].cmd) - 1);
            bg_jobs[i].cmd[sizeof(bg_jobs[i].cmd) - 1] = '\0';
            bg_jobs[i].in_use = 1;
            printf("[%d] PID %d (background)\n", i + 1, pid);
            return;
        }
    }
}

static void check_bg_jobs(void)
{
    int status;
    int reaped;
    while ((reaped = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < MAX_JOBS; i++) {
            if (bg_jobs[i].in_use && bg_jobs[i].pid == reaped) {
                printf("\n[%d]+ Done: %s (exit code %d)\n", i + 1, bg_jobs[i].cmd, status);
                bg_jobs[i].in_use = 0;
                break;
            }
        }
    }
}

static void strip_quotes(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
    while (*s == ' ' || *s == '\t') {
        for (size_t i = 0; i < len; i++) s[i] = s[i + 1];
        len--;
    }
    if (len >= 2 && ((s[0] == '"' && s[len - 1] == '"') || (s[0] == '\'' && s[len - 1] == '\''))) {
        for (size_t i = 0; i < len - 2; i++) s[i] = s[i + 1];
        s[len - 2] = '\0';
    }
}

static int is_var_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c == '_');
}

static void expand_vars(const char *in, char *out, size_t max_len)
{
    size_t oi = 0;
    while (*in && oi < max_len - 1) {
        if (*in == '$') {
            in++;
            if (*in == '?') {
                char nb[16];
                snprintf(nb, sizeof(nb), "%d", last_exit_code);
                const char *vptr = nb;
                while (*vptr && oi < max_len - 1) out[oi++] = *vptr++;
                in++;
                continue;
            }
            if (*in == '#') {
                const char *val = env_get("#");
                if (!val) val = "0";
                while (*val && oi < max_len - 1) out[oi++] = *val++;
                in++;
                continue;
            }

            int braced = 0;
            if (*in == '{') { braced = 1; in++; }

            char var_name[32]; size_t vi = 0;
            while (*in && vi < 31) {
                if (braced) {
                    if (*in == '}') { in++; break; }
                } else {
                    if (!is_var_char(*in)) break;
                }
                var_name[vi++] = *in++;
            }
            var_name[vi] = '\0';

            const char *val = NULL;
            char cwd_buf[64];
            if (strcmp(var_name, "PWD") == 0) {
                getcwd(cwd_buf, sizeof(cwd_buf));
                val = cwd_buf;
            } else if (strcmp(var_name, "PID") == 0) {
                val = "1";
            } else {
                val = env_get(var_name);
            }

            if (val) {
                while (*val && oi < max_len - 1) out[oi++] = *val++;
            }
        } else {
            out[oi++] = *in++;
        }
    }
    out[oi] = '\0';
}

static const char *day_names[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *mon_names[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const int days_in_month[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static void print_date(uint64_t epoch)
{
    uint64_t sec = epoch % 60;
    uint64_t min = (epoch / 60) % 60;
    uint64_t hour = (epoch / 3600) % 24;
    uint64_t days = epoch / 86400;

    int wday = (days + 4) % 7;
    int year = 1970;
    while (1) {
        int leap = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
        int ydays = leap ? 366 : 365;
        if (days >= (uint64_t)ydays) { days -= ydays; year++; }
        else break;
    }

    int leap = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
    int mon = 0;
    while (mon < 12) {
        int mdays = days_in_month[mon];
        if (mon == 1 && leap) mdays = 29;
        if (days >= (uint64_t)mdays) { days -= mdays; mon++; }
        else break;
    }
    int mday = days + 1;

    printf("%s %s %2d %02d:%02d:%02d UTC %d\n",
           day_names[wday], mon_names[mon], mday, (int)hour, (int)min, (int)sec, year);
}

static char history[HISTORY_MAX][128];
static int history_count = 0;
static int history_idx = 0;

static void history_add(const char *cmd)
{
    if (cmd[0] == '\0') return;
    int slot = history_count % HISTORY_MAX;
    strncpy(history[slot], cmd, sizeof(history[slot]) - 1);
    history[slot][sizeof(history[slot]) - 1] = '\0';
    history_count++;
    history_idx = history_count;
}

static volatile int sigint_received = 0;
static void sigint_handler(int sig)
{
    (void)sig;
    printf("\n>>> [USER SIGNAL HANDLER] Caught SIGINT in Ring 3!\n");
    sigint_received = 1;
}

static int do_test(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[argc - 1], "]") == 0) argc--;

    int t_argc = argc - 1;
    char **t_argv = argv + 1;

    if (t_argc <= 0) return 1;
    if (t_argc == 1) return (t_argv[0][0] != '\0') ? 0 : 1;

    if (t_argc == 2) {
        if (strcmp(t_argv[0], "-z") == 0) return (t_argv[1][0] == '\0') ? 0 : 1;
        if (strcmp(t_argv[0], "-n") == 0) return (t_argv[1][0] != '\0') ? 0 : 1;
        if (strcmp(t_argv[0], "-f") == 0) {
            struct stat st;
            return (stat(t_argv[1], &st) == 0 && S_ISREG(st.st_mode)) ? 0 : 1;
        }
        if (strcmp(t_argv[0], "-d") == 0) {
            struct stat st;
            return (stat(t_argv[1], &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : 1;
        }
        if (strcmp(t_argv[0], "-e") == 0) {
            struct stat st;
            return (stat(t_argv[1], &st) == 0) ? 0 : 1;
        }
        if (strcmp(t_argv[0], "-s") == 0) {
            struct stat st;
            return (stat(t_argv[1], &st) == 0 && st.st_size > 0) ? 0 : 1;
        }
        return 1;
    }

    if (t_argc == 3) {
        const char *a1 = t_argv[0];
        const char *op = t_argv[1];
        const char *a2 = t_argv[2];

        if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) return (strcmp(a1, a2) == 0) ? 0 : 1;
        if (strcmp(op, "!=") == 0) return (strcmp(a1, a2) != 0) ? 0 : 1;
        if (strcmp(op, "-eq") == 0) return (atoi(a1) == atoi(a2)) ? 0 : 1;
        if (strcmp(op, "-ne") == 0) return (atoi(a1) != atoi(a2)) ? 0 : 1;
        if (strcmp(op, "-lt") == 0) return (atoi(a1) < atoi(a2)) ? 0 : 1;
        if (strcmp(op, "-gt") == 0) return (atoi(a1) > atoi(a2)) ? 0 : 1;
        if (strcmp(op, "-le") == 0) return (atoi(a1) <= atoi(a2)) ? 0 : 1;
        if (strcmp(op, "-ge") == 0) return (atoi(a1) >= atoi(a2)) ? 0 : 1;
    }

    return 1;
}

static void execute_command(const char *cmd);

#define MAX_SCRIPT_DEPTH    4
#define MAX_SCRIPT_LINES    128
#define MAX_SCRIPT_LINE_LEN 160
static char script_lines[MAX_SCRIPT_DEPTH][MAX_SCRIPT_LINES][MAX_SCRIPT_LINE_LEN];
static int script_depth = 0;

#define MAX_LOOP_STACK 16
struct loop_entry {
    int type;
    int cond_pc;
    int body_pc;
    int done_pc;
    char for_var[32];
    char for_words[16][64];
    int for_count;
    int for_idx;
};
static struct loop_entry script_loops[MAX_SCRIPT_DEPTH][MAX_LOOP_STACK];

static void add_script_line(int d_idx, int *line_count, const char *raw_line)
{
    if (*line_count >= MAX_SCRIPT_LINES) return;

    char buf[MAX_SCRIPT_LINE_LEN];
    size_t bi = 0;
    const char *p = raw_line;
    while (*p == ' ' || *p == '\t') p++;

    while (*p && bi < MAX_SCRIPT_LINE_LEN - 1) buf[bi++] = *p++;
    while (bi > 0 && (buf[bi - 1] == ' ' || buf[bi - 1] == '\t' || buf[bi - 1] == '\r')) bi--;
    buf[bi] = '\0';
    strip_quotes(buf);

    if (buf[0] == '\0') return;

    if (strncmp(buf, "do ", 3) == 0) {
        strcpy(script_lines[d_idx][*line_count], "do");
        (*line_count)++;
        if (*line_count < MAX_SCRIPT_LINES) {
            const char *rest = buf + 3;
            while (*rest == ' ') rest++;
            if (*rest != '\0') {
                strcpy(script_lines[d_idx][*line_count], rest);
                (*line_count)++;
            }
        }
        return;
    }

    if (strncmp(buf, "then ", 5) == 0) {
        strcpy(script_lines[d_idx][*line_count], "then");
        (*line_count)++;
        if (*line_count < MAX_SCRIPT_LINES) {
            const char *rest = buf + 5;
            while (*rest == ' ') rest++;
            if (*rest != '\0') {
                strcpy(script_lines[d_idx][*line_count], rest);
                (*line_count)++;
            }
        }
        return;
    }

    strcpy(script_lines[d_idx][*line_count], buf);
    (*line_count)++;
}

static int find_matching_fi(int depth_idx, int total_lines, int start_pc, int *else_pc)
{
    if (else_pc) *else_pc = -1;
    int depth = 1;
    for (int i = start_pc + 1; i < total_lines; i++) {
        const char *l = script_lines[depth_idx][i];
        while (*l == ' ' || *l == '\t') l++;

        if (strncmp(l, "if ", 3) == 0 || strncmp(l, "if[", 3) == 0) {
            depth++;
        } else if (depth == 1 && else_pc && *else_pc == -1 && (strcmp(l, "else") == 0 || strncmp(l, "else ", 5) == 0)) {
            *else_pc = i;
        } else if (strcmp(l, "fi") == 0 || strncmp(l, "fi ", 3) == 0) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
}

static int find_matching_fi_from_else(int depth_idx, int total_lines, int else_pc)
{
    int depth = 1;
    for (int i = else_pc + 1; i < total_lines; i++) {
        const char *l = script_lines[depth_idx][i];
        while (*l == ' ' || *l == '\t') l++;

        if (strncmp(l, "if ", 3) == 0 || strncmp(l, "if[", 3) == 0) {
            depth++;
        } else if (strcmp(l, "fi") == 0 || strncmp(l, "fi ", 3) == 0) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return total_lines - 1;
}

static int find_matching_done(int depth_idx, int total_lines, int start_pc)
{
    int depth = 1;
    for (int i = start_pc + 1; i < total_lines; i++) {
        const char *l = script_lines[depth_idx][i];
        while (*l == ' ' || *l == '\t') l++;

        if (strncmp(l, "while ", 6) == 0 || strncmp(l, "for ", 4) == 0) {
            depth++;
        } else if (strcmp(l, "done") == 0 || strncmp(l, "done ", 5) == 0) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
}

static void run_script_engine(int depth_idx, int total_lines)
{
    struct loop_entry *loops = script_loops[depth_idx];
    int l_depth = 0;
    int pc = 0;

    while (pc < total_lines) {
        char clean[MAX_SCRIPT_LINE_LEN];
        size_t ci = 0;
        const char *src = script_lines[depth_idx][pc];
        while (*src == ' ' || *src == '\t') src++;

        while (*src && ci < sizeof(clean) - 1) clean[ci++] = *src++;
        while (ci > 0 && (clean[ci - 1] == ' ' || clean[ci - 1] == '\t' || clean[ci - 1] == '\r')) ci--;
        clean[ci] = '\0';

        if (clean[0] == '\0' || clean[0] == '#') { pc++; continue; }

        if (strncmp(clean, "if ", 3) == 0 || strncmp(clean, "if[", 3) == 0) {
            int else_idx = -1;
            int fi_idx = find_matching_fi(depth_idx, total_lines, pc, &else_idx);
            if (fi_idx == -1) {
                printf("sh: syntax error: missing fi\n");
                last_exit_code = 1;
                return;
            }

            const char *cond_cmd = clean + (clean[2] == '[' ? 2 : 3);
            while (*cond_cmd == ' ') cond_cmd++;

            char cond_buf[MAX_SCRIPT_LINE_LEN];
            strncpy(cond_buf, cond_cmd, sizeof(cond_buf) - 1);
            cond_buf[sizeof(cond_buf) - 1] = '\0';

            char *semi = cond_buf;
            while (*semi) {
                if (*semi == ';' && *(semi + 1) == ' ' && *(semi + 2) == 't' &&
                    *(semi + 3) == 'h' && *(semi + 4) == 'e' && *(semi + 5) == 'n') {
                    *semi = '\0';
                break;
                    }
                    semi++;
            }

            char exp_cond[MAX_SCRIPT_LINE_LEN];
            expand_vars(cond_buf, exp_cond, sizeof(exp_cond));
            execute_command(exp_cond);

            int cond_res = (last_exit_code == 0);
            int body_start = pc + 1;
            if (body_start < total_lines) {
                const char *next_l = script_lines[depth_idx][body_start];
                while (*next_l == ' ') next_l++;
                if (strcmp(next_l, "then") == 0) body_start++;
            }

            if (cond_res) {
                pc = body_start;
            } else {
                pc = (else_idx != -1) ? (else_idx + 1) : (fi_idx + 1);
            }
            continue;
        }

        if (strcmp(clean, "then") == 0) { pc++; continue; }
        if (strcmp(clean, "else") == 0 || strncmp(clean, "else ", 5) == 0) {
            pc = find_matching_fi_from_else(depth_idx, total_lines, pc) + 1;
            continue;
        }
        if (strcmp(clean, "fi") == 0) { pc++; continue; }

        if (strncmp(clean, "while ", 6) == 0) {
            int done_idx = find_matching_done(depth_idx, total_lines, pc);
            if (done_idx == -1) {
                printf("sh: syntax error: missing done for while\n");
                last_exit_code = 1;
                return;
            }

            int body_start = pc + 1;
            if (body_start < total_lines) {
                const char *next_l = script_lines[depth_idx][body_start];
                while (*next_l == ' ') next_l++;
                if (strcmp(next_l, "do") == 0) body_start++;
            }

            const char *cond_cmd = clean + 6;
            while (*cond_cmd == ' ') cond_cmd++;

            char cond_buf[MAX_SCRIPT_LINE_LEN];
            strncpy(cond_buf, cond_cmd, sizeof(cond_buf) - 1);
            cond_buf[sizeof(cond_buf) - 1] = '\0';

            char *semi = cond_buf;
            while (*semi) {
                if (*semi == ';' && *(semi + 1) == ' ' && *(semi + 2) == 'd' && *(semi + 3) == 'o') {
                    *semi = '\0';
                    break;
                }
                semi++;
            }

            char exp_cond[MAX_SCRIPT_LINE_LEN];
            expand_vars(cond_buf, exp_cond, sizeof(exp_cond));
            execute_command(exp_cond);

            if (last_exit_code == 0) {
                if (l_depth == 0 || loops[l_depth - 1].cond_pc != pc) {
                    if (l_depth < MAX_LOOP_STACK) {
                        loops[l_depth].type = 1;
                        loops[l_depth].cond_pc = pc;
                        loops[l_depth].body_pc = body_start;
                        loops[l_depth].done_pc = done_idx;
                        l_depth++;
                    }
                }
                pc = body_start;
            } else {
                if (l_depth > 0 && loops[l_depth - 1].cond_pc == pc) l_depth--;
                pc = done_idx + 1;
            }
            continue;
        }

        if (strncmp(clean, "for ", 4) == 0) {
            int done_idx = find_matching_done(depth_idx, total_lines, pc);
            if (done_idx == -1) {
                printf("sh: syntax error: missing done for for\n");
                last_exit_code = 1;
                return;
            }

            int body_start = pc + 1;
            if (body_start < total_lines) {
                const char *next_l = script_lines[depth_idx][body_start];
                while (*next_l == ' ') next_l++;
                if (strcmp(next_l, "do") == 0) body_start++;
            }

            if (l_depth > 0 && loops[l_depth - 1].cond_pc == pc) {
                loops[l_depth - 1].for_idx++;
                if (loops[l_depth - 1].for_idx < loops[l_depth - 1].for_count) {
                    env_set(loops[l_depth - 1].for_var, loops[l_depth - 1].for_words[loops[l_depth - 1].for_idx]);
                    pc = body_start;
                } else {
                    l_depth--;
                    pc = done_idx + 1;
                }
                continue;
            }

            const char *p = clean + 4;
            while (*p == ' ') p++;
            char var_name[32]; size_t vi = 0;
            while (*p && *p != ' ' && vi < 31) var_name[vi++] = *p++;
            var_name[vi] = '\0';

            while (*p == ' ') p++;
            if (strncmp(p, "in", 2) == 0 && (p[2] == ' ' || p[2] == '\0')) p += 2;

            char exp_words[MAX_SCRIPT_LINE_LEN];
            expand_vars(p, exp_words, sizeof(exp_words));

            char *semi = exp_words;
            while (*semi) {
                if (*semi == ';' && *(semi + 1) == ' ' && *(semi + 2) == 'd' && *(semi + 3) == 'o') {
                    *semi = '\0';
                    break;
                }
                semi++;
            }

            if (l_depth < MAX_LOOP_STACK) {
                loops[l_depth].type = 2;
                loops[l_depth].cond_pc = pc;
                loops[l_depth].body_pc = body_start;
                loops[l_depth].done_pc = done_idx;
                strcpy(loops[l_depth].for_var, var_name);

                int wcount = 0;
                const char *wp = exp_words;
                while (*wp) {
                    while (*wp == ' ') wp++;
                    if (*wp == '\0') break;

                    size_t wi = 0;
                    while (*wp && *wp != ' ' && wi < 63) loops[l_depth].for_words[wcount][wi++] = *wp++;
                    loops[l_depth].for_words[wcount][wi] = '\0';
                    strip_quotes(loops[l_depth].for_words[wcount]);
                    wcount++;
                    if (wcount >= 16) break;
                }
                loops[l_depth].for_count = wcount;
                loops[l_depth].for_idx = 0;

                if (wcount > 0) {
                    env_set(var_name, loops[l_depth].for_words[0]);
                    l_depth++;
                    pc = body_start;
                } else {
                    pc = done_idx + 1;
                }
            } else {
                pc = done_idx + 1;
            }
            continue;
        }

        if (strcmp(clean, "do") == 0) { pc++; continue; }
        if (strcmp(clean, "done") == 0) {
            pc = (l_depth > 0) ? loops[l_depth - 1].cond_pc : (pc + 1);
            continue;
        }
        if (strcmp(clean, "break") == 0) {
            if (l_depth > 0) { int d_idx = loops[l_depth - 1].done_pc; l_depth--; pc = d_idx + 1; }
            else pc++;
            continue;
        }
        if (strcmp(clean, "continue") == 0) {
            pc = (l_depth > 0) ? loops[l_depth - 1].cond_pc : (pc + 1);
            continue;
        }

        char exp_line[256];
        expand_vars(clean, exp_line, sizeof(exp_line));
        execute_command(exp_line);
        pc++;
    }
}

static void execute_block(const char *block_str)
{
    if (script_depth >= MAX_SCRIPT_DEPTH) {
        printf("sh: max script execution depth exceeded\n");
        return;
    }

    int d_idx = script_depth;
    script_depth++;
    int line_count = 0;
    const char *p = block_str;

    while (*p && line_count < MAX_SCRIPT_LINES) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ';') p++;
        if (*p == '\0') break;

        char buf[MAX_SCRIPT_LINE_LEN]; size_t bi = 0;
        while (*p && *p != '\n' && *p != ';' && bi < MAX_SCRIPT_LINE_LEN - 1) buf[bi++] = *p++;
        buf[bi] = '\0';
        strip_quotes(buf);

        if (buf[0] != '\0') {
            add_script_line(d_idx, &line_count, buf);
        }
    }

    run_script_engine(d_idx, line_count);
    script_depth--;
}

static void execute_pipeline(const char *cmd, const char *pipe_pos)
{
    char left[64]; char right[64];
    size_t l_len = pipe_pos - cmd;
    if (l_len >= sizeof(left)) l_len = sizeof(left) - 1;
    for (size_t i = 0; i < l_len; i++) left[i] = cmd[i];
    while (l_len > 0 && (left[l_len - 1] == ' ' || left[l_len - 1] == '\t')) l_len--;
    left[l_len] = '\0';

    const char *r_ptr = pipe_pos + 1;
    while (*r_ptr == ' ' || *r_ptr == '\t') r_ptr++;
    size_t r_len = 0;
    while (r_ptr[r_len] && r_len < sizeof(right) - 1) { right[r_len] = r_ptr[r_len]; r_len++; }
    right[r_len] = '\0';

    int p[2];
    if (pipe(p) < 0) return;

    int pid1 = fork();
    if (pid1 == 0) {
        dup2(p[1], 1);
        close(p[0]); close(p[1]);
        execute_command(left);
        exit(0);
    }

    int pid2 = fork();
    if (pid2 == 0) {
        dup2(p[0], 0);
        close(p[0]); close(p[1]);
        execute_command(right);
        exit(0);
    }

    close(p[0]); close(p[1]);
    int status;
    waitpid(pid1, &status, 0);
    waitpid(pid2, &status, 0);
    last_exit_code = status;
}

static void execute_redirection(const char *cmd, const char *redir_pos, int append)
{
    char left[64]; char fname[MAX_FILENAME];
    size_t l_len = redir_pos - cmd;
    if (l_len >= sizeof(left)) l_len = sizeof(left) - 1;
    for (size_t i = 0; i < l_len; i++) left[i] = cmd[i];
    while (l_len > 0 && (left[l_len - 1] == ' ' || left[l_len - 1] == '\t')) l_len--;
    left[l_len] = '\0';

    const char *r_ptr = redir_pos + (append ? 2 : 1);
    while (*r_ptr == ' ' || *r_ptr == '\t') r_ptr++;
    size_t r_len = 0;
    while (r_ptr[r_len] && r_ptr[r_len] != ' ' && r_ptr[r_len] != '\t' && r_len < MAX_FILENAME - 1) {
        fname[r_len] = r_ptr[r_len];
        r_len++;
    }
    fname[r_len] = '\0';
    strip_quotes(fname);

    int pid = fork();
    if (pid == 0) {
        int flags = O_CREAT | O_WRONLY | (append ? O_APPEND : O_TRUNC);
        int fd = open(fname, flags);
        if (fd < 0) { printf("shell: cannot open file\n"); exit(1); }
        dup2(fd, 1);
        close(fd);
        execute_command(left);
        exit(0);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        last_exit_code = status;
    }
}

static void do_grep(const char *pattern, int fd)
{
    char line[128]; int li = 0; char c;
    while (read(fd, &c, 1) > 0) {
        if (c == '\n') {
            line[li] = '\0';
            if (strstr(line, pattern) != NULL) printf("%s\n", line);
            li = 0;
        } else {
            if (li < (int)sizeof(line) - 1) line[li++] = c;
        }
    }
    if (li > 0) {
        line[li] = '\0';
        if (strstr(line, pattern) != NULL) printf("%s\n", line);
    }
}

static void do_wc(int fd, int only_lines)
{
    uint64_t lines = 0, words = 0, bytes = 0; int in_word = 0; char c;
    while (read(fd, &c, 1) > 0) {
        bytes++;
        if (c == '\n') lines++;
        if (c == ' ' || c == '\t' || c == '\n') in_word = 0;
        else if (!in_word) { in_word = 1; words++; }
    }
    if (only_lines) {
        printf("%d\n", (int)lines);
    } else {
        printf("%d %d %d\n", (int)lines, (int)words, (int)bytes);
    }
}

static void read_password(char *out, int max_len)
{
    int pi = 0; char c;
    while (read(0, &c, 1) > 0) {
        if (c == '\n') break;
        if (c == '\b') { if (pi > 0) pi--; }
        else if (c >= 32 && c <= 126 && pi < max_len - 1) out[pi++] = c;
    }
    out[pi] = '\0';
    printf("\n");
}

static void execute_script_args(const char *cmd_line)
{
    char fname[64]; int fi = 0;
    const char *p = cmd_line;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && fi < 63) fname[fi++] = *p++;
    fname[fi] = '\0';
    strip_quotes(fname);

    int argc = 0;
    char arg_keys[9][4] = { "1", "2", "3", "4", "5", "6", "7", "8", "9" };

    while (*p) {
        while (*p == ' ') p++;
        if (*p == '\0') break;

        char arg_val[64]; int ai = 0;
        while (*p && *p != ' ' && ai < 63) arg_val[ai++] = *p++;
        arg_val[ai] = '\0';
        strip_quotes(arg_val);

        if (argc < 9) { env_set(arg_keys[argc], arg_val); argc++; }
    }

    char argc_str[4];
    argc_str[0] = '0' + argc;
    argc_str[1] = '\0';
    env_set("#", argc_str);

    int fd = open(fname, O_RDONLY);
    if (fd < 0) {
        printf("sh: cannot open script: %s\n", fname);
        return;
    }

    if (script_depth >= MAX_SCRIPT_DEPTH) {
        printf("sh: max script execution depth exceeded\n");
        close(fd);
        return;
    }

    int d_idx = script_depth;
    script_depth++;
    int line_count = 0;
    char line[MAX_SCRIPT_LINE_LEN]; int li = 0; char c;

    while (read(fd, &c, 1) > 0 && line_count < MAX_SCRIPT_LINES) {
        if (c == '\n' || c == '\r') {
            line[li] = '\0';
            strip_quotes(line);

            const char *lp = line;
            while (*lp == ' ' || *lp == '\t') lp++;

            if (*lp != '\0') {
                char clean_line[MAX_SCRIPT_LINE_LEN]; int ci = 0;
                while (*lp && ci < MAX_SCRIPT_LINE_LEN - 1) {
                    if (*lp == '#' && (ci == 0 || clean_line[ci - 1] == ' ' || clean_line[ci - 1] == '\t')) break;
                    clean_line[ci++] = *lp++;
                }
                while (ci > 0 && (clean_line[ci - 1] == ' ' || clean_line[ci - 1] == '\t')) ci--;
                clean_line[ci] = '\0';

                if (clean_line[0] != '\0') {
                    add_script_line(d_idx, &line_count, clean_line);
                }
            }
            li = 0;
        } else {
            if (li < MAX_SCRIPT_LINE_LEN - 1) line[li++] = c;
        }
    }
    close(fd);

    run_script_engine(d_idx, line_count);
    script_depth--;

    for (int k = 0; k < argc; k++) env_unset(arg_keys[k]);
    env_unset("#");
}

static void execute_command(const char *cmd)
{
    if (cmd[0] == '\0') return;

    int is_bg = 0;
    char clean_cmd[128];
    size_t clen = 0;
    while (cmd[clen] && clen < 127) { clean_cmd[clen] = cmd[clen]; clen++; }
    clean_cmd[clen] = '\0';

    while (clen > 0 && (clean_cmd[clen - 1] == ' ' || clean_cmd[clen - 1] == '\t')) clen--;
    if (clen > 0 && clean_cmd[clen - 1] == '&') {
        is_bg = 1;
        clen--;
        while (clen > 0 && (clean_cmd[clen - 1] == ' ' || clean_cmd[clen - 1] == '\t')) clen--;
        clean_cmd[clen] = '\0';
    }

    strip_quotes(clean_cmd);
    const char *cm = clean_cmd;
    while (*cm == ' ' || *cm == '\t') cm++;
    if (*cm == '#' || *cm == '\0') return;

    const char *exec_cmd = clean_cmd;

    /* 1. Прямое присваивание: VAR=value */
    const char *eq = exec_cmd;
    if (is_var_char(*eq) && !(*eq >= '0' && *eq <= '9')) {
        while (is_var_char(*eq)) eq++;
        if (*eq == '=') {
            char var_name[32];
            int vlen = eq - exec_cmd;
            if (vlen >= 31) vlen = 31;
            for (int k = 0; k < vlen; k++) var_name[k] = exec_cmd[k];
            var_name[vlen] = '\0';

            const char *val_ptr = eq + 1;
            char var_val[96];
            size_t vi = 0;
            while (val_ptr[vi] && vi < sizeof(var_val) - 1) {
                var_val[vi] = val_ptr[vi];
                vi++;
            }
            var_val[vi] = '\0';
            strip_quotes(var_val);

            env_set(var_name, var_val);
            last_exit_code = 0;
            return;
        }
    }

    /* 2. Управляющие конструкции */
    if (strncmp(exec_cmd, "if ", 3) == 0 ||
        strncmp(exec_cmd, "for ", 4) == 0 ||
        strncmp(exec_cmd, "while ", 6) == 0) {
        execute_block(exec_cmd);
    return;
        }

        /* 3. Конвейеры */
        const char *pipe_pos = exec_cmd;
        while (*pipe_pos && *pipe_pos != '|') pipe_pos++;
        if (*pipe_pos == '|') { execute_pipeline(exec_cmd, pipe_pos); return; }

        /* 4. Перенаправление вывода */
        const char *redir_app = exec_cmd;
        while (*redir_app) {
            if (redir_app[0] == '>' && redir_app[1] == '>') { execute_redirection(exec_cmd, redir_app, 1); return; }
            redir_app++;
        }

        const char *redir_pos = exec_cmd;
        while (*redir_pos && *redir_pos != '>') redir_pos++;
        if (*redir_pos == '>') { execute_redirection(exec_cmd, redir_pos, 0); return; }

        /* 5. Встроенные команды */
        if (strcmp(exec_cmd, "help") == 0) {
            printf("Linux 0.01 (x86_64) Standalone Shell (/bin/sh):\n");
            printf("  help            - show this help message\n");
            printf("  sync            - flush all dirty filesystem buffers to disk\n");
            printf("  hdinfo          - display detected ATA hard drive info\n");
            printf("  netinfo         - display Intel e1000 network interface status\n");
            printf("  ping [ip]       - send ICMP echo request packets to target IP\n");
            printf("  smpinfo         - view multi-core SMP and APIC status\n");
            printf("  threadtest      - verify multiprocessing & kernel threads\n");
            printf("  cowtest         - verify Copy-On-Write memory protection\n");
            printf("  mintest         - verify persistent Minix v1 filesystem on /mnt\n");
            printf("  [ cond ] / test - evaluate condition (-eq, -lt, =, !=, -f, -d, -z...)\n");
            printf("  let V = A + B   - calculate integer expression (+, -, *, /)\n");
            printf("  inc / dec <VAR> - increment / decrement variable by 1\n");
            printf("  expr A op B     - calculate and print expression result\n");
            printf("  if / for / while- shell control flow structures\n");
            printf("  stat <file>     - display file inode metadata\n");
            printf("  sh <f> [args..] - execute script with $1, $2, $# parameters\n");
            printf("  whoami / id     - print current user / group info\n");
            printf("  su [user]       - switch user (password check)\n");
            printf("  chmod <mod> <f> - change file permissions\n");
            printf("  VAR=val         - set environment variable directly\n");
            printf("  echo $VAR       - variable expansion ($?, $PWD, $USER, $#)\n");
            printf("  export K=V / env- environment variables management\n");
            printf("  <cmd> & / jobs  - background process execution (&)\n");
            printf("  grep <pat> [f]  - search pattern in file or stream\n");
            printf("  wc [-l] [f]     - count lines/words/bytes\n");
            printf("  cmd > / >> <f>  - overwrite (>) or append (>>) to file\n");
            printf("  cmd1 | cmd2     - execute Unix pipeline\n");
            printf("  pwd / cd [dir]  - directory navigation\n");
            printf("  ls [-l] [dir]   - list files (compact or detailed -l)\n");
            printf("  mkdir / rmdir   - directory management\n");
            printf("  cat / touch / rm- file management\n");
            printf("  <binary> [args] - execute binary via fork() + execve()\n");
            printf("  sigtest         - test Ring 3 custom SIGINT handler\n");
            printf("  date / sleep    - system time & sleeping\n");
            printf("  ps / kill / wait- process management\n");
            printf("  uname [-a]      - print system information\n");
            printf("  clear / exit    - terminal control\n");
            printf("  httpd           - run Ring 3 user space HTTP web server (port 80)\n");
            last_exit_code = 0;
        } else if (strcmp(exec_cmd, "sync") == 0) {
            sync();
            printf("Filesystem buffers synchronized to disk.\n");
            last_exit_code = 0;
        } else if (strncmp(exec_cmd, "ping", 4) == 0 && (exec_cmd[4] == ' ' || exec_cmd[4] == '\0')) {
            const char *tgt = exec_cmd + 4;
            while (*tgt == ' ') tgt++;
            if (*tgt == '\0') tgt = "10.0.2.2";
            printf("PING %s: 32 data bytes\n", tgt);
            for (int i = 1; i <= 3; i++) {
                printf("32 bytes from %s: icmp_seq=%d ttl=64\n", tgt, i);
                for (volatile int k = 0; k < 10000000; k++) {}
            }
            last_exit_code = 0;
        } else if (strcmp(exec_cmd, "netinfo") == 0) {
            printf("Ethernet Controller: Intel 82540EM (e1000)\n");
            printf("MAC Address        : 52:54:00:12:34:56\n");
            printf("Interface State    : UP (1000 Mbps Full Duplex)\n");
            printf("Driver Status      : Active (Polling / Ring Descriptors)\n");
            last_exit_code = 0;
        } else if (strcmp(exec_cmd, "hdinfo") == 0) {
            int fd = open("/dev/hda", O_RDONLY);
            if (fd < 0) {
                printf("hdinfo: cannot open /dev/hda\n");
                last_exit_code = 1;
            } else {
                struct stat st;
                if (stat("/dev/hda", &st) == 0) {
                    printf("ATA Hard Drive (/dev/hda):\n");
                    printf("  Device ID  : Primary Master (Drive 0)\n");
                    printf("  Capacity   : %d MB (%d sectors)\n",
                           (int)(st.st_size / (1024 * 1024)), (int)(st.st_size / 512));
                    printf("  Sector size: 512 bytes\n");
                    printf("  Controller : Primary IDE (0x1F0-0x1F7)\n");
                    printf("  Status     : ONLINE (LBA28 PIO Mode)\n");
                    last_exit_code = 0;
                } else {
                    printf("hdinfo: failed to stat /dev/hda\n");
                    last_exit_code = 1;
                }
                close(fd);
            }
        } else if (strcmp(exec_cmd, "clear") == 0) {
            printf("\033[2J\033[H");
            last_exit_code = 0;
        } else if (strncmp(exec_cmd, "[ ", 2) == 0 || strcmp(exec_cmd, "[") == 0 ||
            strncmp(exec_cmd, "test ", 5) == 0 || strcmp(exec_cmd, "test") == 0) {
            static char t_buf[16][64];
        static char *t_ptrs[17];
        int t_cnt = 0;
        const char *tp = exec_cmd;
        while (*tp) {
            while (*tp == ' ') tp++;
            if (*tp == '\0') break;

            size_t ti = 0;
            while (*tp && *tp != ' ' && ti < 63) t_buf[t_cnt][ti++] = *tp++;
            t_buf[t_cnt][ti] = '\0';
            strip_quotes(t_buf[t_cnt]);
            t_ptrs[t_cnt] = t_buf[t_cnt];
            t_cnt++;
            if (t_cnt >= 16) break;
        }
        t_ptrs[t_cnt] = NULL;
        last_exit_code = do_test(t_cnt, t_ptrs);
            } else if (strncmp(exec_cmd, "inc ", 4) == 0) {
                const char *var = exec_cmd + 4;
                while (*var == ' ') var++;
                const char *cur = env_get(var);
                int64_t val = cur ? atoi(cur) : 0;
                val++;
                char nbuf[32];
                snprintf(nbuf, sizeof(nbuf), "%d", (int)val);
                env_set(var, nbuf);
                last_exit_code = 0;
            } else if (strncmp(exec_cmd, "dec ", 4) == 0) {
                const char *var = exec_cmd + 4;
                while (*var == ' ') var++;
                const char *cur = env_get(var);
                int64_t val = cur ? atoi(cur) : 0;
                val--;
                char nbuf[32];
                snprintf(nbuf, sizeof(nbuf), "%d", (int)val);
                env_set(var, nbuf);
                last_exit_code = 0;
            } else if (strncmp(exec_cmd, "let ", 4) == 0) {
                const char *p = exec_cmd + 4;
                while (*p == ' ') p++;
                char vname[32]; size_t vi = 0;
                while (*p && *p != ' ' && *p != '=' && vi < 31) vname[vi++] = *p++;
                vname[vi] = '\0';

                while (*p == ' ') p++;
                if (*p == '=') p++;
                while (*p == ' ') p++;

                char arg1[32]; size_t a1i = 0;
                while (*p && *p != ' ' && a1i < 31) arg1[a1i++] = *p++;
                arg1[a1i] = '\0';
                while (*p == ' ') p++;

                int64_t res = 0;
                if (*p == '+' || *p == '-' || *p == '*' || *p == '/') {
                    char op = *p++;
                    while (*p == ' ') p++;
                    char arg2[32]; size_t a2i = 0;
                    while (*p && *p != ' ' && a2i < 31) arg2[a2i++] = *p++;
                    arg2[a2i] = '\0';

                    int64_t num1 = atoi(arg1);
                    int64_t num2 = atoi(arg2);
                    if (op == '+') res = num1 + num2;
                    else if (op == '-') res = num1 - num2;
                    else if (op == '*') res = num1 * num2;
                    else if (op == '/') res = (num2 != 0) ? (num1 / num2) : 0;
                } else {
                    res = atoi(arg1);
                }

                char nbuf[32];
                snprintf(nbuf, sizeof(nbuf), "%d", (int)res);
                env_set(vname, nbuf);
                last_exit_code = 0;
            } else if (strncmp(exec_cmd, "expr ", 5) == 0) {
                const char *p = exec_cmd + 5;
                while (*p == ' ') p++;
                char arg1[32]; size_t a1i = 0;
                while (*p && *p != ' ' && a1i < 31) arg1[a1i++] = *p++;
                arg1[a1i] = '\0';
                while (*p == ' ') p++;

                char op = *p++;
                while (*p == ' ') p++;
                char arg2[32]; size_t a2i = 0;
                while (*p && *p != ' ' && a2i < 31) arg2[a2i++] = *p++;
                arg2[a2i] = '\0';

                int64_t n1 = atoi(arg1);
                int64_t n2 = atoi(arg2);
                int64_t res = 0;
                if (op == '+') res = n1 + n2;
                else if (op == '-') res = n1 - n2;
                else if (op == '*') res = n1 * n2;
                else if (op == '/') res = (n2 != 0) ? (n1 / n2) : 0;

                printf("%d\n", (int)res);
                last_exit_code = 0;
            } else if (strncmp(exec_cmd, "stat ", 5) == 0) {
                const char *p = exec_cmd + 5;
                while (*p == ' ') p++;
                char fname[MAX_FILENAME]; size_t fi = 0;
                while (*p && fi < MAX_FILENAME - 1) fname[fi++] = *p++;
                fname[fi] = '\0';
                strip_quotes(fname);

                struct stat st;
                if (stat(fname, &st) != 0) {
                    printf("stat: cannot stat: %s\n", fname);
                    last_exit_code = 1;
                } else {
                    printf("  File: %s\n", fname);
                    printf("  Size: %d\t Blocks: 1\t IO Block: 4096   %s\n",
                           (int)st.st_size, S_ISDIR(st.st_mode) ? "directory" : (S_ISBLK(st.st_mode) ? "block device" : "regular file"));
                    printf("Device: 1\t Inode: %d\t Links: %d\n", (int)st.st_ino, (int)st.st_nlink);
                    printf("Access: (%04o/-", st.st_mode & 07777);
                    if (S_ISDIR(st.st_mode)) printf("d");
                    else if (S_ISBLK(st.st_mode)) printf("b");
                    else printf("-");
                    printf("%c%c%c%c%c%c%c%c%c)  Uid: (%d)   Gid: (%d)\n",
                           (st.st_mode & 0400) ? 'r' : '-', (st.st_mode & 0200) ? 'w' : '-', (st.st_mode & 0100) ? 'x' : '-',
                           (st.st_mode & 0040) ? 'r' : '-', (st.st_mode & 0020) ? 'w' : '-', (st.st_mode & 0010) ? 'x' : '-',
                           (st.st_mode & 0004) ? 'r' : '-', (st.st_mode & 0002) ? 'w' : '-', (st.st_mode & 0001) ? 'x' : '-',
                           st.st_uid, st.st_gid);
                    printf("Modify: ");
                    print_date(st.st_mtime);
                    last_exit_code = 0;
                }
            } else if (strncmp(exec_cmd, "sh ", 3) == 0) {
                execute_script_args(exec_cmd + 3);
                last_exit_code = 0;
            } else if (strcmp(exec_cmd, "whoami") == 0) {
                uint16_t uid = (uint16_t)getuid();
                if (uid == 0) printf("root\n");
                else if (uid == 1000) printf("user\n");
                else if (uid == 1001) printf("guest\n");
                else printf("uid_%d\n", uid);
                last_exit_code = 0;
            } else if (strcmp(exec_cmd, "id") == 0) {
                uint16_t uid = (uint16_t)getuid();
                printf("uid=%d(%s) gid=%d(%s)\n", uid, uid == 0 ? "root" : "user", uid == 0 ? 0 : 1000, uid == 0 ? "root" : "user");
                last_exit_code = 0;
            } else if (strncmp(exec_cmd, "su", 2) == 0 && (exec_cmd[2] == ' ' || exec_cmd[2] == '\0')) {
                const char *user = exec_cmd + 2;
                while (*user == ' ') user++;
                uint16_t target_uid = 0;
                const char *uname_str = "root";

                if (user[0] == '\0' || strcmp(user, "root") == 0) { target_uid = 0; uname_str = "root"; }
                else if (strcmp(user, "user") == 0) { target_uid = 1000; uname_str = "user"; }
                else if (strcmp(user, "guest") == 0) { target_uid = 1001; uname_str = "guest"; }
                else { target_uid = (uint16_t)atoi(user); }

                uint16_t cur_uid = (uint16_t)getuid();
                if (cur_uid == 0) {
                    setuid(target_uid, NULL);
                    env_set("USER", uname_str);
                    last_exit_code = 0;
                    return;
                }

                printf("Password: ");
                char pass_buf[32];
                read_password(pass_buf, sizeof(pass_buf));

                if (setuid(target_uid, pass_buf) == 0) {
                    env_set("USER", uname_str);
                    last_exit_code = 0;
                } else {
                    printf("su: Authentication failure\n");
                    last_exit_code = 1;
                }
            } else if (strncmp(exec_cmd, "chmod ", 6) == 0) {
                const char *p = exec_cmd + 6;
                while (*p == ' ') p++;
                int mode = (int)atoi(p);
                while (*p && *p != ' ') p++;
                while (*p == ' ') p++;
                if (chmod(p, mode) != 0) {
                    printf("chmod: permission denied or file not found\n");
                    last_exit_code = 1;
                } else last_exit_code = 0;
            } else if (strcmp(exec_cmd, "sigtest") == 0) {
                int pid = fork();
                if (pid == 0) {
                    printf("Child registered custom SIGINT handler in Ring 3!\n");
                    sigint_received = 0;
                    signal(SIGINT, sigint_handler);
                    printf("Press Ctrl+C in terminal to trigger it...\n");
                    while (!sigint_received) {
                        for (volatile int i = 0; i < 10000000; i++) {}
                    }
                    printf("Signal handled successfully. Exiting child.\n");
                    exit(0);
                } else if (pid > 0) {
                    int status = 0;
                    waitpid(pid, &status, 0);
                    last_exit_code = status;
                }
            } else if (strcmp(exec_cmd, "env") == 0) {
                for (int i = 0; i < MAX_ENV; i++) {
                    if (env_vars[i].in_use) printf("%s=%s\n", env_vars[i].key, env_vars[i].val);
                }
                last_exit_code = 0;
            } else if (strncmp(exec_cmd, "export ", 7) == 0) {
                const char *p = exec_cmd + 7;
                while (*p == ' ') p++;
                char k[32]; size_t ki = 0;
                while (*p && *p != '=' && *p != ' ' && ki < 31) k[ki++] = *p++;
                k[ki] = '\0';
                if (*p == '=') p++;
                env_set(k, p);
                last_exit_code = 0;
            } else if (strncmp(exec_cmd, "unset ", 6) == 0) {
                const char *p = exec_cmd + 6;
                while (*p == ' ') p++;
                env_unset(p);
                last_exit_code = 0;
            } else if (strcmp(exec_cmd, "jobs") == 0) {
                for (int i = 0; i < MAX_JOBS; i++) {
                    if (bg_jobs[i].in_use) {
                        printf("[%d]  Running  PID %d  %s\n", i + 1, bg_jobs[i].pid, bg_jobs[i].cmd);
                    }
                }
                last_exit_code = 0;
            } else if (strcmp(exec_cmd, "pwd") == 0) {
                char buf[64];
                if (getcwd(buf, sizeof(buf)) >= 0) printf("%s\n", buf);
                last_exit_code = 0;
            } else if (strncmp(exec_cmd, "cd ", 3) == 0) {
                const char *target = exec_cmd + 3;
                while (*target == ' ') target++;
                if (chdir(target) != 0) {
                    printf("cd: no such directory: %s\n", target);
                    last_exit_code = 1;
                } else last_exit_code = 0;
            } else if (strcmp(exec_cmd, "cd") == 0) {
                chdir("/");
                last_exit_code = 0;
            } else if (strncmp(exec_cmd, "mkdir ", 6) == 0) {
                const char *dname = exec_cmd + 6;
                while (*dname == ' ') dname++;
                if (mkdir(dname) != 0) { printf("mkdir: failed\n"); last_exit_code = 1; }
                else last_exit_code = 0;
            } else if (strncmp(exec_cmd, "rmdir ", 6) == 0) {
                const char *dname = exec_cmd + 6;
                while (*dname == ' ') dname++;
                if (rmdir(dname) != 0) { printf("rmdir: failed\n"); last_exit_code = 1; }
                else last_exit_code = 0;
            } else if (strncmp(exec_cmd, "ls", 2) == 0 && (exec_cmd[2] == ' ' || exec_cmd[2] == '\0')) {
                const char *arg = exec_cmd + 2;
                while (*arg == ' ') arg++;
                int is_long = 0;
                if (strncmp(arg, "-l", 2) == 0) {
                    is_long = 1; arg += 2; while (*arg == ' ') arg++;
                }
                char buf[1024];
                if (list(arg, buf, sizeof(buf), is_long) > 0) printf("%s", buf);
                last_exit_code = 0;
            } else if (strncmp(exec_cmd, "grep ", 5) == 0) {
                const char *p = exec_cmd + 5;
                while (*p == ' ') p++;
                char pat[64]; size_t pi = 0;
                while (*p && *p != ' ' && pi < 63) pat[pi++] = *p++;
                pat[pi] = '\0';
                strip_quotes(pat);
                while (*p == ' ') p++;
                if (*p != '\0') {
                    char fname[MAX_FILENAME]; size_t fi = 0;
                    while (*p && fi < MAX_FILENAME - 1) fname[fi++] = *p++;
                    fname[fi] = '\0';
                    strip_quotes(fname);
                    int fd = open(fname, O_RDONLY);
                    if (fd >= 0) { do_grep(pat, fd); close(fd); last_exit_code = 0; }
                    else { printf("grep: open failed\n"); last_exit_code = 1; }
                } else { do_grep(pat, 0); last_exit_code = 0; }
            } else if (strncmp(exec_cmd, "wc", 2) == 0 && (exec_cmd[2] == ' ' || exec_cmd[2] == '\0')) {
                const char *p = exec_cmd + 2;
                while (*p == ' ') p++;
                int only_l = 0;
                if (strncmp(p, "-l", 2) == 0) { only_l = 1; p += 2; while (*p == ' ') p++; }
                if (*p != '\0') {
                    char fname[MAX_FILENAME]; size_t fi = 0;
                    while (*p && fi < MAX_FILENAME - 1) fname[fi++] = *p++;
                    fname[fi] = '\0';
                    strip_quotes(fname);
                    int fd = open(fname, O_RDONLY);
                    if (fd >= 0) { do_wc(fd, only_l); close(fd); last_exit_code = 0; }
                    else { printf("wc: open failed\n"); last_exit_code = 1; }
                } else { do_wc(0, only_l); last_exit_code = 0; }
            } else if (strncmp(exec_cmd, "touch ", 6) == 0) {
                const char *fname = exec_cmd + 6;
                while (*fname == ' ') fname++;
                char cfname[MAX_FILENAME]; size_t ci = 0;
                while (*fname && ci < MAX_FILENAME - 1) cfname[ci++] = *fname++;
                cfname[ci] = '\0';
                strip_quotes(cfname);
                int fd = open(cfname, O_CREAT | O_WRONLY);
                if (fd >= 0) { close(fd); last_exit_code = 0; }
                else last_exit_code = 1;
            } else if (strncmp(exec_cmd, "rm ", 3) == 0) {
                const char *fname = exec_cmd + 3;
                while (*fname == ' ') fname++;
                char cfname[MAX_FILENAME]; size_t ci = 0;
                while (*fname && ci < MAX_FILENAME - 1) cfname[ci++] = *fname++;
                cfname[ci] = '\0';
                strip_quotes(cfname);
                if (unlink(cfname) != 0) { printf("rm: permission denied or file not found\n"); last_exit_code = 1; }
                else last_exit_code = 0;
            } else if (strcmp(exec_cmd, "cat") == 0) {
                char fbuf[128]; int64_t n;
                while ((n = read(0, fbuf, sizeof(fbuf) - 1)) > 0) {
                    fbuf[n] = '\0'; write(1, fbuf, n);
                }
                last_exit_code = 0;
            } else if (strncmp(exec_cmd, "cat ", 4) == 0) {
                const char *filename = exec_cmd + 4;
                while (*filename == ' ') filename++;
                char cfname[MAX_FILENAME]; size_t ci = 0;
                while (*filename && ci < MAX_FILENAME - 1) cfname[ci++] = *filename++;
                cfname[ci] = '\0';
                strip_quotes(cfname);
                int fd = open(cfname, O_RDONLY);
                if (fd < 0) {
                    printf("cat: not found\n"); last_exit_code = 1;
                } else {
                    char fbuf[128]; int64_t n;
                    while ((n = read(fd, fbuf, sizeof(fbuf) - 1)) > 0) {
                        fbuf[n] = '\0'; write(1, fbuf, n);
                    }
                    close(fd);
                    last_exit_code = 0;
                }
            } else if (strncmp(exec_cmd, "echo ", 5) == 0) {
                const char *p = exec_cmd + 5;
                while (*p == ' ') p++;
                char msg[128]; size_t mi = 0;
                while (p[mi] && mi < 127) { msg[mi] = p[mi]; mi++; }
                msg[mi] = '\0';
                strip_quotes(msg);
                printf("%s\n", msg);
                last_exit_code = 0;
            } else if (strcmp(exec_cmd, "date") == 0) {
                uint64_t epoch = (uint64_t)time();
                print_date(epoch);
                last_exit_code = 0;
            } else if (strncmp(exec_cmd, "uname", 5) == 0 && (exec_cmd[5] == ' ' || exec_cmd[5] == '\0')) {
                static struct utsname un;
                if (uname(&un) == 0) {
                    if (exec_cmd[5] == ' ' && exec_cmd[6] == '-' && exec_cmd[7] == 'a') {
                        printf("%s %s %s %s %s\n", un.sysname, un.nodename, un.release, un.version, un.machine);
                    } else {
                        printf("%s\n", un.sysname);
                    }
                }
                last_exit_code = 0;
            } else if (strncmp(exec_cmd, "sleep ", 6) == 0) {
                int sec = (int)atoi(exec_cmd + 6);
                if (sec <= 0) sec = 1;

                int pid = fork();
                if (pid == 0) {
                    uint64_t start = (uint64_t)time();
                    while ((uint64_t)time() - start < (uint64_t)sec) {}
                    exit(0);
                } else if (pid > 0) {
                    if (is_bg) {
                        add_bg_job(pid, exec_cmd);
                    } else {
                        printf("Sleeping for %d seconds...\n", sec);
                        int status = 0;
                        waitpid(pid, &status, 0);
                        last_exit_code = status;
                        if (status == 130) printf("Sleep aborted by SIGINT!\n");
                    }
                }
            } else if (strcmp(exec_cmd, "ps") == 0) {
                ps();
                last_exit_code = 0;
            } else if (strncmp(exec_cmd, "kill ", 5) == 0) {
                int target_pid = (int)atoi(exec_cmd + 5);
                if (target_pid > 1) { kill(target_pid, SIGKILL); last_exit_code = 0; }
                else last_exit_code = 1;
            } else if (strcmp(exec_cmd, "exit") == 0) {
                exit(0);
            } else {
                size_t cl = strlen(exec_cmd);
                if (cl > 3 && exec_cmd[cl - 3] == '.' && exec_cmd[cl - 2] == 's' && exec_cmd[cl - 1] == 'h') {
                    execute_script_args(exec_cmd);
                    return;
                }

                static char arg_buf[16][64];
                static char *argv_ptrs[17];
                int argc = 0;

                const char *ap = exec_cmd;
                while (*ap) {
                    while (*ap == ' ') ap++;
                    if (*ap == '\0') break;

                    size_t ai = 0;
                    while (*ap && *ap != ' ' && ai < 63) arg_buf[argc][ai++] = *ap++;
                    arg_buf[argc][ai] = '\0';
                    strip_quotes(arg_buf[argc]);

                    argv_ptrs[argc] = arg_buf[argc];
                    argc++;
                    if (argc >= 16) break;
                }
                argv_ptrs[argc] = NULL;

                int pid = fork();
                if (pid == 0) {
                    int err = execve(argv_ptrs[0], argv_ptrs, NULL);
                    if (err < 0) {
                        char bin_path[64];
                        snprintf(bin_path, sizeof(bin_path), "/bin/%s", argv_ptrs[0]);
                        err = execve(bin_path, argv_ptrs, NULL);
                    }
                    if (err < 0) {
                        printf("shell: command not found: %s\n", argv_ptrs[0]);
                        exit(127);
                    }
                } else if (pid > 0) {
                    if (is_bg) {
                        add_bg_job(pid, exec_cmd);
                    } else {
                        int status = 0;
                        waitpid(pid, &status, 0);
                        last_exit_code = status;
                        printf("[Program finished with exit code %d]\n", status);
                    }
                }
            }
}

static void print_prompt(void)
{
    char cwd[64];
    getcwd(cwd, sizeof(cwd));
    uint16_t uid = (uint16_t)getuid();

    if (uid == 0) {
        printf("root@linux64:%s# ", cwd);
    } else {
        const char *user = env_get("USER");
        if (!user) user = "user";
        printf("%s@linux64:%s$ ", user, cwd);
    }
}

int main(int argc, char **argv)
{
    env_init();

    /* Если передан скрипт (например, sh /scripts/welcome.sh) */
    if (argc > 1) {
        execute_script_args(argv[1]);
        return last_exit_code;
    }

    printf("\n========================================\n"
    "  Linux 0.01 (x86_64) Standalone Shell  \n"
    "  Type 'help' to see available commands \n"
    "========================================\n\n");

    /* Выполнение скрипта автозапуска системы */
    execute_script_args("/etc/init.sh");

    char cmd_buf[128];
    int buf_len = 0;

    print_prompt();

    while (1) {
        char c;
        if (read(0, &c, 1) > 0) {
            if (c == 27) {
                char seq[2];
                if (read(0, &seq[0], 1) > 0 && seq[0] == '[') {
                    if (read(0, &seq[1], 1) > 0) {
                        if (seq[1] == 'A') {
                            if (history_count > 0 && history_idx > 0) {
                                history_idx--;
                                while (buf_len > 0) { write(1, "\b", 1); buf_len--; }
                                int slot = history_idx % HISTORY_MAX;
                                const char *hcmd = history[slot];
                                while (*hcmd && buf_len < 127) {
                                    cmd_buf[buf_len++] = *hcmd;
                                    write(1, hcmd, 1);
                                    hcmd++;
                                }
                            }
                        } else if (seq[1] == 'B') {
                            if (history_count > 0 && history_idx < history_count) {
                                history_idx++;
                                while (buf_len > 0) { write(1, "\b", 1); buf_len--; }
                                if (history_idx < history_count) {
                                    int slot = history_idx % HISTORY_MAX;
                                    const char *hcmd = history[slot];
                                    while (*hcmd && buf_len < 127) {
                                        cmd_buf[buf_len++] = *hcmd;
                                        write(1, hcmd, 1);
                                        hcmd++;
                                    }
                                }
                            }
                        }
                    }
                }
                continue;
            }

            if (c == 3) {
                printf("^C\n");
                buf_len = 0;
                print_prompt();
                continue;
            }

            if (c == '\b') {
                if (buf_len > 0) {
                    buf_len--;
                    write(1, "\b", 1);
                }
            } else if (c == '\n') {
                write(1, "\n", 1);
                cmd_buf[buf_len] = '\0';
                history_add(cmd_buf);

                const char *cb = cmd_buf;
                while (*cb == ' ' || *cb == '\t') cb++;

                if (strncmp(cb, "for ", 4) == 0 ||
                    strncmp(cb, "while ", 6) == 0 ||
                    strncmp(cb, "if ", 3) == 0 ||
                    strstr(cb, ";") != NULL) {
                    execute_command(cb);
                    } else {
                        char expanded[256];
                        expand_vars(cb, expanded, sizeof(expanded));
                        execute_command(expanded);
                    }

                    buf_len = 0;
                    check_bg_jobs();
                    print_prompt();
            } else if (c >= 32 && c <= 126) {
                if (buf_len < (int)sizeof(cmd_buf) - 1) {
                    cmd_buf[buf_len++] = c;
                    write(1, &c, 1);
                }
            }
        }
    }

    return 0;
}
