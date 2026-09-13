// ============================================================
// EpochOS - 定时器 (PIT, IRQ0) 与系统时钟
// ============================================================
#ifndef EPOCHOS_TIMER_H
#define EPOCHOS_TIMER_H

#include "types.h"

#define TIMER_HZ 100

void timer_init(void);
uint32_t timer_get_ticks(void);   // 开机以来的 tick 数
uint32_t timer_get_uptime(void);  // 开机以来的秒数

// 睡眠指定毫秒数 (基于 tick, hlt 省电等待)
void timer_sleep(uint32_t ms);

#endif
