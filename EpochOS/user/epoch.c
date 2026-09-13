/* ============================================================
 * EpochOS epoch-libc 用户态库实现 (阶段4/5)
 * 纯用户态静态库: 直接 int 0x80 调 EpochOS 最小 syscall 子集
 *   exit(1) getpid(2) read(3) write(4) open(5) close(6)
 *   execve(11) lseek(19) writev(20) ioctl(54) brk(45) sysinfo(141)
 * 提供: crt 入口解析 argc/argv -> main -> exit
 *       printf/sprintf/snprintf(%d/%s/%x/%c/%u)
 *       puts/putchar / fopen/fclose/fread/fwrite
 *       strlen/strcmp/strcpy/strncpy/strcat/atoi/isdigit/toupper
 *       memcpy/memset / malloc/free (brk bump + free-list, 16B header)
 * 编译: i686-elf-gcc -m32 -ffreestanding -fno-pie -fno-stack-protector
 *       -fno-builtin -nostdlib -Os -ffunction-sections -fdata-sections
 * ============================================================ */
#include "epoch.h"
#include <stdarg.h>

/* ---- Linux i386 syscall 号 (与内核 syscall.h 一致) ---- */
#define SYS_EXIT   1
#define SYS_GETPID 2
#define SYS_READ   3
#define SYS_WRITE  4
#define SYS_OPEN   5
#define SYS_CLOSE  6
#define SYS_EXECVE 11
#define SYS_LSEEK  19
#define SYS_WRITEV 20
#define SYS_BRK    45
#define SYS_IOCTL  54
#define SYS_SYSINFO 141
/* 阶段6 新增 (与内核 syscall.h 一致) */
#define SYS_STAT         106
#define SYS_FSTAT        108
#define SYS_GETTIMEOFDAY 78
#define SYS_MMAP         90
#define SYS_MUNMAP       91
#define SYS_NANOSLEEP    162
#define SYS_EXIT_GROUP   252

/* Linux open flags (与内核一致) */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0x40
#define O_TRUNC   0x200

/* int 0x80 调用: eax=nr, ebx/ecx/edx = a/b/c; 返回 eax */
static long sys3(long nr, long a, long b, long c) {
    long ret;
    __asm__ __volatile__("int $0x80"
                         : "=a"(ret)
                         : "a"(nr), "b"(a), "c"(b), "d"(c)
                         : "memory");
    return ret;
}
static long sys1(long nr, long a) { return sys3(nr, a, 0, 0); }

/* 6 参 syscall: 第 6 参 (mmap 的 offset) 走 ebp */
static long sys6(long nr, long a, long b, long c, long d, long e, long f) {
    long ret;
    __asm__ __volatile__(
        "movl %7, %%ebp\n\t"
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e), "g"(f)
        : "%ebp", "memory");
    return ret;
}

/* ============================================================
 * 入口: 被 crt0 _start 以 &argc 调用; 解析 argc/argv, 调 main
 * ============================================================ */
void epoch_c_entry(unsigned int* sp) {
    int argc = (int)sp[0];
    char** argv = (char**)(sp + 1);
    int rc = main(argc, argv);
    exit(rc);
}

void _exit(int code) {
    sys1(SYS_EXIT, code);
    for (;;) { /* ring3 hlt 会 GPF: 纯自旋等调度器切走 */ }
}
void exit(int code) { _exit(code); }

/* ============================================================
 * 系统调用包装
 * ============================================================ */
