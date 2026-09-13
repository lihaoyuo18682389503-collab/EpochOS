// ============================================================
// EpochOS - 图形驱动 (VBE LFB, 32bpp)
// boot2 在实模式设置 VBE 模式 0x118 (800x600x32),
// bootinfo 位于 0x5000, 内核在此读取并映�?LFB
// ============================================================
#ifndef EPOCHOS_GFX_H
#define EPOCHOS_GFX_H

#include "types.h"

// 常用颜色 (0xRRGGBB)
#define COL_BLACK        0x000000
#define COL_WHITE        0xFFFFFF
#define COL_SILVER       0xC0C0C0
#define COL_GRAY         0x808080
#define COL_DARK_GRAY    0x404040
#define COL_RED          0xFF0000
#define COL_DARK_RED     0x800000
#define COL_GREEN        0x00FF00
#define COL_DARK_GREEN   0x008000
#define COL_BLUE         0x0000FF
#define COL_DARK_BLUE    0x000080
#define COL_YELLOW       0xFFFF00
#define COL_CYAN         0x00FFFF
#define COL_MAGENTA      0xFF00FF
#define COL_ORANGE       0xFF8000
#define COL_NAVY         0x000040
#define COL_TEAL         0x008080
#define COL_DESKTOP_TOP  0x1A3A6B
#define COL_DESKTOP_BOT  0x0A1628
#define COL_TITLEBAR     0x2E5BA6
#define COL_TITLEBAR_ACT 0x1E4E8C
#define COL_TASKBAR      0x1B2A3C

// 文本颜色 -> RGB 映射 (16 �?VGA 调色�?
uint32_t vga_color_to_rgb(uint8_t color);

void gfx_init(void);             // 读取 bootinfo, 映射 LFB; 成功则图形模式可
int  gfx_available(void);        // 1=图形模式已就
int  gfx_width(void);
int  gfx_height(void);

void gfx_putpixel(int x, int y, uint32_t color);
uint32_t gfx_getpixel(int x, int y);
void gfx_fill_rect(int x, int y, int w, int h, uint32_t color);
void gfx_fill_rect_raw(int x, int y, int w, int h, uint32_t color);
void gfx_draw_rect(int x, int y, int w, int h, uint32_t color);   // 空心矩形
void gfx_draw_line(int x0, int y0, int x1, int y1, uint32_t color);
void gfx_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);   // 8x16
void gfx_draw_text(int x, int y, const char* s, uint32_t fg, uint32_t bg);
void gfx_fill_screen(uint32_t color);
void gfx_flush(void);            // 图形模式无操�?(直接�?LFB)

// ---- 双缓�?----
int  gfx_backbuffer_init(void);  // 分配离屏缓冲, 之后所有绘制写�?backbuffer
void gfx_swap(void);             // backbuffer 一次性复制到 LFB
uint32_t* gfx_draw_target(void); // 当前绘制目标地址 (双缓冲时=backbuffer, 否则=LFB)

// ---- 炫酷特效绘制原语 ----
void gfx_fill_round_rect(int x, int y, int w, int h, int r, uint32_t color);   // 圆角填充
void gfx_draw_round_rect(int x, int y, int w, int h, int r, uint32_t color);   // 圆角空心
void gfx_fill_gradient_v(int x, int y, int w, int h, uint32_t c_top, uint32_t c_bot); // 垂直渐变
void gfx_fill_alpha(int x, int y, int w, int h, uint32_t color, int alpha);    // 半透明混合填充
void gfx_fill_glass_round_rect(int x, int y, int w, int h, int r, int blur, int alpha); // 毛玻璃圆角面
void gfx_fill_soft_gradient(int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom, int alpha); // 柔和渐变半透明面
void gfx_draw_shadow(int x, int y, int w, int h, int depth);                   // 右下阴影
void gfx_draw_glow_spot(int cx, int cy, int radius, uint32_t color, int max_alpha); // 圆形渐隐光晕
void gfx_draw_char_glow(int x, int y, char c, uint32_t fg, uint32_t glow);     // 发光字符
void gfx_draw_text_glow(int x, int y, const char* s, uint32_t fg, uint32_t glow);
uint32_t gfx_mix(uint32_t a, uint32_t b, int t);                               // 颜色插�?0-255

// 供外部直接操�?LFB (vga_render 高性能路径)
uint32_t* gfx_framebuffer(void);
int gfx_pitch(void);
void gfx_panic_screen(uint32_t int_no, uint32_t err, const char* msg, uint32_t cr2, uint32_t eip);  // 直写 LFB 红屏 panic

// ---- 脏矩形跟�?(部分提交优化) ----
void gfx_mark_dirty(int x, int y, int w, int h);  // 标记区域为脏
void gfx_dirty_reset(void);                       // 清空脏区记录
void gfx_set_draw_target(uint32_t addr);          // 切换绘制目标地址

#endif
