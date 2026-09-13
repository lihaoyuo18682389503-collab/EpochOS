// ============================================================
// EpochOS - 图形驱动实现 (VBE LFB, 32bpp)
// ============================================================
#include "gfx.h"
#include "paging.h"
#include "font8x16.h"
#include "serial.h"
#include "mm.h"

// boot2 写入的 bootinfo (位于 0x5000)
struct bootinfo {
    uint32_t width;    // 0x00
    uint32_t height;   // 0x04
    uint32_t bpp;      // 0x08
    uint32_t pitch;    // 0x0C
    uint32_t lfb;      // 0x10
    uint32_t vbe_ok;   // 0x14
};

static inline void vbe_out(uint16_t port, uint8_t v) {
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint8_t vbe_in(uint16_t port) {
    uint8_t r; __asm__ __volatile__("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}
static inline void vbe_outw(uint16_t port, uint16_t v) {
    __asm__ __volatile__("outw %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint16_t vbe_inw(uint16_t port) {
    uint16_t r; __asm__ __volatile__("inw %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

static uint32_t g_lfb = 0;
static uint32_t g_draw_base = 0;   // 当前绘制目标 (默认 LFB; 双缓冲时指向 backbuffer)
static uint32_t g_back = 0;        // backbuffer 地址 (0=未分配)
static int g_w = 80;
static int g_h = 25;
static int g_pitch = 160;
static int g_ok = 0;

uint32_t vga_color_to_rgb(uint8_t color) {
    static const uint32_t palette[16] = {
        0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
        0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
        0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
        0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
    };
    return palette[color & 0x0F];
}

void gfx_init(void) {
    struct bootinfo* bi = (struct bootinfo*)0x5000;
    if (!bi->vbe_ok || bi->bpp != 32) {
        serial_write_str("[gfx] VBE not available, text mode fallback\n");
        g_ok = 0;
        return;
    }
    // 映射 LFB 物理页 (恒等映射, 4KB 粒度)
    uint32_t total = bi->pitch * bi->height;
    for (uint32_t off = 0; off < total; off += 4096) {
        paging_map_page(bi->lfb + off, bi->lfb + off,
                        PAGE_PRESENT | PAGE_WRITABLE);
    }
    g_lfb = bi->lfb;
    g_draw_base = g_lfb;
    g_w = (int)bi->width;
    g_h = (int)bi->height;
    g_pitch = (int)bi->pitch;
    g_ok = 1;
    serial_write_str("[gfx] VBE framebuffer ready\n");
    serial_printf(COM1, "[gfx] %dx%dx%d pitch=%d lfb=0x%x\n",
                  g_w, g_h, bi->bpp, g_pitch, g_lfb);
    // DBG: 读取 VBE 寄存器验证模式状态
    {
        uint16_t r[10];
        for (int i = 0; i < 10; i++) {
            vbe_outw(0x1CE, (uint16_t)i);
            r[i] = vbe_inw(0x1CF);
        }
        serial_printf(COM1, "[dbg] VBE id=%x xres=%d yres=%d bpp=%d enable=%x virt=%d xoff=%d yoff=%d\n",
                      r[0], r[1], r[2], r[3], r[4], r[6], r[8], r[9]);
        // 不再强制重设 VBE 模式: boot2 已通过 VBE BIOS 设置 800x600x32 并启用 LFB (enable=0x41)。
        // 此前通过索引寄存器重设 enable=0x01 会丢失 bit6(LFB 使能), 导致 QEMU 显示表面
        // 不再读取内核写入的 LFB, 画面出现花屏/错位/色彩失真 (内核渲染本身正常)。
        serial_printf(COM1, "[dbg] keep VBE mode (set by boot2), enable=0x%x LFB=%s\n",
                      r[4], (r[4] & 0x40) ? "on" : "OFF");
    }
}

int gfx_available(void) { return g_ok; }
int gfx_width(void) { return g_w; }
int gfx_height(void) { return g_h; }

void gfx_putpixel(int x, int y, uint32_t color) {
    if (!g_ok) return;
    if (x < 0 || y < 0 || x >= g_w || y >= g_h) return;
    uint32_t* p = (uint32_t*)(g_draw_base + (uint32_t)y * g_pitch + (uint32_t)x * 4);
    *p = color;
}

uint32_t gfx_getpixel(int x, int y) {
    if (!g_ok) return 0;
    if (x < 0 || y < 0 || x >= g_w || y >= g_h) return 0;
    return *(uint32_t*)(g_draw_base + (uint32_t)y * g_pitch + (uint32_t)x * 4);
}

void gfx_fill_rect_raw(int x, int y, int w, int h, uint32_t color) {
    // 不追踪脏矩形: 调用方需自行 gfx_mark_dirty (用于合并多次填充为单个脏矩形)
    if (!g_ok) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_w) w = g_w - x;
    if (y + h > g_h) h = g_h - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = 0; yy < h; yy++) {
        uint32_t* p = (uint32_t*)(g_draw_base + (uint32_t)(y + yy) * g_pitch + (uint32_t)x * 4);
        for (int xx = 0; xx < w; xx++) {
            p[xx] = color;
        }
    }
}

void gfx_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!g_ok) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_w) w = g_w - x;
    if (y + h > g_h) h = g_h - y;
    if (w <= 0 || h <= 0) return;
    gfx_mark_dirty(x, y, w, h);
    gfx_fill_rect_raw(x, y, w, h, color);
}

void gfx_draw_rect(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    gfx_fill_rect(x, y, w, 1, color);
    gfx_fill_rect(x, y + h - 1, w, 1, color);
    gfx_fill_rect(x, y, 1, h, color);
    gfx_fill_rect(x + w - 1, y, 1, h, color);
}

void gfx_draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    gfx_mark_dirty(x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1,
        (x0 < x1 ? x1 - x0 : x0 - x1) + 1, (y0 < y1 ? y1 - y0 : y0 - y1) + 1);
    // Bresenham
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    while (1) {
        gfx_putpixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

void gfx_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg) {
    if (!g_ok) return;
    gfx_mark_dirty(x, y, 8, 16);
    if (c < 0x20 || c > 0x7E) c = ' ';
    const uint8_t* glyph = font8x16[c - 0x20];
    for (int yy = 0; yy < 16; yy++) {
        uint8_t line = glyph[yy];
        for (int xx = 0; xx < 8; xx++) {
            uint32_t color = (line & (1 << (7 - xx))) ? fg : bg;
            gfx_putpixel(x + xx, y + yy, color);
        }
    }
}

void gfx_draw_text(int x, int y, const char* s, uint32_t fg, uint32_t bg) {
    while (*s) {
        gfx_draw_char(x, y, *s, fg, bg);
        x += 8;
        s++;
    }
}

void gfx_fill_screen(uint32_t color) {
    gfx_fill_rect(0, 0, g_w, g_h, color);
}

void gfx_flush(void) {
    // LFB 直写, 无需刷新
}

// ================= 炫酷特效绘制原语 =================

static int isqrt(int n) {
    if (n <= 0) return 0;
    int x = n, y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

uint32_t gfx_mix(uint32_t a, uint32_t b, int t) {
    // t: 0..255, 返回 a->b 的插值
    int r = ((a >> 16) & 0xFF) * (255 - t) / 255 + ((b >> 16) & 0xFF) * t / 255;
    int g = ((a >> 8) & 0xFF) * (255 - t) / 255 + ((b >> 8) & 0xFF) * t / 255;
    int bl = (a & 0xFF) * (255 - t) / 255 + (b & 0xFF) * t / 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

void gfx_fill_round_rect(int x, int y, int w, int h, int r, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    gfx_mark_dirty(x, y, w, h);
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    // 主体矩形 (去除四角)
    gfx_fill_rect(x + r, y, w - 2 * r, h, color);
    gfx_fill_rect(x, y + r, w, h - 2 * r, color);
    // 四角: 距离圆心 > r 的点不画
    for (int cy = 0; cy < r; cy++) {
        for (int cx = 0; cx < r; cx++) {
            int d = (r - 1 - cx) * (r - 1 - cx) + (r - 1 - cy) * (r - 1 - cy);
            if (d <= r * r) {
                gfx_putpixel(x + cx, y + cy, color);
                gfx_putpixel(x + w - 1 - cx, y + cy, color);
                gfx_putpixel(x + cx, y + h - 1 - cy, color);
                gfx_putpixel(x + w - 1 - cx, y + h - 1 - cy, color);
            }
        }
    }
}

void gfx_draw_round_rect(int x, int y, int w, int h, int r, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    gfx_mark_dirty(x, y, w, h);
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    // 四边直线部分
    gfx_fill_rect(x + r, y, w - 2 * r, 1, color);
    gfx_fill_rect(x + r, y + h - 1, w - 2 * r, 1, color);
    gfx_fill_rect(x, y + r, 1, h - 2 * r, color);
    gfx_fill_rect(x + w - 1, y + r, 1, h - 2 * r, color);
    // 四角圆弧
    for (int cy = 0; cy <= r; cy++) {
        for (int cx = 0; cx <= r; cx++) {
            int d = (r - cx) * (r - cx) + (r - cy) * (r - cy);
            if (d <= r * r + r && d >= r * r - r) {
                gfx_putpixel(x + cx, y + cy, color);
                gfx_putpixel(x + w - 1 - cx, y + cy, color);
                gfx_putpixel(x + cx, y + h - 1 - cy, color);
                gfx_putpixel(x + w - 1 - cx, y + h - 1 - cy, color);
            }
        }
    }
}

void gfx_fill_gradient_v(int x, int y, int w, int h, uint32_t c_top, uint32_t c_bot) {
    if (!g_ok || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_w) w = g_w - x;
    if (y + h > g_h) h = g_h - y;
    if (w <= 0 || h <= 0) return;
    gfx_mark_dirty(x, y, w, h);
    for (int yy = 0; yy < h; yy++) {
        uint32_t color = gfx_mix(c_top, c_bot, yy * 255 / (h > 0 ? h : 1));
        if (y + yy >= g_h) {
            serial_printf(COM1, "[gfv!!] yy=%d y=%d h=%d g_h=%d OVERFLOW\n", yy, y, h, g_h);
            return;
        }
        uint32_t* p = (uint32_t*)(g_draw_base + (uint32_t)(y + yy) * g_pitch + (uint32_t)x * 4);
        for (int xx = 0; xx < w; xx++) p[xx] = color;
    }
}

void gfx_fill_alpha(int x, int y, int w, int h, uint32_t color, int alpha) {
    if (!g_ok || w <= 0 || h <= 0) return;
    if (alpha <= 0) return;
    if (alpha >= 255) { gfx_fill_rect(x, y, w, h, color); return; }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_w) w = g_w - x;
    if (y + h > g_h) h = g_h - y;
    if (w <= 0 || h <= 0) return;
    gfx_mark_dirty(x, y, w, h);
    int ia = 255 - alpha;
    int cr = ((color >> 16) & 0xFF) * alpha;
    int cg = ((color >> 8) & 0xFF) * alpha;
    int cb = (color & 0xFF) * alpha;
    for (int yy = 0; yy < h; yy++) {
        uint32_t* p = (uint32_t*)(g_draw_base + (uint32_t)(y + yy) * g_pitch + (uint32_t)x * 4);
        for (int xx = 0; xx < w; xx++) {
            uint32_t dst = p[xx];
            int r = (((dst >> 16) & 0xFF) * ia + cr) >> 8;
            int g = (((dst >> 8) & 0xFF) * ia + cg) >> 8;
            int b = ((dst & 0xFF) * ia + cb) >> 8;
            p[xx] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

// 毛玻璃圆角面板: 对区域内背景做 box 模糊, 再叠加半透明白色, 模拟 frosted-glass
void gfx_fill_glass_round_rect(int x, int y, int w, int h, int r, int blur, int alpha) {
    if (!g_ok || w <= 0 || h <= 0) return;
    gfx_mark_dirty(x, y, w, h);
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (blur < 1) blur = 1;
    if (alpha <= 0) return;
    if (alpha > 200) alpha = 200;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > g_w) x1 = g_w;
    int y1 = y + h; if (y1 > g_h) y1 = g_h;
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            // 圆角裁剪
            if (px < x + r && py < y + r) {
                int dx = (x + r - 1) - px, dy = (y + r - 1) - py;
                if (dx * dx + dy * dy > r * r) continue;
            }
            if (px >= x + w - r && py < y + r) {
                int dx = px - (x + w - r), dy = (y + r - 1) - py;
                if (dx * dx + dy * dy > r * r) continue;
            }
            if (px < x + r && py >= y + h - r) {
                int dx = (x + r - 1) - px, dy = py - (y + h - r);
                if (dx * dx + dy * dy > r * r) continue;
            }
            if (px >= x + w - r && py >= y + h - r) {
                int dx = px - (x + w - r), dy = py - (y + h - r);
                if (dx * dx + dy * dy > r * r) continue;
            }
            // box blur 采样 (菱形邻域: 采样点数 = 1+2*blur*(blur+1), 比方形邻域少约 50%)
            int rr = 0, gg = 0, bb = 0, n = 0;
            for (int dy = -blur; dy <= blur; dy++) {
                int ady = dy < 0 ? -dy : dy;
                int half = blur - ady;                 // 菱形每行宽度
                for (int dx = -half; dx <= half; dx++) {
                    int sx = px + dx, sy = py + dy;
                    if (sx < 0 || sy < 0 || sx >= g_w || sy >= g_h) continue;
                    uint32_t c = gfx_getpixel(sx, sy);
                    rr += (c >> 16) & 0xFF;
                    gg += (c >> 8) & 0xFF;
                    bb += c & 0xFF;
                    n++;
                }
            }
            if (n == 0) continue;
            rr /= n; gg /= n; bb /= n;
            // 叠加半透明白 (毛玻璃发白)
            rr = (rr * (255 - alpha) + 255 * alpha) / 255;
            gg = (gg * (255 - alpha) + 255 * alpha) / 255;
            bb = (bb * (255 - alpha) + 255 * alpha) / 255;
            if (rr > 255) rr = 255;
            if (gg > 255) gg = 255;
            if (bb > 255) bb = 255;
            gfx_putpixel(px, py, ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bb);
        }
    }
}

void gfx_draw_shadow(int x, int y, int w, int h, int depth) {
    if (depth <= 0) return;
    // 边带阴影: 每层只画右侧竖带 + 下侧横带 (原实现整窗平移叠加, 视觉等效,
    // 但像素量从 O(w*h*depth) 降到 O((w+h)*depth), 消除拖动/开窗卡顿)
    for (int d = 1; d <= depth; d++) {
        int alpha = 90 - d * (90 / (depth + 1));
        if (alpha <= 0) alpha = 8;
        gfx_fill_alpha(x + w + d - 1, y + d, 1, h, 0x000000, alpha);
        gfx_fill_alpha(x + d, y + h + d - 1, w, 1, 0x000000, alpha);
    }
}

void gfx_draw_glow_spot(int cx, int cy, int radius, uint32_t color, int max_alpha) {
    if (!g_ok || radius <= 0 || max_alpha <= 0) return;
    gfx_mark_dirty(cx - radius, cy - radius, radius * 2 + 1, radius * 2 + 1);
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > radius * radius) continue;
            int dist = isqrt(d2);
            int alpha = max_alpha * (radius - dist) / radius;
            if (alpha <= 0) continue;
            int px = cx + dx, py = cy + dy;
            if (px < 0 || py < 0 || px >= g_w || py >= g_h) continue;
            uint32_t dst = gfx_getpixel(px, py);
            int ia = 255 - alpha;
            int r = (((dst >> 16) & 0xFF) * ia + ((color >> 16) & 0xFF) * alpha) >> 8;
            int g = (((dst >> 8) & 0xFF) * ia + ((color >> 8) & 0xFF) * alpha) >> 8;
            int b = ((dst & 0xFF) * ia + (color & 0xFF) * alpha) >> 8;
            gfx_putpixel(px, py, ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
        }
    }
}

