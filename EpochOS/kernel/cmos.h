// ============================================================
// EpochOS - RTC 实时时钟 (CMOS) 驱动
// 读取 CMOS 寄存器获取真实日期/时间
// ============================================================
#ifndef EPOCHOS_CMOS_H
#define EPOCHOS_CMOS_H

#include "types.h"

// 本地时区偏移 (单位: 小时)。CMOS/QEMU 的 RTC 按 UTC 走时, 本机为东八区,
// 不做偏移会让显示整体慢 8 小时。切换时区只需修改此宏 (支持负值)。
#ifndef RTC_TZ_OFFSET_HOURS
#define RTC_TZ_OFFSET_HOURS 8
#endif

struct rtc_time {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;   // 完整年份, 如 2026
    uint8_t weekday; // 1=Sunday ... 7=Saturday
};

// 读取当前时间 (BCD 已转十进制, 并已按 RTC_TZ_OFFSET_HOURS 换算为本地时间)
void rtc_read(struct rtc_time* t);

// 将日期时间格式化为 "YYYY-MM-DD HH:MM:SS" 写入 buf
void rtc_format(char* buf, uint32_t max);

// 星期几的中文名
const char* rtc_weekday_name(uint8_t weekday);

#endif
