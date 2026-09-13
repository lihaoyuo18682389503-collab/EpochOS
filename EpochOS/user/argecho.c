/* ============================================================
 * EpochOS epoch-libc 测试程序 1: 命令行参数回显
 * 验证: crt0 argc/argv 解析 + printf(%d/%s) + exit(返回码)
 * 运行: elf /bin/argecho hello epoch 42
 * ============================================================ */
#include "epoch.h"

int main(int argc, char** argv) {
    printf("argecho: pid ok, argc=%d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("  argv[%d]=\"%s\"\n", i, argv[i]);
    }
    printf("argecho: done (return %d)\n", argc);
    return argc % 64;
}
