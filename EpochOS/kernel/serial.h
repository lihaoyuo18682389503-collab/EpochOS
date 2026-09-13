// ============================================================
// EpochOS - 串口驱动 (COM1/COM2, 16550 UART)
// ============================================================
#ifndef EPOCHOS_SERIAL_H
#define EPOCHOS_SERIAL_H

#include "types.h"

#define COM1 0x3F8
#define COM2 0x2F8

int serial_init(uint16_t port);
void serial_putc(uint16_t port, char c);
void serial_write(uint16_t port, const char* s);
void serial_printf(uint16_t port, const char* fmt, ...);
int serial_received(uint16_t port);
char serial_getc(uint16_t port);

// 便捷宏: 向 COM1 输出字符串
#define serial_write_str(s) serial_write(COM1, (s))

#endif
