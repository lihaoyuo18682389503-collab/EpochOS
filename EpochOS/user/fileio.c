/* ============================================================
 * EpochOS epoch-libc 测试程序 3: 文件读写 + malloc/free
 * 验证: open/read/write/close/lseek + 堆 (brk 扩展/释放复用)
 * 读取预置 welcome.txt, 偏移重读, 向 hello.txt 追加标签,
 * 循环 malloc/free 观察堆地址页级增长。
 * ============================================================ */
#include "epoch.h"

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("fileio: start\n");

    /* 1) open + read 预置文件 (整段, 用 malloc 缓冲) */
    int fd = open("welcome.txt", 0);
    if (fd < 0) { printf("fileio: open welcome.txt FAILED\n"); return 1; }
    char* big = (char*)malloc(160);
    if (!big) { printf("fileio: malloc(160) FAILED\n"); return 2; }
    int n = read(fd, big, 159);
    if (n > 0) big[n] = 0;
    printf("fileio: welcome.txt fd=%d read=%d bytes\n", fd, n);
    printf("fileio: head=\"");
    puts(big);           /* 输出完整头部 */

    /* 2) lseek 回文件头, 只读 8 字节验证偏移语义 */
    int p0 = lseek(fd, 0, 0);
    char small[16];
    int m = read(fd, small, 8);
    small[m > 0 ? m : 0] = 0;
    printf("fileio: lseek -> %d, re-read %d bytes: %s\n", p0, m, small);
    close(fd);

    /* 3) open 写 hello.txt (EpochOS 写=追加到 ramfs 文件) */
    fd = open("hello.txt", 0);
    if (fd < 0) { printf("fileio: open hello.txt FAILED\n"); return 3; }
    int w = write(fd, "[epoch-libc v4]", 15);
    close(fd);
    printf("fileio: hello.txt append write=%d bytes\n", w);

    /* 4) 重新读 hello.txt 验证已追加 */
    fd = open("hello.txt", 0);
    if (fd >= 0) {
        char again[96];
        int k = read(fd, again, 95);
        again[k > 0 ? k : 0] = 0;
        close(fd);
        printf("fileio: hello.txt now (%d bytes): %s", k, again);
        if (again[k > 0 ? k - 1 : 0] != '\n') printf("\n");
    }

    /* 5) 堆压力: 多轮 malloc/free, 观察页级扩展与释放复用 */
    printf("fileio: heap brk0 test, malloc chain:\n");
    char* a1 = (char*)malloc(32);
    char* a2 = (char*)malloc(512);
    char* a3 = (char*)malloc(2048);
    printf("fileio:   a1=%x a2=%x a3=%x\n", (unsigned)a1, (unsigned)a2,
           (unsigned)a3);
    if (a1 && a2 && a3) {
        memset(a1, 'A', 31); a1[31] = 0;
        memset(a2, 'B', 511); a2[511] = 0;
        memset(a3, 'C', 2047); a3[2047] = 0;
        printf("fileio:   a1[0]=%c a2[0]=%c a3[2046]=%c\n",
               a1[0], a2[0], a3[2046]);
    }
    free(a2);                    /* 释放中块 -> 进入 free list */
    char* a4 = (char*)malloc(400);
    printf("fileio:   after free(512) malloc(400) -> a4=%x (reuse=%d)\n",
           (unsigned)a4, a4 != NULL ? (a4 == a2 ? 1 : 0) : 0);
    free(a1);
    free(a3);
    free(a4);
    free(big);
    printf("fileio: done\n");
    return 0;
}
