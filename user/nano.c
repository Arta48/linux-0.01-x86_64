#include "ulibc.h"

#define SCREEN_ROWS 25
#define SCREEN_COLS 80
#define TEXT_ROWS   22

#define MAX_LINES   512
#define LINE_LEN    160

static char lines[MAX_LINES][LINE_LEN];
static int total_lines = 0;

static int cur_row = 0;
static int cur_col = 0;
static int top_row = 0;

static char filename[64];
static char status_bar[80];
static char cut_buffer[LINE_LEN];
static int cut_valid = 0;
static int modified = 0;

static void set_status(const char *msg)
{
    strncpy(status_bar, msg, sizeof(status_bar) - 1);
    status_bar[sizeof(status_bar) - 1] = '\0';
}

static void load_file(const char *fname)
{
    strncpy(filename, fname, sizeof(filename) - 1);
    filename[sizeof(filename) - 1] = '\0';
    total_lines = 0;
    modified = 0;

    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        lines[0][0] = '\0';
        total_lines = 1;
        set_status("[ New File ]");
        return;
    }

    char c;
    int ci = 0;
    while (read(fd, &c, 1) > 0 && total_lines < MAX_LINES) {
        if (c == '\n' || c == '\r') {
            lines[total_lines][ci] = '\0';
            total_lines++;
            ci = 0;
        } else if (ci < LINE_LEN - 1) {
            lines[total_lines][ci++] = c;
        }
    }
    if (ci > 0 && total_lines < MAX_LINES) {
        lines[total_lines][ci] = '\0';
        total_lines++;
    }
    close(fd);

    if (total_lines == 0) {
        lines[0][0] = '\0';
        total_lines = 1;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "[ Read %d lines from %s ]", total_lines, filename);
    set_status(msg);
}

static void save_file(void)
{
    int fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC);
    if (fd < 0) {
        set_status("[ Error: Cannot open file for writing! ]");
        return;
    }

    int write_err = 0;
    for (int i = 0; i < total_lines; i++) {
        size_t len = strlen(lines[i]);
        if (write(fd, lines[i], len) < 0) write_err = 1;
        if (write(fd, "\n", 1) < 0) write_err = 1;
    }
    close(fd);

    if (write_err) {
        set_status("[ Error: Write failed! File may be protected ]");
        return;
    }

    modified = 0;
    char msg[64];
    snprintf(msg, sizeof(msg), "[ Wrote %d lines to %s ]", total_lines, filename);
    set_status(msg);
}

static void render(void)
{
    printf("\033[H");

    /* 1. Верхняя панель (инвертированное видео) */
    printf("\033[7m");
    char header[SCREEN_COLS + 1];
    snprintf(header, sizeof(header), "  GNU nano 0.01          File: %s%s", filename, modified ? " [Modified]" : "");
    printf("%s", header);
    for (int i = (int)strlen(header); i < SCREEN_COLS; i++) {
        printf(" ");
    }
    printf("\033[0m\r\n");

    /* 2. Текстовое поле */
    for (int r = 0; r < TEXT_ROWS; r++) {
        int file_line_idx = top_row + r;
        if (file_line_idx < total_lines) {
            char *line = lines[file_line_idx];
            int len = (int)strlen(line);
            if (len > SCREEN_COLS) len = SCREEN_COLS;
            for (int k = 0; k < len; k++) printf("%c", line[k]);
            printf("\033[K\r\n");
        } else {
            printf("~\033[K\r\n");
        }
    }

    /* 3. Строка состояния */
    printf("%s\033[K\r\n", status_bar);

    /* 4. Панель горячих клавиш (инвертированное видео) */
    printf("\033[7m ^O WriteOut   ^X Exit   ^K Cut Text   ^U Uncut Text   ^C Pos \033[0m");

    /* 5. Позиционирование курсора */
    int screen_y = (cur_row - top_row) + 2;
    int screen_x = cur_col + 1;
    printf("\033[%d;%dH", screen_y, screen_x);
}

static void insert_char(char c)
{
    char *l = lines[cur_row];
    int len = (int)strlen(l);
    if (len >= LINE_LEN - 2) return;

    for (int i = len; i >= cur_col; i--) {
        l[i + 1] = l[i];
    }
    l[cur_col] = c;
    cur_col++;
    modified = 1;
}

static void insert_newline(void)
{
    if (total_lines >= MAX_LINES - 1) return;

    for (int i = total_lines; i > cur_row + 1; i--) {
        strcpy(lines[i], lines[i - 1]);
    }

    char *curr = lines[cur_row];
    char *next = lines[cur_row + 1];

    strcpy(next, curr + cur_col);
    curr[cur_col] = '\0';

    total_lines++;
    cur_row++;
    cur_col = 0;
    modified = 1;
}

