// ============================================================
// EpochOS - PS/2 鼠标驱动 (IRQ12)
// ============================================================
#include "mouse.h"
#include "interrupts.h"
#include "serial.h"

#define PS2_CMD_PORT   0x64
#define PS2_DATA_PORT  0x60
#define MOUSE_IRQ      12

struct mouse_state g_mouse;

static uint8_t mouse_cycle = 0;
static uint8_t mouse_packet[3];

// 等待 8042 状态
void mouse_wait(uint8_t a_type) {
    uint32_t timeout = 100000;
    if (a_type == 0) {
        // 等待输入缓冲空 (可写)
        while (timeout--) {
            uint8_t st;
            __asm__ __volatile__("inb %1, %0" : "=a"(st) : "Nd"((uint16_t)0x64));
            if (!(st & 0x02)) return;
        }
    } else {
        // 等待输出缓冲有数据 (可读)
        while (timeout--) {
            uint8_t st;
            __asm__ __volatile__("inb %1, %0" : "=a"(st) : "Nd"((uint16_t)0x64));
            if (st & 0x01) return;
        }
    }
}

void mouse_write(uint8_t val) {
    mouse_wait(0);
    __asm__ __volatile__("outb %0, %1" : : "a"((uint8_t)0xD4), "Nd"((uint16_t)0x64));
    mouse_wait(0);
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"((uint16_t)0x60));
}

uint8_t mouse_read(void) {
    mouse_wait(1);
    uint8_t ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"((uint16_t)0x60));
    return ret;
}

static void mouse_irq_handler(struct regs* r) {
    (void)r;
    uint8_t data;
    __asm__ __volatile__("inb %1, %0" : "=a"(data) : "Nd"((uint16_t)0x60));

    if (mouse_cycle == 0) {
        // 检查同步字节 bit3 必须为 1
        if (!(data & 0x08)) return;
        mouse_packet[0] = data;
        mouse_cycle = 1;
    } else if (mouse_cycle == 1) {
        mouse_packet[1] = data;
        mouse_cycle = 2;
    } else {
        mouse_packet[2] = data;
        mouse_cycle = 0;

        // 解析增量 (8-bit 带符号); 累加直到 GUI 消费, 防止大位移拆包时丢包
        int dx = (int)(int8_t)mouse_packet[1];
        int dy = (int)(int8_t)mouse_packet[2];
        g_mouse.dx += dx;
        g_mouse.dy += -dy;
        if (g_mouse.dx > 2000) g_mouse.dx = 2000;
        if (g_mouse.dx < -2000) g_mouse.dx = -2000;
        if (g_mouse.dy > 2000) g_mouse.dy = 2000;
        if (g_mouse.dy < -2000) g_mouse.dy = -2000;
        g_mouse.x += dx;
        g_mouse.y -= dy;
        g_mouse.buttons = mouse_packet[0] & 0x07;
        g_mouse.active = 1;
        serial_printf(COM1, "[m] pkt=%02X dx=%d dy=%d bt=%d x=%d y=%d\n",
                      mouse_packet[0], dx, dy, g_mouse.buttons, g_mouse.x, g_mouse.y);
    }
}

// 初始化鼠标: 使能辅助设备 + IRQ12
int mouse_init(void) {
    g_mouse.x = 0;
    g_mouse.y = 0;
    g_mouse.dx = 0;
    g_mouse.dy = 0;
    g_mouse.buttons = 0;
    g_mouse.active = 0;
    mouse_cycle = 0;

    // 等待空闲
    mouse_wait(0);
    // 使能辅助设备
    __asm__ __volatile__("outb %0, %1" : : "a"((uint8_t)0xA8), "Nd"((uint16_t)0x64));
    mouse_wait(0);

    // 读取 8042 控制器命令字节, 置位 bit1(IRQ12 使能) 与 bit5(辅助设备时钟使能)
    __asm__ __volatile__("outb %0, %1" : : "a"((uint8_t)0x20), "Nd"((uint16_t)0x64));
    mouse_wait(1);
    uint8_t cmd;
    __asm__ __volatile__("inb %1, %0" : "=a"(cmd) : "Nd"((uint16_t)0x60));
    cmd |= 0x02;   // enable IRQ12
    cmd |= 0x20;   // enable aux clock
    mouse_wait(0);
    __asm__ __volatile__("outb %0, %1" : : "a"((uint8_t)0x60), "Nd"((uint16_t)0x64));
    mouse_wait(0);
    __asm__ __volatile__("outb %0, %1" : : "a"(cmd), "Nd"((uint16_t)0x60));
    mouse_wait(0);

    // 设置默认设置
    mouse_write(0xF6);
    mouse_read();
    // 启用数据报告
    mouse_write(0xF4);
    mouse_read();

    // 挂接 IRQ12
    irq_install_handler(MOUSE_IRQ, mouse_irq_handler);
    return 1;
}

// 手动轮询模式 (非中断)
void mouse_update(void) {
    // 若中断模式已启用且无数据, 直接返回
    if (g_mouse.active) return;
    // 尝试读取 (仅当输出缓冲有数据)
    uint8_t st;
    __asm__ __volatile__("inb %1, %0" : "=a"(st) : "Nd"((uint16_t)0x64));
    if (st & 0x01) {
        uint8_t data;
        __asm__ __volatile__("inb %1, %0" : "=a"(data) : "Nd"((uint16_t)0x60));
        if (mouse_cycle == 0) {
            if (!(data & 0x08)) return;
            mouse_packet[0] = data;
            mouse_cycle = 1;
        } else if (mouse_cycle == 1) {
            mouse_packet[1] = data;
            mouse_cycle = 2;
        } else {
            mouse_packet[2] = data;
            mouse_cycle = 0;
            int dx = (int)(int8_t)mouse_packet[1];
            int dy = (int)(int8_t)mouse_packet[2];
            g_mouse.dx = dx;
            g_mouse.dy = -dy;
            g_mouse.x += dx;
            g_mouse.y -= dy;
            g_mouse.buttons = mouse_packet[0] & 0x07;
            g_mouse.active = 1;
        }
    }
}
