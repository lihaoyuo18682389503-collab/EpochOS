// ============================================================
// EpochOS - 最小系统调用层 (Linux 兼容号)
// 用户态 int 0x80 -> syscall_handler
// ============================================================
#ifndef EPOCHOS_SYSCALL_H
#define EPOCHOS_SYSCALL_H

#include "interrupts.h"

// Linux x86 syscall 号 (阶段5扩展集)
#define SYS_EXIT    1
#define SYS_GETPID  2
#define SYS_READ    3
#define SYS_WRITE   4
#define SYS_OPEN    5
#define SYS_CLOSE   6
#define SYS_EXECVE  11
#define SYS_LSEEK   19
#define SYS_WRITEV  20
#define SYS_BRK     45
#define SYS_IOCTL   54
#define SYS_SYSINFO 141

// 阶段6扩展 (Linux 兼容 ABI 拓宽, 目标: 让经适配工具链编译的 Linux 程序可运行)
#define SYS_STAT         106   // stat(path, statbuf)
#define SYS_FSTAT        108   // fstat(fd, statbuf)
#define SYS_GETTIMEOFDAY 78    // gettimeofday(tv, tz)
#define SYS_MMAP         90    // mmap(addr,len,prot,flags,fd,offset) -> ebx/ecx/edx/esi/edi/ebp
#define SYS_MUNMAP       91    // munmap(addr, len)
#define SYS_NANOSLEEP    162   // nanosleep(req, rem)
#define SYS_EXIT_GROUP   252   // exit_group(code)

// int 0x80 分发入口 (由 isr_handler 调用)
void syscall_handler(struct regs* r);

// 初始化系统调用子系统 (清零 fd 表); 必须在任何用户进程运行前调用一次
void syscall_init(void);

#endif
