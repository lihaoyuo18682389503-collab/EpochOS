/* ============================================================
 * EpochOS - ELF loader 测试程序 2 (C 静态 ELF)
 * 无 libc: 直接 int 0x80 调 EpochOS 最小 syscall 子集
 *   write(1) / open / read / close / brk(0) / exit(7)
 * 编译: i686-elf-gcc -m32 -ffreestanding -fno-pie -fno-stack-protector
 *       -fno-builtin -nostdlib -static -c hello_c.c
 * 链接: i686-elf-ld -m elf_i386 -Ttext 0x08048000 -e _start
 * ============================================================ */
typedef unsigned int u32;
typedef unsigned char u8;

static u32 syscall3(u32 nr, u32 a, u32 b, u32 c) {
    u32 ret;
    __asm__ __volatile__("int $0x80"
                         : "=a"(ret)
                         : "a"(nr), "b"(a), "c"(b), "d"(c)
                         : "memory");
    return ret;
}

static u32 slen(const char* s) {
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

static void wmsg(const char* s) {
    syscall3(4, 1, (u32)s, slen(s));   /* SYS_write(fd=1) */
}

static void whex(u32 v) {
    char buf[8];
    int i;
    for (i = 0; i < 8; i++) {
        u32 d = (v >> (28 - i * 4)) & 0xF;
        buf[i] = (char)(d < 10 ? ('0' + d) : ('a' + d - 10));
    }
    syscall3(4, 1, (u32)buf, 8);       /* SYS_write */
    wmsg("\n");
}

void _start(void) {
    char buf[64];
    int fd, n;
    u32 brk0;

    wmsg("hello_c: C static ELF entry ok, brk0=");
    brk0 = syscall3(45, 0, 0, 0);      /* SYS_brk(0): 查询当前 break */
    whex(brk0);

    /* open + read + close: welcome.txt 由内核预置在 ramfs */
    fd = (int)syscall3(5, (u32)"welcome.txt", 0, 0);   /* SYS_open */
    if (fd >= 0) {
        n = (int)syscall3(3, (u32)fd, (u32)buf, 24);   /* SYS_read */
        syscall3(6, (u32)fd, 0, 0);                    /* SYS_close */
        wmsg("read(24)=");
        whex((u32)(n >= 0 ? (u32)n : 0xFFFFFFFFu));
        if (n > 0) {
            buf[n] = 0;
            wmsg("head=");
            wmsg(buf);
            wmsg("\n");
        }
    } else {
        wmsg("open failed\n");
    }

    syscall3(1, 7, 0, 0);              /* SYS_exit(7) */
    for (;;) { }                       /* 保护: exit 后 eip 不回改, 自旋 */
}