int write(int fd, const void* buf, int count) {
    return (int)sys3(SYS_WRITE, fd, (long)buf, count);
}
int read(int fd, void* buf, int count) {
    return (int)sys3(SYS_READ, fd, (long)buf, count);
}
int open(const char* path, int flags) {
    return (int)sys3(SYS_OPEN, (long)path, flags, 0);
}
int close(int fd) {
    return (int)sys1(SYS_CLOSE, fd);
}
int brk(void* addr) {
    long r = sys1(SYS_BRK, (long)addr);
    return (r == (long)addr) ? 0 : -1;
}
void* sbrk(int incr) {
    long old = sys1(SYS_BRK, 0);
    if (incr == 0) return (void*)old;
    long nr = sys1(SYS_BRK, old + incr);
    return (nr == old + incr) ? (void*)old : (void*)-1;
}
int lseek(int fd, int offset, int whence) {
    return (int)sys3(SYS_LSEEK, fd, offset, whence);
}
int getpid(void) {
    return (int)sys1(SYS_GETPID, 0);
}
int writev(int fd, const struct iovec* iov, int iovcnt) {
    return (int)sys3(SYS_WRITEV, fd, (long)iov, iovcnt);
}
int ioctl(int fd, int request, void* argp) {
    return (int)sys3(SYS_IOCTL, fd, request, (long)argp);
}
int sysinfo(struct epoch_sysinfo* info) {
    if (!info) return -1;
    return (int)sys1(SYS_SYSINFO, (long)info);
}
int execve(const char* path, char** argv, char** envp) {
    long r = sys3(SYS_EXECVE, (long)path, (long)argv, (long)envp);
    return (int)r;             /* 成功时内核改写 iret 帧, 不会返回 */
}

/* ============================================================
 * 阶段6: 拓宽的 POSIX 封装 (兼容经适配工具链编译的 Linux 程序)
 * ============================================================ */
void* mmap(void* addr, size_t length, int prot, int flags, int fd, unsigned int offset) {
    return (void*)sys6(SYS_MMAP, (long)addr, (long)length, (long)prot,
                       (long)flags, (long)fd, (long)offset);
}
int munmap(void* addr, size_t length) {
    return (int)sys3(SYS_MUNMAP, (long)addr, (long)length, 0);
}
int gettimeofday(struct timeval* tv, void* tz) {
    return (int)sys3(SYS_GETTIMEOFDAY, (long)tv, (long)tz, 0);
}
int nanosleep(const struct timespec* req, struct timespec* rem) {
    return (int)sys3(SYS_NANOSLEEP, (long)req, (long)rem, 0);
}
void exit_group(int code) {
    sys1(SYS_EXIT_GROUP, code);
    for (;;) { /* ring3 hlt 会 GPF: 纯自旋等调度器切走 */ }
}
int fstat(int fd, struct stat* st) {
    return (int)sys3(SYS_FSTAT, (long)fd, (long)st, 0);
}
int stat(const char* path, struct stat* st) {
    return (int)sys3(SYS_STAT, (long)path, (long)st, 0);
}

/* ============================================================
 * 输出: 行缓冲一次 write, 减少 syscall
 * ============================================================ */
static char outbuf[160];
static int outn = 0;

static void out_flush(void) {
    if (outn > 0) {
        write(1, outbuf, outn);
        outn = 0;
    }
}
static void out_char(char c) {
    if (outn >= (int)sizeof(outbuf)) out_flush();
    outbuf[outn++] = c;
}

int putchar(int c) {
    char b = (char)c;
    int r = write(1, &b, 1);
    return (r == 1) ? c : -1;
}

int puts(const char* s) {
    int n = write(1, s, (int)strlen(s));
    write(1, "\n", 1);
    return n >= 0 ? 0 : -1;
}

/* 反向填充十进制/十六进制数字, 返回长度 */
static int fmt_unsigned(char* dst, unsigned long v, int base, int upper) {
    char tmp[16];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v > 0) {
        int d = (int)(v % (unsigned long)base);
        tmp[i++] = (char)(d < 10 ? ('0' + d)
                                 : (upper ? ('A' + d - 10) : ('a' + d - 10)));
        v /= (unsigned long)base;
    }
    for (int j = 0; j < i; j++) dst[j] = tmp[i - 1 - j];
    return i;
}

int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') { out_char(*p); continue; }
        p++;
        if (!*p) break;
        switch (*p) {
        case '%': out_char('%'); break;
        case 'c': out_char((char)va_arg(ap, int)); break;
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            for (; *s; s++) out_char(*s);
            break;
        }
        case 'd': {
            int v = va_arg(ap, int);
            char tmp[16];
            int n = 0;
            if (v < 0) { out_char('-'); v = -v; }
            n = fmt_unsigned(tmp, (unsigned long)v, 10, 0);
            for (int i = 0; i < n; i++) out_char(tmp[i]);
            break;
        }
        case 'u': {
            char tmp[16];
            int n = fmt_unsigned(tmp, (unsigned long)va_arg(ap, unsigned int), 10, 0);
            for (int i = 0; i < n; i++) out_char(tmp[i]);
            break;
        }
        case 'x':
        case 'X': {
            char tmp[16];
            int n = fmt_unsigned(tmp, (unsigned long)va_arg(ap, unsigned int), 16,
                                 *p == 'X');
            for (int i = 0; i < n; i++) out_char(tmp[i]);
            break;
        }
        default:
            out_char('%');
            out_char(*p);
            break;
        }
    }
    va_end(ap);
    out_flush();
    return 0;
}

