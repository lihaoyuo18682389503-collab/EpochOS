// ============================================================
// EpochOS - 分页 / 虚拟内存管理实现
// 4KB 页: 页目录(1024) -> 页表(1024) -> 4KB 页
// 恒等映射 [0, MEMORY_END) 保证开启分页后内核不崩
// ============================================================

#include "paging.h"
#include "mm.h"

#define PAGE_SIZE  4096
#define ENTRIES    1024
#define DIRECTORY_INDEX(v) ((v) >> 22)
#define TABLE_INDEX(v)     (((v) >> 12) & 0x3FF)

page_entry_t* kernel_page_directory = 0;

static uint32_t page_tables[ENTRIES] __attribute__((aligned(4096)));

// 内部: 分配一页 (4KB 对齐) 用于页表, 失败返回 0
static uint32_t alloc_page_table(void) {
    uint32_t p = (uint32_t)kmalloc(4096);
    if (!p) return 0;
    // 清零
    for (int i = 0; i < 1024; i++)
        ((uint32_t*)p)[i] = 0;
    return p;
}

// 初始化分页: 建立页目录, 恒等映射 [0, MEMORY_END),
// 高地址内核映射 [KERNEL_VIRT_BASE, KERNEL_VIRT_BASE+MEMORY_END), 开启分页
void paging_init(void) {
    uint32_t pd = (uint32_t)kmalloc(4096);
    if (!pd) return;
    kernel_page_directory = (page_entry_t*)pd;

    // 清零页目录
    for (int i = 0; i < ENTRIES; i++)
        kernel_page_directory[i] = 0;

    // 恒等映射 [0, MEMORY_END) (4KB 粒度)
    for (uint32_t addr = 0; addr < MEMORY_END; addr += PAGE_SIZE) {
        paging_map_page(addr, addr, PAGE_PRESENT | PAGE_WRITABLE);
    }

    // 高地址内核映射: 虚拟 [KERNEL_VIRT_BASE + phys) -> phys
    // 复用恒等映射的页表 (PD[hi+k] = PD[k]), 零额外页表开销
    uint32_t hi = KERNEL_VIRT_BASE >> 22;                       // 768
    uint32_t n = (MEMORY_END + 0x3FFFFF) / 0x400000;            // 页表数
    for (uint32_t k = 0; k < n && (hi + k) < ENTRIES; k++) {
        kernel_page_directory[hi + k] = kernel_page_directory[k];
    }

    // 加载页目录并开启分页
    __asm__ __volatile__(
        "mov %0, %%cr3\n\t"
        "mov %%cr0, %%eax\n\t"
        "orl $0x80000000, %%eax\n\t"   // CR0.PG
        "mov %%eax, %%cr0\n\t"
        : : "r"(kernel_page_directory) : "eax", "memory");
}

// 映射虚拟页 -> 物理页 (指定页目录; 用户进程地址空间用)
// 注意: 不执行 invlpg —— 仅当 pd 就是当前 CR3 时才需要
int paging_map_page_in(page_entry_t* pd, uint32_t virt, uint32_t phys, uint32_t flags) {
    if (!pd) return -1;
    uint32_t pd_idx = DIRECTORY_INDEX(virt);
    uint32_t pt_idx = TABLE_INDEX(virt);

    page_entry_t* pt;
    if (pd[pd_idx] & PAGE_PRESENT) {
        // 页表已存在: 取物理地址
        pt = (page_entry_t*)(pd[pd_idx] & 0xFFFFF000);
    } else {
        // 分配新页表
        uint32_t pt_phys = alloc_page_table();
        if (!pt_phys) return -1;
        pd[pd_idx] = (pt_phys & 0xFFFFF000) | PAGE_PRESENT | PAGE_WRITABLE | (flags & PAGE_USER);
        pt = (page_entry_t*)pt_phys;
    }
    pt[pt_idx] = (phys & 0xFFFFF000) | (flags & 0xFFF);
    return 0;
}

