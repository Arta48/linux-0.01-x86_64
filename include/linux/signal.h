#ifndef _LINUX_SIGNAL_H
#define _LINUX_SIGNAL_H

#include <linux/types.h>

#define SIGHUP   1
#define SIGINT   2    /* Прерывание с клавиатуры (Ctrl+C) */
#define SIGQUIT  3
#define SIGKILL  9    /* Безусловное завершение */
#define SIGSEGV  11
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15   /* Запрос на завершение */

#define SIG_DFL  ((void (*)(int))0)  /* Действие по умолчанию */
#define SIG_IGN  ((void (*)(int))1)  /* Игнорировать сигнал */

#endif
