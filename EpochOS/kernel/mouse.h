// ============================================================
// EpochOS - PS/2 鼠标驱动 (IRQ12, 轮询与中断双模式)
// 数据格式: [byte0 flags][byte1 X][byte2 Y], 未启用滚动
// ============================================================
#ifndef EPOCHOS_MOUSE_H
#define EPOCHOS_MOUSE_H

#include "types.h"

#define MOUSE_LEFT_BUTTON   0x01
#define MOUSE_RIGHT_BUTTON  0x02
#define MOUSE_MIDDLE_BUTTON 0x04

struct mouse_state {
    int x, y;              // 累计位移
    int dx, dy;            // 最近一次增量
    uint8_t buttons;       // 按键位
    int active;            // 是否有数据
};

int mouse_init(void);
void mouse_wait(uint8_t a_type);
void mouse_write(uint8_t a_write);
uint8_t mouse_read(void);
void mouse_update(void);   // 手动轮询时调用

extern struct mouse_state g_mouse;

#endif
