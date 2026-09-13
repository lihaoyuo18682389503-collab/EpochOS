// ============================================================
// EpochOS - VGA 文本模式屏幕驱动 (80x25, 16 色)
// ============================================================
#ifndef EPOCHOS_VGA_H
#define EPOCHOS_VGA_H

#include "types.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

// 颜色
enum vga_color {
    COLOR_BLACK = 0, COLOR_BLUE = 1, COLOR_GREEN = 2, COLOR_CYAN = 3,
    COLOR_RED = 4, COLOR_MAGENTA = 5, COLOR_BROWN = 6, COLOR_LIGHT_GREY = 7,
    COLOR_DARK_GREY = 8, COLOR_LIGHT_BLUE = 9, COLOR_LIGHT_GREEN = 10,
    COLOR_LIGHT_CYAN = 11, COLOR_LIGHT_RED = 12, COLOR_LIGHT_MAGENTA = 13,
    COLOR_LIGHT_BROWN = 14, COLOR_WHITE = 15,
};

void vga_init(void);
void vga_clear(uint8_t color);
void vga_set_color(uint8_t fg, uint8_t bg);
void vga_putchar(char c);
void vga_write(const char* s);
void vga_write_color(const char* s, uint8_t fg, uint8_t bg);
void vga_newline(void);
void vga_set_cursor_pos(int row, int col);
void vga_get_cursor_pos(int* row, int* col);
void vga_scroll(void);

// 图形模式下把虚拟缓冲渲染到 LFB 的 (px,py) 位置
void vga_render(int px, int py);

// 格式化输出 (支持 %d %u %x %s %c %%, 可选 %0Nd)
void vga_printf(const char* fmt, ...);

#endif
