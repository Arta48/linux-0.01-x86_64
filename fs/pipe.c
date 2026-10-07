#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/tty.h>

int64_t sys_pipe(int *pipefd)
{
    int rfd = -1, wfd = -1;
    for (int i = 3; i < NR_OPEN; i++) {
        if (!current->filp[i].in_use) {
            if (rfd == -1) {
                rfd = i;
            } else if (wfd == -1) {
                wfd = i;
                break;
            }
        }
    }

    if (rfd == -1 || wfd == -1) {
        return -1;
    }

    uint64_t page = get_free_page();
    if (!page) {
        return -1;
    }

    struct pipe *p = (struct pipe *)page;
    p->head = 0;
    p->tail = 0;
    p->count = 0;
    p->readers = 1;
    p->writers = 1;
    p->ref_count = 2; /* 2 открытых дескриптора: rfd и wfd */

    current->filp[rfd].type = FILE_TYPE_PIPE;
    current->filp[rfd].mode = 1;
    current->filp[rfd].pipe = p;
    current->filp[rfd].in_use = 1;

    current->filp[wfd].type = FILE_TYPE_PIPE;
    current->filp[wfd].mode = 2;
    current->filp[wfd].pipe = p;
    current->filp[wfd].in_use = 1;

    pipefd[0] = rfd;
    pipefd[1] = wfd;

    return 0;
}

int64_t pipe_read(struct file *f, char *buf, uint64_t count)
{
    struct pipe *p = f->pipe;
    if (!p) return -1;

    while (p->count == 0) {
        if (p->writers == 0) {
            return 0; /* EOF */
        }
        __asm__ volatile ("sti");
        schedule();
    }

    uint64_t bytes_read = 0;
    while (bytes_read < count && p->count > 0) {
        buf[bytes_read++] = p->buffer[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
        p->count--;
    }

    return bytes_read;
}

int64_t pipe_write(struct file *f, const char *buf, uint64_t count)
{
    struct pipe *p = f->pipe;
    if (!p || p->readers == 0) {
        return -1; /* Нет читателей */
    }

    uint64_t bytes_written = 0;
    while (bytes_written < count) {
        while (p->count == PIPE_BUF_SIZE) {
            __asm__ volatile ("sti");
            schedule();
        }

        p->buffer[p->head] = buf[bytes_written++];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
        p->count++;
    }

    return bytes_written;
}
