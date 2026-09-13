// ============================================================
// EpochOS - 定时器 (PIT 8254, IRQ0) 与系统时钟
// ============================================================
#include "timer.h"
#include "interrupts.h"

static volatile uint32_t ticks = 0;

static void timer_handler(struct regs* r) {
    (void)r;
    ticks++;
}

void timer_init(void) {
    // PIT 频率 = 1193182 / divisor, 目标 100Hz
    uint32_t divisor = 1193182 / TIMER_HZ;
    outb(0x43, 0x36);                    // 通道0, 低字节+高字节, 方波
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
    irq_install_handler(0, timer_handler);
}

uint32_t timer_get_ticks(void) {
    return ticks;
}

uint32_t timer_get_uptime(void) {
    return ticks / TIMER_HZ;
}

void timer_sleep(uint32_t ms) {
    uint32_t target = ticks + (ms * TIMER_HZ) / 1000;
    while (ticks < target) {
        __asm__ __volatile__("hlt");
    }
}
