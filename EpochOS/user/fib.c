/* ============================================================
 * EpochOS epoch-libc 测试程序 4: 斐波那契 + 素数 (阶段5)
 * 验证: printf / getpid / sysinfo (uptime/mem/procs)
 * 运行: elf /bin/fib
 * ============================================================ */
#include "epoch.h"

static int is_prime(int n) {
    if (n < 2) return 0;
    for (int i = 2; i * i <= n; i++) {
        if (n % i == 0) return 0;
    }
    return 1;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    int pid = getpid();
    printf("fib: pid=%d start\n", pid);

    /* 斐波那契数列 F0..F19 */
    printf("fib: fib(0..19): ");
    int a = 0, b = 1;
    for (int i = 0; i < 20; i++) {
        printf("%d%s", a, i == 19 ? "\n" : " ");
        int c = a + b;
        a = b;
        b = c;
    }

    /* 素数打印 <=200 */
    int cnt = 0;
    printf("fib: primes(<=200): ");
    for (int n = 2; n <= 200; n++) {
        if (is_prime(n)) {
            cnt++;
            if (cnt <= 12) printf("%d ", n);
        }
    }
    printf("... total=%d\n", cnt);

    /* sysinfo: uptime / 内存 / 进程数 (syscall 141) */
    struct epoch_sysinfo si;
    int sr = sysinfo(&si);
    if (sr >= 0) {
        printf("fib: sysinfo uptime=%ds ram=%dK free=%dK procs=%d unit=%d\n",
               si.uptime, si.totalram / 1024, si.freeram / 1024,
               si.procs, si.mem_unit);
    } else {
        printf("fib: sysinfo FAILED (%d)\n", sr);
    }

    printf("fib: done (pid=%d, exit 0)\n", pid);
    return 0;
}
