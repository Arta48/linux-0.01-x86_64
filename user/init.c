/* /bin/init -- первый процесс. Запускает bash; когда он завершается, выключает компьютер. */
#include "ulibc.h"

int main(void)
{
    char *argv[] = { "/bin/bash", "-l", NULL };
    char *envp[] = {
        "HOME=/", "PATH=/bin", "TERM=linux", "USER=root", "LOGNAME=root",
        "SHELL=/bin/bash", "LANG=C.UTF-8", "PS1=\\u@\\h:\\w\\$ ", NULL
    };

    int pid = fork();
    if (pid == 0) {
        execve(argv[0], argv, envp);
        printf("init: cannot execute %s (build with HOST_APPS including bash)\n", argv[0]);
        exit(127);
    }
    if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        if (status == 127 || (status >> 8) == 127)
            for (;;) pause();           /* bash отсутствует: оставить сообщение на экране */
    } else {
        printf("init: fork failed\n");
    }
    sync();
    reboot(0);
    for (;;) pause();
}
