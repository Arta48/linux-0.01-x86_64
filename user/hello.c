#include "ulibc.h"

int main(int argc, char **argv)
{
    if (argc > 1) {
        printf("Hello, %s! Welcome to Linux 0.01 (x86_64).\n", argv[1]);
    } else {
        printf("Hello from Ring 3 standalone binary compiled with ulibc!\n");
        printf("PID: %d, System Time: %d\n", getpid(), (int)time());
    }
    return 0;
}