/* ============================================================
 * 缓冲格式化: sprintf/snprintf (%s %d %x %X %c %u %%)
 * vformat 返回应写长度 (snprintf 语义), cap==0 时只测长不写
 * ============================================================ */
static int vformat(char* dst, int cap, const char* fmt, va_list ap) {
    int n = 0;
#define FMT_PUTC(c) do { if (dst && n + 1 < cap) dst[n] = (char)(c); n++; } while (0)
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') { FMT_PUTC(*p); continue; }
        p++;
        if (!*p) break;
        switch (*p) {
        case '%': FMT_PUTC('%'); break;
        case 'c': FMT_PUTC((char)va_arg(ap, int)); break;
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            for (; *s; s++) FMT_PUTC(*s);
            break;
        }
        case 'd': {
            int v = va_arg(ap, int);
            char tmp[16];
            int m, i;
            if (v < 0) { FMT_PUTC('-'); v = -v; }
            m = fmt_unsigned(tmp, (unsigned long)v, 10, 0);
            for (i = 0; i < m; i++) FMT_PUTC(tmp[i]);
            break;
        }
        case 'u': {
            char tmp[16];
            int m = fmt_unsigned(tmp, (unsigned long)va_arg(ap, unsigned int), 10, 0);
            for (int i = 0; i < m; i++) FMT_PUTC(tmp[i]);
            break;
        }
        case 'x':
        case 'X': {
            char tmp[16];
            int m = fmt_unsigned(tmp, (unsigned long)va_arg(ap, unsigned int), 16,
                                 *p == 'X');
            for (int i = 0; i < m; i++) FMT_PUTC(tmp[i]);
            break;
        }
        default:
            FMT_PUTC('%');
            FMT_PUTC(*p);
            break;
        }
    }
    if (dst && cap > 0) dst[n < cap ? n : cap - 1] = 0;
    return n;
#undef FMT_PUTC
}

int sprintf(char* buf, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vformat(buf, 1 << 28, fmt, ap);
    va_end(ap);
    return n;
}

