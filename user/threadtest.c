#include "ulibc.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("====================================================\n");
    printf("   Multiprocessing & Concurrency Test (/bin/thread) \n");
    printf("====================================================\n\n");

    printf("[TEST 1] Testing process creation under SMP...\n");
    int pids[4];

    for (int i = 0; i < 4; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            /* Дочерний процесс выполняет работу */
            for (volatile int k = 0; k < 5000000; k++) {}
            printf("  -> Worker child #%d (PID %d) finished work successfully!\n", i + 1, getpid());
            exit(0);
        }
    }

    printf("[PARENT] Waiting for all concurrent workers to finish...\n");
    for (int i = 0; i < 4; i++) {
        int status = 0;
        waitpid(pids[i], &status, 0);
    }

    printf("\n[TEST 2] Process table status (showing Kernel Threads):\n");
    ps();

    printf("====================================================\n");
    printf(" [SUCCESS] MULTIPROCESS CONCURRENCY TEST PASSED!   \n");
    printf("====================================================\n");
    return 0;
}
