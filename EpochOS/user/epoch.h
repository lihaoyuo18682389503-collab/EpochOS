/* ============================================================
 * EpochOS epoch-libc 用户态库 - 公共头文件 (阶段4)
 * 无任何系统依赖: 仅定义本库导出 API
 * 编译: -m32 -ffreestanding -fno-builtin -nostdlib
 * ============================================================ */
#ifndef EPOCH_LIBC_H
#define EPOCH_LIBC_H

#define NULL ((void*)0)

typedef unsigned int   size_t;
typedef signed int     ssize_t;

/* ---- 进程入口 (crt0 回调, 库内部) ---- */
int main(int argc, char** argv);

/* ---- 进程控制 ---- */
void exit(int code) __attribute__((noreturn));
void _exit(int code) __attribute__((noreturn));

/* ---- 系统调用包装 ---- */
int write(int fd, const void* buf, int count);
int read(int fd, void* buf, int count);
int open(const char* path, int flags);
int close(int fd);
int brk(void* addr);
void* sbrk(int incr);
int lseek(int fd, int offset, int whence);   /* 0=SET 1=CUR 2=END */
int getpid(void);
struct iovec { void* iov_base; unsigned int iov_len; };
int writev(int fd, const struct iovec* iov, int iovcnt);
int ioctl(int fd, int request, void* argp);
struct epoch_sysinfo {
    unsigned int uptime;     /* 秒 */
    unsigned int totalram;
    unsigned int freeram;
    unsigned int procs;      /* 任务数 */
    unsigned int mem_unit;   /* 1 = 字节 */
    unsigned int pad[11];
};
int sysinfo(struct epoch_sysinfo* info);
/* execve: 成功由内核接管当前进程 (不返回); 失败返回 -1 */
int execve(const char* path, char** argv, char** envp);

/* ---- 阶段6: 拓宽的 POSIX 封装 (兼容经适配工具链编译的 Linux 程序) ---- */
struct timeval { long tv_sec; long tv_usec; };
struct timespec { long tv_sec; long tv_nsec; };
struct stat {
    unsigned int st_dev;
    unsigned int st_ino;
    unsigned int st_mode;
    unsigned int st_nlink;
    unsigned int st_uid;
    unsigned int st_gid;
    unsigned int st_rdev;
    unsigned int st_size;
    unsigned int st_blksize;
    unsigned int st_blocks;
    unsigned int st_atime;
    unsigned int st_mtime;
    unsigned int st_ctime;
};
#define MAP_PRIVATE    0x02
#define MAP_ANONYMOUS  0x20
#define MAP_FIXED      0x10
#define PROT_READ      0x1
#define PROT_WRITE     0x2
#define PROT_EXEC      0x4
/* Linux open(2) flags (与内核 syscall.c 一致) */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0x40
#define O_TRUNC   0x200
#define O_APPEND  0x400
#define S_IFMT   0xF000
#define S_IFREG  0x8000
#define S_IFDIR  0x4000
#define S_IFCHR  0x2000
void* mmap(void* addr, size_t length, int prot, int flags, int fd, unsigned int offset);
int munmap(void* addr, size_t length);
int gettimeofday(struct timeval* tv, void* tz);
int nanosleep(const struct timespec* req, struct timespec* rem);
void exit_group(int code) __attribute__((noreturn));
int fstat(int fd, struct stat* st);
int stat(const char* path, struct stat* st);

/* ---- 标准输出 ---- */
int putchar(int c);
int puts(const char* s);
int printf(const char* fmt, ...);

/* ---- 格式化写缓冲 (阶段5) ---- */
int sprintf(char* buf, const char* fmt, ...);
int snprintf(char* buf, int cap, const char* fmt, ...);

/* ---- 字符串 / 内存 ---- */
size_t strlen(const char* s);
int strcmp(const char* a, const char* b);
char* strcpy(char* dst, const char* src);
char* strncpy(char* dst, const char* src, int n);
char* strcat(char* dst, const char* src);
void* memcpy(void* dst, const void* src, size_t n);
void* memset(void* dst, int c, size_t n);
int atoi(const char* s);
int isdigit(int c);
int toupper(int c);
int tolower(int c);

/* ---- 标准 I/O (文件流, 基于 open/close/read/write, 阶段5) ---- */
typedef struct { int fd; } FILE;
#define EOF (-1)
FILE* fopen(const char* path, const char* mode);
int fclose(FILE* f);
int fread(void* ptr, int size, int nmemb, FILE* f);
int fwrite(const void* ptr, int size, int nmemb, FILE* f);

/* ---- 堆 (brk 之上实现) ---- */
void* malloc(size_t size);
void free(void* p);

#endif
