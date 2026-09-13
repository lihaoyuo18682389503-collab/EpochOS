// ============================================================
// EpochOS - 串口驱动 (16550 UART, PIO 模式)
// 用于内核调试输出, 可被 QEMU -serial file:xxx.log 捕获
// ============================================================
#include "serial.h"
#include "types.h"
#include "string.h"

static inline void outb_p(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb_p(uint16_t port) {
    uint8_t ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// 初始化串口: 8N1, 波特率 38400 (divisor=3 @1.8432MHz)
int serial_init(uint16_t port) {
    outb_p(port + 1, 0x00);    // 禁止中断
    outb_p(port + 3, 0x80);    // 使能 DLAB
    outb_p(port + 0, 0x03);    // 除数低字节 (38400)
    outb_p(port + 1, 0x00);    // 除数高字节
    outb_p(port + 3, 0x03);    // 8N1
    outb_p(port + 2, 0xC7);    // FIFO 使能, 14 字节阈值
    outb_p(port + 4, 0x0B);    // IRQ 使能, RTS/DSR 置位
    return 1;
}

// 等待发送缓冲空
static void serial_wait_tx(uint16_t port) {
    for (int i = 0; i < 100000; i++) {
        if (inb_p(port + 5) & 0x20) return;
    }
}

void serial_putc(uint16_t port, char c) {
    serial_wait_tx(port);
    outb_p(port, (uint8_t)c);
}

void serial_write(uint16_t port, const char* s) {
    while (*s) {
        serial_putc(port, *s++);
    }
}

// 简易格式化输出 (复用 string.h 的 itoa/uitoa)
void serial_printf(uint16_t port, const char* fmt, ...) {
    int* argp = (int*)((uint8_t*)&fmt + sizeof(fmt));
    char buf[32];

    for (const char* p = fmt; *p; p++) {
        if (*p != '%') {
            serial_putc(port, *p);
            continue;
        }
        p++;
        if (*p == 0) break;
        switch (*p) {
        case 'd': {
            int v = *argp++;
            itoa(v, buf, 10);
            serial_write(port, buf);
            break;
        }
        case 'u': {
            unsigned int v = *(unsigned int*)argp++;
            uitoa(v, buf, 10);
            serial_write(port, buf);
            break;
        }
        case 'x': {
            unsigned int v = *(unsigned int*)argp++;
            uitoa(v, buf, 16);
            serial_write(port, buf);
            break;
        }
        case 's': {
            const char* s = *(const char**)argp++;
            serial_write(port, s ? s : "(null)");
            break;
        }
        case 'c': {
            int c = *argp++;
            serial_putc(port, (char)c);
            break;
        }
        case '%':
            serial_putc(port, '%');
            break;
        default:
            serial_putc(port, '%');
            serial_putc(port, *p);
            break;
        }
    }
}

int serial_received(uint16_t port) {
    return inb_p(port + 5) & 0x01;
}

char serial_getc(uint16_t port) {
    while (!serial_received(port));
    return (char)inb_p(port);
}