void gfx_draw_char_glow(int x, int y, char c, uint32_t fg, uint32_t glow) {
    if (!g_ok) return;
    if (c < 0x20 || c > 0x7E) c = ' ';
    const uint8_t* glyph = font8x16[c - 0x20];
    // 先画光晕 (四周扩展 1px)
    for (int yy = 0; yy < 16; yy++) {
        uint8_t line = glyph[yy];
        for (int xx = 0; xx < 8; xx++) {
            if (!(line & (1 << (7 - xx)))) continue;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    gfx_putpixel(x + xx + dx, y + yy + dy, glow);
                }
            }
        }
    }
    // 再画主体
    for (int yy = 0; yy < 16; yy++) {
        uint8_t line = glyph[yy];
        for (int xx = 0; xx < 8; xx++) {
            if (line & (1 << (7 - xx)))
                gfx_putpixel(x + xx, y + yy, fg);
        }
    }
}

void gfx_draw_text_glow(int x, int y, const char* s, uint32_t fg, uint32_t glow) {
    while (*s) {
        gfx_draw_char_glow(x, y, *s, fg, glow);
        x += 8;
        s++;
    }
}

uint32_t* gfx_framebuffer(void) { return (uint32_t*)g_lfb; }
uint32_t* gfx_draw_target(void) { return (uint32_t*)g_draw_base; }
void gfx_set_draw_target(uint32_t addr) { g_draw_base = addr; gfx_dirty_reset(); }
int gfx_pitch(void) { return g_pitch; }

