// ============================================================
// EpochOS - ELF32 加载器 (Linux ELF 静态链接程序兼容, 阶段3)
// 支持: ELF32 EXEC (ET_EXEC), EM_386 (x86), 仅 PT_LOAD 段
// 约束: 静态链接, 无动态链接器 (ld-linux.so 直接拒绝)
// ============================================================
#ifndef EPOCHOS_ELF_H
#define EPOCHOS_ELF_H

#include "types.h"

// ELF32 magic: 0x7F 'E' 'L' 'F' (小端 0x464C457F)
#define ELF_MAGIC32       0x464C457F
#define ELFCLASS32        1
#define ELFDATA2LSB       1
#define ET_EXEC           2
#define EM_386            3

// Program Header 类型
#define PT_LOAD           1

// ELF32 文件头 (52 字节)
struct elf32_ehdr {
    uint8_t  e_ident[16];      // 0x00 magic[4] + class + data + ...
    uint16_t e_type;           // 0x10
    uint16_t e_machine;        // 0x12
    uint32_t e_version;        // 0x14
    uint32_t e_entry;          // 0x18 入口虚拟地址
    uint32_t e_phoff;          // 0x1C program header 偏移
    uint32_t e_shoff;          // 0x20 section header 偏移
    uint32_t e_flags;          // 0x24
    uint16_t e_ehsize;         // 0x28
    uint16_t e_phentsize;      // 0x2A
    uint16_t e_phnum;          // 0x2C
    uint16_t e_shentsize;      // 0x2E
    uint16_t e_shnum;          // 0x30
    uint16_t e_shstrndx;       // 0x32
} __attribute__((packed));

// ELF32 Program Header (32 字节)
struct elf32_phdr {
    uint32_t p_type;           // 0x00
    uint32_t p_offset;         // 0x04 文件内偏移
    uint32_t p_vaddr;          // 0x08 虚拟地址
    uint32_t p_paddr;          // 0x0C 物理地址
    uint32_t p_filesz;         // 0x10 文件大小
    uint32_t p_memsz;          // 0x14 内存大小 (>= filesz, 差额为 bss)
    uint32_t p_flags;          // 0x18 PF_X/PF_W/PF_R
    uint32_t p_align;          // 0x1C
} __attribute__((packed));

#define ELF_MAX_PHDR 8

// 校验并解析 ELF32 头与 Program Headers (纯解析, 无副作用)
// 成功返回 phnum (>=0), 失败返回 -1
int elf_parse(const uint8_t* elf, uint32_t len,
              struct elf32_ehdr* eh, struct elf32_phdr* ph, int max_ph);

// 从 ramfs 读取 ELF 文件并创建用户进程
// path: 文件系统路径 (如 "bin/hello_elf"); name: 进程名 (argv[0])
// argc>=1, argv 其余为命令行参数 (argv[1..])
// 成功返回 slot id, 失败返回 -1
int elf_load_from_fs(const char* path, const char* name,
                     int argc, const char* const* argv);

#endif
