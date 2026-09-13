// ============================================================
// EpochOS - 壁纸系统实现
// 壁纸数据由 build.py 以裸 RGB888 (800x600) 写入镜像固定 LBA:
//   LBA 1596          : 目录区 (4 x 128B 记录: magic + idx + name)
//   LBA 1600 + i*2813 : 第 i 张壁纸 RGB 数据
// 内核启动时预加载 4 张壁纸到内存 (每张 1.44MB, 共 5.76MB),
// 切换壁纸仅需改变当前索引并 blit, 零磁盘延迟。
// ============================================================
#include "wallpaper.h"
#include "ata.h"
#include "gfx.h"
#include "mm.h"
#include "serial.h"
#include "string.h"
#include "vga.h"

static uint8_t* g_walls[WALL_COUNT] = {0, 0, 0, 0};
static int g_loaded = 0;
static int g_current = 0;
static char g_names[WALL_COUNT][24];

int wallpaper_init(void) {
    // 调试: 写入固定物理地址 0x7000 用于 QEMU monitor 验证
    volatile uint32_t* dbg_mem = (volatile uint32_t*)0x7000;
    dbg_mem[0] = 0xDEADBEEF;
    serial_write_str("[wall] init enter\n");
    vga_write_color("[wall] init enter\n", COLOR_LIGHT_CYAN, COLOR_BLACK);
    if (g_loaded) return 1;
    // 读取目录区 (1 扇区, 内 4 条记录)
    uint8_t dir[512];
    int dr = ata_read_sectors(0, WALL_DIR_LBA, 1, dir);
    serial_printf(COM1, "[wall] dir r=%d b0=%d,%d,%d,%d b4=%d,%d,%d,%d b508=%d,%d\n",
                  dr, dir[0], dir[1], dir[2], dir[3], dir[4], dir[5], dir[6], dir[7],
                  dir[508], dir[509]);
    if (dr != 0) {
        serial_write_str("[wall] dir read failed\n");
        return 0;
    }
    for (int i = 0; i < WALL_COUNT; i++) {
        const uint8_t* rec = dir + i * 128;
        if (rec[0] != 'W' || rec[1] != 'A' || rec[2] != 'L') {
            serial_printf(COM1, "[wall] bad magic at %d\n", i);
            return 0;
        }
        int len = rec[3];
        if (len > 23) len = 23;
        for (int k = 0; k < len; k++) g_names[i][k] = (char)rec[4 + k];
        g_names[i][len] = 0;
    }
    // 预加载 4 张壁纸
    for (int i = 0; i < WALL_COUNT; i++) {
        void* buf = kmalloc(WALL_RGB_SIZE);
        if (!buf) {
            serial_printf(COM1, "[wall] no mem for wall %d\n", i);
            return 0;
        }
        uint32_t lba = WALL_DATA_LBA + (uint32_t)i * WALL_SECTORS;
        serial_printf(COM1, "[wall] loading %d buf=%x lba=%u\n", i, (uint32_t)buf, lba);
        // ATA PIO 单次最多 128 扇区, 分块读取整张壁纸
        uint32_t remain = WALL_SECTORS;
        uint8_t* dst = (uint8_t*)buf;
        while (remain > 0) {
            uint32_t chunk = remain > WALL_READ_CHUNK ? WALL_READ_CHUNK : remain;
            if (ata_read_sectors(0, lba, (uint8_t)chunk, dst) != 0) {
                serial_printf(COM1, "[wall] read wall %d failed lba=%u\n", i, lba);
                return 0;
            }
            serial_printf(COM1, "[wall] chunk ok lba=%u remain=%u\n", lba, remain);
            lba += chunk;
            dst += chunk * 512;
            remain -= chunk;
        }
        g_walls[i] = (uint8_t*)buf;
        serial_printf(COM1, "[wall] wall %d done\n", i);
    }
    g_loaded = 1;
    g_current = 0;
    serial_printf(COM1, "[wall] %d wallpapers loaded, %u KB each\n",
                  WALL_COUNT, WALL_RGB_SIZE / 1024);
    return 1;
}

int wallpaper_count(void) { return WALL_COUNT; }
int wallpaper_current(void) { return g_current; }

static uint32_t* g_wall32 = 0;   // 当前壁纸 32bpp 预转换缓存 (800x600x4)
static int g_wall32_idx = -1;

static void build_wall32(int idx) {
    if (!g_loaded || !g_walls[idx]) return;
    if (!g_wall32) g_wall32 = (uint32_t*)kmalloc(WALL_WIDTH * WALL_HEIGHT * 4);
    const uint8_t* s = g_walls[idx];
    uint32_t* d = g_wall32;
    for (int y = 0; y < WALL_HEIGHT; y++) {
        for (int x = 0; x < WALL_WIDTH; x++) {
            uint32_t r = s[0], gg = s[1], b = s[2];
            *d++ = (r << 16) | (gg << 8) | b;
            s += 3;
        }
    }
    g_wall32_idx = idx;
    static int bw_log = 0;
    if (bw_log++ < 3)
        serial_printf(COM1, "[bw] build32 idx=%d addr=%x w=%d\n", idx, (uint32_t)g_wall32, WALL_WIDTH * WALL_HEIGHT * 4);
}

int wallpaper_set(int idx) {
    if (!g_loaded) return 0;
    idx %= WALL_COUNT;
    if (idx < 0) idx += WALL_COUNT;
    g_current = idx;
    return 1;
}

int wallpaper_next(void) { return wallpaper_set(g_current + 1); }
int wallpaper_prev(void) { return wallpaper_set(g_current - 1); }

const char* wallpaper_name(int idx) {
    if (idx < 0 || idx >= WALL_COUNT) return "?";
    return g_names[idx][0] ? g_names[idx] : "Wallpaper";
}

// 将当前壁纸 RGB 数据绘制到当前绘制目标 (backbuffer / LFB)
void wallpaper_blit(void) {
    if (!g_loaded || !g_walls[g_current]) return;
    if (g_wall32_idx != g_current) build_wall32(g_current);
    gfx_mark_dirty(0, 0, gfx_width(), gfx_height());
    uint32_t* dst = gfx_draw_target();
    int pitch = gfx_pitch();
    int w = gfx_width();
    int h = gfx_height();
    if (w > WALL_WIDTH) w = WALL_WIDTH;
    if (h > WALL_HEIGHT) h = WALL_HEIGHT;
    const uint32_t* src = g_wall32;
    for (int y = 0; y < h; y++) {
        uint32_t* line = (uint32_t*)((uint8_t*)dst + (uint32_t)y * pitch);
        memcpy(line, src + (uint32_t)y * WALL_WIDTH, (size_t)w * 4);
    }
}
