/* ============================================================
 * EpochOS epoch-libc 测试程序 6: 字符串/格式化 + 新 syscall (阶段5)
 * 验证: sprintf/snprintf(strcpy/strncpy/strcat/isdigit/toupper)
 *       getpid / ioctl / writev / execve (进程替换为 argecho)
 * 运行: elf /bin/strtest
 * ============================================================ */
#include "epoch.h"

int main(int argc, char** argv) {
    (void)argv;
    printf("str: pid=%d argc=%d start\n", getpid(), argc);

    /* sprintf: %s %d %x %c */
    char buf[80];
    int n1 = sprintf(buf, "[%s d=%d x=%x c=%c]", "EpochOS", -42, 0x5A, 'Z');
    printf("str: sprintf(%d)=%s\n", n1, buf);

    /* snprintf: 截断仍返回应写长度, 缓冲区安全终止 */
    char small[8];
    int n2 = snprintf(small, 8, "Hello%s", "World12345678");
    printf("str: snprintf(cap=8) len=%d val=%s\n", n2, small);

    /* 字符串函数 */
    char s1[40], s2[16];
    strcpy(s1, "hello");
    strcat(s1, "-world");
    strncpy(s2, "abcdefgh", 3);
    s2[3] = 0;
    printf("str: strcpy+strcat=%s strncpy(3)=%s strcmp=%d\n",
           s1, s2, strcmp(s1, "hello-world"));

    /* ctype */
    printf("str: isdigit('5')=%d isdigit('a')=%d toupper('q')=%c tolower('Q')=%c\n",
           isdigit('5'), isdigit('a'), toupper('q'), tolower('Q'));

    /* ioctl(TCGETS) 控制台窗口 */
    char term[64];
    int ir = ioctl(1, 0x5401, term);
    printf("str: ioctl(fd=1,TCGETS)=%d\n", ir);

    /* writev: 两段缓冲一次性写 stdout */
    char p1[] = "str: writev A+B -> ";
    char p2[] = "two-buffers ok!\n";
    struct iovec iov[2];
    iov[0].iov_base = p1;
    iov[0].iov_len = sizeof(p1) - 1;
    iov[1].iov_base = p2;
    iov[1].iov_len = sizeof(p2) - 1;
    int wv = writev(1, iov, 2);
    printf("str: writev wrote %d bytes\n", wv);

    /* execve: 替换当前进程为 argecho (内核接管, 正常不返回) */
    printf("str: execve('/bin/argecho', [argecho exec ok]) ...\n");
    char* av[] = { "argecho", "exec", "ok", 0 };
    int er = execve("/bin/argecho", av, 0);
    printf("str: execve unexpected return %d\n", er);
    return 3;
}
