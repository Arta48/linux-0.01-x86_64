#include "ulibc.h"

int add(int a, int b)
{
    return a + b;
}

int multiply(int a, int b)
{
    return a * b;
}

int factorial(int n)
{
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

int fibonacci(int n)
{
    if (n <= 0) return 0;
    if (n == 1) return 1;
    return fibonacci(n - 1) + fibonacci(n - 2);
}

const char *get_lib_version(void)
{
    return "libmath.so v1.0 (Linux 0.01 x86_64 Shared Object)";
}
