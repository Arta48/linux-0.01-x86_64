#include "ulibc.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("========================================\n");
    printf("   Ring 3 ulibc Comprehensive Tests     \n");
    printf("========================================\n");

    /* 1. Тестирование форматирования printf */
    printf("[TEST 1] String: '%s'\n", "Hello, 64-bit Unix!");
    printf("[TEST 2] Signed Decimals: %d, %d, %d\n", 0, -9999, 123456);
    printf("[TEST 3] Hex Formatting: 0x%x, 0x%08x\n", 0xCAFE, 0x1234);
    printf("[TEST 4] Pointers: main=%p, exit=%p\n", (void *)main, (void *)exit);

    /* 2. Тестирование динамической памяти */
    printf("[TEST 5] Testing dynamic memory allocator (malloc/free)...\n");
    char *dyn_str = (char *)malloc(128);
    if (!dyn_str) {
        printf("[FAIL] malloc returned NULL\n");
        return 1;
    }

    snprintf(dyn_str, 128, "Heap block allocated at %p successfully!", (void *)dyn_str);
    printf("[PASS] %s\n", dyn_str);

    free(dyn_str);
    printf("[PASS] Memory freed successfully.\n");

    printf("========================================\n");
    printf("   All ulibc Ring 3 Tests Passed!       \n");
    printf("========================================\n");
    return 0;
}
