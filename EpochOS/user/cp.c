/* ============================================================
 * EpochOS epoch-libc 测试程序 5: 文件复制工具 cp (阶段5)
 * 验证: fopen/fclose/fread/fwrite (FILE 流), 相对根路径
 * 运行: elf /bin/cp readme.txt copy.txt
 * ============================================================ */
#include "epoch.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("cp: usage: cp <src> <dst>\n");
        return 1;
    }
    printf("cp: %s -> %s (pid=%d)\n", argv[1], argv[2], getpid());

    FILE* in = fopen(argv[1], "r");
    if (!in) {
        printf("cp: cannot open source '%s'\n", argv[1]);
        return 2;
    }
    FILE* out = fopen(argv[2], "w");
    if (!out) {
        printf("cp: cannot create dest '%s'\n", argv[2]);
        fclose(in);
        return 3;
    }

    char buf[128];
    int total = 0;
    for (;;) {
        int n = fread(buf, 1, sizeof(buf), in);
        if (n <= 0) break;
        int w = fwrite(buf, 1, n, out);
        if (w != n) {
            printf("cp: write error at byte %d\n", total);
            fclose(in);
            fclose(out);
            return 4;
        }
        total += n;
    }
    fclose(in);
    fclose(out);
    printf("cp: copied %d bytes\n", total);

    /* 回读验证 */
    FILE* v = fopen(argv[2], "r");
    if (!v) {
        printf("cp: verify open FAILED\n");
        return 5;
    }
    char head[96];
    int k = fread(head, 1, sizeof(head) - 1, v);
    if (k > 0) head[k] = 0;
    fclose(v);
    printf("cp: verify head(%d): %s", k, head);
    if (k > 0 && head[k - 1] != '\n') printf("\n");
    printf("cp: done\n");
    return 0;
}
