// ============================================================
// EpochOS - GDT / TSS 实现
// 在 boot2 的临时 GDT (null/kcode/kdata) 基础上扩展:
//   ucode(0x18, DPL=3) / udata(0x20, DPL=3) / TSS(0x28)
// 供 iret 进入 ring3 用户态与 int 0x80 特权级切换使用
// ============================================================
#include "gdt.h"
#include "paging.h"

struct gdt_entry {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;
    uint8_t  base_hi;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct gdt_entry gdt[6] __attribute__((aligned(8)));
static struct gdt_ptr gdtp;
struct tss g_tss;

// 设置普通 32 位平坦段描述符
static void gdt_set_seg(int idx, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[idx].limit_lo = limit & 0xFFFF;
    gdt[idx].base_lo  = base & 0xFFFF;
    gdt[idx].base_mid = (base >> 16) & 0xFF;
    gdt[idx].access   = access;
    gdt[idx].gran     = (uint8_t)(gran | ((limit >> 16) & 0x0F));
    gdt[idx].base_hi  = (base >> 24) & 0xFF;
}

// 设置 TSS 描述符 (字节粒度)
static void gdt_set_tss(int idx, uint32_t base, uint32_t limit) {
    gdt[idx].limit_lo = limit & 0xFFFF;
    gdt[idx].base_lo  = base & 0xFFFF;
    gdt[idx].base_mid = (base >> 16) & 0xFF;
    gdt[idx].access   = 0x89;    // present, ring0, 32-bit available TSS
    gdt[idx].gran     = 0x00;    // 字节粒度
    gdt[idx].base_hi  = (base >> 24) & 0xFF;
}

void gdt_init(void) {
    // 清零 GDT 与 TSS
    for (int i = 0; i < 6; i++) {
        gdt[i].limit_lo = 0;
        gdt[i].base_lo  = 0;
        gdt[i].base_mid = 0;
        gdt[i].access   = 0;
        gdt[i].gran     = 0;
        gdt[i].base_hi  = 0;
    }
    for (uint32_t i = 0; i < sizeof(struct tss) / 4; i++) {
        ((uint32_t*)&g_tss)[i] = 0;
    }

    gdt_set_seg(1, 0, 0xFFFFF, 0x9A, 0xCF);   // 0x08 kernel code (ring0, exec/read)
    gdt_set_seg(2, 0, 0xFFFFF, 0x92, 0xCF);   // 0x10 kernel data (ring0, r/w)
    gdt_set_seg(3, 0, 0xFFFFF, 0xFA, 0xCF);   // 0x18 user code (ring3, exec/read)
    gdt_set_seg(4, 0, 0xFFFFF, 0xF2, 0xCF);   // 0x20 user data (ring3, r/w)
    gdt_set_tss(5, (uint32_t)&g_tss, sizeof(struct tss) - 1);  // 0x28 TSS

    // 初始 esp0: 由用户进程创建/切换时更新; ss0 固定内核数据段
    g_tss.ss0 = GDT_KDATA;
    g_tss.esp0 = 0;

    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base  = (uint32_t)&gdt[0];

    __asm__ __volatile__("lgdt %0" : : "m"(gdtp));
    __asm__ __volatile__("ltr %%ax" : : "a"(GDT_TSS));
}
