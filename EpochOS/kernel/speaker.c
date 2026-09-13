// ============================================================
// EpochOS - PC 扬声器驱动
// 利用 PIT 通道2 输出方波, 通过 8255 端口使能
// ============================================================
#include "speaker.h"
#include "types.h"
#include "timer.h"

static inline void outb_p(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb_p(uint16_t port) {
    uint8_t ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// 启用扬声器
static void speaker_on(uint32_t freq) {
    if (freq == 0) freq = 1000;
    uint32_t div = 1193180 / freq;
    outb_p(0x43, 0xB6);               // 通道2, 方波, 加载 16bit
    outb_p(0x42, (uint8_t)div);
    outb_p(0x42, (uint8_t)(div >> 8));
    uint8_t tmp = inb_p(0x61);
    if (tmp != (tmp | 0x03)) {
        outb_p(0x61, tmp | 0x03);     // 开启扬声器
    }
}

void speaker_off(void) {
    uint8_t tmp = inb_p(0x61) & 0xFC;
    outb_p(0x61, tmp);
}

void speaker_beep(uint32_t freq, uint32_t duration_ms) {
    speaker_on(freq);
    timer_sleep(duration_ms);
    speaker_off();
}

void speaker_play(int* notes, int* durations, int count) {
    for (int i = 0; i < count; i++) {
        if (notes[i] <= 0) {
            speaker_off();
        } else {
            speaker_on((uint32_t)notes[i]);
        }
        timer_sleep((uint32_t)durations[i]);
    }
    speaker_off();
}
