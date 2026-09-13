// ============================================================
// EpochOS - 分页 / 虚拟内存管理
// 4KB 页, 32 位页目录 + 页表, 恒等映射内核地址空间
// ============================================================
#ifndef EPOCHOS_PAGING_H
#define EPOCHOS_PAGING_H

#include "types.h"

// 页表项标志
#define PAGE_PRESENT  0x1
#define PAGE_WRITABLE 0x2
#define PAGE_USER     0x4
#define PAGE_NX       0x80000000  // 32 位 PAE 无 NX, 保留定义

// 页目录项 / 页表项共用结构 (低 12 位标志, 高 20 位物理地址)
typedef uint32_t page_entry_t;

// 页目录 (1024 项 -> 4GB), 页表 (1024 项 -> 4MB)
extern page_entry_t* kernel_page_directory;

// 内核虚拟基址: 经典 3G+1G 布局, 物理 [0, MEMORY_END) 同时映射到
// 虚拟 [KERNEL_VIRT_BASE, KERNEL_VIRT_BASE+MEMORY_END) (复用同一组页表)
#define KERNEL_VIRT_BASE 0xC0000000

// 初始化分页: 分配页目录/页表, 恒等映射 [0, MEMORY_END),
// 高地址内核映射 [KERNEL_VIRT_BASE, ...), 开启 CR0.PG
void paging_init(void);

// 映射虚拟页到物理页
// flags: PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER ...
int paging_map_page(uint32_t virt, uint32_t phys, uint32_t flags);

// 映射到指定页目录 (用户进程地址空间), 不 invlpg (PD 未装载)
int paging_map_page_in(page_entry_t* pd, uint32_t virt, uint32_t phys, uint32_t flags);

// 取消映射, 返回 0 成功
int paging_unmap_page(uint32_t virt);

// 查询虚拟地址对应的物理地址 (未映射返回 0)
uint32_t paging_get_phys(uint32_t virt);

// 查询页是否已映射
int paging_is_mapped(uint32_t virt);

// 校验 [virt, virt+len) 是否完全落在“当前地址空间的用户可访问页”上
// (每张页都需 PRESENT 且带 PAGE_USER)。系统调用拷贝用户缓冲区前用它做越权/缺页防护,
// 避免用户传入内核指针导致越权读写或三重故障。
int paging_user_access_ok(uint32_t virt, uint32_t len);

// 统计已映射页数
uint32_t paging_mapped_count(void);

// ---- VMM 别名接口 ----
int map_page(uint32_t virt, uint32_t phys, uint32_t flags);
int unmap_page(uint32_t virt);

#endif
