// ============================================================
// EpochOS - 物理内存管理与简单堆分配器
// ============================================================
#ifndef EPOCHOS_MM_H
#define EPOCHOS_MM_H

#include "types.h"

// 内存布局约定 (物理内存)
// MEMORY_END 为可用上限; 堆起点由 mm_init 动态计算为 __bss_end 4KB 对齐,
// 保证 bss 膨胀时页分配器不会覆盖静态变量 (IDT/窗口数组等)
#define MEMORY_END        0x8000000  // 128MB 可用上限 (可按需调整)
extern uint32_t g_mem_start;         // 堆起点 (4KB 对齐, 由 mm_init 设置)
#define PAGE_SIZE         4096
#define MM_PAGE_SIZE      PAGE_SIZE   // 供外部统计使用

void mm_init(void);
void* kmalloc(size_t size);          // 对齐 4KB 的页分配 (物理连续)
void kfree(void* ptr);               // 释放页分配内存
void* kmalloc_small(size_t size);    // 小对象分配 (128B 块)
uint32_t mm_get_free_pages(void);
uint32_t mm_total_pages(void);       // 总页数
uint32_t mm_free_pages(void);        // 空闲页数
uint32_t mm_used_pages(void);        // 已用页数
uint32_t mm_small_total_blocks(void);// 小对象堆总块数
uint32_t mm_small_used_blocks(void); // 小对象堆已用块数

// ---- PMM 单页接口 ----
uint32_t alloc_page(void);           // 分配 1 页 (4KB), 返回物理地址, 0=失败
void free_page(uint32_t addr);       // 释放 1 页
int  reserve_page(uint32_t addr);    // 保留指定物理页 (已占用返回 -1)

// ---- bump 内核堆 (起步版: 线性分配, 不释放) ----
void* kmalloc_bump(size_t size);     // 任意大小, 8 字节对齐
uint32_t mm_bump_used(void);         // 已用字节
uint32_t mm_bump_free(void);         // 剩余字节

#endif
