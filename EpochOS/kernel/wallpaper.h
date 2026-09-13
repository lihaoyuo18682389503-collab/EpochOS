// ============================================================
// EpochOS - 壁纸系统 (镜像 LBA 存储 + 内存预加载 + 双缓冲 blit)
// ============================================================
#ifndef EPOCHOS_WALLPAPER_H
#define EPOCHOS_WALLPAPER_H

#include "types.h"

#define WALL_COUNT      4
#define WALL_WIDTH      800
#define WALL_HEIGHT     600
#define WALL_RGB_SIZE   (WALL_WIDTH * WALL_HEIGHT * 3)

// 镜像 LBA 布局 (与 build.py 严格一致)
#define WALL_DIR_LBA    1596
#define WALL_DATA_LBA   1600
#define WALL_SECTORS    ((uint32_t)((WALL_RGB_SIZE + 511u) / 512u))   // 2813 (与 build.py 一致)
#define WALL_READ_CHUNK 128   // ata_read_sectors 单次最多 128 扇区

// 初始化: 从 ATA 读取全部壁纸到内存 (预加载)
int wallpaper_init(void);
int wallpaper_count(void);
int wallpaper_current(void);
int wallpaper_set(int idx);          // 0..3, 越界取模
int wallpaper_next(void);
int wallpaper_prev(void);
const char* wallpaper_name(int idx); // 简单名称

// 将当前壁纸 RGB 数据直接绘制到当前绘制目标 (backbuffer)
void wallpaper_blit(void);

#endif
