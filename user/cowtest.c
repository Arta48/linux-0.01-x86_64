#include "ulibc.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("====================================================\n");
    printf("   Copy-On-Write (COW) Memory Verification Test     \n");
    printf("====================================================\n\n");

    /* 1. Выделяем страницу памяти в куче родителя */
    char *shared_mem = (char *)malloc(4096);
    if (!shared_mem) {
        printf("[FAIL] malloc failed\n");
        return 1;
    }

    strcpy(shared_mem, "ORIGINAL_PARENT_DATA_12345");
    printf("[PARENT] Allocated page at %p with content: '%s'\n", shared_mem, shared_mem);

    printf("[TEST] Calling fork(). Memory pages will be shared read-only...\n");
    int pid = fork();

    if (pid < 0) {
        printf("[FAIL] Fork failed\n");
        return 1;
    }

    if (pid == 0) {
        /* Дочерний процесс */
        printf("[CHILD] Started (PID %d). Reading shared memory: '%s'\n", getpid(), shared_mem);
        if (strcmp(shared_mem, "ORIGINAL_PARENT_DATA_12345") != 0) {
            printf("[FAIL] Child read wrong data!\n");
            exit(1);
        }

        printf("[CHILD] Writing to shared memory (Triggering Page Fault #14 COW)...\n");
        /* Эта запись вызовет Page Fault, ядро прозрачно дублирует страницу только для ребенка */
        strcpy(shared_mem, "MODIFIED_BY_CHILD_67890");

        printf("[CHILD] Write complete! Content in child: '%s'\n", shared_mem);
        exit(0);
    }

    /* Родительский процесс */
    int status = 0;
    waitpid(pid, &status, 0);

    printf("[PARENT] Child finished. Checking parent's buffer...\n");
    printf("[PARENT] Buffer content in parent: '%s'\n", shared_mem);

    if (strcmp(shared_mem, "ORIGINAL_PARENT_DATA_12345") == 0) {
        printf("\n====================================================\n");
        printf(" [SUCCESS] COW TEST PASSED: PARENT DATA UNTOUCHED!  \n");
        printf("====================================================\n");
        free(shared_mem);
        return 0;
    } else {
        printf("\n[FAIL] Parent data was corrupted by child write!\n");
        free(shared_mem);
        return 1;
    }
}
