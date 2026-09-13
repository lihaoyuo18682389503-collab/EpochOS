// ============================================================
// EpochOS - 键盘驱动 (PS/2, IRQ1)
// 支持普通键 + Shift + CapsLock 组合
// ============================================================
#ifndef EPOCHOS_KEYBOARD_H
#define EPOCHOS_KEYBOARD_H

#include "types.h"

#define KB_BUFFER_SIZE 256

// 方向键特殊编码 (扫描码无 ASCII, 映射为控制字符)
#define KEY_UP      1
#define KEY_DOWN    2
#define KEY_LEFT    3
#define KEY_RIGHT   4

void keyboard_init(void);
int keyboard_getchar(void);      // 非阻塞: 有字符返回字符, 无则 -1
int keyboard_has_data(void);

#endif
