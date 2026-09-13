// ============================================================
// EpochOS - 字符串/内存函数实现
// ============================================================
#include "string.h"

size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

int strcmp(const char* a, const char* b) {
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == 0) {
            return (unsigned char)a[i] - (unsigned char)b[i];
        }
    }
    return 0;
}

char* strcpy(char* dst, const char* src) {
    char* d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

char* strncpy(char* dst, const char* src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = 0;
    }
    return dst;
}

char* strcat(char* dst, const char* src) {
    char* d = dst;
    while (*d) d++;
    while ((*d++ = *src++)) {}
    return dst;
}

char* strchr(const char* s, int c) {
    while (*s) {
        if (*s == (char)c) {
            return (char*)s;
        }
        s++;
    }
    return (c == 0) ? (char*)s : 0;
}

void* memset(void* dst, int c, size_t n) {
    char* d = (char*)dst;
    for (size_t i = 0; i < n; i++) {
        d[i] = (char)c;
    }
    return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
    char* d = (char*)dst;
    const char* s = (const char*)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* x = (const unsigned char*)a;
    const unsigned char* y = (const unsigned char*)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return (int)x[i] - (int)y[i];
        }
    }
    return 0;
}

void itoa(int value, char* buf, int base) {
    char tmp[32];
    int i = 0, neg = 0;
    unsigned int v;
    if (value < 0 && base == 10) {
        neg = 1;
        v = (unsigned int)(-value);
    } else {
        v = (unsigned int)value;
    }
    if (v == 0) {
        tmp[i++] = '0';
    }
    while (v > 0) {
        int d = v % base;
        tmp[i++] = (d < 10) ? ('0' + d) : ('A' + d - 10);
        v /= base;
    }
    int j = 0;
    if (neg) {
        buf[j++] = '-';
    }
    while (i > 0) {
        buf[j++] = tmp[--i];
    }
    buf[j] = 0;
}

void uitoa(uint32_t value, char* buf, int base) {
    char tmp[32];
    int i = 0;
    if (value == 0) {
        tmp[i++] = '0';
    }
    while (value > 0) {
        int d = value % base;
        tmp[i++] = (d < 10) ? ('0' + d) : ('A' + d - 10);
        value /= base;
    }
    int j = 0;
    while (i > 0) {
        buf[j++] = tmp[--i];
    }
    buf[j] = 0;
}

int atoi(const char* s) {
    int r = 0, neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        r = r * 10 + (*s - '0');
        s++;
    }
    return neg ? -r : r;
}