static void backspace(void)
{
    char *curr = lines[cur_row];

    if (cur_col > 0) {
        int len = (int)strlen(curr);
        for (int i = cur_col - 1; i < len; i++) {
            curr[i] = curr[i + 1];
        }
        cur_col--;
        modified = 1;
    } else if (cur_row > 0) {
        char *prev = lines[cur_row - 1];
        int prev_len = (int)strlen(prev);
        int curr_len = (int)strlen(curr);

        if (prev_len + curr_len < LINE_LEN - 1) {
            strcat(prev, curr);
            for (int i = cur_row; i < total_lines - 1; i++) {
                strcpy(lines[i], lines[i + 1]);
            }
            total_lines--;
            cur_row--;
            cur_col = prev_len;
            modified = 1;
        }
    }
}

static void cut_line(void)
{
    if (total_lines == 0) return;

    strcpy(cut_buffer, lines[cur_row]);
    cut_valid = 1;

    if (total_lines == 1) {
        lines[0][0] = '\0';
        cur_col = 0;
    } else {
        for (int i = cur_row; i < total_lines - 1; i++) {
            strcpy(lines[i], lines[i + 1]);
        }
        total_lines--;
        if (cur_row >= total_lines) cur_row = total_lines - 1;
        cur_col = 0;
    }
    modified = 1;
    set_status("[ Cut 1 line ]");
}

static void uncut_line(void)
{
    if (!cut_valid || total_lines >= MAX_LINES - 1) return;

    for (int i = total_lines; i > cur_row; i--) {
        strcpy(lines[i], lines[i - 1]);
    }
    strcpy(lines[cur_row], cut_buffer);
    total_lines++;
    modified = 1;
    set_status("[ Uncut 1 line ]");
}

static void adjust_scroll(void)
{
    if (cur_row < top_row) {
        top_row = cur_row;
    }
    if (cur_row >= top_row + TEXT_ROWS) {
        top_row = cur_row - TEXT_ROWS + 1;
    }
    int len = (int)strlen(lines[cur_row]);
    if (cur_col > len) {
        cur_col = len;
    }
    if (cur_col < 0) {
        cur_col = 0;
    }
}

int main(int argc, char **argv)
{
    const char *target = (argc > 1) ? argv[1] : "newfile.txt";
    load_file(target);

    printf("\033[2J");

    while (1) {
        adjust_scroll();
        render();

        char c;
        if (read(0, &c, 1) <= 0) continue;

        if (c == 24) { /* Ctrl+X: Exit */
            break;
        } else if (c == 15) { /* Ctrl+O: Save */
            save_file();
            continue;
        } else if (c == 11) { /* Ctrl+K: Cut */
            cut_line();
            continue;
        } else if (c == 21) { /* Ctrl+U: Uncut */
            uncut_line();
            continue;
        } else if (c == 3) {  /* Ctrl+C: Pos */
            char msg[64];
            snprintf(msg, sizeof(msg), "[ Line %d/%d, Col %d ]", cur_row + 1, total_lines, cur_col + 1);
            set_status(msg);
            continue;
        }

        if (c == '\033') {
            char seq[2];
            if (read(0, &seq[0], 1) > 0 && seq[0] == '[') {
                if (read(0, &seq[1], 1) > 0) {
                    if (seq[1] == 'A') {
                        if (cur_row > 0) cur_row--;
                    } else if (seq[1] == 'B') {
                        if (cur_row < total_lines - 1) cur_row++;
                    } else if (seq[1] == 'C') {
                        int len = (int)strlen(lines[cur_row]);
                        if (cur_col < len) cur_col++;
                    } else if (seq[1] == 'D') {
                        if (cur_col > 0) cur_col--;
                    } else if (seq[1] == 'H') {
                        cur_col = 0;
                    } else if (seq[1] == 'F') {
                        cur_col = (int)strlen(lines[cur_row]);
                    }
                }
            }
            continue;
        }

        if (c == '\b') {
            backspace();
        } else if (c == '\n' || c == '\r') {
            insert_newline();
        } else if (c == '\t') {
            for (int k = 0; k < 4; k++) insert_char(' ');
        } else if ((unsigned char)c >= 32 && (unsigned char)c <= 126) {
            insert_char(c);
        }
    }

    printf("\033[2J\033[H\033[0m");
    return 0;
}
