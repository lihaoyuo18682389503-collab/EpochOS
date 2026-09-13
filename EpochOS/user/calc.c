/* ============================================================
 * EpochOS epoch-libc 测试程序 2: 简单算术计算器
 * 验证: atoi + printf(%d) + 表达式计算 (shell: * 需独立 token)
 * 运行: elf /bin/calc 6 * 7    -> 42
 *       elf /bin/calc 100 - 37 -> 63
 *       elf /bin/calc 8 / 2    -> 4
 * ============================================================ */
#include "epoch.h"

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("calc: usage: calc <a> <op> <b>   (op=+ - * /)\n");
        return 2;
    }
    int a = atoi(argv[1]);
    int b = atoi(argv[3]);
    char op = argv[2][0];
    int r = 0;

    switch (op) {
    case '+': r = a + b; break;
    case '-': r = a - b; break;
    case '*': r = a * b; break;
    case '/':
        if (b == 0) { printf("calc: divide by zero\n"); return 1; }
        r = a / b;
        break;
    default:
        printf("calc: bad op '%c'\n", op);
        return 1;
    }
    printf("calc: %d %c %d = %d\n", a, op, b, r);
    return r % 64;    /* 退出码限 0-63: 6*7 -> exit(42) */
}
