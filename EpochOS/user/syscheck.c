/* ============================================================
 * EpochOS epoch-libc 测试程序: syscall 2=getpid / 19=lseek 探针
 * 运行: elf /bin/syscheck
 * 实证:
 *   - getpid() 返回当前进程 pid (>0)
 *   - lseek(fd, off, whence) 三种 whence 返回新偏移
 *     0=SEEK_SET / 1=SEEK_CUR / 2=SEEK_END
 *   - lseek 失败路径: 已 close 的 fd、负偏移 -> -1
 * ============================================================ */
#include "epoch.h"

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    int rc = 0;
    printf("syscheck: start (probe getpid/lseek)\n");

    /* 1) getpid: 应 > 0 */
    int pid = getpid();
    printf("syscheck: getpid()=%d\n", pid);
    if (pid <= 0) { printf("syscheck: FAIL getpid (expect >0)\n"); return 1; }

    /* 2) open welcome.txt (预置文件) */
    int fd = open("welcome.txt", 0);
    if (fd < 0) { printf("syscheck: FAIL open welcome.txt\n"); return 2; }
    printf("syscheck: open welcome.txt fd=%d\n", fd);

    /* 3) lseek SEEK_SET=0: 定位到偏移 5 */
    int p0 = lseek(fd, 5, 0);
    char s[16];
    int n = read(fd, s, 3);
    s[n > 0 ? n : 0] = 0;
    printf("syscheck: lseek(5,SET)=%d read3=\"%s\"\n", p0, n > 0 ? s : "?");
    if (p0 != 5) { printf("syscheck: FAIL lseek SET\n"); rc = 1; }

    /* 4) lseek SEEK_CUR=1: 从当前位置 (8) 再 +2 -> 10 */
    int p1 = lseek(fd, 2, 1);
    printf("syscheck: lseek(2,CUR)=%d (expect 10)\n", p1);
    if (p1 != 10) { printf("syscheck: FAIL lseek CUR\n"); rc = 1; }

    /* 5) lseek SEEK_END=2: 定位到文件尾 -4 */
    int p2 = lseek(fd, -4, 2);
    int m = read(fd, s, 4);
    s[m > 0 ? m : 0] = 0;
    printf("syscheck: lseek(-4,END)=%d read4=\"%s\"\n", p2, m > 0 ? s : "?");
    if (p2 < 0) { printf("syscheck: FAIL lseek END\n"); rc = 1; }

    close(fd);

    /* 6) lseek 失败路径: fd 已 close -> -1 */
    int bad1 = lseek(fd, 0, 0);
    printf("syscheck: lseek(closed fd)=%d (expect -1)\n", bad1);
    if (bad1 != -1) { printf("syscheck: FAIL closed-fd guard\n"); rc = 1; }

    /* 7) lseek 失败路径: 负偏移 -> -1 */
    int fd2 = open("welcome.txt", 0);
    int bad2 = lseek(fd2, -1, 0);
    printf("syscheck: lseek(-1,SET)=%d (expect -1)\n", bad2);
    if (bad2 != -1) { printf("syscheck: FAIL negative-offset guard\n"); rc = 1; }
    close(fd2);

    if (rc == 0) {
        printf("syscheck: PASS all getpid/lseek probes\n");
        return 0;
    }
    printf("syscheck: FAILED\n");
    return rc;
}