int snprintf(char* buf, int cap, const char* fmt, ...) {
    if (cap <= 0) return 0;                 /* 简化: 无空间直接失败 */
    va_list ap;
    va_start(ap, fmt);
    int n = vformat(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

/* ============================================================
 * 字符串 / 内存
 * ============================================================ */
size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
char* strcpy(char* dst, const char* src) {
    char* d = dst;
    while ((*d++ = *src++)) ;
    return dst;
}
char* strncpy(char* dst, const char* src, int n) {
    int i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}
char* strcat(char* dst, const char* src) {
    char* d = dst;
    while (*d) d++;
    while ((*d++ = *src++)) ;
    return dst;
}
int isdigit(int c) { return c >= '0' && c <= '9'; }
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
    return dst;
}
void* memset(void* dst, int c, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}
int atoi(const char* s) {
    int v = 0, neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-' || *s == '+') { neg = (*s == '-'); s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

/* ============================================================
 * 标准 I/O: fopen/fclose/fread/fwrite
 * 内核文件 fd 语义: read 按游标 pos; write 恒追加到文件尾
 * ============================================================ */
FILE* fopen(const char* path, const char* mode) {
    if (!path || !mode) return NULL;
    int flags;
    switch (mode[0]) {
    case 'r': flags = (mode[1] == '+') ? O_RDWR : O_RDONLY; break;
    case 'w': flags = O_WRONLY | O_CREAT | O_TRUNC; break;
    case 'a': flags = O_WRONLY | O_CREAT; break;
    default:  return NULL;
    }
    int fd = open(path, flags);
    if (fd < 0) return NULL;
    FILE* f = (FILE*)malloc(sizeof(FILE));
    if (!f) { close(fd); return NULL; }
    f->fd = fd;
    return f;
}
int fclose(FILE* f) {
    if (!f || f->fd < 0) return EOF;
    int r = close(f->fd);
    f->fd = -1;
    free(f);
    return (r == 0) ? 0 : EOF;
}
int fread(void* ptr, int size, int nmemb, FILE* f) {
    if (!f || f->fd < 0 || size <= 0 || nmemb <= 0) return 0;
    int total = size * nmemb;
    int got = 0;
    while (got < total) {
        int n = read(f->fd, (char*)ptr + got, total - got);
        if (n <= 0) break;
        got += n;
    }
    return got / size;
}
int fwrite(const void* ptr, int size, int nmemb, FILE* f) {
    if (!f || f->fd < 0 || size <= 0 || nmemb <= 0) return 0;
    int total = size * nmemb;
    int done = 0;
    while (done < total) {
        int n = write(f->fd, (const char*)ptr + done, total - done);
        if (n <= 0) break;              /* 出错/磁盘满: 返回已写完整元素数 */
        done += n;
    }
    return done / size;
}

/* ============================================================
 * 堆: brk 之上 bump + free-list (first-fit, 16B header)
 * ============================================================ */
#define HBLK_ALIGN   8
#define HBLK_MAGIC   0xE0C0C0DEu
typedef struct hblk {
    unsigned long size;     /* 整块大小 (含 header) */
    int used;               /* 1=在用 0=空闲 */
    struct hblk* next;      /* free list 链接 */
    unsigned int magic;     /* 完整性校验 */
} hblk;

static int heap_ready = 0;
static unsigned long heap_base = 0;  /* 堆首 (== 进程 brk0) */
static unsigned long heap_top = 0;   /* bump 顶 (下一块写入地址) */
static unsigned long heap_brk = 0;   /* 内核已承诺的 brk 上限 */
static hblk* hfree = 0;              /* 空闲块链表 (地址序) */

void* malloc(size_t size) {
    if (!heap_ready) {
        long b0 = sys1(SYS_BRK, 0);          /* 查询初始 break */
        if (b0 <= 0) return NULL;
        heap_base = (unsigned long)b0;
        heap_top = heap_base;
        heap_brk = heap_base;
        heap_ready = 1;
    }
    unsigned long need = (unsigned long)size + sizeof(hblk);
    need = (need + HBLK_ALIGN - 1) & ~(HBLK_ALIGN - 1);

    /* first-fit 空闲块复用 (命中块须从 free list 摘除, 否则重复入链成环) */
    hblk** ppf = &hfree;
    while (*ppf) {
        hblk* b = *ppf;
        if (!b->used && b->size >= need) {
            *ppf = b->next;          /* 摘除 */
            b->next = 0;
            b->used = 1;
            b->magic = HBLK_MAGIC;
            return (char*)b + sizeof(hblk);
        }
        ppf = &b->next;
    }
    /* bump: 内核承诺不足时整页扩展 brk */
    if (heap_top + need > heap_brk) {
        unsigned long req = (heap_top + need + 4095) & ~4095UL;
        long nb = sys1(SYS_BRK, (long)req);
        if (nb != (long)req) return NULL;    /* 超限/OOM */
        heap_brk = req;
    }
    hblk* nb = (hblk*)heap_top;
    nb->size = need;
    nb->used = 1;
    nb->next = 0;
    nb->magic = HBLK_MAGIC;
    heap_top += need;
    return (char*)nb + sizeof(hblk);
}

void free(void* p) {
    if (!p) return;
    hblk* b = (hblk*)((char*)p - sizeof(hblk));
    if (b->magic != HBLK_MAGIC || b->used == 0) return;  /* 坏指针/重复释放防御 */
    b->used = 0;

    /* 按地址序插入 free list */
    hblk** pp = &hfree;
    while (*pp && (unsigned long)*pp < (unsigned long)b) pp = &(*pp)->next;
    b->next = *pp;
    *pp = b;

    /* 与后继合并 */
    if (b->next && (char*)b + b->size == (char*)b->next) {
        b->size += b->next->size;
        b->next = b->next->next;
    }
    /* 与前驱合并 (扫描 free list) */
    for (hblk* q = hfree; q; q = q->next) {
        if (q != b && !q->used && q->next == b && (char*)q + q->size == (char*)b) {
            q->size += b->size;
            q->next = b->next;
            break;
        }
    }
}
