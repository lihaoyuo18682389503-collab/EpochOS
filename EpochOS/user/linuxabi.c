/* ============================================================
 * EpochOS 阶段6 ABI 自测程序 (ring3 ELF)
 * 验证: mmap / munmap / gettimeofday / nanosleep / fstat / exit_group
 * 期望内核输出: "linuxabi: PASS all"
 * ============================================================ */
#include "epoch.h"

static int g_fail = 0;

static void check(int ok, const char* msg) {
    if (ok) printf("[ok]   %s\n", msg);
    else { printf("[FAIL] %s\n", msg); g_fail++; }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("linuxabi: starting Linux-compatible ABI probe\n");

    /* 1. 匿名 mmap 1 页, 读写回环 */
    void* p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(p != (void*)-1, "mmap anonymous returns a valid address");
    if (p != (void*)-1) {
        *(int*)p = 0x12345678;
        check(*(int*)p == 0x12345678, "mmap region is readable/writable");
    }

    /* 2. gettimeofday */
    struct timeval tv;
    tv.tv_sec = 0; tv.tv_usec = 0;
    int g = gettimeofday(&tv, NULL);
    check(g == 0 && tv.tv_sec >= 0, "gettimeofday works");

    /* 3. nanosleep ~0.1s */
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    struct timespec req;
    req.tv_sec = 0; req.tv_nsec = 100000000;   /* 0.1s */
    int ns = nanosleep(&req, NULL);
    gettimeofday(&t1, NULL);
    int delta_ms = (int)((t1.tv_sec - t0.tv_sec) * 1000 +
                         (t1.tv_usec - t0.tv_usec) / 1000);
    char msg[64];
    snprintf(msg, sizeof(msg), "nanosleep ~0.1s (delta %d ms)", delta_ms);
    check(ns == 0 && delta_ms >= 50, msg);

    /* 4. fstat on a ramfs file */
    int fd = open("welcome.txt", O_RDONLY);
    check(fd >= 0, "open welcome.txt");
    struct stat st;
    if (fd >= 0) {
        int r = fstat(fd, &st);
        check(r == 0 && st.st_size > 0, "fstat reports file size > 0");
        close(fd);
    }

    /* 5. munmap */
    if (p != (void*)-1) {
        int r = munmap(p, 4096);
        check(r == 0, "munmap succeeds");
    }

    if (g_fail == 0) {
        printf("linuxabi: PASS all\n");
        exit_group(0);
    }
    printf("linuxabi: FAIL %d checks\n", g_fail);
    exit_group(1);
    return 0;
}
