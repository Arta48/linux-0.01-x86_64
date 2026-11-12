#ifndef _LINUX_TIME_H
#define _LINUX_TIME_H

#include <linux/types.h>

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
};

void time_init(void);
uint64_t kernel_mktime(struct tm *tm);
uint64_t get_current_time(void);

extern uint64_t startup_time;

#endif
