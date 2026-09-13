// ============================================================
// EpochOS - RTC 实时时钟 (CMOS) 驱动实现
// 端口: 0x70 (索引) / 0x71 (数据)
// 注意: CMOS 需要 NMI 禁用位保持 1 (0x80)
// ============================================================

#include "cmos.h"
#include "types.h"

// 从 CMOS 读一个字节
static uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg | 0x80);   // 0x80 = NMI disable
    return inb(0x71);
}

// BCD 转二进制
static uint8_t bcd_to_bin(uint8_t bcd) {
    return (bcd & 0x0F) + ((bcd >> 4) * 10);
}

// ============================================================
// 本地时区换算
// CMOS/QEMU 的 RTC 按 UTC 走时, 而本机处于东八区, 直接显示会比真实时间
// 慢 8 小时。这里在读完寄存器后统一叠加 RTC_TZ_OFFSET_HOURS 偏移, 并同步
// 处理跨日/跨月/跨年与星期几平移, 使状态栏、时钟、date 等上层调用拿到的
// 都是本地时间 (上层无需再关心时区)。
// ============================================================

static int rtc_is_leap(uint16_t y) {
    return ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
}

static uint8_t rtc_month_days(uint16_t y, uint8_t m) {
    static const uint8_t dm[12] = {31, 28, 31, 30, 31, 30,
                                   31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) return 31;
    if (m == 2 && rtc_is_leap(y)) return 29;
    return dm[m - 1];
}

static void rtc_to_local(struct rtc_time* t) {
    int hour = (int)t->hour + RTC_TZ_OFFSET_HOURS;
    int day_shift = 0;
    while (hour >= 24) { hour -= 24; day_shift++; }
    while (hour < 0)   { hour += 24; day_shift--; }
    t->hour = (uint8_t)hour;

    int shift = day_shift;              // 星期几需要同样的平移量
    while (day_shift > 0) {             // 日期向前进位
        if ((int)t->day < (int)rtc_month_days(t->year, t->month)) {
            t->day = (uint8_t)(t->day + 1);
        } else {
            t->day = 1;
            if (t->month == 12) { t->month = 1; t->year = (uint16_t)(t->year + 1); }
            else t->month = (uint8_t)(t->month + 1);
        }
        day_shift--;
    }
    while (day_shift < 0) {             // 日期向前借位
        if (t->day > 1) {
            t->day = (uint8_t)(t->day - 1);
        } else {
            if (t->month == 1) { t->month = 12; t->year = (uint16_t)(t->year - 1); }
            else t->month = (uint8_t)(t->month - 1);
            t->day = rtc_month_days(t->year, t->month);
        }
        day_shift++;
    }

    // 星期几 (1=Sunday ... 7=Saturday) 随日期同步平移并取模
    if (t->weekday >= 1 && t->weekday <= 7) {
        int w = ((int)t->weekday - 1 + shift) % 7;
        if (w < 0) w += 7;
        t->weekday = (uint8_t)(w + 1);
    }
}

void rtc_read(struct rtc_time* t) {
    // 注意: 不等待 RTC 更新完成 (QEMU 下 UIP spin 会卡 ~55ms/次, 拖垮每帧渲染)

    // status B: bit2=1 表示二进制模式 (非 BCD)
    uint8_t status_b = cmos_read(0x0B);
    int binary = (status_b & 0x04) ? 1 : 0;

    uint8_t second = cmos_read(0x00);
    uint8_t minute = cmos_read(0x02);
    uint8_t hour   = cmos_read(0x04);
    uint8_t day    = cmos_read(0x07);
    uint8_t month  = cmos_read(0x08);
    uint8_t year   = cmos_read(0x09);
    uint8_t century_reg = 0x32;          // ACPI 常用 century 寄存器
    uint8_t century = cmos_read(century_reg);
    uint8_t weekday = cmos_read(0x06);

    if (!binary) {
        second = bcd_to_bin(second);
        minute = bcd_to_bin(minute);
        hour   = bcd_to_bin(hour);
        day    = bcd_to_bin(day);
        month  = bcd_to_bin(month);
        year   = bcd_to_bin(year);
        if (century) century = bcd_to_bin(century);
        weekday = bcd_to_bin(weekday);
    }

    // 12 小时制转 24 小时制
    uint8_t pm = hour & 0x80;
    hour &= 0x7F;
    if (pm) {
        if (hour < 12) hour += 12;
    } else {
        if (hour == 12) hour = 0;
    }

    if (century) {
        t->year = (uint16_t)century * 100 + year;
    } else {
        t->year = 2000 + year;
    }
    t->second = second;
    t->minute = minute;
    t->hour   = hour;
    t->day    = day;
    t->month  = month;
    t->weekday = weekday;

    // UTC -> 本地时区 (东八区) 换算, 上层拿到即为本机真实时间
    rtc_to_local(t);
}

void rtc_format(char* buf, uint32_t max) {
    struct rtc_time t;
    rtc_read(&t);
    // 直接构造 "YYYY-MM-DD HH:MM:SS"
    static const char digits[] = "0123456789";
    uint32_t i = 0;
    uint16_t y = t.year;
    // year
    buf[i++] = digits[(y / 1000) % 10];
    buf[i++] = digits[(y / 100) % 10];
    buf[i++] = digits[(y / 10) % 10];
    buf[i++] = digits[y % 10];
    buf[i++] = '-';
    buf[i++] = digits[t.month / 10];
    buf[i++] = digits[t.month % 10];
    buf[i++] = '-';
    buf[i++] = digits[t.day / 10];
    buf[i++] = digits[t.day % 10];
    buf[i++] = ' ';
    buf[i++] = digits[t.hour / 10];
    buf[i++] = digits[t.hour % 10];
    buf[i++] = ':';
    buf[i++] = digits[t.minute / 10];
    buf[i++] = digits[t.minute % 10];
    buf[i++] = ':';
    buf[i++] = digits[t.second / 10];
    buf[i++] = digits[t.second % 10];
    buf[i++] = '\0';
    (void)max;
}

const char* rtc_weekday_name(uint8_t weekday) {
    switch (weekday) {
        case 1: return "Sunday";
        case 2: return "Monday";
        case 3: return "Tuesday";
        case 4: return "Wednesday";
        case 5: return "Thursday";
        case 6: return "Friday";
        case 7: return "Saturday";
        default: return "Unknown";
    }
}
