#ifndef _LINUX_SYSCALL_H
#define _LINUX_SYSCALL_H

#include <linux/types.h>
#include <linux/traps.h>

#define __NR_exit    1
#define __NR_fork    2
#define __NR_read    3
#define __NR_write   4
#define __NR_open    5
#define __NR_close   6
#define __NR_waitpid 7
#define __NR_execve  11
#define __NR_time    13
#define __NR_getpid  20
#define __NR_ps      21
#define __NR_list    22
#define __NR_kill    37
#define __NR_pipe    42
#define __NR_brk     45

void syscall_init(void);
int64_t syscall_dispatcher(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3, struct trap_frame *tf);

#endif
