// ============================================================
// EpochOS - 屏幕驱动与 printf
// 双模式: 文本模式直写 0xB8000; 图形模式写虚拟缓冲后由 vga_render 绘制到 LFB
// ============================================================
#include "vga.h"
#include "gfx.h"
#include "font8x16.h"
#include "types.h"

static uint16_t* const vga_memory = (uint16_t*)0xB8000;
static uint16_t vbuf[VGA_WIDTH * VGA_HEIGHT];
static int cursor_row = 0;
static int cursor_col = 0;
static uint8_t current_color = 0x0A;  // 亮绿/黑

static inline uint16_t make_entry(char c, uint8_t color) {
    return (uint16_t)c | (uint16_t)color << 8;
}

static void update_hw_cursor(void) {
    uint16_t pos = cursor_row * VGA_WIDTH + cursor_col;
    outb(0x3D4, 14);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
    outb(0x3D4, 15);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
}

static void set_cell(int idx, uint16_t entry) {
    vbuf[idx] = entry;
    if (!gfx_available()) {
        vga_memory[idx] = entry;
    }
}

void vga_set_cursor_pos(int row, int col) {
    if (row < 0) row = 0;
    if (row >= VGA_HEIGHT) row = VGA_HEIGHT - 1;
    if (col < 0) col = 0;
    if (col >= VGA_WIDTH) col = VGA_WIDTH - 1;
    cursor_row = row;
    cursor_col = col;
    update_hw_cursor();
}

void vga_get_cursor_pos(int* row, int* col) {
    *row = cursor_row;
    *col = cursor_col;
}

void vga_scroll(void) {
    for (int y = 0; y < VGA_HEIGHT - 1; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            set_cell(y * VGA_WIDTH + x, vbuf[(y + 1) * VGA_WIDTH + x]);
        }
    }
    for (int x = 0; x < VGA_WIDTH; x++) {
        set_cell((VGA_HEIGHT - 1) * VGA_WIDTH + x, make_entry(' ', current_color));
    }
    cursor_row = VGA_HEIGHT - 1;
}

void vga_init(void) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vbuf[i] = make_entry(' ', current_color);
    }
    if (!gfx_available()) {
        for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
            vga_memory[i] = vbuf[i];
        }
    }
    cursor_row = 0;
    cursor_col = 0;
    update_hw_cursor();
}

void vga_clear(uint8_t color) {
    current_color = color;
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        set_cell(i, make_entry(' ', color));
    }
    cursor_row = 0;
    cursor_col = 0;
    update_hw_cursor();
}

void vga_set_color(uint8_t fg, uint8_t bg) {
    current_color = (uint8_t)(fg | bg << 4);
}

void vga_putchar(char c) {
    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
    } else if (c == '\r') {
        cursor_col = 0;
    } else if (c == '\t') {
        for (int i = 0; i < 4; i++) {
            vga_putchar(' ');
        }
        return;
    } else if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
            set_cell(cursor_row * VGA_WIDTH + cursor_col, make_entry(' ', current_color));
        }
    } else {
        set_cell(cursor_row * VGA_WIDTH + cursor_col, make_entry(c, current_color));
        cursor_col++;
    }
    if (cursor_col >= VGA_WIDTH) {
        cursor_col = 0;
        cursor_row++;
    }
    if (cursor_row >= VGA_HEIGHT) {
        vga_scroll();
    }
    update_hw_cursor();
}

void vga_write(const char* s) {
    while (*s) {
        vga_putchar(*s++);
    }
}

void vga_write_color(const char* s, uint8_t fg, uint8_t bg) {
    uint8_t old = current_color;
    vga_set_color(fg, bg);
    vga_write(s);
    current_color = old;
}

void vga_newline(void) {
    vga_putchar('\n');
}

// ---- 图形模式渲染: 将 vbuf 绘制到 LFB 指定位置 ----
void vga_render(int px, int py) {
    if (!gfx_available()) return;
    uint32_t* fb = (uint32_t*)gfx_draw_target();   // 走双缓冲绘制目标, 避免直写 LFB 造成闪烁
    int pitch = gfx_pitch() / 4;   // dword 数
    for (int row = 0; row < VGA_HEIGHT; row++) {
        for (int col = 0; col < VGA_WIDTH; col++) {
            uint16_t entry = vbuf[row * VGA_WIDTH + col];
            uint8_t color = (uint8_t)(entry >> 8);
            uint32_t fg = vga_color_to_rgb(color & 0x0F);
            uint32_t bg = vga_color_to_rgb(color >> 4);
            char c = (char)(entry & 0xFF);
            if (c < 0x20 || c > 0x7E) c = ' ';
            const uint8_t* glyph = font8x16[c - 0x20];
            int bx = px + col * 8;
            int by = py + row * 16;
            for (int yy = 0; yy < 16; yy++) {
                uint8_t line = glyph[yy];
                uint32_t* p = fb + (uint32_t)(by + yy) * pitch + (uint32_t)bx;
                for (int xx = 0; xx < 8; xx++) {
                    p[xx] = (line & (1 << (7 - xx))) ? fg : bg;
                }
            }
        }
    }
}

// ---- 简易 printf ----
void vga_printf(const char* fmt, ...) {
    int* argp = (int*)((uint8_t*)&fmt + sizeof(fmt));
    char buf[32];

    for (const char* p = fmt; *p; p++) {
        if (*p != '%') {
            vga_putchar(*p);
            continue;
        }
        p++;
        if (*p == 0) break;

        int zero_pad = 0;
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            if (*p == '0' && width == 0) {
                zero_pad = 1;
            } else {
                width = width * 10 + (*p - '0');
            }
            p++;
        }

        switch (*p) {
        case 'd': {
            int v = *argp++;
            itoa(v, buf, 10);
            vga_write(buf);
            break;
        }
        case 'u': {
            unsigned int v = *(unsigned int*)argp++;
            uitoa(v, buf, 10);
            vga_write(buf);
            break;
        }
        case 'x': {
            unsigned int v = *(unsigned int*)argp++;
            uitoa(v, buf, 16);
            vga_write(buf);
            break;
        }
        case 'X': {
            unsigned int v = *(unsigned int*)argp++;
            uitoa(v, buf, 16);
            vga_write(buf);
            break;
        }
        case 'c': {
            int v = *argp++;
            vga_putchar((char)v);
            break;
        }
        case 's': {
            const char* s = (const char*)*argp++;
            if (!s) s = "(null)";
            vga_write(s);
            break;
        }
        case 'p': {
            unsigned int v = *(unsigned int*)argp++;
            vga_write("0x");
            uitoa(v, buf, 16);
            vga_write(buf);
            break;
        }
        case '%':
            vga_putchar('%');
            break;
        default:
            vga_putchar('%');
            vga_putchar(*p);
            break;
        }
    }
}
