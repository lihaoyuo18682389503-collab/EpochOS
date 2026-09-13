// ============================================================
// EpochOS - GDT / TSS (ring3 用户态支持)
// 选择子: 0x00 null | 0x08 kcode | 0x10 kdata |
//         0x18 ucode(DPL3) | 0x20 udata(DPL3) | 0x28 TSS
// ============================================================
#ifndef EPOCHOS_GDT_H
#define EPOCHOS_GDT_H

#include "types.h"

#define GDT_KCODE 0x08
#define GDT_KDATA 0x10
#define GDT_UCODE 0x18
#define GDT_UDATA 0x20
#define GDT_TSS   0x28

// 32-bit TSS (104 字节), 与 Intel 文档一致
struct tss {
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

// 全局 TSS (供进程切换时更新 esp0)
extern struct tss g_tss;

// 加载新 GDT (boot2 只建了 ring0 段) 并加载 TSS (ltr)
void gdt_init(void);

#endif