// ---- 双缓冲 ----
int gfx_backbuffer_init(void) {
    if (!g_ok) return 0;
    if (g_back) { g_draw_base = g_back; return 1; }
    uint32_t sz = (uint32_t)g_pitch * (uint32_t)g_h;
    void* p = kmalloc(sz);
    if (!p) return 0;
    // 确保 backbuffer 页已映射 (与 LFB 同粒度)
    for (uint32_t off = 0; off < sz; off += 4096) {
        paging_map_page((uint32_t)p + off, (uint32_t)p + off,
                        PAGE_PRESENT | PAGE_WRITABLE);
    }
    g_back = (uint32_t)p;
    g_draw_base = g_back;
    serial_printf(COM1, "[gfx] backbuffer 0x%x (%u bytes)\n", g_back, sz);
    return 1;
}

// ---- 脏矩形追踪 (部分提交) ----
#define MAX_DIRTY_RECT 16
typedef struct { int x, y, w, h; } dirty_rect_t;
static dirty_rect_t g_dirty[MAX_DIRTY_RECT];
static int g_dirty_n = 0;

void gfx_mark_dirty(int x, int y, int w, int h) {
    if (!g_ok || w <= 0 || h <= 0) return;
    if (g_draw_base != g_back) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_w) w = g_w - x;
    if (y + h > g_h) h = g_h - y;
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < g_dirty_n; i++) {
        dirty_rect_t* d = &g_dirty[i];
        if (x < d->x + d->w + 1 && x + w + 1 > d->x && y < d->y + d->h + 1 && y + h + 1 > d->y) {
            int x2 = x + w > d->x + d->w ? x + w : d->x + d->w;
            int y2 = y + h > d->y + d->h ? y + h : d->y + d->h;
            int ux0 = x < d->x ? x : d->x;
            int uy0 = y < d->y ? y : d->y;
            int a_old = d->w * d->h;
            int a_new = w * h;
            int a_uni = (x2 - ux0) * (y2 - uy0);
            // 细长矩形(如窗口阴影边条)与远处矩形仅轻微相交时, 包围盒并集会产生巨大空洞:
            // 两条相距 400px 的 1px 竖条会被并成 400x398, 拷贝面积虚增 400 倍并把 swap 拖到
            // 整屏级。故当"并集浪费面积"超过较大者且有富余槽位时, 放弃合并、另开新槽。
            if (g_dirty_n < MAX_DIRTY_RECT &&
                (a_uni - a_old - a_new) > (a_old > a_new ? a_old : a_new)) {
                continue;
            }
            d->x = ux0;
            d->y = uy0;
            d->w = x2 - ux0;
            d->h = y2 - uy0;
            return;
        }
    }
    if (g_dirty_n < MAX_DIRTY_RECT) {
        g_dirty[g_dirty_n].x = x; g_dirty[g_dirty_n].y = y;
        g_dirty[g_dirty_n].w = w; g_dirty[g_dirty_n].h = h;
        g_dirty_n++;
    } else {
        // 满: 合并"合并后面积增量最小"的两个矩形, 腾出槽位给新矩形
        // (替代原整屏包围盒合并, 显著减小 swap 拷贝面积)
        int bi = 0, bj = 1, bbest = -1;
        for (int i = 0; i < MAX_DIRTY_RECT; i++) {
            for (int j = i + 1; j < MAX_DIRTY_RECT; j++) {
                int x0 = g_dirty[i].x < g_dirty[j].x ? g_dirty[i].x : g_dirty[j].x;
                int y0 = g_dirty[i].y < g_dirty[j].y ? g_dirty[i].y : g_dirty[j].y;
                int x1 = (g_dirty[i].x + g_dirty[i].w) > (g_dirty[j].x + g_dirty[j].w) ?
                         (g_dirty[i].x + g_dirty[i].w) : (g_dirty[j].x + g_dirty[j].w);
                int y1 = (g_dirty[i].y + g_dirty[i].h) > (g_dirty[j].y + g_dirty[j].h) ?
                         (g_dirty[i].y + g_dirty[i].h) : (g_dirty[j].y + g_dirty[j].h);
                int delta = (x1 - x0) * (y1 - y0) - g_dirty[i].w * g_dirty[i].h - g_dirty[j].w * g_dirty[j].h;
                if (bbest < 0 || delta < bbest) { bbest = delta; bi = i; bj = j; }
            }
        }
        // bi 与 bj 真正合并(两者区域合并, 不丢矩形), bj 槽腾出给新矩形
        dirty_rect_t* di = &g_dirty[bi];
        dirty_rect_t* dj = &g_dirty[bj];
        int nx0 = di->x < dj->x ? di->x : dj->x;
        int ny0 = di->y < dj->y ? di->y : dj->y;
        int nx1 = (di->x + di->w) > (dj->x + dj->w) ? (di->x + di->w) : (dj->x + dj->w);
        int ny1 = (di->y + di->h) > (dj->y + dj->h) ? (di->y + di->h) : (dj->y + dj->h);
        di->x = nx0; di->y = ny0; di->w = nx1 - nx0; di->h = ny1 - ny0;
        dj->x = x; dj->y = y; dj->w = w; dj->h = h;
    }
}

