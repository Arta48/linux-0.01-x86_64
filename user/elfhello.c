#include "ulibc.h"

int main(int argc, char **argv)
{
    printf("====================================================\n");
    printf("   Native ELF-64 Executable Execution Verified!     \n");
    printf("====================================================\n");
    printf("This binary was loaded and executed via ELF-64 loader\n");
    printf("directly from the 64-bit kernel sys_execve dispatcher!\n");
    printf("Arguments count: %d, Binary name: '%s'\n", argc, argv[0]);
    printf("System PID: %d, Current Epoch: %d\n", getpid(), (int)time());
    return 0;
}
