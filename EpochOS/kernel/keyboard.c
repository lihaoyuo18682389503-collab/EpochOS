// ============================================================
// EpochOS - 键盘驱动 (PS/2, IRQ1)
// 扫描码表 (Set 1) + Shift/CapsLock 组合
// ============================================================
#include "keyboard.h"
#include "interrupts.h"

// 键盘缓冲区 (环形)
static volatile char kb_buffer[KB_BUFFER_SIZE];
static volatile int kb_head = 0;
static volatile int kb_tail = 0;
static volatile int kb_count = 0;

static int shift_down = 0;
static int caps_on = 0;

// 普通键扫描码 -> ASCII (小写)
static const char scancode_map[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   /* LCtrl */
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   /* LShift */
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0,   /* RShift */
    '*', 0,   /* Alt */
    ' ', 0,   /* CapsLock */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* F1-F10 */
    0,   /* NumLock */
    0,   /* ScrollLock */
    0,   /* Home */
    0,   /* Up */
    0,   /* PgUp */
    '-',
    0,   /* Left */
    0,
    0,   /* Right */
    '+',
    0,   /* End */
    0,   /* Down */
    0,   /* PgDn */
    0,   /* Ins */
    0,   /* Del */
    0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

// Shift 组合键 -> 大写/符号
static const char scancode_shift_map[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0,   '*', 0,   ' ', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static void kb_push(char c) {
    if (kb_count < KB_BUFFER_SIZE) {
        kb_buffer[kb_tail] = c;
        kb_tail = (kb_tail + 1) % KB_BUFFER_SIZE;
        kb_count++;
    }
}

static void keyboard_handler(struct regs* r) {
    (void)r;
    uint8_t scancode = inb(0x60);

    if (scancode & 0x80) {
        // 按键释放
        uint8_t key = scancode & 0x7F;
        if (key == 0x2A || key == 0x36) {
            shift_down = 0;
        }
        return;
    }

    // 特殊键状态
    if (scancode == 0x2A || scancode == 0x36) {   // Shift 按下
        shift_down = 1;
        return;
    }
    if (scancode == 0x3A) {                        // CapsLock
        caps_on = !caps_on;
        return;
    }
    if (scancode == 0x1D || scancode == 0x38) {    // Ctrl/Alt 忽略
        return;
    }

    // 方向键 (Set1: Up=0x48 Down=0x50 Left=0x4B Right=0x4D)
    if (scancode == 0x48 || scancode == 0x50 || scancode == 0x4B || scancode == 0x4D) {
        char d;
        if (scancode == 0x48) d = KEY_UP;
        else if (scancode == 0x50) d = KEY_DOWN;
        else if (scancode == 0x4B) d = KEY_LEFT;
        else d = KEY_RIGHT;
        kb_push(d);
        return;
    }

    if (scancode < 128) {
        char c;
        if (shift_down) {
            c = scancode_shift_map[scancode];
        } else {
            c = scancode_map[scancode];
        }
        // CapsLock: 字母大小写切换
        if (caps_on && c >= 'a' && c <= 'z') {
            c -= 32;
        } else if (caps_on && c >= 'A' && c <= 'Z') {
            c += 32;
        }
        if (c != 0) {
            kb_push(c);
        }
    }
}

void keyboard_init(void) {
    kb_head = 0;
    kb_tail = 0;
    kb_count = 0;
    shift_down = 0;
    caps_on = 0;
    irq_install_handler(1, keyboard_handler);
}

int keyboard_has_data(void) {
    return kb_count > 0;
}

int keyboard_getchar(void) {
    if (kb_count == 0) {
        return -1;
    }
    char c = kb_buffer[kb_head];
    kb_head = (kb_head + 1) % KB_BUFFER_SIZE;
    kb_count--;
    return (int)c;
}