void gfx_dirty_reset(void) { g_dirty_n = 0; }
int gfx_dirty_count(void) { return g_dirty_n; }

void gfx_swap(void) {
    if (!g_ok || !g_back) return;
    if (g_dirty_n == 0) return;
    for (int i = 0; i < g_dirty_n; i++) {
        dirty_rect_t* d = &g_dirty[i];
        uint32_t* src = (uint32_t*)(g_back + (uint32_t)d->y * g_pitch + (uint32_t)d->x * 4);
        uint32_t* dst = (uint32_t*)(g_lfb  + (uint32_t)d->y * g_pitch + (uint32_t)d->x * 4);
        int pitch4 = g_pitch / 4;
        int w = d->w;
        for (int yy = 0; yy < d->h; yy++) {
            if (w >= 16) {
                // 宽行按 64 位写入: 把 MMIO 写次数减半 (实测 swap 明显下降)
                uint64_t* s8 = (uint64_t*)src;
                uint64_t* d8 = (uint64_t*)dst;
                int n8 = w >> 1;
                for (int k = 0; k < n8; k++) d8[k] = s8[k];
                if (w & 1) dst[w - 1] = src[w - 1];
            } else {
                for (int k = 0; k < w; k++) dst[k] = src[k];
            }
            src += pitch4;
            dst += pitch4;
        }
    }
    { static int dbg_c = 0; if (++dbg_c % 60 == 1) {
        int ar = 0; for (int di = 0; di < g_dirty_n; di++) ar += g_dirty[di].w * g_dirty[di].h;
        serial_printf(COM1, "[swap] n=%d area=%d\n", g_dirty_n, ar);
    } }
    g_dirty_n = 0;
}

