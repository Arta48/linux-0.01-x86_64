#ifndef _LINUX_SYSCALL_H
#define _LINUX_SYSCALL_H

#include <linux/types.h>

#define __NR_exit   1
#define __NR_write  4
#define __NR_getpid 20

int64_t syscall_dispatcher(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3);

#endif
