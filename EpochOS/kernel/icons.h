// EpochOS - 应用图标资源声明 (36x36 RGBA, 自动生成)
#ifndef ICONS_H
#define ICONS_H

#include "types.h"

#define ICON_SIZE 36
#define ICON_PIX  (ICON_SIZE * ICON_SIZE)

typedef struct icon_entry {
    const char* name;
    const uint8_t* rgb;
    const uint8_t* alpha;
} icon_entry;

extern const uint8_t IC_SETTINGS[ICON_PIX*3];
extern const uint8_t IC_SETTINGS_A[ICON_PIX];
extern const uint8_t IC_MANAGER[ICON_PIX*3];
extern const uint8_t IC_MANAGER_A[ICON_PIX];
extern const uint8_t IC_BROWSER[ICON_PIX*3];
extern const uint8_t IC_BROWSER_A[ICON_PIX];
extern const uint8_t IC_TRANSLATE[ICON_PIX*3];
extern const uint8_t IC_TRANSLATE_A[ICON_PIX];
extern const uint8_t IC_CONVERTER[ICON_PIX*3];
extern const uint8_t IC_CONVERTER_A[ICON_PIX];
extern const uint8_t IC_SCREENSHOT[ICON_PIX*3];
extern const uint8_t IC_SCREENSHOT_A[ICON_PIX];
extern const uint8_t IC_PHOTO[ICON_PIX*3];
extern const uint8_t IC_PHOTO_A[ICON_PIX];
extern const uint8_t IC_VIDEO[ICON_PIX*3];
extern const uint8_t IC_VIDEO_A[ICON_PIX];
extern const uint8_t IC_PDF[ICON_PIX*3];
extern const uint8_t IC_PDF_A[ICON_PIX];
extern const uint8_t IC_MUSIC[ICON_PIX*3];
extern const uint8_t IC_MUSIC_A[ICON_PIX];
extern const uint8_t IC_CALENDAR[ICON_PIX*3];
extern const uint8_t IC_CALENDAR_A[ICON_PIX];
extern const uint8_t IC_EDITOR[ICON_PIX*3];
extern const uint8_t IC_EDITOR_A[ICON_PIX];
extern const uint8_t IC_ALARM[ICON_PIX*3];
extern const uint8_t IC_ALARM_A[ICON_PIX];
extern const uint8_t IC_CALCULATOR[ICON_PIX*3];
extern const uint8_t IC_CALCULATOR_A[ICON_PIX];
extern const uint8_t IC_STORE[ICON_PIX*3];
extern const uint8_t IC_STORE_A[ICON_PIX];
extern const uint8_t IC_ZIP[ICON_PIX*3];
extern const uint8_t IC_ZIP_A[ICON_PIX];
extern const uint8_t IC_IDE[ICON_PIX*3];
extern const uint8_t IC_IDE_A[ICON_PIX];
extern const uint8_t IC_FILES[ICON_PIX*3];
extern const uint8_t IC_FILES_A[ICON_PIX];
extern const uint8_t IC_TASKMGR[ICON_PIX*3];
extern const uint8_t IC_TASKMGR_A[ICON_PIX];
extern const uint8_t IC_TERMINAL[ICON_PIX*3];
extern const uint8_t IC_TERMINAL_A[ICON_PIX];
extern const uint8_t IC_DISK[ICON_PIX*3];
extern const uint8_t IC_DISK_A[ICON_PIX];
extern const uint8_t IC_NETWORK[ICON_PIX*3];
extern const uint8_t IC_NETWORK_A[ICON_PIX];

#endif