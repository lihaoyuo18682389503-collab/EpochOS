// ============================================================
// EpochOS - PC 扬声器驱动 (PIT 通道2 + 8255)
// ============================================================
#ifndef EPOCHOS_SPEAKER_H
#define EPOCHOS_SPEAKER_H

#include "types.h"

// 播放指定频率音, 持续 duration_ms 毫秒 (阻塞)
void speaker_beep(uint32_t freq, uint32_t duration_ms);

// 播放一段旋律 (音符频率数组, -1 结束)
void speaker_play(int* notes, int* durations, int count);

// 关闭扬声器
void speaker_off(void);

#endif
