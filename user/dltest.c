#include "ulibc.h"

typedef int (*math_op_fn)(int, int);
typedef int (*math_unary_fn)(int);
typedef const char *(*version_fn)(void);

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("====================================================\n");
    printf("   ELF Dynamic Linker & Shared Library Test (/lib)  \n");
    printf("====================================================\n\n");

    const char *lib_path = "/lib/libmath.so";
    printf("[1] Loading shared object: %s via dlopen()...\n", lib_path);

    void *handle = dlopen(lib_path, 0);
    if (!handle) {
        printf("[FAIL] dlopen() error: %s\n", dlerror());
        return 1;
    }
    printf("[PASS] Library loaded successfully at handle: %p\n", handle);

    printf("[2] Resolving dynamic symbol 'get_lib_version' via dlsym()...\n");
    version_fn ver = (version_fn)dlsym(handle, "get_lib_version");
    if (!ver) {
        printf("[FAIL] dlsym() error: %s\n", dlerror());
        dlclose(handle);
        return 1;
    }
    printf("  -> Dynamic Library String: \"%s\"\n", ver());

    printf("[3] Resolving mathematical functions from .so...\n");
    math_op_fn    f_add  = (math_op_fn)dlsym(handle, "add");
    math_op_fn    f_mul  = (math_op_fn)dlsym(handle, "multiply");
    math_unary_fn f_fact = (math_unary_fn)dlsym(handle, "factorial");
    math_unary_fn f_fib  = (math_unary_fn)dlsym(handle, "fibonacci");

    if (!f_add || !f_mul || !f_fact || !f_fib) {
        printf("[FAIL] Failed to resolve function pointers!\n");
        dlclose(handle);
        return 1;
    }

    printf("  -> 15 + 27 = %d (expected 42)\n", f_add(15, 27));
    printf("  -> 6 * 7   = %d (expected 42)\n", f_mul(6, 7));
    printf("  -> 5!      = %d (expected 120)\n", f_fact(5));
    printf("  -> fib(10) = %d (expected 55)\n", f_fib(10));

    printf("[4] Closing library handle via dlclose()...\n");
    dlclose(handle);
    printf("[PASS] Closed cleanly.\n");

    printf("\n====================================================\n");
    printf(" [SUCCESS] ELF DYNAMIC LINKING & .SO VERIFIED!      \n");
    printf("====================================================\n");
    return 0;
}