// 直写 LFB 的 panic 红屏 (异常时调用, 双缓冲下仍可见)
void gfx_panic_screen(uint32_t int_no, uint32_t err, const char* msg, uint32_t cr2, uint32_t eip) {
    if (!g_ok) return;
    uint32_t saved = g_draw_base;
    g_draw_base = (uint32_t)g_lfb;    // 切到 LFB 直绘
    gfx_fill_screen(0x500000);         // 深红底
    gfx_draw_text(10, 10, "KERNEL PANIC", 0xFFFFFF, 0x500000);
    gfx_draw_text(10, 30, msg, 0xFFFFFF, 0x500000);
    {
        char buf[48];
        uitoa(int_no, buf, 10);
        gfx_draw_text(10, 52, "INT: ", 0xFFFF00, 0x500000);
        gfx_draw_text(60, 52, buf, 0xFFFF00, 0x500000);
        uitoa(err, buf, 10);
        gfx_draw_text(120, 52, "ERR: ", 0xFFFF00, 0x500000);
        gfx_draw_text(170, 52, buf, 0xFFFF00, 0x500000);
        gfx_draw_text(230, 52, "CR2: 0x", 0xFFFF00, 0x500000);
        uitoa(cr2, buf, 16);
        gfx_draw_text(300, 52, buf, 0xFFFF00, 0x500000);
        gfx_draw_text(10, 74, "EIP: 0x", 0xFFFFFF, 0x500000);
        // 固定 8 位 hex (大写)
        {
            static const char hexd[] = "0123456789ABCDEF";
            char hb[9];
            for (int i = 0; i < 8; i++) {
                hb[7 - i] = hexd[(eip >> (4 * i)) & 0xF];
            }
            hb[8] = 0;
            gfx_draw_text(74, 74, hb, 0xFFFFFF, 0x500000);
        }
        uitoa(eip, buf, 10);
        gfx_draw_text(150, 74, buf, 0xFFFFFF, 0x500000);
    }
    gfx_draw_text(10, 100, "System halted. Restart QEMU.", 0xC0C0C0, 0x500000);
    g_draw_base = saved;
}


