// ============================================================
// EpochOS - 标准字符串/内存函数库 (无 libc)
// ============================================================
#ifndef EPOCHOS_STRING_H
#define EPOCHOS_STRING_H

#include "types.h"

size_t strlen(const char* s);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
char* strcpy(char* dst, const char* src);
char* strncpy(char* dst, const char* src, size_t n);
char* strcat(char* dst, const char* src);
char* strchr(const char* s, int c);
void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);
int memcmp(const void* a, const void* b, size_t n);

// 数字转换 (内核内使用, 无 stdio)
void itoa(int value, char* buf, int base);
void uitoa(uint32_t value, char* buf, int base);
int atoi(const char* s);

#endif
