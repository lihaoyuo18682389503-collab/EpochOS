// ============================================================
// EpochOS - 简单内存管理器
// 页分配: 位图管理 4KB 页
// 小对象: 固定 128B 块链 (供字符串/结构体使用)
// ============================================================
#include "mm.h"

// ---- 页位图 ----
// 位图按 128MB / 4KB 固定 32768 页; 实际可用页数取决于动态堆起点
#define MAX_PAGES ((MEMORY_END) / PAGE_SIZE)   // 32768 页上限
static uint8_t page_bitmap[MAX_PAGES / 8];

extern char __bss_end[];           // 链接器符号: bss 段结束地址
uint32_t g_mem_start = 0x300000;   // 堆起点 (默认 3MB, mm_init 校正)

static uint32_t total_pages = 0;
static uint32_t used_pages = 0;

// ---- 小对象堆 (128B 块) ----
#define SMALL_BLOCK_SIZE 128
#define SMALL_BLOCKS_PER_PAGE (PAGE_SIZE / SMALL_BLOCK_SIZE)   // 32 块/页
static uint8_t* small_heap_start = 0;
static uint32_t small_block_count = 0;
static uint8_t small_used[4096];   // 最多 4096 块 = 128KB 小堆

static inline void bitmap_set(uint32_t idx) {
    page_bitmap[idx / 8] |= (uint8_t)(1 << (idx % 8));
}
static inline void bitmap_clear(uint32_t idx) {
    page_bitmap[idx / 8] &= (uint8_t)~(1 << (idx % 8));
}
static inline int bitmap_test(uint32_t idx) {
    return (page_bitmap[idx / 8] >> (idx % 8)) & 1;
}

void mm_init(void) {
    // 堆起点: bss 结束 4KB 对齐, 确保不覆盖静态数据
    g_mem_start = ((uint32_t)__bss_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (g_mem_start < 0x100000) g_mem_start = 0x100000;
    if (g_mem_start >= MEMORY_END) g_mem_start = 0x300000;
    total_pages = (MEMORY_END - g_mem_start) / PAGE_SIZE;
    used_pages = 0;
    for (uint32_t i = 0; i < MAX_PAGES / 8; i++) {
        page_bitmap[i] = 0;
    }
    // 小对象堆: 从堆起点划 128KB, 并标记为已用防止与页分配器 (kmalloc) 冲突
    small_heap_start = (uint8_t*)g_mem_start;
    small_block_count = 128 * 1024 / SMALL_BLOCK_SIZE;   // 1024 块
    uint32_t heap_pages = (128 * 1024 + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t i = 0; i < heap_pages; i++) {
        bitmap_set(i);
    }
    used_pages += heap_pages;
    for (uint32_t i = 0; i < small_block_count; i++) {
        small_used[i] = 0;
    }
}

// 分配 count 个连续空闲页, 返回物理地址
static uint32_t alloc_pages(uint32_t count) {
    for (uint32_t i = 0; i + count <= total_pages; i++) {
        int free_run = 1;
        for (uint32_t j = 0; j < count; j++) {
            if (bitmap_test(i + j)) {
                free_run = 0;
                break;
            }
        }
        if (free_run) {
            for (uint32_t j = 0; j < count; j++) {
                bitmap_set(i + j);
            }
            used_pages += count;
            return g_mem_start + i * PAGE_SIZE;
        }
    }
    return 0;
}

void* kmalloc(size_t size) {
    if (size == 0) {
        return 0;
    }
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t addr = alloc_pages(pages);
    return addr ? (void*)addr : 0;
}

void kfree(void* ptr) {
    if (!ptr) {
        return;
    }
    uint32_t addr = (uint32_t)ptr;
    if (addr < g_mem_start || addr >= MEMORY_END) {
        return;
    }
    uint32_t idx = (addr - g_mem_start) / PAGE_SIZE;
    // 单页释放 (简化: 调用方需保证是 kmalloc 单页结果)
    if (bitmap_test(idx)) {
        bitmap_clear(idx);
        used_pages--;
    }
}

void* kmalloc_small(size_t size) {
    if (size == 0 || size > SMALL_BLOCK_SIZE) {
        return 0;
    }
    for (uint32_t i = 0; i < small_block_count; i++) {
        if (!small_used[i]) {
            small_used[i] = 1;
            return small_heap_start + i * SMALL_BLOCK_SIZE;
        }
    }
    return 0;
}

uint32_t mm_get_free_pages(void) {
    return total_pages - used_pages;
}

uint32_t mm_total_pages(void) {
    return total_pages;
}

uint32_t mm_free_pages(void) {
    return total_pages - used_pages;
}

uint32_t mm_used_pages(void) {
    return used_pages;
}

uint32_t mm_small_total_blocks(void) {
    return small_block_count;
}

uint32_t mm_small_used_blocks(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < small_block_count; i++) {
        if (small_used[i]) n++;
    }
    return n;
}

// ---- PMM 单页接口 ----
uint32_t alloc_page(void) {
    return alloc_pages(1);
}

void free_page(uint32_t addr) {
    if (!addr) return;
    if (addr < g_mem_start || addr >= MEMORY_END) return;
    uint32_t idx = (addr - g_mem_start) / PAGE_SIZE;
    if (idx >= total_pages) return;
    if (bitmap_test(idx)) {
        bitmap_clear(idx);
        if (used_pages) used_pages--;
    }
}

int reserve_page(uint32_t addr) {
    if (addr < g_mem_start || addr >= MEMORY_END) return -1;
    uint32_t idx = (addr - g_mem_start) / PAGE_SIZE;
    if (idx >= total_pages) return -1;
    if (bitmap_test(idx)) return -1;   // 已占用
    bitmap_set(idx);
    used_pages++;
    return 0;
}

// ---- bump 内核堆 (起步版: 静态池线性分配, 不释放) ----
#define BUMP_POOL_SIZE (256 * 1024)
static uint8_t bump_pool[BUMP_POOL_SIZE] __attribute__((aligned(8)));
static uint32_t bump_used = 0;

void* kmalloc_bump(size_t size) {
    if (size == 0) return 0;
    uint32_t asz = (size + 7u) & ~7u;
    if (bump_used + asz > BUMP_POOL_SIZE) return 0;
    void* p = bump_pool + bump_used;
    bump_used += asz;
    return p;
}

uint32_t mm_bump_used(void) { return bump_used; }
uint32_t mm_bump_free(void) { return BUMP_POOL_SIZE - bump_used; }
