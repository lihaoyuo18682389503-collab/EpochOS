// ============================================================
// EpochOS - ELF32 加载器实现 (阶段3)
// elf_parse: 纯解析校验 (供 task.c process_create_elf 使用)
// elf_load_from_fs: ramfs -> 解析 -> 创建用户进程
// ============================================================
#include "elf.h"
#include "fs.h"
#include "task.h"
#include "string.h"
#include "serial.h"

// 校验 ELF header 与 phdr 表, 填充 eh/ph
int elf_parse(const uint8_t* elf, uint32_t len,
              struct elf32_ehdr* eh, struct elf32_phdr* ph, int max_ph) {
    if (!elf || !eh || !ph || len < sizeof(struct elf32_ehdr)) return -1;

    // Windows PE/EXE 检测 (DOS 头首两字节 "MZ"): EpochOS 无法运行 Windows 程序
    // (需要完整 PE 加载器 + Win32k/GDI/User 子系统, 超出从零内核的范畴),
    // 这里给出明确、友好的拒绝信息, 而不是报晦涩的 "bad magic"。
    if (elf[0] == 'M' && elf[1] == 'Z') {
        serial_write_str("[elf] Windows PE/EXE (MZ header) detected - NOT supported by EpochOS.\n");
        serial_write_str("[elf]       (Running Windows .exe needs a PE loader + Win32k/GDI/User subsystem; out of scope for this from-scratch kernel.)\n");
        return -1;
    }

    // 复制头避免未对齐访问
    memcpy(eh, elf, sizeof(struct elf32_ehdr));

    // magic / class / data / type / machine 校验
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') {
        serial_write_str("[elf] bad magic\n");
        return -1;
    }
    if (eh->e_ident[4] != ELFCLASS32) {
        serial_write_str("[elf] not 32-bit\n");
        return -1;
    }
    if (eh->e_ident[5] != ELFDATA2LSB) {
        serial_write_str("[elf] not little-endian\n");
        return -1;
    }
    if (eh->e_type != ET_EXEC) {
        serial_printf(COM1, "[elf] not ET_EXEC (type=%u) - dynamic/shared unsupported\n", eh->e_type);
        return -1;
    }
    if (eh->e_machine != EM_386) {
        serial_printf(COM1, "[elf] not EM_386 (machine=%u)\n", eh->e_machine);
        return -1;
    }
    if (eh->e_phoff == 0 || eh->e_phnum == 0) {
        serial_write_str("[elf] no program headers\n");
        return -1;
    }
    if (eh->e_phnum > (uint16_t)max_ph) {
        serial_printf(COM1, "[elf] too many phdr (%u)\n", eh->e_phnum);
        return -1;
    }
    if ((uint32_t)eh->e_phoff + (uint32_t)eh->e_phnum * sizeof(struct elf32_phdr) > len) {
        serial_write_str("[elf] phdr table out of range\n");
        return -1;
    }

    // 解析 program headers
    for (int i = 0; i < eh->e_phnum; i++) {
        memcpy(&ph[i], elf + eh->e_phoff + i * sizeof(struct elf32_phdr),
               sizeof(struct elf32_phdr));
        if (ph[i].p_type == PT_LOAD) {
            // 段数据必须都在文件范围内 (filesz 部分)
            if (ph[i].p_offset + ph[i].p_filesz > len) {
                serial_printf(COM1, "[elf] seg%d file range overflow (off=%u sz=%u len=%u)\n",
                              i, ph[i].p_offset, ph[i].p_filesz, len);
                return -1;
            }
            if (ph[i].p_memsz < ph[i].p_filesz) {
                serial_write_str("[elf] memsz < filesz\n");
                return -1;
            }
        }
    }
    return eh->e_phnum;
}

// 高层入口: 从 ramfs 读取 ELF -> 解析 -> 创建用户进程
int elf_load_from_fs(const char* path, const char* name,
                     int argc, const char* const* argv) {
    if (!path || !name) return -1;
    uint8_t buf[FS_MAX_SIZE];
    int n = fs_read(path, buf, sizeof(buf));
    if (n < 0) {
        serial_printf(COM1, "[elf] fs_read failed: %s\n", path);
        return -1;
    }
    struct elf32_ehdr eh;
    struct elf32_phdr ph[ELF_MAX_PHDR];
    int phnum = elf_parse(buf, (uint32_t)n, &eh, ph, ELF_MAX_PHDR);
    if (phnum < 0) {
        serial_printf(COM1, "[elf] parse failed: %s\n", path);
        return -1;
    }
    int slot = process_create_elf(name, buf, (uint32_t)n, &eh, ph, phnum,
                                  argc, argv);
    if (slot < 0) {
        serial_printf(COM1, "[elf] process_create_elf failed: %s\n", path);
        return -1;
    }
    serial_printf(COM1, "[elf] loaded '%s' name='%s' entry=0x%x slot=%d\r\n",
                  path, name, eh.e_entry, slot);
    return slot;
}