// 映射虚拟页 -> 物理页 (当前内核页目录)
int paging_map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    int rc = paging_map_page_in(kernel_page_directory, virt, phys, flags);
    if (rc == 0) {
        __asm__ __volatile__("invlpg (%0)" : : "r"(virt) : "memory");
    }
    return rc;
}

// 取消映射
int paging_unmap_page(uint32_t virt) {
    if (!kernel_page_directory) return -1;
    uint32_t pd_idx = DIRECTORY_INDEX(virt);
    if (!(kernel_page_directory[pd_idx] & PAGE_PRESENT)) return -1;
    page_entry_t* pt = (page_entry_t*)(kernel_page_directory[pd_idx] & 0xFFFFF000);
    pt[TABLE_INDEX(virt)] = 0;
    __asm__ __volatile__("invlpg (%0)" : : "r"(virt) : "memory");
    return 0;
}

// 查询虚拟地址对应的物理地址
uint32_t paging_get_phys(uint32_t virt) {
    if (!kernel_page_directory) return 0;
    uint32_t pd_idx = DIRECTORY_INDEX(virt);
    if (!(kernel_page_directory[pd_idx] & PAGE_PRESENT)) return 0;
    page_entry_t* pt = (page_entry_t*)(kernel_page_directory[pd_idx] & 0xFFFFF000);
    page_entry_t e = pt[TABLE_INDEX(virt)];
    if (!(e & PAGE_PRESENT)) return 0;
    return (e & 0xFFFFF000) | (virt & 0xFFF);
}

// 页是否已映射
int paging_is_mapped(uint32_t virt) {
    if (!kernel_page_directory) return 0;
    uint32_t pd_idx = DIRECTORY_INDEX(virt);
    if (!(kernel_page_directory[pd_idx] & PAGE_PRESENT)) return 0;
    page_entry_t* pt = (page_entry_t*)(kernel_page_directory[pd_idx] & 0xFFFFF000);
    return (pt[TABLE_INDEX(virt)] & PAGE_PRESENT) ? 1 : 0;
}

// 校验 [virt, virt+len) 是否完全落在当前地址空间的用户可访问页上
int paging_user_access_ok(uint32_t virt, uint32_t len) {
    if (len == 0) return 1;                       // 空区间视为可访问
    if (len > (0xFFFFFFFFu - virt) + 1) return 0; // virt+len 越过 4GB 边界
    uint32_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    page_entry_t* pd = (page_entry_t*)(cr3 & 0xFFFFF000);  // 低内存恒等映射, 物理==虚拟
    uint32_t first = virt & 0xFFFFF000;
    uint32_t last  = (virt + len - 1) & 0xFFFFF000;
    for (uint32_t va = first; ; va += PAGE_SIZE) {
        uint32_t pd_idx = DIRECTORY_INDEX(va);
        if (!(pd[pd_idx] & PAGE_PRESENT)) return 0;
        page_entry_t* pt = (page_entry_t*)(pd[pd_idx] & 0xFFFFF000);
        page_entry_t e = pt[TABLE_INDEX(va)];
        if (!(e & PAGE_PRESENT)) return 0;
        if (!(e & PAGE_USER)) return 0;          // 内核页 (无 USER 位) 一律拒绝
        if (va == last) break;
    }
    return 1;
}

// 统计已映射页数 (遍历所有存在的页表)
uint32_t paging_mapped_count(void) {
    if (!kernel_page_directory) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < ENTRIES; i++) {
        if (!(kernel_page_directory[i] & PAGE_PRESENT)) continue;
        page_entry_t* pt = (page_entry_t*)(kernel_page_directory[i] & 0xFFFFF000);
        for (uint32_t j = 0; j < ENTRIES; j++) {
            if (pt[j] & PAGE_PRESENT) count++;
        }
    }
    return count;
}

// ---- VMM 别名接口 ----
int map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    return paging_map_page(virt, phys, flags);
}

int unmap_page(uint32_t virt) {
    return paging_unmap_page(virt);
}
