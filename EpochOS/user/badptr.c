/* ============================================================
 * EpochOS 用户态探针: 系统调用用户指针越权防护 (EFAULT guard)
 * 运行: elf /bin/badptr
 * 实证: 向各系统调用传入“内核区域”或“未映射”指针, 内核必须返回 -1
 *       (EFAULT) 而不是越权读写内核内存或触发缺页三重故障。
 *
 * 内核防护点 (kernel/syscall.c + kernel/paging.c):
 *   - do_write / do_read / do_open / do_writev / do_sysinfo / do_execve
 *   - copy_user_str 全程用 paging_user_access_ok 校验 (PRESENT && PAGE_USER)
 * 内核高位映射 0xC0000000 与恒等映射 [0, MEMORY_END) 均无 USER 位 -> 一律拒绝。
 * ============================================================ */
#include "epoch.h"

/* 故意指向内核区域 / 未映射区的“坏指针” */
#define KERN_HIGH   ((const void*)0xC0000000u)   /* 高地址内核映射, 无 USER 位 */
#define KERN_LOW    ((const void*)0x00000010u)   /* 恒等映射低位, 内核页 */
#define UNMAPPED    ((const void*)0x7FFFFFF0u)   /* 用户区上沿附近(通常未映射) */

static int g_fail = 0;

static void probe(const char* name, int got, int expect_neg) {
    if (expect_neg && got < 0) {
        printf("badptr: OK   %s -> %d (拒绝)\n", name, got);
    } else if (!expect_neg && got >= 0) {
        printf("badptr: OK   %s -> %d (接受)\n", name, got);
    } else {
        printf("badptr: FAIL %s -> %d (期望 %s)\n",
               name, got, expect_neg ? "拒绝(-1)" : "接受(>=0)");
        g_fail = 1;
    }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("badptr: start (probe EFAULT guard)\n");

    /* 1) write 到内核高位地址 -> 应拒绝 */
    probe("write(1, 0xC0000000, 5)",   write(1, KERN_HIGH, 5), 1);
    /* 2) write 到恒等映射低位内核页 -> 应拒绝 */
    probe("write(1, 0x10, 5)",         write(1, KERN_LOW, 5), 1);
    /* 3) write 到未映射用户区 -> 应拒绝 */
    probe("write(1, 0x7FFFFFF0, 5)",   write(1, UNMAPPED, 5), 1);

    /* 4) read 到内核低位地址 -> 应拒绝 (用真实 fd 才能走到 buf 校验) */
    int bf = open("/bin/badptr", 0);   /* 本程序自身必在镜像中, 确保 fd 有效 */
    if (bf >= 0) {
        probe("read(bf, 0x10, 4)",     read(bf, (void*)0x10, 4), 1);
        close(bf);
    } else {
        printf("badptr: SKIP read guard (open self failed)\n");
    }

    /* 5) open 路径指针指向内核区 -> 应拒绝 */
    probe("open(0xC0000000, 0)",       open((const char*)KERN_HIGH, 0), 1);

    /* 6) sysinfo 结构指针指向内核区 -> 应拒绝 */
    struct epoch_sysinfo si;
    probe("sysinfo(0xC0000000)",       sysinfo((struct epoch_sysinfo*)KERN_HIGH), 1);

    /* 7) writev iovec 数组指针指向内核区 -> 应拒绝 */
    struct iovec iov;
    iov.iov_base = (void*)"hi"; iov.iov_len = 2;
    probe("writev(1, 0xC0000000, 1)",  writev(1, (const struct iovec*)KERN_HIGH, 1), 1);

    /* 8) 对照组: 合法写入必须成功 (证明 guard 不误伤正常调用) */
    probe("write(1, \"ok\\n\", 3)",      write(1, "ok\n", 3), 0);

    if (g_fail == 0) {
        printf("badptr: PASS all EFAULT probes (内核拒绝越权用户指针)\n");
        return 0;
    }
    printf("badptr: FAILED\n");
    return 1;
}