// 柔和渐变填充: 顶部/底部颜色线性插值 (行预计算查找表, 每帧无逐像素插值),
// 支持圆角裁剪与 alpha 半透明 (毛玻璃透出壁纸), 用于标题栏/内容背景/任务栏。
void gfx_fill_soft_gradient(int x, int y, int w, int h, int r,
                            uint32_t top, uint32_t bottom, int alpha) {
    if (!g_ok || w <= 0 || h <= 0 || alpha <= 0) return;
    if (alpha >= 255) alpha = 254;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_w) w = g_w - x;
    if (y + h > g_h) h = g_h - y;
    if (w <= 0 || h <= 0) return;
    gfx_mark_dirty(x, y, w, h);
    static uint32_t row_tab[600];
    int n = h > 0 ? h : 1;
    if (n > 599) n = 599;
    int tr = (top >> 16) & 0xFF, tg = (top >> 8) & 0xFF, tb = top & 0xFF;
    int br = (bottom >> 16) & 0xFF, bg = (bottom >> 8) & 0xFF, bb = bottom & 0xFF;
    for (int i = 0; i < n; i++) {
        int rr = tr + ((br - tr) * i) / (n - 1);
        int gg = tg + ((bg - tg) * i) / (n - 1);
        int bbb = tb + ((bb - tb) * i) / (n - 1);
        row_tab[i] = ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bbb;
    }
    int ia = 255 - alpha;
    int x1 = x + w, y1 = y + h;
    for (int py = y; py < y1; py++) {
        int row = py - y;
        if (row < 0) row = 0;
        if (row >= n) row = n - 1;
        uint32_t col = row_tab[row];
        int cr = ((col >> 16) & 0xFF) * alpha;
        int cg = ((col >> 8) & 0xFF) * alpha;
        int cb = (col & 0xFF) * alpha;
        uint32_t* p0 = (uint32_t*)(g_draw_base + (uint32_t)py * g_pitch + (uint32_t)x * 4);
        for (int px = x; px < x1; px++) {
            if (r > 0) {
                // 圆角裁剪: 四个角的半径外区域跳过
                int dx0 = px - x, dy0 = py - y;
                int dxr = x1 - 1 - px, dyr = y1 - 1 - py;
                if ((dx0 < r && dy0 < r && (r - dx0) * (r - dx0) + (r - dy0) * (r - dy0) > r * r) ||
                    (dxr < r && dy0 < r && (r - dxr) * (r - dxr) + (r - dy0) * (r - dy0) > r * r) ||
                    (dx0 < r && dyr < r && (r - dx0) * (r - dx0) + (r - dyr) * (r - dyr) > r * r) ||
                    (dxr < r && dyr < r && (r - dxr) * (r - dxr) + (r - dyr) * (r - dyr) > r * r))
                    continue;
            }
            uint32_t dst = p0[px - x];
            int r1 = (((dst >> 16) & 0xFF) * ia + cr) >> 8;
            int g1 = (((dst >> 8) & 0xFF) * ia + cg) >> 8;
            int b1 = ((dst & 0xFF) * ia + cb) >> 8;
            p0[px - x] = ((uint32_t)r1 << 16) | ((uint32_t)g1 << 8) | (uint32_t)b1;
        }
    }
}
