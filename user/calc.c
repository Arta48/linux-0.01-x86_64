#include "ulibc.h"

int main(int argc, char **argv)
{
    if (argc < 4) {
        printf("Usage: calc <num1> <+|-|*|/> <num2>\n");
        printf("Example: calc 42 * 2\n");
        return 1;
    }

    int64_t a = atoi(argv[1]);
    char op = argv[2][0];
    int64_t b = atoi(argv[3]);
    int64_t res = 0;

    if (op == '+') res = a + b;
    else if (op == '-') res = a - b;
    else if (op == '*') res = a * b;
    else if (op == '/') {
        if (b == 0) {
            printf("calc: error: division by zero\n");
            return 1;
        }
        res = a / b;
    } else {
        printf("calc: error: unsupported operator '%c'\n", op);
        return 1;
    }

    printf("%d %c %d = %d (hex: 0x%x)\n", (int)a, op, (int)b, (int)res, (uint32_t)res);
    return 0;
}
