// ============================================================
// EpochOS - 图形用户界面实现
// 800x600 VBE 32bpp, 桌面渐变 + 任务栏 + 层叠窗口
// 窗口: 终端(80x25 虚拟缓冲) / 文件浏览器 / 关于 / 帮助
// 交互: 鼠标点击/拖拽/双击, 键盘输入, RTC 时钟
// ============================================================
#include "gui.h"
#include "gfx.h"
#include "vga.h"
#include "mouse.h"
#include "keyboard.h"
#include "shell.h"
#include "fs.h"
#include "cmos.h"
#include "timer.h"
#include "string.h"
#include "serial.h"
#include "mm.h"
#include "ata.h"
#include "wallpaper.h"
#include "cursor.h"
#include "icons.h"
#include "speaker.h"
#include "task.h"
#include "elf.h"

#define TASKBAR_H   26
#define STATUS_BAR_H 28
#define STATUS_ICON_W 44
static int wallpaper_index = 0;   // 当前壁纸 (0-3)
// Settings 窗口状态: 垃圾清理 / 一键卸载
static int settings_state = 0;        // 0=正常 1=卸载确认中 2=已卸载
static char settings_info[64];        // 状态/结果文本
#define TITLE_H     18
#define WIN_MAX     48

// 文件浏览器条目缓存
#define ENTRY_MAX   64
struct file_entry {
    char     name[FS_MAX_NAME];
    int      is_dir;
    uint32_t size;
};

static struct {
    int used;
    int type;
    char title[24];
    int x, y;            // 内容区左上角 (标题栏之下)
    int w, h;            // 内容区尺寸
    int visible;
    int maximized;       // 1=最大化 (win11 风格)
    int r_x, r_y, r_w, r_h;  // 还原位置
    // 文件浏览器状态
    int file_scroll;
    int file_sel;
    char file_dir[FS_MAX_NAME];
    struct file_entry entries[ENTRY_MAX];
    int entry_count;
    // 通用应用状态 (各内置软件复用)
    int app_state;       // 界面状态/页签
    int app_sel;         // 选中项/焦点项
    char app_buf[256];   // 文本缓冲 (编辑器/翻译/浏览器地址等)
    int app_num;         // 数值状态 (计算器/闹钟等)
    int app_scroll;      // 滚动偏移
    int dirty;           // 脏标记: 需要整窗重绘
    int px, py;          // 上一帧窗口位置 (拖动恢复用)
} wins[WIN_MAX];

// 照片查看器: 显示 /tmp/shot.bin (32bpp raw 截图)
static uint8_t photo_raw[800 * 600 * 4];
static int photo_loaded = 0;
static int photo_err = 0;

static int win_count = 0;
static int active_win = -1;
static void u32_to_str(uint32_t v, char* out);
static int calc_eval(const char* expr, char* out, int outlen);
static int cursor_x = 400, cursor_y = 300;
static int prev_cursor_x = -1, prev_cursor_y = -1;   // 上一帧光标位置 (残影恢复用)
static int drag_win = -1;
static int drag_ox = 0, drag_oy = 0;
static uint32_t g_desk_cache = 0;   // 桌面静态层缓存 (壁纸+渐变)
static int g_desk_ready = 0;
static int desk_need_full = 1;      // 需要全屏恢复桌面
// 桌面右键菜单
#define DMENU_ITEMS 5
static int dmenu_open = 0;
static int dmenu_x = 0, dmenu_y = 0;
static int dmenu_sel = -1;
static const char* dmenu_labels[DMENU_ITEMS] = {
    "Open Terminal", "Refresh", "Next Wallpaper", "System Info", "Close"
};
static uint64_t icons_dirty = ~0ULL; // 需要重绘的桌面图标位图 (bit i=图标i需重绘; 选中/悬停/背景恢复时置全1)
static int status_dirty = 1;
static int status_clock_dirty = 0;        // 需要重绘顶部状态栏 (时间刷新/光标相交/全屏恢复时置位)
static int taskbar_dirty = 1;       // 需要重绘任务栏 (窗口激活变化/光标相交/全屏恢复时置位)
static int g_tb_sig = -1;           // 任务栏窗口状态签名 (检测窗口变化)
static char g_dt_label[24];         // 状态栏日期文本缓存
static char g_dt_time[8];           // 状态栏时间文本缓存 (HH:MM)
static int g_dt_refresh = 0;        // 时间缓存刷新帧计数
static uint8_t prev_buttons = 0;
static int g_mouse_log_cnt = 0;   // 鼠标日志节流计数器
static int dbl_x = -100, dbl_y = -100;
static uint32_t dbl_tick = 0;

// ---------------- Windows 风格 UI 状态 ----------------
#define TASKBAR_ICON_H 20      // 任务栏图标区高

// 桌面图标 (Win11 风格: 位图图标+文字, 双击打开)
// 布局改造: 桌面只保留 Files/Browser 两个图标; 设置入口固定在左上角状态栏;
// 其余全部应用图标放入任务栏 Dock (任务栏数组全量应用; 桌面仅取前 2)
#define DESKTOP_ICONS 2
#define ALL_APPS      46      // 全量应用数组长度 (含 Files/Browser/Settings)
#define TASKBAR_APPS  43      // 任务栏显示数: 排除 Files/Browser/Settings (桌面/左上角入口)
#define TASKBAR_OFF   3       // 任务栏起始偏移 (跳过 Files/Browser/Settings)
static const char* desk_names[ALL_APPS] = {
    "Files", "Browser", "Settings", "Manager", "Translate",
    "Converter", "Screenshot", "Photo", "Video", "PDF",
    "Music", "Calendar", "Editor", "Alarm", "Calculator",
    "Store", "Zip", "IDE", "TaskMgr", "Terminal",
    "Disk", "Net", "Snake", "2048", "Mines",
    "Brick", "Paint", "Notes", "UnitConv", "Stopwatch",
    "HexView", "Tetris", "TicTac", "Memory", "Find",
    "BaseConv", "Random", "TextStats", "Sudoku", "Typing",
    "Pomodoro", "Guess", "Dice", "Todo", "PassGen", "Clock"
};
static const int desk_types[ALL_APPS] = {
    WIN_FILES, WIN_BROWSER, WIN_SETTINGS, WIN_MANAGER, WIN_TRANSLATE,
    WIN_CONVERT, WIN_SHOT, WIN_PHOTO, WIN_VIDEO, WIN_PDF,
    WIN_MUSIC, WIN_CALENDAR, WIN_EDITOR, WIN_ALARM, WIN_CALC,
    WIN_STORE, WIN_ZIP, WIN_IDE, WIN_TASKMGR, WIN_TERMINAL,
    WIN_DISK, WIN_NET, WIN_SNAKE, WIN_2048, WIN_MINES,
    WIN_BRICK, WIN_PAINT, WIN_NOTE, WIN_UNIT, WIN_STOPW,
    WIN_HEXVIEW, WIN_TETRIS, WIN_TICTAC, WIN_MEMORY, WIN_FIND,
    WIN_BASECONV, WIN_RANDOM, WIN_TEXTSTATS, WIN_SUDOKU, WIN_TYPING,
    WIN_POMODORO, WIN_GUESS, WIN_DICE, WIN_TODO, WIN_PASSGEN, WIN_CLOCK
};
// 任务栏/桌面图标位图 (NULL = 代码绘制兜底; 桌面仅取前 DESKTOP_ICONS 项)
static const struct icon_entry* desk_icons[ALL_APPS] = {
    &(struct icon_entry){"", IC_FILES, IC_FILES_A},
    &(struct icon_entry){"", IC_BROWSER, IC_BROWSER_A},
    &(struct icon_entry){"", IC_SETTINGS, IC_SETTINGS_A},
    &(struct icon_entry){"", IC_MANAGER, IC_MANAGER_A},
    &(struct icon_entry){"", IC_TRANSLATE, IC_TRANSLATE_A},
    &(struct icon_entry){"", IC_CONVERTER, IC_CONVERTER_A},
    &(struct icon_entry){"", IC_SCREENSHOT, IC_SCREENSHOT_A},
    &(struct icon_entry){"", IC_PHOTO, IC_PHOTO_A},
    &(struct icon_entry){"", IC_VIDEO, IC_VIDEO_A},
    &(struct icon_entry){"", IC_PDF, IC_PDF_A},
    &(struct icon_entry){"", IC_MUSIC, IC_MUSIC_A},
    &(struct icon_entry){"", IC_CALENDAR, IC_CALENDAR_A},
    &(struct icon_entry){"", IC_EDITOR, IC_EDITOR_A},
    &(struct icon_entry){"", IC_ALARM, IC_ALARM_A},
    &(struct icon_entry){"", IC_CALCULATOR, IC_CALCULATOR_A},
    &(struct icon_entry){"", IC_STORE, IC_STORE_A},
    &(struct icon_entry){"", IC_ZIP, IC_ZIP_A},
    &(struct icon_entry){"", IC_IDE, IC_IDE_A},
    &(struct icon_entry){"", IC_TASKMGR, IC_TASKMGR_A},
    &(struct icon_entry){"", IC_TERMINAL, IC_TERMINAL_A},
    &(struct icon_entry){"", IC_DISK, IC_DISK_A},
    &(struct icon_entry){"", IC_NETWORK, IC_NETWORK_A},
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,   // 22-30: 代码绘制
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,   // 31-39: 新增应用代码绘制
    NULL, NULL, NULL, NULL, NULL, NULL     // 40-45: 代码绘制
};
static int taskbar_page = 0;    // 任务栏 Dock 分页 (每页 TASKBAR_PER_PAGE 个)
#define TASKBAR_PER_PAGE 20
static int desk_sel = -1;           // 当前选中桌面图标 (选中高亮)
static int desk_hover = -1;         // hover 图标
static int desk_dbl = -1;           // 双击目标
static uint32_t desk_dbl_tick = 0;

// ---------------- 桌面右键菜单 ----------------
#define CTX_NUM 4
static const char* ctx_items[CTX_NUM] = {
    "Open Terminal", "Refresh Desktop", "Next Wallpaper", "System Info"
};
static int ctx_open = 0;            // 菜单是否展开
static int ctx_x = 0, ctx_y = 0;    // 菜单位置
static int ctx_sel = -1;            // hover 项
#define CTX_W 190
#define CTX_H (CTX_NUM * 26 + 8)

// ---------------- 通用工具 ----------------
static uint32_t rng_state = 0x9E3779B9u;
static uint32_t rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static int rng_below(int n) { return (int)(rng_next() % (uint32_t)n); }
static void desk_blit_rect(int x, int y, int w, int h);

// 判断某窗口类型是否为"商店应用"(可安装/卸载)
static const char* store_marker_for_type(int type) {
    switch (type) {
    case WIN_SNAKE:   return "/apps/snake";
    case WIN_2048:    return "/apps/2048";
    case WIN_MINES:   return "/apps/mines";
    case WIN_BRICK:   return "/apps/brick";
    case WIN_PAINT:   return "/apps/paint";
    case WIN_NOTE:    return "/apps/notes";
    case WIN_UNIT:    return "/apps/unit";
    case WIN_STOPW:   return "/apps/stopwatch";
    case WIN_HEXVIEW: return "/apps/hexview";
    case WIN_TODO:    return "/apps/todo";
    case WIN_PASSGEN:  return "/apps/passgen";
    case WIN_CLOCK:   return "/apps/clock";
    default: return 0;
    }
}
static int app_is_installed(int type) {
    const char* m = store_marker_for_type(type);
    return m ? fs_exists(m) : 1;
}

// ---------------- 贪吃蛇 ----------------
#define SNAKE_COLS 24
#define SNAKE_ROWS 18
static uint8_t snake_body[SNAKE_COLS * SNAKE_ROWS][2];
static int snake_len;
static int snake_dir;        // 0=上 1=右 2=下 3=左
static int snake_alive;
static int snake_score;
static int snake_fx, snake_fy;
static uint32_t snake_last;  // 上次移动 tick
static int snake_paused;
static void snake_reset(void) {
    snake_len = 3;
    snake_body[0][0] = 6;  snake_body[0][1] = 9;
    snake_body[1][0] = 5;  snake_body[1][1] = 9;
    snake_body[2][0] = 4;  snake_body[2][1] = 9;
    snake_dir = 1;          // 右
    snake_alive = 1;
    snake_score = 0;
    snake_paused = 0;
    snake_fx = 12; snake_fy = 9;
    snake_last = timer_get_ticks();
}
static void snake_place_food(void) {
    int cx = snake_fx, cy = snake_fy;
    for (int tries = 0; tries < 200; tries++) {
        int x = rng_below(SNAKE_COLS), y = rng_below(SNAKE_ROWS);
        int occ = 0;
        for (int k = 0; k < snake_len; k++) {
            if (snake_body[k][0] == x && snake_body[k][1] == y) { occ = 1; break; }
        }
        if (!occ) { snake_fx = x; snake_fy = y; return; }
    }
    snake_fx = cx; snake_fy = cy;
}
static void snake_step(void) {
    if (!snake_alive || snake_paused) return;
    int nx = snake_body[0][0], ny = snake_body[0][1];
    if (snake_dir == 0) ny--;
    else if (snake_dir == 1) nx++;
    else if (snake_dir == 2) ny++;
    else nx--;
    if (nx < 0 || ny < 0 || nx >= SNAKE_COLS || ny >= SNAKE_ROWS) { snake_alive = 0; return; }
    int ate = (nx == snake_fx && ny == snake_fy);
    // 撞自身 (含即将吃掉尾部时尾部移动, 简化: 撞任何身体段即死)
    for (int k = 0; k < snake_len; k++) {
        if (snake_body[k][0] == nx && snake_body[k][1] == ny) { snake_alive = 0; return; }
    }
    for (int k = snake_len; k > 0; k--) {
        snake_body[k][0] = snake_body[k-1][0];
        snake_body[k][1] = snake_body[k-1][1];
    }
    snake_body[0][0] = nx; snake_body[0][1] = ny;
    if (ate) {
        snake_len++;
        snake_score += 10;
        if (snake_len >= SNAKE_COLS * SNAKE_ROWS) { snake_alive = 0; return; }
        snake_place_food();
    }
}

// ---------------- 2048 ----------------
static uint8_t g2048[4][4];
static int g2048_score;
static int g2048_over;
static void g2048_spawn(void);
static void g2048_reset(void) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) g2048[i][j] = 0;
    g2048_score = 0; g2048_over = 0;
    g2048_spawn(); g2048_spawn();
}
static void g2048_spawn(void) {
    int empty[16][2], n = 0;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++)
        if (g2048[i][j] == 0) { empty[n][0] = i; empty[n][1] = j; n++; }
    if (n == 0) return;
    int k = rng_below(n);
    g2048[empty[k][0]][empty[k][1]] = (rng_below(10) == 0) ? 2 : 1;
}
// dir: 0=左 1=右 2=上 3=下; 返回是否产生移动
static int g2048_move(int dir) {
    int moved = 0;
    for (int a = 0; a < 4; a++) {
        int line[4] = {0, 0, 0, 0}, nl = 0;
        for (int b = 0; b < 4; b++) {
            int v;
            if (dir == 0) v = g2048[a][b];
            else if (dir == 1) v = g2048[a][3-b];
            else if (dir == 2) v = g2048[b][a];
            else v = g2048[3-b][a];
            if (v) line[nl++] = v;
        }
        int merged[4] = {0,0,0,0}, nm = 0;
        for (int k = 0; k < nl; k++) {
            if (nm > 0 && line[k] == merged[nm-1]) {
                merged[nm-1]++;
                g2048_score += (1 << merged[nm-1]);
                moved = 1;
            } else {
                merged[nm++] = line[k];
            }
        }
        while (nm < 4) merged[nm++] = 0;
        for (int b = 0; b < 4; b++) {
            int nv = merged[b];
            if (dir == 0) { if (g2048[a][b] != nv) moved = 1; g2048[a][b] = nv; }
            else if (dir == 1) { if (g2048[a][3-b] != nv) moved = 1; g2048[a][3-b] = nv; }
            else if (dir == 2) { if (g2048[b][a] != nv) moved = 1; g2048[b][a] = nv; }
            else { if (g2048[3-b][a] != nv) moved = 1; g2048[3-b][a] = nv; }
        }
    }
    return moved;
}
static void g2048_check_over(void) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        if (g2048[i][j] == 0) { g2048_over = 0; return; }
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        if (j < 3 && g2048[i][j] == g2048[i][j+1]) { g2048_over = 0; return; }
        if (i < 3 && g2048[i][j] == g2048[i+1][j]) { g2048_over = 0; return; }
    }
    g2048_over = 1;
}

// ---------------- 扫雷 ----------------
#define MINES_N 16
#define MINES_M 16
#define MINES_BOMBS 34
static uint8_t mines_g[MINES_N][MINES_M];  // 1=雷
static uint8_t mines_v[MINES_N][MINES_M];  // 已翻开
static uint8_t mines_f[MINES_N][MINES_M];  // 标记
static int mines_first;
static int mines_over;   // 0=进行 1=胜利 2=失败
static int mines_open;
static int mines_flags;
static void mines_reset(void) {
    for (int i = 0; i < MINES_N; i++) for (int j = 0; j < MINES_M; j++) {
        mines_g[i][j] = 0; mines_v[i][j] = 0; mines_f[i][j] = 0;
    }
    mines_first = 1; mines_over = 0; mines_open = 0; mines_flags = 0;
}
static void mines_gen(int sx, int sy) {
    int placed = 0;
    while (placed < MINES_BOMBS) {
        int x = rng_below(MINES_N), y = rng_below(MINES_M);
        if (x == sx && y == sy) continue;
        if (mines_g[x][y]) continue;
        mines_g[x][y] = 1; placed++;
    }
    mines_first = 0;
}
static int mines_count(int x, int y) {
    int c = 0;
    for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) {
        int nx = x + dx, ny = y + dy;
        if (nx < 0 || ny < 0 || nx >= MINES_N || ny >= MINES_M) continue;
        if (mines_g[nx][ny]) c++;
    }
    return c;
}
static void mines_flood(int x, int y) {
    if (x < 0 || y < 0 || x >= MINES_N || y >= MINES_M) return;
    if (mines_v[x][y] || mines_f[x][y]) return;
    mines_v[x][y] = 1; mines_open++;
    if (mines_count(x, y) == 0 && !mines_g[x][y]) {
        for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++)
            mines_flood(x + dx, y + dy);
    }
}
static void mines_open_cell(int x, int y) {
    if (mines_over) return;
    if (mines_f[x][y]) return;
    if (mines_first) { mines_gen(x, y); }
    if (mines_g[x][y]) {
        mines_v[x][y] = 1;
        mines_over = 2;   // 失败
        return;
    }
    mines_flood(x, y);
    if (mines_open >= MINES_N * MINES_M - MINES_BOMBS) mines_over = 1;
}
static void mines_toggle_flag(int x, int y) {
    if (mines_over || mines_v[x][y]) return;
    mines_f[x][y] = !mines_f[x][y];
    if (mines_f[x][y]) mines_flags++; else mines_flags--;
}

// ---------------- 打砖块 ----------------
#define BRICK_COLS 9
#define BRICK_ROWS 5
static uint8_t brick_g[BRICK_ROWS][BRICK_COLS];
static int brick_score;
static int brick_over;     // 0=进行 1=胜利 2=失败
static int brick_paddle;   // 挡板左缘 x (px)
static int brick_bx, brick_by;    // 球坐标 1/16 px
static int brick_bvx, brick_bvy;
static int brick_run;
static void brick_reset(void) {
    for (int r = 0; r < BRICK_ROWS; r++) for (int c = 0; c < BRICK_COLS; c++) brick_g[r][c] = 1;
    brick_score = 0; brick_over = 0; brick_run = 1;
    brick_paddle = 220;
    brick_bx = 320 << 4; brick_by = 260 << 4;
    brick_bvx = 96; brick_bvy = -96;
}
#define BRICK_CELL_W 52
#define BRICK_CELL_H 20
static void brick_step(void) {
    if (!brick_run || brick_over) return;
    brick_bx += brick_bvx;
    brick_by += brick_bvy;
    int bx = brick_bx >> 4, by = brick_by >> 4;
    // 左右墙
    if (bx < 4) { brick_bx = 4; brick_bvx = -brick_bvx; }
    if (bx > 556) { brick_bx = 556; brick_bvx = -brick_bvx; }
    // 顶墙
    if (by < 40) { brick_by = 40; brick_bvy = -brick_bvy; }
    // 底部
    if (by > 336) { brick_over = 2; brick_run = 0; return; }
    // 砖块碰撞
    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            if (!brick_g[r][c]) continue;
            int rx = 10 + c * (BRICK_CELL_W + 4);
            int ry = 50 + r * (BRICK_CELL_H + 4);
            if (bx + 5 >= rx && bx <= rx + BRICK_CELL_W &&
                by + 5 >= ry && by <= ry + BRICK_CELL_H) {
                brick_g[r][c] = 0;
                brick_score += 10;
                brick_bvy = -brick_bvy;
                if (bx < rx + BRICK_CELL_W / 2) brick_bvx = -((brick_bvx > 0) ? brick_bvx : -brick_bvx);
                else brick_bvx = -((brick_bvx < 0) ? brick_bvx : -brick_bvx);
                break;
            }
        }
    }
    // 挡板碰撞
    if (by + 5 >= 316 && by + 5 <= 336 &&
        bx + 5 >= brick_paddle && bx <= brick_paddle + 70) {
        brick_by = 316 - 5;
        brick_bvy = -brick_bvy;
        // 按落点改变水平方向
        int off = (bx - brick_paddle) - 35;   // -35..35
        brick_bvx = (off * 4) / 5;
        if (brick_bvx > 0 && brick_bvx < 32) brick_bvx = 32;
        if (brick_bvx < 0 && brick_bvx > -32) brick_bvx = -32;
    }
    // 通关
    int left = 0;
    for (int r = 0; r < BRICK_ROWS; r++) for (int c = 0; c < BRICK_COLS; c++)
        if (brick_g[r][c]) left++;
    if (left == 0) { brick_over = 1; brick_run = 0; }
}

// ---------------- 绘图板 ----------------
#define PAINT_MAX 1024
static int paint_x0[PAINT_MAX], paint_y0[PAINT_MAX];
static int paint_x1[PAINT_MAX], paint_y1[PAINT_MAX];
static uint32_t paint_col[PAINT_MAX];
static int paint_n;
static int paint_cur_col;
static int paint_drawing;

// ---------------- 待办清单 Todo ----------------
#define TODO_MAX     16          // 待办项上限 (纯静态数组)
#define TODO_LEN     48          // 单项文本长度
static char todo_items[TODO_MAX][TODO_LEN];
static int  todo_done[TODO_MAX];
static int  todo_n;              // 当前项数
static int  todo_scroll;         // 列表滚动偏移
static void todo_add(const char* s) {
    if (todo_n >= TODO_MAX) return;
    if (!s || !*s) return;
    int i;
    for (i = 0; s[i] && i < TODO_LEN - 1; i++) todo_items[todo_n][i] = s[i];
    todo_items[todo_n][i] = 0;
    todo_done[todo_n] = 0;
    todo_n++;
    todo_scroll = todo_n;        // 自动滚到底, 新项可见
}
static void todo_clear_done(void) {
    int w = 0;
    for (int r = 0; r < todo_n; r++) {
        if (todo_done[r]) continue;
        for (int i = 0; i < TODO_LEN; i++) todo_items[w][i] = todo_items[r][i];
        todo_done[w] = todo_done[r];
        w++;
    }
    todo_n = w;
    if (todo_scroll > todo_n) todo_scroll = todo_n;
}
static int todo_vis_rows(int wh) {
    int r = (wh - 130) / 28;   // 列表区: 顶=122, 底=高-8
    if (r < 1) r = 1;
    return r;
}

// ---------------- 便签 (用 app_buf 存文本, app_num 为光标) ----------------

// ---------------- 单位换算 ----------------
static int unit_cat;    // 0=长度 1=重量 2=温度
static int unit_from, unit_to;
// 长度(基准mm): 0=mm 1=cm 2=m 3=km ; 重量(基准mg): 0=mg 1=g 2=kg 3=t ; 温度: 0=C 1=F 2=K
static const int unit_l[4] = {1, 10, 1000, 1000000};
static const int unit_w[4] = {1, 1000, 1000000, 1000000000};
static void unit_reset(void) { unit_cat = 0; unit_from = 0; unit_to = 1; }

// ---------------- 秒表 ----------------
static int stopw_run;
static uint32_t stopw_start;
static uint32_t stopw_acc;

// ---------------- TextStats 文本统计 ----------------
static char ts_text[256];
static void ts_clear(void) { ts_text[0] = 0; }

// ---------------- Sudoku 数独 ----------------
static uint8_t sd_puzzle[81], sd_solution[81], sd_fixed[81];
static int sd_sel;              // 当前选中格 (-1=未选)
static int sd_cur;              // 当前谜题索引
static int sd_msg;              // 0=正常 1=正确 2=有误
#define SD_NUM_PUZZLES 2
static void sd_load_puzzle(int idx) {
    static const uint8_t sd_sol_data[SD_NUM_PUZZLES][81] = {
        {5,3,4,6,7,8,9,1,2, 6,7,2,1,9,5,3,4,8, 1,9,8,3,4,2,5,6,7,
         8,5,9,7,6,1,4,2,3, 4,2,6,8,5,3,7,9,1, 7,1,3,9,2,4,8,5,6,
         9,6,1,5,3,7,2,8,4, 2,8,7,4,1,9,6,3,5, 3,4,5,2,8,6,1,7,9},
        {8,1,2,7,5,3,6,4,9, 9,4,3,6,8,2,1,7,5, 6,7,5,4,9,1,2,8,3,
         1,5,4,2,3,7,8,9,6, 3,6,9,8,4,5,7,2,1, 2,8,7,1,6,9,5,3,4,
         5,2,1,9,7,4,3,6,8, 4,3,8,5,2,6,9,1,7, 7,9,6,3,1,8,4,5,2}
    };
    static const uint8_t sd_mask_data[SD_NUM_PUZZLES][81] = {
        {1,1,0,0,1,0,0,0,0, 1,0,0,1,1,1,0,0,0, 0,1,1,0,0,0,0,1,0,
         1,0,0,0,1,0,0,0,1, 1,0,0,1,0,1,0,0,1, 1,0,0,0,1,0,0,0,1,
         0,1,0,0,0,0,1,1,0, 0,0,0,1,1,1,0,0,1, 0,0,0,0,1,0,0,1,1},
        {1,1,0,0,1,0,0,1,1, 1,0,1,0,1,0,0,0,1, 0,1,0,0,1,0,0,1,0,
         1,0,0,1,0,0,0,1,1, 0,1,0,1,0,1,0,0,1, 0,0,1,0,0,1,0,1,0,
         0,0,1,0,1,0,0,0,1, 1,0,0,0,0,1,0,0,1, 0,1,0,0,1,0,0,0,1}
    };
    sd_cur = idx;
    sd_sel = -1;
    sd_msg = 0;
    for (int k = 0; k < 81; k++) {
        sd_solution[k] = sd_sol_data[idx][k];
        sd_fixed[k] = sd_mask_data[idx][k];
        sd_puzzle[k] = sd_fixed[k] ? sd_solution[k] : 0;
    }
}

// ---------------- TypingTest 打字测试 ----------------
static const char* ty_texts[3] = {
    "the quick brown fox jumps over the lazy dog",
    "epoch is a small x86 operating system written from scratch",
    "practice makes perfect keep typing every single day"
};
static int ty_idx, ty_pos, ty_err;
static uint32_t ty_start;       // 开始时的 uptime 秒
static int ty_started, ty_done;
static void ty_load(int idx) { ty_idx = idx; ty_pos = 0; ty_err = 0; ty_started = 0; ty_done = 0; ty_start = 0; }

// ---------------- Pomodoro 番茄钟 ----------------
static int pom_run, pom_work;   // run 标志 / 1=工作 0=休息
static uint32_t pom_left;       // 剩余秒数
static uint32_t pom_total;      // 当前阶段总秒数
static uint32_t pom_deadline;   // 截止 tick (以秒计)
static int pom_sessions;        // 已完成工作段数
static void pom_reset(void) { pom_run = 0; pom_work = 1; pom_total = 25 * 60; pom_left = pom_total; pom_deadline = 0; pom_sessions = 0; }

// ---------------- GuessNumber 猜数字 ----------------
static int gs_target, gs_tries, gs_over, gs_msg;
static char gs_buf[8];
static void gs_new(void) { gs_target = (int)(rng_next() % 100) + 1; gs_tries = 0; gs_over = 0; gs_msg = 0; gs_buf[0] = 0; }

// ---------------- DiceRoller 掷骰子 ----------------
static int dc_side;             // 当前骰子面数
static int dc_result;           // 最近结果
static int dc_hist[6];          // 历史结果
static int dc_hist_n;           // 历史数量
static void dc_roll(void) { int r = (int)(rng_next() % (uint32_t)dc_side) + 1; dc_result = r; if (dc_hist_n < 6) { dc_hist[dc_hist_n++] = r; } else { for (int k = 0; k < 5; k++) dc_hist[k] = dc_hist[k + 1]; dc_hist[5] = r; } }

// ---------------- 十六进制查看器 ----------------
static uint8_t hex_data[2048];
static int hex_len;
static char hex_name[FS_MAX_NAME];

// ---------------- Converter: 真实转换辅助 (定义在文件后部, 点击分发先使用) ----------------
static uint8_t conv_in[FS_MAX_SIZE];
static uint8_t conv_out[FS_MAX_SIZE * 3];
static int conv_txt2md(const uint8_t* in, int n, uint8_t* out, int max);
static int conv_md2html(const uint8_t* in, int n, uint8_t* out, int max);
static int conv_upper(const uint8_t* in, int n, uint8_t* out, int max);
static int conv_hex(const uint8_t* in, int n, uint8_t* out, int max);

// ---------------- Store 应用列表 ----------------
struct store_app { const char* name; const char* desc; int type; const char* marker; };
static const struct store_app store_apps[12] = {
    {"Snake",   "Classic snake game",       WIN_SNAKE,   "/apps/snake"},
    {"2048",    "Slide and merge tiles",    WIN_2048,    "/apps/2048"},
    {"Mines",   "Minesweeper puzzle",       WIN_MINES,   "/apps/mines"},
    {"Brick",   "Breakout arcade",          WIN_BRICK,   "/apps/brick"},
    {"Paint",   "Draw with mouse",          WIN_PAINT,   "/apps/paint"},
    {"Notes",   "Simple text notes",        WIN_NOTE,    "/apps/notes"},
    {"UnitConv","Length/weight/temp units", WIN_UNIT,    "/apps/unit"},
    {"Stopwatch","Timer & clock",           WIN_STOPW,   "/apps/stopwatch"},
    {"HexView", "Hex dump of files",        WIN_HEXVIEW, "/apps/hexview"},
    {"Todo",    "Checklist & tasks",        WIN_TODO,    "/apps/todo"},
    {"PassGen", "Random password generator",WIN_PASSGEN, "/apps/passgen"},
    {"Clock",   "Clock & stopwatch",        WIN_CLOCK,   "/apps/clock"},
};

// ---------------- IDE 微语言解释器 ----------------
#define IDE_VARS 16
static char ide_vnames[IDE_VARS][16];
static int  ide_vvals[IDE_VARS];
static int  ide_vn;
static char ide_out[600];
static int  ide_outn;
static int  ide_out_scroll;
static void ide_out_add(const char* s) {
    while (*s && ide_outn < (int)sizeof(ide_out) - 2) ide_out[ide_outn++] = *s++;
    ide_out[ide_outn] = 0;
}
static void ide_out_add_char(char c) {
    if (ide_outn < (int)sizeof(ide_out) - 2) ide_out[ide_outn++] = c;
    ide_out[ide_outn] = 0;
}
static void ide_out_num(int v) {
    char tmp[16]; itoa(v, tmp, 10);
    ide_out_add(tmp);
}
static int ide_var_find(const char* n) {
    for (int i = 0; i < ide_vn; i++) if (strcmp(ide_vnames[i], n) == 0) return i;
    return -1;
}
static int ide_var_set(const char* n, int v) {
    int i = ide_var_find(n);
    if (i < 0) {
        if (ide_vn >= IDE_VARS) return -1;
        strncpy(ide_vnames[ide_vn], n, 15);
        ide_vnames[ide_vn][15] = 0;
        ide_vvals[ide_vn] = v;
        ide_vn++;
    } else ide_vvals[i] = v;
    return 0;
}
// 表达式求值: 递归下降, 支持 + - * / % ( ) 数字 变量
static const char* ide_expr_p;
static int ide_expr_error;
static int ide_expr_addsub(void);
static int ide_expr_primary(void) {
    while (*ide_expr_p == ' ') ide_expr_p++;
    if (*ide_expr_p == '(') {
        ide_expr_p++;
        int v = ide_expr_addsub();
        while (*ide_expr_p == ' ') ide_expr_p++;
        if (*ide_expr_p == ')') ide_expr_p++;
        return v;
    }
    if (*ide_expr_p == '-') { ide_expr_p++; return -ide_expr_primary(); }
    if (*ide_expr_p >= '0' && *ide_expr_p <= '9') {
        int v = 0;
        while (*ide_expr_p >= '0' && *ide_expr_p <= '9') v = v * 10 + (*ide_expr_p++ - '0');
        return v;
    }
    // 变量名
    if ((*ide_expr_p >= 'a' && *ide_expr_p <= 'z') || (*ide_expr_p >= 'A' && *ide_expr_p <= 'Z')) {
        char nm[16]; int k = 0;
        while ((*ide_expr_p >= 'a' && *ide_expr_p <= 'z') || (*ide_expr_p >= 'A' && *ide_expr_p <= 'Z') ||
               (*ide_expr_p >= '0' && *ide_expr_p <= '9') || *ide_expr_p == '_') {
            if (k < 15) nm[k++] = *ide_expr_p;
            ide_expr_p++;
        }
        nm[k] = 0;
        int i = ide_var_find(nm);
        if (i >= 0) return ide_vvals[i];
        ide_expr_error = 1;
        return 0;
    }
    ide_expr_error = 1;
    return 0;
}
static int ide_expr_muldiv(void) {
    int v = ide_expr_primary();
    for (;;) {
        while (*ide_expr_p == ' ') ide_expr_p++;
        char op = *ide_expr_p;
        if (op == '*' || op == '/' || op == '%') {
            ide_expr_p++;
            int r = ide_expr_primary();
            if (op == '*') v = v * r;
            else if (op == '/') v = (r == 0) ? 0 : v / r;
            else v = (r == 0) ? 0 : v % r;
        } else break;
    }
    return v;
}
static int ide_expr_addsub(void) {
    int v = ide_expr_muldiv();
    for (;;) {
        while (*ide_expr_p == ' ') ide_expr_p++;
        char op = *ide_expr_p;
        if (op == '+' || op == '-') {
            ide_expr_p++;
            int r = ide_expr_muldiv();
            if (op == '+') v += r; else v -= r;
        } else break;
    }
    return v;
}
static void ide_run(const char* code) {
    ide_outn = 0; ide_out[0] = 0; ide_out_scroll = 0;
    ide_vn = 0;
    int if_stack[8]; int if_top = -1;
    int skip_cnt = 0;
    const char* ln = code;
    while (*ln) {
        const char* eol = ln;
        while (*eol && *eol != '\n') eol++;
        char line[128]; int k = 0;
        const char* p = ln;
        while (p < eol && k < 127) line[k++] = *p++;
        line[k] = 0;
        // 去除首尾空白
        int a = 0;
        while (line[a] == ' ' || line[a] == '\t') a++;
        int b = k - 1;
        while (b >= a && (line[b] == ' ' || line[b] == '\t' || line[b] == '\r')) b--;
        line[b + 1] = 0;
        const char* st = line + a;
        if (st[0]) {
            if (strncmp(st, "if", 2) == 0 && (st[2] == ' ' || st[2] == 0)) {
                if (if_top < 7) {
                    if (skip_cnt > 0) {
                        if_stack[++if_top] = 0;
                        skip_cnt++;
                    } else {
                        ide_expr_p = st + 2; ide_expr_error = 0;
                        int cond = ide_expr_addsub();
                        int take = (cond != 0 && !ide_expr_error);
                        if_stack[++if_top] = take;
                        if (!take) skip_cnt = 1;
                    }
                }
            } else if (strcmp(st, "endif") == 0) {
                if (if_top >= 0) {
                    int was = if_stack[if_top--];
                    if (was == 0 && skip_cnt > 0) skip_cnt--;
                }
            } else if (skip_cnt == 0) {
                if (strncmp(st, "print", 5) == 0 && (st[5] == ' ' || st[5] == 0)) {
                    const char* arg = st + 5;
                    while (*arg == ' ') arg++;
                    if (*arg == '"') {
                        const char* q = arg + 1;
                        while (*q && *q != '"') {
                            ide_out_add_char(*q);
                            q++;
                        }
                        if (*q == '"') q++;
                        while (*q) { if (*q == ' ' || *q == '\t') q++; else break; }
                        if (*q) { ide_out_add(" "); ide_expr_p = q; ide_expr_error = 0; ide_out_num(ide_expr_addsub()); }
                    } else if (*arg) {
                        ide_expr_p = arg; ide_expr_error = 0;
                        int v = ide_expr_addsub();
                        if (ide_expr_error) ide_out_add("?expr");
                        else ide_out_num(v);
                    } else {
                        ide_out_add("\n");
                    }
                } else if (strcmp(st, "help") == 0) {
                    ide_out_add("cmds: print, x=expr, if/endif\n");
                } else {
                    // 赋值: name = expr
                    const char* eq = strchr(st, '=');
                    if (eq) {
                        char nm[16]; int nk = 0;
                        const char* q = st;
                        while (q < eq && nk < 15 && *q != ' ') nm[nk++] = *q++;
                        nm[nk] = 0;
                        ide_expr_p = eq + 1; ide_expr_error = 0;
                        int v = ide_expr_addsub();
                        if (!ide_expr_error) ide_var_set(nm, v);
                    }
                }
            }
        }
        if (*eol == 0) break;
        ln = eol + 1;
    }
    ide_out[ide_outn] = 0;
}

// ---------------- Translate 词条表 (>=20) ----------------
static const char* tr_words[24][2] = {
    {"hello", "你好"}, {"world", "世界"}, {"epoch", "纪元"}, {"os", "操作系统"},
    {"file", "文件"}, {"window", "窗口"}, {"terminal", "终端"}, {"mouse", "鼠标"},
    {"keyboard", "键盘"}, {"desktop", "桌面"}, {"game", "游戏"}, {"tool", "工具"},
    {"help", "帮助"}, {"settings", "设置"}, {"browser", "浏览器"}, {"store", "商店"},
    {"time", "时间"}, {"memory", "内存"}, {"disk", "磁盘"}, {"network", "网络"},
    {"calculator", "计算器"}, {"system", "系统"}, {"snake", "蛇"}, {"paint", "画图"}
};

// 前向声明
static int add_window(int type, const char* title, int x, int y, int w, int h);

// ---------------- 基础绘制辅助 ----------------

static int win_top(int i) { return wins[i].y - TITLE_H; }

static void gui_activate(int i) {
    if (active_win >= 0 && active_win < win_count) wins[active_win].dirty = 1;
    active_win = i;   // z 序: 渲染时活动窗口最后绘制 = 置顶
    if (i >= 0 && i < win_count) wins[i].dirty = 1;
    // 可观测性: 窗口激活事件上串口, 供自动化自测校验 (防伪造)
    serial_printf(COM1, "[g] activate win=%d type=%d\n", i,
                  (i >= 0 && i < win_count) ? wins[i].type : -1);
}

// ---------------- 桌面图标与开始菜单 (Win11 风格) ----------------

// 桌面图标布局: 图标 36x36, 文字在下, 四列 (避开顶部状态栏)
#define DESK_X0    12
#define DESK_Y0    (STATUS_BAR_H + 12)
#define DESK_COL_W 94
#define DESK_ROW_H 70
static void desk_icon_rect(int idx, int* x, int* y, int* w, int* h) {
    int col = idx % 4;
    int row = idx / 4;
    *x = DESK_X0 + col * DESK_COL_W;
    *y = DESK_Y0 + row * DESK_ROW_H;
    *w = 90;
    *h = 62;
}

// 绘制带 alpha 的 RGBA 图标到当前绘制目标
static void gfx_draw_rgba_icon(int x, int y, int w, int h,
                               const uint8_t* rgb, const uint8_t* a) {
    gfx_mark_dirty(x, y, w, h);
    uint32_t* dst = gfx_draw_target();
    int pitch = gfx_pitch();
    int sw = gfx_width(), sh = gfx_height();
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= sh) continue;
        uint32_t* line = (uint32_t*)((uint8_t*)dst + (uint32_t)yy * pitch);
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= sw) continue;
            int al = a[j * w + i];
            if (al == 0) continue;
            uint32_t c = ((uint32_t)rgb[(j*w+i)*3] << 16) |
                         ((uint32_t)rgb[(j*w+i)*3+1] << 8) |
                         rgb[(j*w+i)*3+2];
            if (al >= 250) {
                line[xx] = c;
            } else {
                uint32_t d = line[xx];
                int r = (int)((c >> 16) & 255) * al + (int)((d >> 16) & 255) * (255 - al);
                int g = (int)((c >> 8) & 255) * al + (int)((d >> 8) & 255) * (255 - al);
                int b = (int)(c & 255) * al + (int)(d & 255) * (255 - al);
                line[xx] = ((r / 255) << 16) | ((g / 255) << 8) | (b / 255);
            }
        }
    }
}

// 按最近邻采样将 src_w x src_h 的 RGBA 图标缩放到 dst_w x dst_h 后绘制
static void gfx_draw_rgba_icon_scaled(int x, int y, int dst_w, int dst_h,
                                      int src_w, int src_h,
                                      const uint8_t* rgb, const uint8_t* a) {
    gfx_mark_dirty(x, y, dst_w, dst_h);
    uint32_t* dst = gfx_draw_target();
    int pitch = gfx_pitch();
    int sw = gfx_width(), sh = gfx_height();
    for (int j = 0; j < dst_h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= sh) continue;
        int sy = (j * src_h) / dst_h;
        if (sy >= src_h) sy = src_h - 1;
        uint32_t* line = (uint32_t*)((uint8_t*)dst + (uint32_t)yy * pitch);
        for (int i = 0; i < dst_w; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= sw) continue;
            int sx = (i * src_w) / dst_w;
            if (sx >= src_w) sx = src_w - 1;
            int al = a[sy * src_w + sx];
            if (al == 0) continue;
            uint32_t c = ((uint32_t)rgb[(sy*src_w+sx)*3] << 16) |
                         ((uint32_t)rgb[(sy*src_w+sx)*3+1] << 8) |
                         rgb[(sy*src_w+sx)*3+2];
            if (al >= 250) {
                line[xx] = c;
            } else {
                uint32_t d = line[xx];
                int r = (int)((c >> 16) & 255) * al + (int)((d >> 16) & 255) * (255 - al);
                int g = (int)((c >> 8) & 255) * al + (int)((d >> 8) & 255) * (255 - al);
                int b = (int)(c & 255) * al + (int)(d & 255) * (255 - al);
                line[xx] = ((r / 255) << 16) | ((g / 255) << 8) | (b / 255);
            }
        }
    }
}

// 无位图图标的代码绘制兜底 (翻译/压缩/任务管理器/终端/磁盘)
static void draw_desktop_icon_art(int idx, int cx, int cy) {
    uint32_t col = 0x7FA8E8;
    switch (desk_types[idx]) {
    case WIN_TERMINAL: col = 0x10A050; break;
    case WIN_TRANSLATE: col = 0x2E9EC8; break;
    case WIN_ZIP:       col = 0xD08A2E; break;
    case WIN_TASKMGR:   col = 0x5A6AC0; break;
    case WIN_DISK:      col = 0x4A6A8A; break;
    case WIN_SNAKE:     col = 0x2E9E4F; break;
    case WIN_2048:      col = 0xD08A2E; break;
    case WIN_MINES:     col = 0x6E7A8A; break;
    case WIN_BRICK:     col = 0xC0504D; break;
    case WIN_PAINT:     col = 0x8A5AC0; break;
    case WIN_NOTE:      col = 0xD0A02E; break;
    case WIN_UNIT:      col = 0x20A0A0; break;
    case WIN_STOPW:     col = 0x20B0A0; break;
    case WIN_CLOCK:     col = 0x3576C8; break;
    case WIN_HEXVIEW:   col = 0x2E5A8A; break;
    default:            col = 0x7FA8E8; break;
    }
    gfx_fill_round_rect(cx - 14, cy - 14, 28, 28, 9, COL_WHITE);
    gfx_fill_alpha(cx - 14, cy - 14, 28, 28, col, 34);
    switch (desk_types[idx]) {
    case WIN_TERMINAL:
        gfx_fill_round_rect(cx - 10, cy - 7, 20, 14, 3, 0x101418);
        gfx_draw_text(cx - 7, cy - 2, ">_", 0x3CF06A, 0x101418);
        break;
    case WIN_TRANSLATE:
        gfx_fill_round_rect(cx - 9, cy - 6, 18, 12, 3, col);
        gfx_draw_text(cx - 6, cy - 2, "A", COL_WHITE, col);
        gfx_draw_text(cx + 2, cy + 2, "\xe8", COL_WHITE, col);
        break;
    case WIN_ZIP:
        gfx_fill_round_rect(cx - 8, cy - 9, 16, 18, 3, col);
        gfx_fill_round_rect(cx - 6, cy - 11, 12, 5, 2, 0xB07020);
        gfx_draw_text(cx - 4, cy - 3, "Z", COL_WHITE, col);
        break;
    case WIN_TASKMGR:
        for (int k = 0; k < 4; k++) {
            int bh = 4 + k * 3;
            gfx_fill_round_rect(cx - 10 + k * 6, cy + 6 - bh, 4, bh, 2, col);
        }
        break;
    case WIN_DISK:
        gfx_fill_round_rect(cx - 10, cy - 5, 20, 10, 3, col);
        gfx_fill_rect(cx - 10, cy + 1, 20, 3, 0x2E4A66);
        gfx_fill_round_rect(cx + 4, cy - 3, 3, 3, 1, COL_WHITE);
        break;
    case WIN_SNAKE:
        gfx_fill_round_rect(cx - 9, cy - 9, 18, 18, 4, 0xEAF6EE);
        gfx_fill_round_rect(cx - 6, cy - 6, 10, 10, 3, col);
        gfx_fill_round_rect(cx + 2, cy - 2, 5, 5, 2, col);
        gfx_fill_round_rect(cx - 8, cy + 5, 3, 3, 1, 0x202020);
        gfx_fill_round_rect(cx + 1, cy + 5, 3, 3, 1, 0x202020);
        break;
    case WIN_2048:
        gfx_fill_round_rect(cx - 9, cy - 9, 18, 18, 4, 0xFBF0E2);
        gfx_fill_round_rect(cx - 6, cy - 6, 12, 12, 3, col);
        gfx_draw_text(cx - 3, cy - 3, "2", COL_WHITE, col);
        break;
    case WIN_MINES:
        gfx_fill_round_rect(cx - 9, cy - 9, 18, 18, 4, 0xE8ECF2);
        for (int k = 0; k < 3; k++)
            gfx_fill_round_rect(cx - 7 + k * 7, cy - 7 + k * 7, 5, 5, 1, col);
        break;
    case WIN_BRICK:
        gfx_fill_round_rect(cx - 9, cy - 9, 18, 18, 4, 0xF7E9E8);
        gfx_fill_round_rect(cx - 8, cy - 6, 16, 5, 2, col);
        gfx_fill_round_rect(cx - 8, cy + 1, 16, 5, 2, col);
        gfx_fill_round_rect(cx - 8, cy + 8, 16, 3, 2, 0xB07020);
        break;
    case WIN_PAINT:
        gfx_fill_round_rect(cx - 9, cy - 9, 18, 18, 4, 0xF0EAF8);
        gfx_fill_round_rect(cx - 7, cy - 5, 12, 10, 3, col);
        gfx_fill_round_rect(cx - 5, cy - 3, 3, 3, 1, 0xE85A5A);
        gfx_fill_round_rect(cx - 1, cy - 3, 3, 3, 1, 0x5AA85A);
        gfx_fill_round_rect(cx + 3, cy - 3, 3, 3, 1, 0x5A5AE8);
        break;
    case WIN_NOTE:
        gfx_fill_round_rect(cx - 8, cy - 10, 16, 20, 3, 0xFFF8E0);
        gfx_fill_round_rect(cx - 6, cy - 6, 12, 2, 1, 0xB0A080);
        gfx_fill_round_rect(cx - 6, cy - 2, 12, 2, 1, 0xB0A080);
        gfx_fill_round_rect(cx - 6, cy + 2, 12, 2, 1, 0xB0A080);
        gfx_fill_round_rect(cx - 6, cy + 6, 7, 2, 1, 0xD0A02E);
        break;
    case WIN_UNIT:
        gfx_fill_round_rect(cx - 9, cy - 9, 18, 18, 4, 0xE6F6F6);
        gfx_draw_text(cx - 8, cy - 6, "=", 0x20A0A0, 0xE6F6F6);
        gfx_fill_round_rect(cx - 6, cy - 1, 12, 8, 2, col);
        gfx_draw_text(cx - 4, cy + 1, "cm", COL_WHITE, col);
        break;
    case WIN_STOPW:
        gfx_fill_round_rect(cx - 9, cy - 9, 18, 18, 9, 0xE8F8F4);
        gfx_fill_round_rect(cx - 6, cy - 6, 12, 12, 6, col);
        gfx_fill_round_rect(cx - 1, cy - 4, 2, 6, 1, COL_WHITE);
        gfx_fill_round_rect(cx - 4, cy - 3, 2, 2, 1, 0xE85A5A);
        break;
    case WIN_HEXVIEW:
        gfx_fill_round_rect(cx - 9, cy - 9, 18, 18, 4, 0xE4ECF4);
        for (int k = 0; k < 4; k++) {
            gfx_fill_round_rect(cx - 7 + (k % 2) * 8, cy - 7 + (k / 2) * 8, 6, 6, 1,
                                (k % 2) ? 0x4A7AA8 : col);
        }
        break;
    case WIN_TETRIS:
        gfx_fill_round_rect(cx - 9, cy - 9, 18, 18, 4, 0xEAF2FA);
        gfx_fill_round_rect(cx - 7, cy - 8, 6, 6, 1, col);
        gfx_fill_round_rect(cx - 1, cy - 8, 6, 6, 1, col);
        gfx_fill_round_rect(cx - 1, cy - 2, 6, 6, 1, col);
        gfx_fill_round_rect(cx + 5, cy - 2, 6, 6, 1, 0x2E9E4F);
        break;
    case WIN_TICTAC:
        gfx_fill_round_rect(cx - 9, cy - 9, 18, 18, 4, 0xF0ECFA);
        gfx_draw_round_rect(cx - 6, cy - 6, 5, 5, 1, col);
        gfx_draw_text(cx - 5, cy - 5, "x", col, 0xF0ECFA);
        gfx_draw_round_rect(cx + 1, cy + 1, 5, 5, 1, 0x2E9E4F);
        gfx_draw_text(cx + 2, cy + 2, "o", 0x2E9E4F, 0xF0ECFA);
        break;
    case WIN_MEMORY:
        gfx_fill_round_rect(cx - 9, cy - 9, 18, 18, 4, 0xEAF6EE);
        gfx_fill_round_rect(cx - 7, cy - 7, 5, 5, 1, col);
        gfx_fill_round_rect(cx - 1, cy - 7, 5, 5, 1, 0x2E9EC8);
        gfx_fill_round_rect(cx - 7, cy - 1, 5, 5, 1, 0xE8A020);
        gfx_fill_round_rect(cx - 1, cy - 1, 5, 5, 1, 0x5A6AC0);
        break;
    case WIN_FIND:
        gfx_fill_round_rect(cx - 9, cy - 9, 18, 18, 9, 0xEAF2FA);
        gfx_fill_round_rect(cx - 5, cy - 5, 7, 7, 4, COL_WHITE);
        gfx_draw_round_rect(cx - 5, cy - 5, 7, 7, 4, col);
        gfx_fill_round_rect(cx + 1, cy + 1, 4, 4, 1, col);
        break;
    case WIN_BASECONV:
        gfx_fill_round_rect(cx - 9, cy - 9, 18, 18, 4, 0xE4F0F8);
        gfx_draw_text(cx - 8, cy - 6, "D", 0x4A7AA8, 0xE4F0F8);
        gfx_draw_text(cx + 1, cy - 6, "H", col, 0xE4F0F8);
        gfx_draw_text(cx - 8, cy + 3, "B", 0x2E9E4F, 0xE4F0F8);
        gfx_draw_text(cx + 1, cy + 3, "O", 0xC0504D, 0xE4F0F8);
        break;
    case WIN_RANDOM:
        gfx_fill_round_rect(cx - 9, cy - 9, 18, 18, 9, 0xF6F0E8);
        gfx_draw_text(cx - 8, cy - 5, "?", 0x4A7AA8, 0xF6F0E8);
        gfx_draw_text(cx + 1, cy - 5, "?", col, 0xF6F0E8);
        gfx_draw_text(cx - 8, cy + 4, "?", 0x2E9E4F, 0xF6F0E8);
        gfx_draw_text(cx + 1, cy + 4, "?", 0xC0504D, 0xF6F0E8);
        break;
    default:
        gfx_fill_round_rect(cx - 9, cy - 7, 18, 14, 3, col);
        gfx_draw_text(cx - 4, cy - 2, "^^", 0x506070, col);
        break;
    }
}

// ---- profiling (PIT 0x40 计数器, 1 tick=838ns) ----
static inline uint32_t pit_now(void) {
    outb(0x43, 0x00);
    uint8_t lo = inb(0x40);
    uint8_t hi = inb(0x40);
    return (uint32_t)((hi << 8) | lo);
}
static volatile uint32_t prof_desktop=0, prof_wins=0, prof_task=0, prof_status=0, prof_menu=0, prof_cur=0, prof_swap=0, prof_cnt=0;

static void render_desktop_icons(uint64_t mask) {
    for (int i = 0; i < DESKTOP_ICONS; i++) {
        if (!(mask & (1ULL << i))) continue;
        int x, y, w, h;
        desk_icon_rect(i, &x, &y, &w, &h);
        int sel = (i == desk_sel) || (i == desk_hover);
        int cx = x + w / 2;
        int cy = y + 28;
        if (desk_icons[i]) {
            // 图标本体 (蓝渐变圆角, 直接绘制无白色底块)
            gfx_draw_rgba_icon(cx - 18, cy - 18, ICON_SIZE, ICON_SIZE, desk_icons[i]->rgb, desk_icons[i]->alpha);
            if (sel) {
                // 选中态: 毛玻璃圆角底 + 淡蓝描边
                gfx_fill_glass_round_rect(cx - 21, cy - 21, ICON_SIZE + 6, ICON_SIZE + 6, 12, 1, 70);
                gfx_draw_round_rect(cx - 21, cy - 21, ICON_SIZE + 6, ICON_SIZE + 6, 12, 0xA0C0F0);
            }
        } else {
            draw_desktop_icon_art(i, cx, cy);
        }
        // 文字: 毛玻璃圆角胶囊 (白底半透明, 深灰字)
        int tw = (int)strlen(desk_names[i]) * 8;
        int tx = x + (w - tw) / 2;
        if (tx < x) tx = x;
        uint32_t tfg = sel ? 0x1E4E8C : 0x2A3038;
        gfx_fill_round_rect(tx - 3, y + 40, tw + 6, 17, 8, 0xFFFFFF);
        gfx_fill_alpha(tx - 3, y + 40, tw + 6, 17, 0xFFFFFF, 170);
        gfx_draw_text(tx, y + 41, desk_names[i], tfg, 0xFFFFFF);
    }
}

// 开始菜单: 顶部用户栏 + 程序列表 (Win11 网格)


// 开始菜单项

// 打开指定类型的窗口 (若已隐藏则显示并激活)
static void open_window_by_type(int type) {
    // 商店应用未安装时: 打开商店并提示
    if (store_marker_for_type(type)) {
        if (!app_is_installed(type)) {
            for (int i = 0; i < win_count; i++) {
                if (wins[i].used && wins[i].type == WIN_STORE) {
                    wins[i].visible = 1;
                    desk_blit_rect(wins[i].x, win_top(i), wins[i].w, TITLE_H + wins[i].h);
                    wins[i].dirty = 1;
                    gui_activate(i);
                    strcpy(wins[i].app_buf, "Not installed. Press Install first.");
                    return;
                }
            }
            add_window(WIN_STORE, "App Store", 90, 40, 560, 456);
            gui_activate(win_count - 1);
            strcpy(wins[win_count - 1].app_buf, "Not installed. Press Install first.");
            return;
        }
    }
    for (int i = 0; i < win_count; i++) {
        if (wins[i].used && wins[i].type == type) {
            wins[i].visible = 1;
            if (!wins[i].dirty)
                desk_blit_rect(wins[i].x, win_top(i), wins[i].w, TITLE_H + wins[i].h);
            wins[i].dirty = 1;
            gui_activate(i);
            return;
        }
    }
    // 未创建则创建 (一般不会走到)
    switch (type) {
    case WIN_TERMINAL: add_window(WIN_TERMINAL, "Terminal", 240, 30, 520, 360); break;
    case WIN_FILES:    add_window(WIN_FILES, "Files", 60, 60, 320, 380); break;
    case WIN_ABOUT:    add_window(WIN_ABOUT, "About", 120, 80, 420, 280); break;
    case WIN_HELP:     add_window(WIN_HELP, "Help", 120, 90, 340, 220); break;
    case WIN_SETTINGS: add_window(WIN_SETTINGS, "Settings", 120, 90, 420, 300); break;
    case WIN_MANAGER:  add_window(WIN_MANAGER, "System Manager", 100, 60, 460, 340); break;
    case WIN_BROWSER:  add_window(WIN_BROWSER, "Browser", 80, 40, 560, 400); break;
    case WIN_TRANSLATE:add_window(WIN_TRANSLATE, "Translate", 120, 70, 440, 320); break;
    case WIN_CONVERT:  add_window(WIN_CONVERT, "Converter", 120, 70, 440, 320); break;
    case WIN_SHOT:     add_window(WIN_SHOT, "Screenshot", 140, 90, 400, 260); break;
    case WIN_PHOTO:    add_window(WIN_PHOTO, "Photo", 90, 50, 520, 380); break;
    case WIN_VIDEO:    add_window(WIN_VIDEO, "Video", 90, 50, 560, 380); break;
    case WIN_PDF:      add_window(WIN_PDF, "PDF Reader", 100, 50, 500, 400); break;
    case WIN_MUSIC:    add_window(WIN_MUSIC, "Music", 120, 70, 440, 320); break;
    case WIN_CALENDAR: add_window(WIN_CALENDAR, "Calendar", 120, 60, 460, 360); break;
    case WIN_EDITOR:   add_window(WIN_EDITOR, "Text Editor", 110, 60, 480, 360); break;
    case WIN_ALARM:    add_window(WIN_ALARM, "Alarm", 150, 90, 380, 260); break;
    case WIN_CALC:     add_window(WIN_CALC, "Calculator", 160, 90, 340, 340); break;
    case WIN_STORE:    add_window(WIN_STORE, "App Store", 90, 40, 560, 456); break;
    case WIN_ZIP:      add_window(WIN_ZIP, "Zip", 130, 70, 420, 300); break;
    case WIN_IDE:      add_window(WIN_IDE, "IDE", 80, 30, 600, 420); break;
    case WIN_TASKMGR:  add_window(WIN_TASKMGR, "Task Manager", 140, 60, 440, 360); break;
    case WIN_DISK:     add_window(WIN_DISK, "Disk Manager", 120, 60, 480, 360); break;
    case WIN_NET:      add_window(WIN_NET, "Network Manager", 120, 60, 480, 360); break;
    case WIN_SNAKE:    add_window(WIN_SNAKE, "Snake", 160, 60, 420, 420); break;
    case WIN_2048:     add_window(WIN_2048, "2048", 170, 70, 400, 420); break;
    case WIN_MINES:    add_window(WIN_MINES, "Minesweeper", 120, 40, 560, 480); break;
    case WIN_BRICK:    add_window(WIN_BRICK, "Brick Breaker", 100, 40, 580, 400); break;
    case WIN_PAINT:    add_window(WIN_PAINT, "Paint", 120, 50, 560, 420); break;
    case WIN_NOTE:     add_window(WIN_NOTE, "Notes", 140, 60, 460, 360); break;
    case WIN_UNIT:     add_window(WIN_UNIT, "Unit Converter", 150, 80, 440, 380); break;
    case WIN_STOPW:    add_window(WIN_STOPW, "Stopwatch", 200, 100, 360, 320); break;
    case WIN_HEXVIEW:  add_window(WIN_HEXVIEW, "Hex Viewer", 120, 50, 560, 420); break;
    case WIN_TETRIS:   add_window(WIN_TETRIS, "Tetris", 200, 60, 300, 440); break;
    case WIN_TICTAC:   add_window(WIN_TICTAC, "TicTacToe", 250, 90, 300, 380); break;
    case WIN_MEMORY:   add_window(WIN_MEMORY, "Memory", 190, 50, 420, 440); break;
    case WIN_FIND:     add_window(WIN_FIND, "File Search", 120, 70, 480, 360); break;
    case WIN_BASECONV: add_window(WIN_BASECONV, "Base Converter", 160, 80, 400, 340); break;
    case WIN_RANDOM:   add_window(WIN_RANDOM, "Random", 190, 90, 360, 300); break;
    case WIN_TEXTSTATS:add_window(WIN_TEXTSTATS, "TextStats", 150, 80, 440, 380); break;
    case WIN_SUDOKU:   add_window(WIN_SUDOKU, "Sudoku", 120, 50, 440, 500); break;
    case WIN_TYPING:   add_window(WIN_TYPING, "Typing Test", 130, 70, 500, 380); break;
    case WIN_POMODORO: add_window(WIN_POMODORO, "Pomodoro", 200, 100, 380, 340); break;
    case WIN_GUESS:    add_window(WIN_GUESS, "Guess Number", 180, 90, 400, 340); break;
    case WIN_DICE:     add_window(WIN_DICE, "Dice Roller", 170, 80, 420, 380); break;
    case WIN_TODO:     add_window(WIN_TODO, "Todo List", 150, 60, 460, 430); wins[win_count - 1].app_buf[0] = 0; break;
    case WIN_PASSGEN:  add_window(WIN_PASSGEN, "Password Generator", 170, 80, 420, 350); wins[win_count - 1].app_buf[0] = 0; break;
    case WIN_CLOCK:    add_window(WIN_CLOCK, "Clock & Stopwatch", 200, 70, 400, 380); wins[win_count - 1].app_buf[0] = 0; break;
    default: break;
    }
    // 修复: 新建窗口必须激活, 否则 active_win=-1 时键盘注入被丢弃
    if (win_count > 0 && wins[win_count - 1].used && wins[win_count - 1].type == type)
        gui_activate(win_count - 1);
}

// 桌面右键菜单动作
static void dmenu_action(int idx) {
    switch (idx) {
    case 0: open_window_by_type(WIN_TERMINAL); break;
    case 1:
        desk_need_full = 1;
        icons_dirty = ~0ULL;
        status_dirty = 1;
        break;
    case 2:
        wallpaper_next();
        desk_need_full = 1;
        break;
    case 3: open_window_by_type(WIN_ABOUT); break;
    case 4: break; // Close
    default: break;
    }
    dmenu_open = 0;
    desk_need_full = 1;
}

static void render_desktop_menu(void) {
    if (!dmenu_open) return;
    int iw = 150, ih = DMENU_ITEMS * 30 + 8;
    int x = dmenu_x, y = dmenu_y;
    // 菜单 hover 更新
    dmenu_sel = -1;
    if (cursor_x >= x && cursor_x < x + iw && cursor_y >= y && cursor_y < y + ih) {
        int idx = (cursor_y - y - 4) / 30;
        if (idx >= 0 && idx < DMENU_ITEMS) dmenu_sel = idx;
    }
    gfx_draw_shadow(x + 2, y + 2, iw, ih, 3);
    gfx_fill_glass_round_rect(x, y, iw, ih, 8, 1, 70);
    gfx_draw_round_rect(x, y, iw, ih, 8, 0x9AA8BC);
    for (int k = 0; k < DMENU_ITEMS; k++) {
        int iy = y + 4 + k * 30;
        uint32_t fg = (k == dmenu_sel) ? 0x103060 : 0x2A3038;
        if (k == dmenu_sel) {
            gfx_fill_round_rect(x + 4, iy, iw - 8, 26, 5, 0xD5E4F7);
            gfx_fill_alpha(x + 4, iy, iw - 8, 26, 0xD5E4F7, 150);
        }
        gfx_draw_text(x + 12, iy + 6, dmenu_labels[k], fg, 0xFFFFFF);
    }
}


// 取窗口顶部 y 坐标 (标题栏)
// ---------------- 文件浏览器 ----------------

static void files_refresh(int i) {
    char buf[2048];
    const char* dir = wins[i].file_dir;
    int n = fs_list_dir(dir, buf, sizeof(buf));
    wins[i].entry_count = 0;
    if (n < 0) return;
    char* p = buf;
    while (*p && wins[i].entry_count < ENTRY_MAX) {
        char* line = p;
        while (*p && *p != '\n') p++;
        int is_last = (*p == 0);
        if (*p == '\n') *p++ = 0;
        if (line[0] == 0) { if (is_last) break; continue; }
        struct file_entry* e = &wins[i].entries[wins[i].entry_count];
        // 解析: "name/" (目录) 或 "name <size>" (文件)
        int len = (int)strlen(line);
        int is_dir = 0;
        if (len > 0 && line[len - 1] == '/') { is_dir = 1; line[len - 1] = 0; }
        // 名字: 找最后一个空格 (目录名可能不含空格)
        int name_end = len;
        if (!is_dir) {
            int sp = -1;
            for (int k = 0; k < len; k++) if (line[k] == ' ') sp = k;
            if (sp > 0) { name_end = sp; e->size = (uint32_t)atoi(line + sp + 1); }
            else e->size = 0;
        } else {
            e->size = 0;
        }
        if (name_end > FS_MAX_NAME - 1) name_end = FS_MAX_NAME - 1;
        for (int k = 0; k < name_end; k++) e->name[k] = line[k];
        e->name[name_end] = 0;
        e->is_dir = is_dir;
        wins[i].entry_count++;
        if (is_last) break;
    }
    if (wins[i].file_sel >= wins[i].entry_count) wins[i].file_sel = 0;
    if (wins[i].file_scroll > 0 && wins[i].file_scroll >= wins[i].entry_count)
        wins[i].file_scroll = 0;
}

static void files_join_path(int i, const char* name, char* out, uint32_t max) {
    const char* d = wins[i].file_dir;
    if (d[0] == '/' && d[1] == 0) {
        // 根目录
        uint32_t o = 0;
        out[o++] = '/';
        const char* s = name;
        while (*s && o < max - 1) out[o++] = *s++;
        out[o] = 0;
    } else {
        uint32_t o = 0;
        const char* s = d;
        while (*s && o < max - 1) out[o++] = *s++;
        if (o > 0 && out[o - 1] != '/') out[o++] = '/';
        s = name;
        while (*s && o < max - 1) out[o++] = *s++;
        out[o] = 0;
    }
}

static void files_open(int i, int idx) {
    if (idx < 0 || idx >= wins[i].entry_count) return;
    struct file_entry* e = &wins[i].entries[idx];
    char full[FS_MAX_NAME];
    files_join_path(i, e->name, full, sizeof(full));
    if (e->is_dir) {
        strncpy(wins[i].file_dir, full, FS_MAX_NAME - 1);
        wins[i].file_dir[FS_MAX_NAME - 1] = 0;
        wins[i].file_sel = 0;
        wins[i].file_scroll = 0;
        files_refresh(i);
    } else {
        // 读文件内容 (前 4 字节用于 ELF 探测)
        uint8_t buf[4096];
        int n = fs_read(full, buf, sizeof(buf) - 1);
        if (n < 0) {
            vga_printf("Error: cannot read %s\n", full);
            return;
        }
        int is_elf = (n >= 4 && buf[0] == 0x7F && buf[1] == 'E' &&
                      buf[2] == 'L' && buf[3] == 'F');
        if (is_elf) {
            // 双击运行 ELF: 打开终端并启动用户进程 (与 shell 'elf' 等价)
            const char* base = full;
            for (const char* p = full; *p; p++) if (*p == '/') base = p + 1;
            char name[TASK_NAME_MAX];
            strncpy(name, base, TASK_NAME_MAX - 1);
            name[TASK_NAME_MAX - 1] = 0;
            const char* uargv[2];
            uargv[0] = name;
            int slot = elf_load_from_fs(full, name, 1, uargv);
            if (slot < 0) {
                vga_printf("Error: cannot run %s (not ELF32 static?)\n", full);
                serial_printf(COM1, "[gui] run failed: %s\r\n", full);
                return;
            }
            // 确保有终端窗口可见输出
            int have_term = 0;
            for (int w = 0; w < WIN_MAX; w++) {
                if (wins[w].used && wins[w].type == WIN_TERMINAL) { have_term = 1; break; }
            }
            if (!have_term) open_window_by_type(WIN_TERMINAL);
            uint32_t pid = task_get(slot) ? task_get(slot)->pid : 0;
            vga_printf("[gui] run %s -> pid %u (slot %d)\n", full, pid, slot);
            serial_printf(COM1, "[gui] run %s slot=%d pid=%u\r\n", full, slot, pid);
            return;
        }
        // 普通文本文件: 内容打印到终端
        buf[n] = 0;
        vga_write_color("==== ", COLOR_LIGHT_CYAN, COLOR_BLACK);
        vga_write_color(full, COLOR_WHITE, COLOR_BLACK);
        vga_write(" (");
        char num[16];
        uitoa((uint32_t)n, num, 10);
        vga_write_color(num, COLOR_WHITE, COLOR_BLACK);
        vga_write_color(" bytes) ====\n", COLOR_LIGHT_CYAN, COLOR_BLACK);
        vga_write((char*)buf);
        vga_newline();
    }
}

static void files_updir(int i) {
    const char* d = wins[i].file_dir;
    if (d[0] == '/' && d[1] == 0) return;    // 已在根
    int last = 0;
    for (int k = 0; d[k]; k++) if (d[k] == '/') last = k;
    char nd[FS_MAX_NAME];
    if (last <= 0) {
        nd[0] = '/'; nd[1] = 0;
    } else {
        for (int k = 0; k < last; k++) nd[k] = d[k];
        nd[last] = 0;
    }
    strncpy(wins[i].file_dir, nd, FS_MAX_NAME - 1);
    wins[i].file_dir[FS_MAX_NAME - 1] = 0;
    wins[i].file_sel = 0;
    wins[i].file_scroll = 0;
    files_refresh(i);
}

// ---------------- 事件处理 ----------------

static void handle_taskbar(int mx) {
    // 任务栏应用图标: 分页 Dock, 点击打开对应应用; 右侧翻页按钮
    int w = gfx_width();
    int page_max = TASKBAR_PER_PAGE;
    int pages = (TASKBAR_APPS + page_max - 1) / page_max;
    int nav_w = (pages > 1) ? 44 : 0;
    int avail = w - 12 - nav_w;
    int count = TASKBAR_APPS - taskbar_page * page_max;
    if (count > page_max) count = page_max;
    int bw = (avail - (count - 1) * 4) / count;
    if (bw > 30) bw = 30;
    if (bw < 16) bw = 16;
    int bx = 6 + (avail - count * bw - (count - 1) * 4) / 2;
    int start = taskbar_page * page_max;
    for (int i = 0; i < count; i++) {
        if (mx >= bx && mx < bx + bw) {
            open_window_by_type(desk_types[start + i + TASKBAR_OFF]);
            return;
        }
        bx += bw + 4;
    }
    // 翻页按钮
    if (pages > 1) {
        int y = gfx_height() - TASKBAR_H;
        int ny = y + (TASKBAR_H - 18) / 2;
        int nx = w - 12 - 40;
        if (mx >= nx && mx < nx + 18) { taskbar_page--; if (taskbar_page < 0) taskbar_page = pages - 1; taskbar_dirty = 1; return; }
        if (mx >= nx + 22 && mx < nx + 40) { taskbar_page++; if (taskbar_page >= pages) taskbar_page = 0; taskbar_dirty = 1; return; }
    }
}

// 桌面图标区域点击 (返回命中的图标索引, -1 未命中)
static int desk_hit(int mx, int my) {
    for (int i = 0; i < DESKTOP_ICONS; i++) {
        int x, y, w, h;
        desk_icon_rect(i, &x, &y, &w, &h);
        if (mx >= x && mx < x + w && my >= y && my < y + h) return i;
    }
    return -1;
}

static void handle_desktop_click(int mx, int my) {
    int idx = desk_hit(mx, my);
    if (desk_sel >= 0) icons_dirty |= (1ULL << desk_sel);
    desk_sel = -1;
    if (idx < 0) return;
    // 双击检测
    uint32_t now = timer_get_ticks();
    if (idx == desk_dbl && now - desk_dbl_tick < 30) {
        desk_dbl = -1;
        desk_sel = -1;
        if (desk_types[idx] >= 0) open_window_by_type(desk_types[idx]);
        return;
    }
    desk_dbl = idx;
    desk_dbl_tick = now;
    desk_sel = idx;   // 单击选中
    if (desk_sel >= 0) icons_dirty |= (1ULL << desk_sel);
}

// 开始菜单点击 (返回是否消费)

// 窗口按钮: 返回 -1 未命中, 0=关闭, 1=最小化, 2=最大化
static int win_title_button_hit(int i, int mx, int my) {
    int wx = wins[i].x;
    int wy = win_top(i);
    int ww = wins[i].w;
    if (my >= wy && my < wy + TITLE_H) {
        int bx = wx + ww - 62;   // 三个按钮: [关闭][最大化][最小化] 从右
        // 关闭: 最右
        if (mx >= wx + ww - 20 && mx < wx + ww - 4) return 0;
        // 最大化
        if (mx >= wx + ww - 40 && mx < wx + ww - 24) return 2;
        // 最小化
        if (mx >= wx + ww - 60 && mx < wx + ww - 44) return 1;
    }
    return -1;
}

static void win_toggle_max(int i) {
    int sw = gfx_width();
    int sh = gfx_height() - TASKBAR_H - STATUS_BAR_H;
    if (wins[i].maximized) {
        wins[i].maximized = 0;
        wins[i].x = wins[i].r_x;
        wins[i].y = wins[i].r_y;
        wins[i].w = wins[i].r_w;
        wins[i].h = wins[i].r_h;
    } else {
        wins[i].maximized = 1;
        wins[i].r_x = wins[i].x;
        wins[i].r_y = wins[i].y;
        wins[i].r_w = wins[i].w;
        wins[i].r_h = wins[i].h;
        wins[i].x = 0;
        wins[i].y = STATUS_BAR_H + TITLE_H;   // 顶栏下方
        wins[i].w = sw;
        wins[i].h = sh - TITLE_H;             // 内容区占满剩余高度
    }
}

static void handle_files_click(int i, int mx, int my) {
    int wc_x = wins[i].x;
    int wc_y = wins[i].y;
    int ww = wins[i].w;
    int wh = wins[i].h;
    // 底部按钮行
    int btn_y = wc_y + wh - 28;
    if (my >= btn_y && my < btn_y + 22) {
        int rel = mx - wc_x - 8;
        // 按钮: [打开] [删除] [保存] [刷新] [↑]
        const int bw = 44, gap = 4;
        int btn = -1;
        for (int k = 0; k < 5; k++) {
            int b0 = k * (bw + gap);
            if (rel >= b0 && rel < b0 + bw) { btn = k; break; }
        }
        if (btn == 0) {          // 打开
            if (wins[i].file_sel >= 0 && wins[i].file_sel < wins[i].entry_count)
                files_open(i, wins[i].file_sel);
        } else if (btn == 1) {   // 删除
            if (wins[i].file_sel >= 0 && wins[i].file_sel < wins[i].entry_count) {
                struct file_entry* e = &wins[i].entries[wins[i].file_sel];
                char full[FS_MAX_NAME];
                files_join_path(i, e->name, full, sizeof(full));
                if (e->is_dir) fs_rmdir(full);
                else fs_delete(full);
                vga_printf("[gui] removed %s\n", full);
                files_refresh(i);
            }
        } else if (btn == 2) {   // 保存
            fs_disk_save();
            vga_write_color("[gui] files saved to disk\n", COLOR_LIGHT_GREEN, COLOR_BLACK);
        } else if (btn == 3) {   // 刷新
            files_refresh(i);
        } else if (btn == 4) {   // 上级
            files_updir(i);
        }
        return;
    }
    // 列表区
    int list_y0 = wc_y + 22;
    int row_h = 18;
    int n = wins[i].entry_count;
    int max_rows = (btn_y - 4 - list_y0) / row_h;
    if (max_rows < 1) max_rows = 1;
    if (my >= list_y0 && my < list_y0 + max_rows * row_h) {
        int row = (my - list_y0) / row_h;
        int idx = wins[i].file_scroll + row;
        if (idx < n) {
            uint32_t now = timer_get_ticks();
            if (idx == wins[i].file_sel &&
                mx >= dbl_x - 3 && mx <= dbl_x + 3 &&
                my >= dbl_y - 3 && my <= dbl_y + 3 &&
                now - dbl_tick < 35) {
                // 双击: 打开
                wins[i].file_sel = idx;
                files_open(i, idx);
            } else {
                wins[i].file_sel = idx;
                dbl_x = mx; dbl_y = my; dbl_tick = now;
            }
        }
    }
}

// 垃圾清理: 删除 /tmp 下所有文件, 结果写入 settings_info
static void do_clean_garbage(void) {
    char buf[1024];
    fs_list_dir("/tmp", buf, sizeof(buf));
    int cleaned = 0;
    uint32_t freed = 0;
    char* p = buf;
    while (*p && cleaned < FS_MAX_FILES) {
        // 解析一行: "name size\n" 或 "name/\n"
        char name[FS_MAX_NAME];
        int k = 0;
        while (*p && *p != '\n' && *p != ' ' && *p != '/') {
            if (k < FS_MAX_NAME - 1) name[k++] = *p;
            p++;
        }
        name[k] = 0;
        uint32_t sz = 0;
        if (*p == ' ') {
            p++;
            while (*p >= '0' && *p <= '9') { sz = sz * 10 + (*p - '0'); p++; }
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        if (name[0]) {
            char full[FS_MAX_NAME + 8];
            strcpy(full, "/tmp/");
            strcat(full, name);
            if (fs_delete(full) == 0) { cleaned++; freed += sz; }
        }
    }
    char num[16];
    if (cleaned > 0) {
        strcpy(settings_info, "Cleaned ");
        uitoa(cleaned, num, 10);
        strcat(settings_info, num);
        strcat(settings_info, " file(s), ");
        uitoa(freed, num, 10);
        strcat(settings_info, num);
        strcat(settings_info, " B freed");
    } else {
        strcpy(settings_info, "No garbage found");
    }
    settings_state = 0;
}

// 一键卸载: 清空文件系统并保存到磁盘
static void do_uninstall(void) {
    fs_format();
    fs_mark_dirty();
    fs_disk_save();
    strcpy(settings_info, "EpochOS uninstalled. Close QEMU, delete epochos.img");
    settings_state = 2;
}

// ============ 新增功能窗口: 状态变量 (前置, 供 click/key/render 使用) ============
// 随机数生成器
static char rng_min[8], rng_max[8], rng_out[16];
// 随机密码生成器 (Password Generator)
static const int pgen_lens[4] = {8, 12, 16, 20};
static int pgen_len_i = 2;                 // index into pgen_lens (default 16)
static int pgen_opt[4] = {1, 1, 1, 1};     // [upper, lower, digit, symbol]
static char pgen_out[24];                  // generated password
static char pgen_clip[24];                 // pseudo clipboard (Copy)
static char pgen_msg[48];                  // status message
// 时钟/秒表 (Clock & Stopwatch): 秒表以 TIMER_HZ 为基准累计
static int clk_sw_run = 0;          // 1=计时中
static uint32_t clk_sw_acc = 0;     // 已累计 tick
static uint32_t clk_sw_start = 0;   // 本次启动时的 tick
static uint32_t clk_sw_elapsed(void) {
    uint32_t el = clk_sw_acc;
    if (clk_sw_run) el += timer_get_ticks() - clk_sw_start;
    return el;
}
static void clk_sw_reset(void) { clk_sw_run = 0; clk_sw_acc = 0; clk_sw_start = 0; }
static int rng_focus = 0;   // 0=Min, 1=Max
// 进制转换器
static char bc_input[20];
static int bc_mode = 0;     // 0=Dec->Bin/Oct/Hex, 1=Hex->Dec
static char bc_bin[40], bc_oct[16], bc_hex[16], bc_dec[16];
// 井字棋
static char tictac[9];
static int tictac_turn = 0;   // 0=X(玩家), 1=O(AI)
static int tictac_over = 0, tictac_win = 0;  // 0=进行,1=X胜,2=O胜,3=平
// 记忆翻牌
static uint8_t mem_cards[16], mem_flip[16], mem_state[16];
static int mem_open = -1, mem_moves = 0, mem_matched = 0, mem_over = 0;
static uint8_t mem_wait = 0;      // >0: 等待翻回 (存第二张索引+1)
static uint32_t mem_wait_last = 0;
// 文件搜索
static char find_query[32];
static char find_tmp[3300], find_out[3300];
static int find_scroll = 0;
// 俄罗斯方块
#define TETRIS_W 10
#define TETRIS_H 20
static uint8_t tetris_board[TETRIS_W * TETRIS_H];
static int tetris_piece = 0, tetris_rot = 0, tetris_px = 0, tetris_py = 0;
static int tetris_score = 0;
static uint8_t tetris_running = 0, tetris_paused = 0;
static uint32_t tetris_last = 0;
static const int tetris_shape[7][8] = {
    {0,0,1,0,2,0,3,0},   // I
    {0,0,1,0,0,1,1,1},   // O
    {0,0,1,0,2,0,1,1},   // T
    {1,0,2,0,0,1,1,1},   // S
    {0,0,1,0,1,1,2,1},   // Z
    {0,0,0,1,1,1,2,1},   // J
    {2,0,0,1,1,1,2,1},   // L
};
// 新功能窗口函数前置声明 (实现位于文件尾部)
static void render_tetris(int i);
static void render_tictac(int i);
static void render_memory(int i);
static void render_find(int i);
static void render_baseconv(int i);
static void render_random(int i);
static void tetris_tick(void);
static void tetris_move(int dx);
static void tetris_rotate(void);
static void tetris_drop(void);
static void tetris_reset(void);
static int tetris_hit(int px, int py, int piece, int rot);
static void tetris_spawn(void);
static void tictac_reset(void);
static void mem_reset(void);
static void mem_tick(void);
static void find_do(void);
static void bc_convert(void);
static int bc_hexval(char c);
static void rng_gen(void);
static void pgen_gen(void);
static void pgen_copy(void);
static void pgen_term(void);
static void render_textstats(int i);
static void render_sudoku(int i);
static void render_typing(int i);
static void render_pomodoro(int i);
static void render_guess(int i);
static void render_dice(int i);
static void render_todo(int i);
static void render_passgen(int i);
static void render_clock(int i);
static uint32_t clk_sw_elapsed(void);
static void clk_sw_reset(void);

static void render_ide(int i);

static int tictac_win_line(int p);
static int tictac_draw(void);
static void tictac_ai(void);

static void gui_click_content(int i, int mx, int my) {
    wins[i].dirty = 1;
    switch (wins[i].type) {
    case WIN_TERMINAL:
        break;   // 终端只需获得焦点
    case WIN_FILES:
        handle_files_click(i, mx, my);
        break;
    case WIN_MANAGER: {
        int wx = wins[i].x, wy = wins[i].y;
        int y = wy + 48 + 72;   // 与 render_manager 中按钮行一致
        if (my >= y && my < y + 26) {
            if (mx >= wx + 14 && mx < wx + 14 + 120) {   // Optimize
                strcpy(wins[i].app_buf, "Optimized: cache & heap defragged.");
            } else if (mx >= wx + 144 && mx < wx + 144 + 120) {  // Clean /tmp
                do_clean_garbage();
                strcpy(wins[i].app_buf, settings_info);
            }
        }
        break;
    }
    case WIN_BROWSER:
        break;  // 地址栏由键盘输入处理
    case WIN_TRANSLATE:
        break;  // 输入由键盘处理
    case WIN_CONVERT: {
        int wx = wins[i].x, wy = wins[i].y;
        int y = wy + 48;
        for (int k = 0; k < 4; k++) {
            int ry = y + k * 30;
            if (my >= ry && my < ry + 26 && mx >= wx + 10 && mx < wx + 10 + wins[i].w - 20) {
                wins[i].app_sel = k;
                break;
            }
        }
        int by = wy + wins[i].h - 44;
        if (my >= by && my < by + 28 && mx >= wx + 10 && mx < wx + 110) {
            int sel = wins[i].app_sel;
            if (sel < 0) sel = 0;
            if (sel > 3) sel = 3;
            static const char* srcs[4] = {"/doc.txt", "/doc.md", "/doc.txt", "/doc.txt"};
            static const char* dsts[4] = {"/doc.md", "/doc.html", "/doc.upper", "/doc.hex"};
            const char* src = srcs[sel];
            const char* dst = dsts[sel];
            if (!fs_exists(src)) {
                strcpy(wins[i].app_buf, "Source file missing");
                break;
            }
            int n = fs_read(src, conv_in, FS_MAX_SIZE);
            int o = 0;
            switch (sel) {
                case 0: o = conv_txt2md(conv_in, n, conv_out, sizeof(conv_out)); break;
                case 1: o = conv_md2html(conv_in, n, conv_out, sizeof(conv_out)); break;
                case 2: o = conv_upper(conv_in, n, conv_out, sizeof(conv_out)); break;
                default: o = conv_hex(conv_in, n, conv_out, sizeof(conv_out)); break;
            }
            fs_create(dst);
            fs_write(dst, conv_out, (uint32_t)o);
            fs_mark_dirty();
            char msg[64];
            strcpy(msg, "Written ");
            strcat(msg, dst);
            strcat(msg, " (");
            uitoa((uint32_t)o, msg + strlen(msg), 10);
            strcat(msg, " B)");
            strcpy(wins[i].app_buf, msg);
        }
        break;
    }
    case WIN_SHOT: {
        int wx = wins[i].x, wy = wins[i].y;
        int y = wy + 48 + 24;
        if (my >= y && my < y + 28) {
            if (mx >= wx + 14 && mx < wx + 14 + 120) {   // Capture
                // 将当前 backbuffer 内容写为 /tmp/shot.bin
                fs_create("/tmp/shot.bin");
                fs_write("/tmp/shot.bin", (uint8_t*)gfx_draw_target(),
                         gfx_width() * gfx_height() * 4);
                fs_mark_dirty();
                strcpy(wins[i].app_buf, "Captured to /tmp/shot.bin");
            } else if (mx >= wx + 144 && mx < wx + 144 + 120) {  // Open Photo
                strcpy(wins[i].app_buf, "Open /tmp/shot.bin in Photo viewer");
            }
        }
        break;
    }
    case WIN_PHOTO: {
        int wx = wins[i].x, wy = wins[i].y;
        int y = wy + 90;
        if (my >= y && my < y + 26) {
            if (mx >= wx + 14 && mx < wx + 164) {          // Open snapshot
                photo_err = 0;
                photo_loaded = 0;
                if (fs_exists("/tmp/shot.bin")) {
                    int n = fs_read("/tmp/shot.bin", photo_raw, sizeof(photo_raw));
                    if (n >= 800 * 600 * 4) photo_loaded = 1;
                    else photo_err = 1;
                } else {
                    photo_err = 1;
                }
            } else if (mx >= wx + 172 && mx < wx + 292) {  // Clear
                photo_loaded = 0;
                photo_err = 0;
            }
        }
        break;
    }
    case WIN_VIDEO: {
        int wx = wins[i].x, wy = wins[i].y;
        int cy = wy + wins[i].h - 58;
        if (my >= cy && my < cy + 40) {
            int rel = mx - wx - 22;
            if (rel >= 0 && rel < 30) {
                wins[i].app_state = !wins[i].app_state;
                if (!wins[i].app_state) wins[i].app_scroll = (int)timer_get_ticks();
            }
        }
        break;
    }
    case WIN_PDF:
        break;
    case WIN_MUSIC: {
        int wx = wins[i].x, wy = wins[i].y;
        int y = wy + 48;
        for (int k = 0; k < 5; k++) {
            int ry = y + k * 26;
            if (my >= ry && my < ry + 22 && mx >= wx + 10 && mx < wx + 10 + wins[i].w - 20) {
                wins[i].app_sel = k;
                break;
            }
        }
        int by = wy + wins[i].h - 44;
        if (my >= by && my < by + 28) {
            if (mx >= wx + 10 && mx < wx + 70) {   // Play
                static const char* songs[5] = {"Startup Theme", "Desktop Flow", "Kernel Tune", "Shell Beats", "Goodbye"};
                static int notes[] = {523, 587, 659, 784, 659, 587, 523, -1};
                static int durs[] = {150, 150, 150, 200, 150, 150, 300, 0};
                speaker_play(notes, durs, 7);
                strcpy(wins[i].app_buf, "Playing: ");
                strcat(wins[i].app_buf, songs[wins[i].app_sel]);
            } else if (mx >= wx + 78 && mx < wx + 138) {  // Stop
                speaker_off();
                strcpy(wins[i].app_buf, "Stopped");
            }
        }
        break;
    }
    case WIN_CALENDAR:
        break;
    case WIN_EDITOR: {
        int wx = wins[i].x, wy = wins[i].y;
        int sy = wy + wins[i].h - 40;
        if (my >= sy && my < sy + 26 && mx >= wx + 8 && mx < wx + 88) {  // Save
            fs_create("/doc.txt");
            fs_write("/doc.txt", (uint8_t*)wins[i].app_buf, (uint32_t)strlen(wins[i].app_buf));
            fs_mark_dirty();
            wins[i].app_state = 1;
        }
        break;
    }
    case WIN_ALARM: {
        int wx = wins[i].x, wy = wins[i].y;
        int y = wy + 48 + 40 + 22;
        if (my >= y && my < y + 26 && mx >= wx + 14 && mx < wx + 114) {  // Test Beep
            speaker_beep(880, 200);
        }
        break;
    }
    case WIN_CALC: {
        int wx = wins[i].x, wy = wins[i].y;
        int dy = wy + 46;
        int bw = (wins[i].w - 28) / 4, bh = 26;
        int kx = wx + 10, ky = dy + 50;
        const char* keys[20] = {"7","8","9","/", "4","5","6","*", "1","2","3","-", "0","C","=","+", ".","(",")","Del"};
        for (int k = 0; k < 20; k++) {
            int col = k % 4, row = k / 4;
            int bx = kx + col * (bw + 2), byy = ky + row * (bh + 2);
            if (byy + bh > wy + wins[i].h - 8) break;
            if (mx >= bx && mx < bx + bw && my >= byy && my < byy + bh) {
                if (keys[k][0] == 'C') {
                    wins[i].app_buf[0] = 0;
                    wins[i].app_num = 0;
                } else if (keys[k][0] == '=') {
                    char res[64];
                    calc_eval(wins[i].app_buf, res, sizeof(res));
                    wins[i].app_num = 1;
                    strcpy(wins[i].app_buf, res);
                } else if (keys[k][0] == 'D') {  // Del
                    int len = (int)strlen(wins[i].app_buf);
                    if (len > 0) wins[i].app_buf[len - 1] = 0;
                } else {
                    // 结果/错误态: 数字重新开始, 运算符继续运算
                    int is_digit = (keys[k][0] >= '0' && keys[k][0] <= '9') || keys[k][0] == '.';
                    int len = (int)strlen(wins[i].app_buf);
                    if (wins[i].app_num == 1 || wins[i].app_num == 2) {
                        if (is_digit) wins[i].app_buf[0] = 0;
                        wins[i].app_num = 0;
                        len = (int)strlen(wins[i].app_buf);
                    }
                    if (len < (int)sizeof(wins[i].app_buf) - 2) {
                        wins[i].app_buf[len] = keys[k][0];
                        wins[i].app_buf[len + 1] = 0;
                    }
                }
                break;
            }
        }
        break;
    }
    case WIN_STORE: {
        int wx = wins[i].x, wy = wins[i].y;
        int y = wy + 48;
        for (int k = 0; k < 12; k++) {
            int ry = y + k * 28;
            if (my >= ry && my < ry + 24 && mx >= wx + 10 && mx < wx + 10 + wins[i].w - 20) {
                if (mx >= wx + wins[i].w - 80) {
                    // 点击右侧 Install/Uninstall
                    const char* m = store_apps[k].marker;
                    int inst = app_is_installed(store_apps[k].type);
                    if (!inst) {
                        fs_mkdir("/apps");
                        if (fs_create(m) == 0) {
                            fs_write(m, (const uint8_t*)"1", 1);
                            fs_mark_dirty();
                            strcpy(wins[i].app_buf, "Installed: ");
                            strcat(wins[i].app_buf, store_apps[k].name);
                        } else {
                            strcpy(wins[i].app_buf, "Install failed: no slot");
                        }
                    } else {
                        if (fs_delete(m) == 0) {
                            fs_mark_dirty();
                            strcpy(wins[i].app_buf, "Uninstalled: ");
                            strcat(wins[i].app_buf, store_apps[k].name);
                        } else {
                            strcpy(wins[i].app_buf, "Uninstall failed");
                        }
                    }
                } else {
                    wins[i].app_sel = k;
                }
                break;
            }
        }
        break;
    }
    case WIN_ZIP: {
        int wx = wins[i].x, wy = wins[i].y;
        int by = wy + wins[i].h - 44;
        if (my >= by && my < by + 28) {
            if (mx >= wx + 10 && mx < wx + 110) {
                // Compress: /doc.txt -> /doc.epz (RLE)
                if (!fs_exists("/doc.txt")) {
                    strcpy(wins[i].app_buf, "No /doc.txt (create with Editor)");
                    break;
                }
                uint8_t in[FS_MAX_SIZE];
                int n = fs_read("/doc.txt", in, FS_MAX_SIZE);
                uint8_t out[FS_MAX_SIZE];
                int oi = 0, pi = 0;
                while (pi < n && oi < FS_MAX_SIZE - 2) {
                    int run = 1;
                    while (pi + run < n && in[pi + run] == in[pi] && run < 255) run++;
                    out[oi++] = in[pi];
                    out[oi++] = (uint8_t)run;
                    pi += run;
                }
                fs_create("/doc.epz");
                fs_write("/doc.epz", out, (uint32_t)oi);
                fs_mark_dirty();
                char msg[64]; strcpy(msg, "Compressed: "); uitoa(n, msg + 12, 10);
                strcat(msg, " -> "); uitoa(oi, msg + strlen(msg), 10); strcat(msg, " bytes");
                strcpy(wins[i].app_buf, msg);
            } else if (mx >= wx + 120 && mx < wx + 220) {
                // Extract: /doc.epz -> /doc2.txt
                if (!fs_exists("/doc.epz")) {
                    strcpy(wins[i].app_buf, "No /doc.epz - compress first");
                    break;
                }
                uint8_t in[FS_MAX_SIZE];
                int n = fs_read("/doc.epz", in, FS_MAX_SIZE);
                uint8_t out[FS_MAX_SIZE];
                int oi = 0, pi = 0;
                while (pi < n && oi < FS_MAX_SIZE) {
                    uint8_t b = in[pi++];
                    uint8_t run = (pi < n) ? in[pi++] : 1;
                    if (run == 0) run = 1;
                    for (int k = 0; k < run && oi < FS_MAX_SIZE; k++) out[oi++] = b;
                }
                fs_create("/doc2.txt");
                fs_write("/doc2.txt", out, (uint32_t)oi);
                fs_mark_dirty();
                char msg[64]; strcpy(msg, "Extracted: "); uitoa(oi, msg + 11, 10);
                strcat(msg, " bytes -> /doc2.txt");
                strcpy(wins[i].app_buf, msg);
            }
        }
        break;
    }
    case WIN_IDE: {
        int wx = wins[i].x, wy = wins[i].y;
        int by = wy + wins[i].h - 46;
        if (my >= by && my < by + 28 && mx >= wx + 10 && mx < wx + 100) {  // Run
            ide_run(wins[i].app_buf);
        }
        if (my >= by && my < by + 28 && mx >= wx + 108 && mx < wx + 228) {  // Clear Out
            ide_outn = 0;
            ide_out[0] = 0;
            ide_out_scroll = 0;
        }
        break;
    }
    case WIN_SNAKE: {
        // 点击 = 开始/继续 (方向键移动)
        if (!snake_alive) snake_reset();
        else snake_paused = !snake_paused;
        break;
    }
    case WIN_2048: {
        if (g2048_over) g2048_reset();
        break;
    }
    case WIN_MINES: {
        // 左键翻开
        int wx = wins[i].x, wy = wins[i].y;
        int ox = wx + 14, oy = wy + 46;
        int cw = 18, ch = 18;
        if (cw * MINES_N + 8 > wins[i].w - 28) cw = (wins[i].w - 36) / MINES_N;
        if (ch * MINES_M + 8 > wins[i].h - 92) ch = (wins[i].h - 100) / MINES_M;
        int gx = (mx - ox) / (cw + 1), gy = (my - oy) / (ch + 1);
        if (gx >= 0 && gy >= 0 && gx < MINES_N && gy < MINES_M) {
            if (mines_over == 2 || mines_over == 1) mines_reset();
            mines_open_cell(gx, gy);
        }
        break;
    }
    case WIN_BRICK: {
        if (brick_over || !brick_run) { brick_reset(); }
        else { brick_run = 1; }
        break;
    }
    case WIN_PAINT: {
        int wx = wins[i].x, wy = wins[i].y;
        int py = wy + 44;
        int bxx = wx + 10 + 6 * 30 + 16;
        if (mx >= bxx && mx < bxx + 64 && my >= py && my < py + 26) {  // Clear
            paint_n = 0;
            paint_drawing = 0;
            wins[i].app_buf[0] = 0;
            wins[i].dirty = 1;
            break;
        }
        for (int k = 0; k < 6; k++) {
            int px = wx + 10 + k * 30;
            if (mx >= px && mx < px + 26 && my >= py && my < py + 26) {
                paint_cur_col = k;
                break;
            }
        }
        break;
    }
    case WIN_NOTE: {
        int wx = wins[i].x, wy = wins[i].y;
        int by = wy + wins[i].h - 32;
        if (my >= by && my < by + 24) {
            if (mx >= wx + 10 && mx < wx + 74) {        // Save
                fs_create("/notes.txt");
                fs_write("/notes.txt", (const uint8_t*)wins[i].app_buf,
                         (uint32_t)strlen(wins[i].app_buf));
                fs_mark_dirty();
                wins[i].app_state = 1;
            } else if (mx >= wx + 82 && mx < wx + 146) { // Load
                if (fs_exists("/notes.txt")) {
                    fs_read("/notes.txt", (uint8_t*)wins[i].app_buf, 255);
                    wins[i].app_buf[255] = 0;
                    wins[i].app_state = 0;
                }
            } else if (mx >= wx + 154 && mx < wx + 214) { // Clear
                wins[i].app_buf[0] = 0;
                wins[i].app_state = 0;
            }
        }
        break;
    }
    case WIN_UNIT: {
        int wx = wins[i].x, wy = wins[i].y;
        int y = wy + 48;
        for (int k = 0; k < 3; k++) {
            int px = wx + 10 + k * 90;
            if (mx >= px && mx < px + 84 && my >= y && my < y + 24) {
                unit_cat = k;
                unit_from = 0;
                unit_to = (k == 2) ? 1 : 1;
                break;
            }
        }
        int un = (unit_cat == 2) ? 3 : 4;
        for (int k = 0; k < un; k++) {
            int px = wx + 10 + k * 84;
            if (mx >= px && mx < px + 78 && my >= y + 34 && my < y + 58) {
                unit_from = k;
                break;
            }
            if (mx >= px && mx < px + 78 && my >= y + 68 && my < y + 92) {
                unit_to = k;
                break;
            }
        }
        break;
    }
    case WIN_STOPW: {
        int wx = wins[i].x, wy = wins[i].y;
        int by = wy + 120;
        if (my >= by && my < by + 28) {
            if (mx >= wx + 14 && mx < wx + 104) {        // Start/Stop
                if (stopw_run) {
                    stopw_acc += timer_get_ticks() - stopw_start;
                    stopw_run = 0;
                } else {
                    stopw_start = timer_get_ticks();
                    stopw_run = 1;
                }
            } else if (mx >= wx + 112 && mx < wx + 202) { // Reset
                stopw_run = 0;
                stopw_acc = 0;
            }
        }
        break;
    }
    case WIN_HEXVIEW: {
        int wx = wins[i].x, wy = wins[i].y;
        int y = wy + 46;
        if (mx >= wx + wins[i].w - 90 && mx < wx + wins[i].w - 10 &&
            my >= y && my < y + 26) {                     // Load
            strcpy(hex_name, "/doc.txt");
            hex_len = 0;
            if (fs_exists(hex_name)) {
                hex_len = fs_read(hex_name, hex_data, sizeof(hex_data));
            }
            wins[i].app_scroll = 0;
            wins[i].app_buf[0] = 0;
            char msg[40]; strcpy(msg, "Loaded "); uitoa(hex_len, msg + 7, 10);
            strcat(msg, " bytes");
            strcpy(wins[i].app_buf, msg);
        }
        break;
    }
    case WIN_TICTAC: {
        int wx = wins[i].x, wy = wins[i].y;
        if (tictac_over) { tictac_reset(); break; }
        if (tictac_turn != 0) break;
        int gx = wx + 14, gy = wy + 48, gs = 78, gap = 6;
        int r = (my - gy) / (gs + gap), c = (mx - gx) / (gs + gap);
        if (r < 0 || r > 2 || c < 0 || c > 2) break;
        int idx = r * 3 + c;
        if (tictac[idx]) break;
        tictac[idx] = 1; tictac_turn = 1;
        if (tictac_win_line(1)) { tictac_over = 1; tictac_win = 1; }
        else if (tictac_draw()) { tictac_over = 1; tictac_win = 3; }
        else tictac_ai();
        break;
    }
    case WIN_MEMORY: {
        int wx = wins[i].x, wy = wins[i].y;
        if (mem_wait) break;
        if (my >= wy + 284 && my < wy + 310 && mx >= wx + 14 && mx < wx + 110) { mem_reset(); break; }
        int cw = 44, gap = 8, ox = wx + 14, oy = wy + 48;
        int r = (my - oy) / (cw + gap), c = (mx - ox) / (cw + gap);
        int idx = r * 4 + c;
        if (r < 0 || r > 3 || c < 0 || c > 3 || idx < 0 || idx > 15) break;
        if (mem_state[idx] || mem_flip[idx]) break;
        mem_flip[idx] = 1;
        mem_moves++;
        if (mem_open < 0) { mem_open = idx; }
        else {
            if (mem_cards[mem_open] == mem_cards[idx]) {
                mem_state[mem_open] = 2; mem_state[idx] = 2;
                mem_matched++;
                mem_open = -1;
                if (mem_matched == 8) mem_over = 1;
            } else {
                mem_wait = (uint8_t)(idx + 1);
                mem_wait_last = timer_get_ticks();
            }
        }
        break;
    }
    case WIN_FIND: {
        int wx = wins[i].x, wy = wins[i].y;
        int y = wy + 48;
        if (mx >= wx + wins[i].w - 96 && mx < wx + wins[i].w - 14 &&
            my >= y && my < y + 30) { find_do(); }
        break;
    }
    case WIN_BASECONV: {
        int wx = wins[i].x, wy = wins[i].y;
        int y = wy + 48 + 40;
        if (my >= y && my < y + 26) {
            if (mx >= wx + 14 && mx < wx + 102) { bc_mode = 0; bc_convert(); }
            else if (mx >= wx + 110 && mx < wx + 198) { bc_mode = 1; bc_convert(); }
        }
        break;
    }
    case WIN_RANDOM: {
        int wx = wins[i].x, wy = wins[i].y;
        int y = wy + 48;
        if (my >= y - 4 && my < y + 22 && mx >= wx + 70 && mx < wx + 160) { rng_focus = 0; break; }
        y += 30;
        if (my >= y - 4 && my < y + 22 && mx >= wx + 70 && mx < wx + 160) { rng_focus = 1; break; }
        y += 36;
        if (my >= y && my < y + 28 && mx >= wx + 14 && mx < wx + 110) { rng_gen(); break; }
        break;
    }
    case WIN_TEXTSTATS: {
        int wx = wins[i].x, wy = wins[i].y;
        int y = wy + 48 + 104 + 136;
        if (mx >= wx + 14 && mx < wx + 110 && my >= y && my < y + 28) { ts_clear(); }
        break;
    }
    case WIN_SUDOKU: {
        int wx = wins[i].x, wy = wins[i].y;
        int ox = wx + 14, oy = wy + 48;
        int cell = (wins[i].w - 28) / 9; if (cell > 34) cell = 34;
        if (mx >= ox && mx < ox + cell * 9 && my >= oy && my < oy + cell * 9) {
            int c = (mx - ox) / cell, r = (my - oy) / cell;
            if (r >= 0 && r < 9 && c >= 0 && c < 9) { sd_sel = r * 9 + c; sd_msg = 0; }
        }
        break;
    }
    case WIN_TYPING: {
        int wx = wins[i].x, wy = wins[i].y;
        if (mx >= wx + 14 && mx < wx + 110 && my >= wy + 48 && my < wy + 78) { ty_load((ty_idx + 1) % 3); }
        break;
    }
    case WIN_POMODORO: {
        int wx = wins[i].x, wy = wins[i].y;
        int by = wy + 146;
        if (mx >= wx + 14 && mx < wx + 104 && my >= by && my < by + 28) {
            if (!pom_run) { pom_run = 1; pom_deadline = timer_get_uptime() + pom_left; }
            else pom_run = 0;
        } else if (mx >= wx + 114 && mx < wx + 204 && my >= by && my < by + 28) {
            pom_reset();
        }
        break;
    }
    case WIN_GUESS: {
        int wx = wins[i].x, wy = wins[i].y;
        int y = wy + 48;
        if (mx >= wx + 154 && mx < wx + 234 && my >= y && my < y + 30) { gs_new(); }
        break;
    }
    case WIN_DICE: {
        int wx = wins[i].x, wy = wins[i].y;
        int by = wy + 48;
        static const int sides2[7] = {4, 6, 8, 10, 12, 20, 100};
        for (int k = 0; k < 7; k++) {
            int x = wx + 14 + k * 56;
            if (mx >= x && mx < x + 50 && my >= by && my < by + 26) { dc_side = sides2[k]; break; }
        }
        int rby = by + 44 + 104;
        if (mx >= wx + 14 && mx < wx + 110 && my >= rby && my < rby + 28) { dc_roll(); }
        break;
    }
    case WIN_TODO: {
        int wx = wins[i].x, wy = wins[i].y;
        int w = wins[i].w, h = wins[i].h;
        // 输入行: [输入框][Add]
        if (my >= wy + 48 && my < wy + 78) {
            int bx = wx + w - 102;
            if (mx >= bx && mx < bx + 88) {         // Add 按钮
                todo_add(wins[i].app_buf);
                wins[i].app_buf[0] = 0;
                wins[i].dirty = 1;
            }
            break;
        }
        // 操作行: [Clear done][计数][^][v]
        if (my >= wy + 86 && my < wy + 112) {
            if (mx >= wx + 14 && mx < wx + 110) {   // Clear done
                todo_clear_done();
                wins[i].dirty = 1;
            } else if (mx >= wx + w - 90 && mx < wx + w - 56) {  // 上滚
                if (todo_scroll > 0) { todo_scroll--; wins[i].dirty = 1; }
            } else if (mx >= wx + w - 48 && mx < wx + w - 14) {  // 下滚
                int vis = todo_vis_rows(h);
                int mxr = todo_n - vis; if (mxr < 0) mxr = 0;
                if (todo_scroll < mxr) { todo_scroll++; wins[i].dirty = 1; }
            }
            break;
        }
        // 列表行: 勾选框 / 删除按钮
        int top = wy + 122;
        int vis = todo_vis_rows(h);
        int row = (my - top) / 28;
        if (my >= top && row >= 0 && row < vis) {
            int idx = todo_scroll + row;
            if (idx >= 0 && idx < todo_n) {
                if (mx >= wx + 18 && mx < wx + 48) {            // toggle done
                    todo_done[idx] = !todo_done[idx];
                    wins[i].dirty = 1;
                } else if (mx >= wx + w - 40 && mx < wx + w - 10) {  // delete item
                    for (int k = idx; k < todo_n - 1; k++) {
                        for (int j = 0; j < TODO_LEN; j++) todo_items[k][j] = todo_items[k + 1][j];
                        todo_done[k] = todo_done[k + 1];
                    }
                    todo_n--;
                    if (todo_scroll > todo_n) todo_scroll = todo_n;
                    if (todo_scroll < 0) todo_scroll = 0;
                    wins[i].dirty = 1;
                }
            }
        }
        break;
    }
    case WIN_PASSGEN: {
        int wx = wins[i].x, wy = wins[i].y;
        int w = wins[i].w, h = wins[i].h;
        int y0 = wy + 48;
        // Length row: 8/12/16/20
        if (my >= y0 && my < y0 + 24) {
            for (int k = 0; k < 4; k++) {
                int bx = wx + 74 + k * 54;
                if (mx >= bx && mx < bx + 48) {
                    pgen_len_i = k;
                    strcpy(pgen_msg, "Length: ");
                    uitoa((uint32_t)pgen_lens[k], pgen_msg + 8, 10);
                    strcat(pgen_msg, " chars");
                    if (pgen_out[0]) pgen_out[0] = 0;
                    wins[i].dirty = 1;
                    break;
                }
            }
            break;
        }
        // Charset row: A-Z a-z 0-9 !@#
        if (my >= y0 + 34 && my < y0 + 56) {
            for (int c = 0; c < 4; c++) {
                int bx = wx + 74 + c * 64;
                if (mx >= bx && mx < bx + 58) {
                    pgen_opt[c] = !pgen_opt[c];
                    wins[i].dirty = 1;
                    break;
                }
            }
            break;
        }
        // Actions row: Generate / Copy / Terminal
        if (my >= y0 + 64 && my < y0 + 90) {
            if (mx >= wx + 14 && mx < wx + 122) {
                if (!(pgen_opt[0] || pgen_opt[1] || pgen_opt[2] || pgen_opt[3])) {
                    strcpy(pgen_msg, "Select at least one charset");
                } else {
                    pgen_gen();
                }
                wins[i].dirty = 1;
            } else if (mx >= wx + 134 && mx < wx + 210) {
                if (!pgen_out[0]) strcpy(pgen_msg, "Generate a password first");
                else pgen_copy();
                wins[i].dirty = 1;
            } else if (mx >= wx + 222 && mx < wx + 314) {
                if (!pgen_out[0]) strcpy(pgen_msg, "Generate a password first");
                else pgen_term();
                wins[i].dirty = 1;
            }
            break;
        }
        break;
    }
    case WIN_CLOCK: {
        int wx = wins[i].x, wy = wins[i].y;
        int by = wy + 216;   // 与 render_clock 按钮行一致
        if (my >= by && my < by + 28) {
            if (mx >= wx + 14 && mx < wx + 104) {
                if (clk_sw_run) { clk_sw_acc = clk_sw_elapsed(); clk_sw_run = 0; }
                else { clk_sw_start = timer_get_ticks(); clk_sw_run = 1; }
                wins[i].dirty = 1;
            } else if (mx >= wx + 112 && mx < wx + 202) {
                clk_sw_reset();
                wins[i].dirty = 1;
            }
        }
        break;
    }
    case WIN_TASKMGR: {
        int wx = wins[i].x, wc_y = wins[i].y;
        int y0 = wc_y + 48 + 40;   // 与 render_taskmgr 任务行起点一致
        int n = task_get_count();
        if (n > TASK_MAX) n = TASK_MAX;
        for (int k = 0; k < n; k++) {
            const struct task* t = task_get(k);
            if (!t || t->state == TASK_EMPTY) continue;
            int ry = y0;
            y0 += 18;
            if (my >= ry && my < ry + 18 && mx >= wx && mx < wx + wins[i].w) {
                if (t->id > 0) {
                    if (task_kill(t->id) == 0) {
                        strcpy(wins[i].app_buf, "Task ended: ");
                        strcat(wins[i].app_buf, t->name);
                    }
                }
                break;
            }
        }
        break;
    }
    case WIN_DISK: {
        int wx = wins[i].x, wy = wins[i].y;
        int y = wy + 48 + 90;
        if (my >= y && my < y + 26 && mx >= wx + 14 && mx < wx + 134) {  // Save FS
            fs_disk_save();
            strcpy(wins[i].app_buf, "FS saved to disk");
        }
        break;
    }
    case WIN_NET: {
        int wx = wins[i].x, wy = wins[i].y;
        int y = wy + 48 + 90;
        if (my >= y && my < y + 26 && mx >= wx + 14 && mx < wx + 114) {  // Refresh
            wins[i].app_state = !wins[i].app_state;
        }
        break;
    }
    case WIN_SETTINGS: {
        int wx = wins[i].x, wy = wins[i].y;
        // "Next Wallpaper" 按钮 (距顶 108, 高 24)
        int byy = wy + 108;
        if (mx >= wx + 14 && mx < wx + 14 + 120 &&
            my >= byy && my < byy + 24) {
            wallpaper_index = (wallpaper_index + 1) % wallpaper_count();
            wallpaper_set(wallpaper_index);
            break;
        }
        // 系统工具按钮 (距顶 224, 高 26)
        int oy = wy + 224;
        if (my >= oy && my < oy + 26) {
            if (mx >= wx + 14 && mx < wx + 14 + 130) {   // Clean Garbage
                do_clean_garbage();
                break;
            }
            if (mx >= wx + 154 && mx < wx + 154 + 92) {  // Uninstall
                settings_state = (settings_state == 1) ? 0 : 1;
                break;
            }
        }
        // 卸载确认按钮 (距顶 258)
        if (settings_state == 1) {
            int sy = wy + 258;
            if (my >= sy + 5 && my < sy + 29) {
                if (mx >= wx + 160 && mx < wx + 160 + 44) {  // Yes
                    do_uninstall();
                    break;
                }
                if (mx >= wx + 210 && mx < wx + 210 + 44) {  // No
                    settings_state = 0;
                    break;
                }
            }
        }
        break;
    }
    default:
        break;
    }
}

static void gui_on_click(int mx, int my) {
    int screen_h = gfx_height();
    // 0. 顶部状态栏: 点击左上角设置图标打开设置窗口, 其余区域消费事件
    if (my < STATUS_BAR_H) {
        serial_printf(COM1, "[g] click TOPBAR mx=%d my=%d -> %s\n", mx, my,
                      (mx < STATUS_ICON_W) ? "OPEN_SETTINGS" : "consumed");
        if (mx < STATUS_ICON_W) open_window_by_type(WIN_SETTINGS);
        return;
    }
    // 1. 任务栏
    if (my >= screen_h - TASKBAR_H) {
        serial_printf(COM1, "[g] click TASKBAR\n");
        handle_taskbar(mx);
        return;
    }
    // 3. 桌面图标 (仅点击未被窗口覆盖的区域)
    int covered = 0;
    for (int i = 0; i < win_count; i++) {
        if (!wins[i].used || !wins[i].visible) continue;
        int wy = win_top(i);
        if (mx >= wins[i].x && mx < wins[i].x + wins[i].w &&
            my >= wy && my < wy + TITLE_H + wins[i].h) { covered = 1; serial_printf(COM1, "[g] click covered-by win%d type=%d\n", i, wins[i].type); break; }
    }
    if (!covered) {
        serial_printf(COM1, "[g] click DESKTOP at %d,%d\n", mx, my);
        handle_desktop_click(mx, my);
    }
    // 4. 优先命中活动窗口 (置顶层), 再按数组反向查找
    if (active_win >= 0 && wins[active_win].used && wins[active_win].visible) {
        int i = active_win;
        int wx = wins[i].x;
        int wy = win_top(i);
        if (mx >= wx && mx < wx + wins[i].w &&
            my >= wy && my < wy + TITLE_H + wins[i].h) {
            if (my < wy + TITLE_H) {
                int btn = win_title_button_hit(i, mx, my);
                serial_printf(COM1, "[g] click WIN%d type=%d btn=%d at %d,%d (win x=%d y=%d w=%d h=%d)\n",
                              i, wins[i].type, btn, mx, my, wins[i].x, wins[i].y, wins[i].w, wins[i].h);
                if (btn == 0) {
                    desk_blit_rect(wins[i].x, win_top(i), wins[i].w, TITLE_H + wins[i].h);
                    icons_dirty = ~0ULL;
                    wins[i].visible = 0;
                    active_win = -1;
                    taskbar_dirty = 1;
                    return;
                } else if (btn == 1) {   // 最小化
                    desk_blit_rect(wins[i].x, win_top(i), wins[i].w, TITLE_H + wins[i].h);
                    icons_dirty = ~0ULL;
                    wins[i].visible = 0;
                    if (active_win == i) active_win = -1;
                    taskbar_dirty = 1;
                    return;
                } else if (btn == 2) {   // 最大化/还原
                    win_toggle_max(i);
                    return;
                }
                if (!wins[i].maximized) {   // 最大化时不可拖动
                    drag_win = i;
                    drag_ox = mx - wins[i].x;
                    drag_oy = my - wy;
                }
                return;
            }
            gui_click_content(i, mx, my);
            return;
        }
    }
    for (int i = win_count - 1; i >= 0; i--) {
        if (!wins[i].used || !wins[i].visible) continue;
        int wx = wins[i].x;
        int wy = win_top(i);
        if (mx >= wx && mx < wx + wins[i].w &&
            my >= wy && my < wy + TITLE_H + wins[i].h) {
            gui_activate(i);
            if (my < wy + TITLE_H) {
                // 关闭 / 最小化 / 最大化 按钮
                int btn = win_title_button_hit(i, mx, my);
                if (btn == 0) {
                    wins[i].visible = 0;
                    if (active_win == i) active_win = -1;
                    desk_blit_rect(wx, wy, wins[i].w, TITLE_H + wins[i].h);
                    taskbar_dirty = 1;
                    return;
                } else if (btn == 1) {   // 最小化
                    wins[i].visible = 0;
                    if (active_win == i) active_win = -1;
                    desk_blit_rect(wx, wy, wins[i].w, TITLE_H + wins[i].h);
                    taskbar_dirty = 1;
                    return;
                } else if (btn == 2) {   // 最大化/还原
                    win_toggle_max(i);
                    desk_need_full = 1;
                    return;
                }
                if (!wins[i].maximized) {   // 最大化时不可拖动
                    drag_win = i;
                    drag_ox = mx - wins[i].x;
                    drag_oy = my - wy;
                }
                return;
            }
            gui_click_content(i, mx, my);
            return;
        }
    }
    active_win = -1;
}

// ============ 串口测试钩子 (QEMU 自动化自测, 防伪造) ============
// 协议: 每行一命令
//   open:<type>  打开窗口 (如 open:17 = 计算器)
//   key:<text>   向活动窗口注入键盘输入 ('\r' 触发计算器求值)
//   echo         回执 "pong"
static char test_inject[64];
static int test_inject_n = 0;

static void gui_test_poll(void) {
    static char tline[64];
    static int tlen = 0;
    while (serial_received(COM1)) {
        char ch = serial_getc(COM1);
        if (ch == '\n') {
            tline[tlen] = 0;
            tlen = 0;
            if (tline[0] == 0) continue;
            if (strncmp(tline, "open:", 5) == 0) {
                int t = atoi(tline + 5);
                open_window_by_type(t);
                serial_printf(COM1, "[test] opened type=%d\n", t);
            } else if (strncmp(tline, "key:", 4) == 0) {
                const char* p = tline + 4;
                while (*p && test_inject_n < 62) test_inject[test_inject_n++] = *p++;
                serial_printf(COM1, "[test] inject=%d\n", test_inject_n);
            } else if (strncmp(tline, "click:", 6) == 0) {
                int cx = atoi(tline + 6);
                const char* cp = strchr(tline + 6, ',');
                int cy = cp ? atoi(cp + 1) : 0;
                gui_on_click(cx, cy);
                serial_printf(COM1, "[test] click %d,%d\n", cx, cy);
            } else if (strncmp(tline, "press:", 6) == 0) {
                int cx = atoi(tline + 6);
                const char* cp = strchr(tline + 6, ',');
                int cy = cp ? atoi(cp + 1) : 0;
                cursor_x = cx; cursor_y = cy;
                g_mouse.buttons |= MOUSE_LEFT_BUTTON;
                serial_printf(COM1, "[test] press %d,%d\n", cx, cy);
            } else if (strncmp(tline, "release", 7) == 0) {
                g_mouse.buttons &= ~MOUSE_LEFT_BUTTON;
                serial_printf(COM1, "[test] release\n");
            } else if (strncmp(tline, "mv:", 3) == 0) {
                int dx = atoi(tline + 3);
                const char* cp = strchr(tline + 3, ',');
                int dy = cp ? atoi(cp + 1) : 0;
                g_mouse.dx += dx; g_mouse.dy += dy;
                serial_printf(COM1, "[test] mv %d,%d\n", dx, dy);
            } else if (strcmp(tline, "echo") == 0) {
                serial_write_str("[test] pong\n");
            } else {
                serial_printf(COM1, "[test] unknown:%s\n", tline);
            }
        } else if (tlen < 62) {
            tline[tlen++] = ch;
        }
    }
}

static void gui_handle_input(void) {
    // 键盘 → 活动终端窗口 (串口测试注入优先, 走同一键盘处理路径)
    int c = -1;
    if (test_inject_n > 0) {
        c = (unsigned char)test_inject[0];
        for (int i = 0; i < test_inject_n - 1; i++) test_inject[i] = test_inject[i + 1];
        test_inject_n--;
    } else {
        c = keyboard_getchar();
    }
    if (c >= 0) {
        if (c == 0x1B) {  // ESC = close active window
            if (active_win >= 0 && wins[active_win].used) {
                int ci = active_win;
                int cxx = wins[ci].x, cyy = win_top(ci);
                wins[ci].visible = 0;
                active_win = -1;
                desk_blit_rect(cxx, cyy, wins[ci].w, TITLE_H + wins[ci].h);
                taskbar_dirty = 1;
            }
        } else if (active_win >= 0 && wins[active_win].used) {
            int tp = wins[active_win].type;
            // 可观测性: 按键已到达活动窗口 (证明未丢输入)
            serial_printf(COM1, "[k] key=%d win=%d type=%d\n", c, active_win, tp);
            if (tp == WIN_TERMINAL) {
                shell_process_char((char)c);
            } else if (tp == WIN_SNAKE) {
                if (c == KEY_UP && snake_dir != 2) snake_dir = 0;
                else if (c == KEY_DOWN && snake_dir != 0) snake_dir = 2;
                else if (c == KEY_LEFT && snake_dir != 3) snake_dir = 1;
                else if (c == KEY_RIGHT && snake_dir != 1) snake_dir = 3;
                else if (c == 'r' || c == 'R') snake_reset();
                else if (c == 'p' || c == 'P') snake_paused = !snake_paused;
            } else if (tp == WIN_2048) {
                if (c == KEY_UP) g2048_move(0);
                else if (c == KEY_DOWN) g2048_move(2);
                else if (c == KEY_LEFT) g2048_move(1);
                else if (c == KEY_RIGHT) g2048_move(3);
                else if (c == 'r' || c == 'R') g2048_reset();
            } else if (tp == WIN_MINES) {
                if (c == 'r' || c == 'R') mines_reset();
            } else if (tp == WIN_BRICK) {
                if (c == 'r' || c == 'R') brick_reset();
            } else if (tp == WIN_PAINT) {
                if (c == 's' || c == 'S') {
                    // 保存为 /paint.pts (打包线段)
                    fs_create("/paint.pts");
                    uint8_t hdr[4];
                    hdr[0] = (uint8_t)(paint_n & 255);
                    hdr[1] = (uint8_t)((paint_n >> 8) & 255);
                    hdr[2] = (uint8_t)paint_cur_col;
                    hdr[3] = 0;
                    fs_write("/paint.pts", hdr, 4);
                    for (int k = 0; k < paint_n; k++) {
                        uint8_t seg[9];
                        seg[0] = (uint8_t)(paint_x0[k] & 255);
                        seg[1] = (uint8_t)((paint_x0[k] >> 8) & 255);
                        seg[2] = (uint8_t)(paint_y0[k] & 255);
                        seg[3] = (uint8_t)((paint_y0[k] >> 8) & 255);
                        seg[4] = (uint8_t)(paint_x1[k] & 255);
                        seg[5] = (uint8_t)((paint_x1[k] >> 8) & 255);
                        seg[6] = (uint8_t)(paint_y1[k] & 255);
                        seg[7] = (uint8_t)((paint_y1[k] >> 8) & 255);
                        seg[8] = (uint8_t)paint_col[k];
                        fs_append("/paint.pts", seg, 9);
                    }
                    fs_mark_dirty();
                    strcpy(wins[active_win].app_buf, "Saved to /paint.pts");
                    wins[active_win].dirty = 1;
                } else if (c == 'c' || c == 'C') {
                    paint_n = 0;
                    paint_drawing = 0;
                    wins[active_win].app_buf[0] = 0;
                    wins[active_win].dirty = 1;
                }
            } else if (tp == WIN_NOTE) {
                if (c == '\b') {
                    int len = (int)strlen(wins[active_win].app_buf);
                    if (len > 0) {
                        wins[active_win].app_buf[len - 1] = 0;
                        wins[active_win].app_state = 0;
                    }
                } else if (c >= 32) {
                    int len = (int)strlen(wins[active_win].app_buf);
                    if (len < 254) {
                        wins[active_win].app_buf[len] = (char)c;
                        wins[active_win].app_buf[len + 1] = 0;
                        wins[active_win].app_state = 0;
                    }
                }
            } else if (tp == WIN_UNIT) {
                if (c == '\b') {
                    int len = (int)strlen(wins[active_win].app_buf);
                    if (len > 0) wins[active_win].app_buf[len - 1] = 0;
                } else if (c >= '0' && c <= '9') {
                    int len = (int)strlen(wins[active_win].app_buf);
                    if (len < 9) {
                        wins[active_win].app_buf[len] = (char)c;
                        wins[active_win].app_buf[len + 1] = 0;
                    }
                } else if (c == '-') {
                    int len = (int)strlen(wins[active_win].app_buf);
                    if (len == 0) { wins[active_win].app_buf[0] = '-'; wins[active_win].app_buf[1] = 0; }
                }
            } else if (tp == WIN_STOPW) {
                if (c == ' ' || c == 's' || c == 'S') {
                    if (stopw_run) {
                        stopw_acc += timer_get_ticks() - stopw_start;
                        stopw_run = 0;
                    } else {
                        stopw_start = timer_get_ticks();
                        stopw_run = 1;
                    }
                } else if (c == 'r' || c == 'R') {
                    stopw_run = 0;
                    stopw_acc = 0;
                }
            } else if (tp == WIN_HEXVIEW) {
                if (c == ']' || c == KEY_DOWN || c == KEY_RIGHT) wins[active_win].app_scroll += 3;
                else if (c == '[' || c == KEY_UP || c == KEY_LEFT) wins[active_win].app_scroll -= 3;
            } else if (tp == WIN_TETRIS) {
                if (c == KEY_LEFT) tetris_move(-1);
                else if (c == KEY_RIGHT) tetris_move(1);
                else if (c == KEY_DOWN) { if (!tetris_hit(tetris_px, tetris_py + 1, tetris_piece, tetris_rot)) tetris_py++; }
                else if (c == KEY_UP) tetris_rotate();
                else if (c == ' ') tetris_drop();
                else if (c == 'r' || c == 'R') tetris_reset();
                else if (c == 'p' || c == 'P') tetris_paused = !tetris_paused;
            } else if (tp == WIN_TICTAC) {
                if (c == 'r' || c == 'R') tictac_reset();
            } else if (tp == WIN_MEMORY) {
                if (c == 'r' || c == 'R') mem_reset();
            } else if (tp == WIN_FIND) {
                if (c == '\b') {
                    int len = (int)strlen(find_query);
                    if (len > 0) { find_query[len - 1] = 0; }
                } else if (c == '\r' || c == '\n') {
                    find_do();
                } else if (c >= 32 && c < 127) {
                    int len = (int)strlen(find_query);
                    if (len < 30) { find_query[len] = (char)c; find_query[len + 1] = 0; }
                }
            } else if (tp == WIN_BASECONV) {
                if (c == '\b') {
                    int len = (int)strlen(bc_input);
                    if (len > 0) bc_input[len - 1] = 0;
                } else if (c == '\r' || c == '\n') {
                    bc_convert();
                } else if (c >= '0' && c <= '9') {
                    int len = (int)strlen(bc_input);
                    if (len < 18) { bc_input[len] = (char)c; bc_input[len + 1] = 0; }
                } else if (bc_mode == 1 && bc_hexval((char)c) >= 0) {
                    int len = (int)strlen(bc_input);
                    if (len < 16) { bc_input[len] = (char)c; bc_input[len + 1] = 0; }
                }
            } else if (tp == WIN_RANDOM) {
                char* buf = rng_focus ? rng_max : rng_min;
                int cap = rng_focus ? 7 : 7;
                if (c == '\b') {
                    int len = (int)strlen(buf);
                    if (len > 0) buf[len - 1] = 0;
                } else if (c == '\r' || c == '\n') {
                    rng_gen();
                } else if ((c >= '0' && c <= '9') || c == '-') {
                    int len = (int)strlen(buf);
                    if (len < cap) { buf[len] = (char)c; buf[len + 1] = 0; }
                }
            } else if (tp == WIN_TEXTSTATS) {
                if (c == '\b') {
                    int len = (int)strlen(ts_text);
                    if (len > 0) ts_text[len - 1] = 0;
                } else if (c >= 32 && c < 127) {
                    int len = (int)strlen(ts_text);
                    if (len < 255) { ts_text[len] = (char)c; ts_text[len + 1] = 0; }
                }
            } else if (tp == WIN_SUDOKU) {
                if (sd_sel >= 0) {
                    int rr = sd_sel / 9, cc = sd_sel % 9;
                    if (c >= '1' && c <= '9') { if (!sd_fixed[sd_sel]) { sd_puzzle[sd_sel] = (uint8_t)(c - '0'); sd_msg = 0; } }
                    else if (c == '\b' || c == '0') { if (!sd_fixed[sd_sel]) { sd_puzzle[sd_sel] = 0; sd_msg = 0; } }
                    else if (c == KEY_UP) { if (rr > 0) sd_sel = (rr - 1) * 9 + cc; }
                    else if (c == KEY_DOWN) { if (rr < 8) sd_sel = (rr + 1) * 9 + cc; }
                    else if (c == KEY_LEFT) { if (cc > 0) sd_sel = rr * 9 + cc - 1; }
                    else if (c == KEY_RIGHT) { if (cc < 8) sd_sel = rr * 9 + cc + 1; }
                }
                if (c == 'r' || c == 'R') sd_load_puzzle((sd_cur + 1) % SD_NUM_PUZZLES);
                else if (c == 'c' || c == 'C') {
                    int ok = 1;
                    for (int k = 0; k < 81; k++) if (sd_puzzle[k] != sd_solution[k]) { ok = 0; break; }
                    sd_msg = ok ? 1 : 2;
                }
            } else if (tp == WIN_TYPING) {
                const char* ts = ty_texts[ty_idx];
                if (c == 'r' || c == 'R' || c == ' ') {
                    ty_load((ty_idx + 1) % 3);
                } else if (ty_done) {
                    /* ignore further typing until switch */
                } else if (c == '\b') {
                    if (ty_pos > 0) ty_pos--;
                } else if (c >= 32 && c < 127) {
                    if (!ty_started) { ty_started = 1; ty_start = timer_get_uptime(); }
                    if (ty_pos < (int)strlen(ts)) {
                        if ((char)c == ts[ty_pos]) ty_pos++;
                        else ty_err++;
                        if (ty_pos >= (int)strlen(ts)) ty_done = 1;
                    }
                }
            } else if (tp == WIN_POMODORO) {
                if (c == ' ') {
                    if (!pom_run) { pom_run = 1; pom_deadline = timer_get_uptime() + pom_left; }
                    else pom_run = 0;
                } else if (c == 'r' || c == 'R') pom_reset();
            } else if (tp == WIN_GUESS) {
                if (c == '\b') {
                    int len = (int)strlen(gs_buf);
                    if (len > 0) gs_buf[len - 1] = 0;
                } else if (c == '\r' || c == '\n') {
                    if (!gs_over) {
                        int v = atoi(gs_buf);
                        if (v >= 1 && v <= 100) {
                            gs_tries++;
                            if (v == gs_target) { gs_over = 1; gs_msg = 0; }
                            else if (v > gs_target) { gs_msg = 1; }
                            else { gs_msg = 2; }
                            gs_buf[0] = 0;
                        }
                    }
                } else if (c >= '0' && c <= '9') {
                    int len = (int)strlen(gs_buf);
                    if (len < 3) { gs_buf[len] = (char)c; gs_buf[len + 1] = 0; }
                }
            } else if (tp == WIN_DICE) {
                if (c == ' ') dc_roll();
                else if (c >= '1' && c <= '7') {
                    static const int sides3[7] = {4, 6, 8, 10, 12, 20, 100};
                    dc_side = sides3[c - '1'];
                }
            } else if (tp == WIN_TODO) {
                if (c == '\r' || c == '\n') {
                    todo_add(wins[active_win].app_buf);
                    wins[active_win].app_buf[0] = 0;
                    wins[active_win].dirty = 1;
                } else if (c == '\b') {
                    int len = (int)strlen(wins[active_win].app_buf);
                    if (len > 0) { wins[active_win].app_buf[len - 1] = 0; wins[active_win].dirty = 1; }
                } else if (c == KEY_UP || c == KEY_DOWN) {
                    int vis = todo_vis_rows(wins[active_win].h);
                    int mx = todo_n - vis; if (mx < 0) mx = 0;
                    if (c == KEY_UP) { if (todo_scroll > 0) { todo_scroll--; wins[active_win].dirty = 1; } }
                    else { if (todo_scroll < mx) { todo_scroll++; wins[active_win].dirty = 1; } }
                } else if (c >= 32 && c < 127) {
                    int len = (int)strlen(wins[active_win].app_buf);
                    if (len < 250) {
                        wins[active_win].app_buf[len] = (char)c;
                        wins[active_win].app_buf[len + 1] = 0;
                        wins[active_win].dirty = 1;
                    }
                }
            } else if (tp == WIN_PASSGEN) {
                if (c == '\r' || c == '\n' || c == ' ' || c == 'g' || c == 'G') {
                    if (!(pgen_opt[0] || pgen_opt[1] || pgen_opt[2] || pgen_opt[3]))
                        strcpy(pgen_msg, "Select at least one charset");
                    else pgen_gen();
                    wins[active_win].dirty = 1;
                } else if (c == 'c' || c == 'C') {
                    if (!pgen_out[0]) strcpy(pgen_msg, "Generate a password first");
                    else pgen_copy();
                    wins[active_win].dirty = 1;
                } else if (c == 't' || c == 'T') {
                    if (!pgen_out[0]) strcpy(pgen_msg, "Generate a password first");
                    else pgen_term();
                    wins[active_win].dirty = 1;
                } else if (c >= '1' && c <= '4') {
                    pgen_len_i = c - '1';
                    strcpy(pgen_msg, "Length: ");
                    uitoa((uint32_t)pgen_lens[pgen_len_i], pgen_msg + 8, 10);
                    strcat(pgen_msg, " chars");
                    if (pgen_out[0]) pgen_out[0] = 0;
                    wins[active_win].dirty = 1;
                }
            } else if (tp == WIN_CLOCK) {
                if (c == ' ' || c == '\r' || c == '\n') {
                    if (clk_sw_run) { clk_sw_acc = clk_sw_elapsed(); clk_sw_run = 0; }
                    else { clk_sw_start = timer_get_ticks(); clk_sw_run = 1; }
                    wins[active_win].dirty = 1;
                } else if (c == 'r' || c == 'R') {
                    clk_sw_reset();
                    wins[active_win].dirty = 1;
                }
            } else if (tp == WIN_BROWSER) {
                if (c == '\r' || c == '\n') {
                    // 跳转: 解析 epoch:// 地址
                    const char* url = wins[active_win].app_buf;
                    if (strncmp(url, "epoch://", 8) == 0) {
                        wins[active_win].app_state = 1;
                    } else {
                        strcpy(wins[active_win].app_buf, "epoch://home");
                    }
                } else if (c == '\b') {
                    int len = (int)strlen(wins[active_win].app_buf);
                    if (len > 0) wins[active_win].app_buf[len - 1] = 0;
                } else if (c >= 32 && c < 127) {
                    int len = (int)strlen(wins[active_win].app_buf);
                    if (len < 120) {
                        wins[active_win].app_buf[len] = (char)c;
                        wins[active_win].app_buf[len + 1] = 0;
                    }
                }
            } else if (tp == WIN_TRANSLATE) {
                if (c == '\r' || c == '\n') {
                    // 查询: 保留输入, 结果放 app_num=-1 表示已查询
                    wins[active_win].app_num = 1;
                } else if (c == '\b') {
                    int len = (int)strlen(wins[active_win].app_buf);
                    if (len > 0) wins[active_win].app_buf[len - 1] = 0;
                } else if (c >= 32 && c < 127) {
                    int len = (int)strlen(wins[active_win].app_buf);
                    if (len < 30) {
                        wins[active_win].app_buf[len] = (char)c;
                        wins[active_win].app_buf[len + 1] = 0;
                    }
                }
            } else if (tp == WIN_CALC) {
                if (c == '\r' || c == '\n') {
                    char res[64];
                    calc_eval(wins[active_win].app_buf, res, sizeof(res));
                    wins[active_win].app_num = 1;
                    strcpy(wins[active_win].app_buf, res);
                } else if (c == '\b') {
                    int len = (int)strlen(wins[active_win].app_buf);
                    if (len > 0) wins[active_win].app_buf[len - 1] = 0;
                    wins[active_win].app_num = 0;
                } else if ((c >= '0' && c <= '9') || c == '.' || c == '+' ||
                           c == '-' || c == '*' || c == '/' || c == '(' || c == ')') {
                    int len = (int)strlen(wins[active_win].app_buf);
                    if (wins[active_win].app_num == 1 || wins[active_win].app_num == 2) {
                        if (c >= '0' && c <= '9') wins[active_win].app_buf[0] = 0;
                        wins[active_win].app_num = 0;
                        len = (int)strlen(wins[active_win].app_buf);
                    }
                    if (len < (int)sizeof(wins[active_win].app_buf) - 2) {
                        wins[active_win].app_buf[len] = (char)c;
                        wins[active_win].app_buf[len + 1] = 0;
                    }
                }
            } else if (tp == WIN_IDE) {
                if (c == '\r' || c == '\n') {
                    int len = (int)strlen(wins[active_win].app_buf);
                    if (len < 250) {
                        wins[active_win].app_buf[len] = '\n';
                        wins[active_win].app_buf[len + 1] = 0;
                    }
                } else if (c == '\b') {
                    int len = (int)strlen(wins[active_win].app_buf);
                    if (len > 0) wins[active_win].app_buf[len - 1] = 0;
                } else if (c == '\t') {
                    int len = (int)strlen(wins[active_win].app_buf);
                    if (len < 250) {
                        wins[active_win].app_buf[len] = ' ';
                        wins[active_win].app_buf[len + 1] = ' ';
                        wins[active_win].app_buf[len + 2] = 0;
                    }
                } else if (c >= 32 && c < 127) {
                    int len = (int)strlen(wins[active_win].app_buf);
                    if (len < 250) {
                        wins[active_win].app_buf[len] = (char)c;
                        wins[active_win].app_buf[len + 1] = 0;
                    }
                }
            }
            if (active_win >= 0 && wins[active_win].used) wins[active_win].dirty = 1;
        } else {
            // 可观测性: 无活动窗口时按键被丢弃 -> 计数一次, 供自测判定"零丢输入"
            serial_printf(COM1, "[k] drop key=%d (no active window)\n", c);
        }
    }
    // 鼠标移动 (消费累加量, 消费后清零)
    int mdx = g_mouse.dx;
    int mdy = g_mouse.dy;
    g_mouse.dx = 0;
    g_mouse.dy = 0;
    cursor_x += mdx;
    cursor_y += mdy;
    if (cursor_x < 0) cursor_x = 0;
    if (cursor_y < 0) cursor_y = 0;
    if (cursor_x >= gfx_width()) cursor_x = gfx_width() - 1;
    if (cursor_y >= gfx_height()) cursor_y = gfx_height() - 1;
    if (++g_mouse_log_cnt >= 30) {   // 节流: 每30帧打一次, 避免淹没 [m] pkt
        g_mouse_log_cnt = 0;
        serial_printf(COM1, "[g] mdx=%d mdy=%d bt=%d cur=%d,%d\n",
                      mdx, mdy, g_mouse.buttons, cursor_x, cursor_y);
    }
    // hover 状态更新
    {
        int sy = gfx_height() - TASKBAR_H;
        if (drag_win >= 0 && (mdx != 0 || mdy != 0) && wins[drag_win].used)
            wins[drag_win].dirty = 1;   // 拖拽窗口: 每帧重绘
        if (cursor_y >= sy && (mdx != 0 || mdy != 0)) taskbar_dirty = 1;
        // 桌面图标 hover (未被窗口遮挡)
        int new_hover = -1;
        if (cursor_y < sy) {
            int covered = 0;
            for (int i = 0; i < win_count; i++) {
                if (!wins[i].used || !wins[i].visible) continue;
                int wy = win_top(i);
                if (cursor_x >= wins[i].x && cursor_x < wins[i].x + wins[i].w &&
                    cursor_y >= wy && cursor_y < wy + TITLE_H + wins[i].h) { covered = 1; break; }
            }
            if (!covered) new_hover = desk_hit(cursor_x, cursor_y);
        }
        if (new_hover != desk_hover) {
            if (desk_hover >= 0) icons_dirty |= (1ULL << desk_hover);
            desk_hover = new_hover;
            if (desk_hover >= 0) icons_dirty |= (1ULL << desk_hover);
        }
    }
    uint8_t b = g_mouse.buttons;
    if ((b & MOUSE_LEFT_BUTTON) && !(prev_buttons & MOUSE_LEFT_BUTTON)) {
        if (dmenu_open) {
            // 先处理右键菜单
            int iw = 150, ih = DMENU_ITEMS * 30 + 8;
            if (cursor_x >= dmenu_x && cursor_x < dmenu_x + iw &&
                cursor_y >= dmenu_y && cursor_y < dmenu_y + ih) {
                int idx = (cursor_y - dmenu_y - 4) / 30;
                if (idx >= 0 && idx < DMENU_ITEMS) dmenu_action(idx);
                else { dmenu_open = 0; desk_need_full = 1; }
            } else {
                dmenu_open = 0;
                desk_need_full = 1;
            }
        } else {
            gui_on_click(cursor_x, cursor_y);
        }
    }
    if ((b & MOUSE_RIGHT_BUTTON) && !(prev_buttons & MOUSE_RIGHT_BUTTON)) {
        int handled = 0;
        // 扫雷窗口内右键 = 标记/取消标记地雷
        for (int wi = 0; wi < win_count; wi++) {
            if (!wins[wi].used || !wins[wi].visible) continue;
            if (wins[wi].type != WIN_MINES) continue;
            int wx = wins[wi].x, wy = wins[wi].y;
            if (cursor_x >= wx && cursor_x < wx + wins[wi].w &&
                cursor_y >= wy && cursor_y < wy + wins[wi].h) {
                int ox = wx + 14, oy = wy + 46;
                int cw = 18, ch = 18;
                if (cw * MINES_N + 8 > wins[wi].w - 28) cw = (wins[wi].w - 36) / MINES_N;
                if (ch * MINES_M + 8 > wins[wi].h - 92) ch = (wins[wi].h - 100) / MINES_M;
                int gx = (cursor_x - ox) / (cw + 1), gy = (cursor_y - oy) / (ch + 1);
                if (gx >= 0 && gy >= 0 && gx < MINES_N && gy < MINES_M) {
                    mines_toggle_flag(gx, gy);
                    handled = 1;
                }
                break;
            }
        }
        // 桌面空白右键 → 打开桌面右键菜单
        if (!handled) {
            int sy = gfx_height() - TASKBAR_H;
            if (cursor_y >= STATUS_BAR_H && cursor_y < sy) {
                int covered = 0;
                for (int wi = 0; wi < win_count; wi++) {
                    if (!wins[wi].used || !wins[wi].visible) continue;
                    int wy = win_top(wi);
                    if (cursor_x >= wins[wi].x && cursor_x < wins[wi].x + wins[wi].w &&
                        cursor_y >= wy && cursor_y < wy + TITLE_H + wins[wi].h) { covered = 1; break; }
                }
                if (!covered) {
                    dmenu_x = cursor_x; dmenu_y = cursor_y;
                    if (dmenu_x + 150 > gfx_width()) dmenu_x = gfx_width() - 150;
                    if (dmenu_y + DMENU_ITEMS * 30 + 8 > sy)
                        dmenu_y = sy - (DMENU_ITEMS * 30 + 8);
                    dmenu_open = 1;
                    desk_need_full = 1;
                } else {
                    dmenu_open = 0;
                }
            } else {
                dmenu_open = 0;
            }
        }
    }
    if (b & MOUSE_LEFT_BUTTON) {
        if (drag_win >= 0 && wins[drag_win].visible) {
            // 记录旧位置 (render_desktop 恢复旧位置壁纸并标记重绘)
            wins[drag_win].px = wins[drag_win].x;
            wins[drag_win].py = wins[drag_win].y;
            wins[drag_win].x = cursor_x - drag_ox;
            wins[drag_win].y = cursor_y - drag_oy;
            if (wins[drag_win].x < 0) wins[drag_win].x = 0;
            if (wins[drag_win].y < TITLE_H + 2) wins[drag_win].y = TITLE_H + 2;
            if (wins[drag_win].x + wins[drag_win].w > gfx_width())
                wins[drag_win].x = gfx_width() - wins[drag_win].w;
            if (wins[drag_win].y + wins[drag_win].h > gfx_height() - TASKBAR_H)
                wins[drag_win].y = gfx_height() - TASKBAR_H - wins[drag_win].h;
        }
    } else {
        drag_win = -1;
    }
    prev_buttons = b;
}

// ---------------- 渲染 ----------------

// 状态栏/任务栏毛玻璃预渲染缓存: 每帧全屏 box blur 是卡顿主因,
// 这两条固定区域只在开机时对壁纸模糊一次, 之后每帧直接 blit。
#define BAR_CACHE_W 800
#define BAR_CACHE_STATUS_H 28
#define BAR_CACHE_TASKBAR_H 26
static uint8_t g_status_cache[BAR_CACHE_W * BAR_CACHE_STATUS_H * 3];
static uint8_t g_taskbar_cache[BAR_CACHE_W * BAR_CACHE_TASKBAR_H * 3];
static uint32_t g_status_cache32[BAR_CACHE_W * BAR_CACHE_STATUS_H];
static uint32_t g_taskbar_cache32[BAR_CACHE_W * BAR_CACHE_TASKBAR_H];
static int g_bar_cache_ok = 0;

static void bar_cache_fill_region(uint8_t* cache, int y0, int h, int alpha) {
    int w = gfx_width(), H = gfx_height();
    if (w > BAR_CACHE_W) w = BAR_CACHE_W;
    if (h > BAR_CACHE_STATUS_H && h > BAR_CACHE_TASKBAR_H) h = BAR_CACHE_STATUS_H;
    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            int rr = 0, gg = 0, bb = 0, n = 0;
            for (int dy = -1; dy <= 1; dy++) {
                int ady = dy < 0 ? -dy : dy;
                for (int dx = -(1 - ady); dx <= (1 - ady); dx++) {
                    int sx = px + dx, sy = y0 + py + dy;
                    if (sx < 0 || sy < 0 || sx >= w || sy >= H) continue;
                    uint32_t c = gfx_getpixel(sx, sy);
                    rr += (c >> 16) & 0xFF;
                    gg += (c >> 8) & 0xFF;
                    bb += c & 0xFF;
                    n++;
                }
            }
            if (n == 0) continue;
            rr /= n; gg /= n; bb /= n;
            rr = (rr * (255 - alpha) + 255 * alpha) / 255; if (rr > 255) rr = 255;
            gg = (gg * (255 - alpha) + 255 * alpha) / 255; if (gg > 255) gg = 255;
            bb = (bb * (255 - alpha) + 255 * alpha) / 255; if (bb > 255) bb = 255;
            int o = (py * BAR_CACHE_W + px) * 3;
            cache[o] = (uint8_t)rr; cache[o + 1] = (uint8_t)gg; cache[o + 2] = (uint8_t)bb;
        }
        // 行渐变叠加: 顶部亮白蓝 -> 底部柔和蓝灰 (毛玻璃 + 流体渐变, 开机构建一次)
        int g_r = 0xF4 + ((0xC9 - 0xF4) * py) / (h > 0 ? h : 1);
        int g_g = 0xF8 + ((0xDC - 0xF8) * py) / (h > 0 ? h : 1);
        int g_b = 0xFC + ((0xF0 - 0xFC) * py) / (h > 0 ? h : 1);
        for (int px = 0; px < w; px++) {
            int o = (py * BAR_CACHE_W + px) * 3;
            int rr = (cache[o] * 130 + g_r * 125) / 255;
            int gg = (cache[o + 1] * 130 + g_g * 125) / 255;
            int bb = (cache[o + 2] * 130 + g_b * 125) / 255;
            if (rr > 255) rr = 255; if (gg > 255) gg = 255; if (bb > 255) bb = 255;
            cache[o] = (uint8_t)rr; cache[o + 1] = (uint8_t)gg; cache[o + 2] = (uint8_t)bb;
        }
    }
}

static void build_bar_caches(void) {
    int h = gfx_height();
    bar_cache_fill_region(g_status_cache, 0, STATUS_BAR_H, 72);
    bar_cache_fill_region(g_taskbar_cache, h - TASKBAR_H, TASKBAR_H, 64);
    for (int i = 0; i < BAR_CACHE_W * BAR_CACHE_STATUS_H; i++) {
        uint8_t* p = g_status_cache + i * 3;
        g_status_cache32[i] = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
    for (int i = 0; i < BAR_CACHE_W * BAR_CACHE_TASKBAR_H; i++) {
        uint8_t* p = g_taskbar_cache + i * 3;
        g_taskbar_cache32[i] = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
    g_bar_cache_ok = 1;
}

static void blit_bar_cache(const uint8_t* cache, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    const uint32_t* src32;
    if (cache == (const uint8_t*)g_status_cache) src32 = (const uint32_t*)g_status_cache32;
    else if (cache == (const uint8_t*)g_taskbar_cache) src32 = (const uint32_t*)g_taskbar_cache32;
    else return;
    int maxh = (src32 == g_status_cache32) ? BAR_CACHE_STATUS_H : BAR_CACHE_TASKBAR_H;
    int cw = gfx_width() < BAR_CACHE_W ? gfx_width() : BAR_CACHE_W;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > cw) w = cw - x;
    if (h > maxh) h = maxh;
    if (w <= 0 || h <= 0) return;
    uint32_t* dst = gfx_draw_target();
    int pitch = gfx_pitch();
    for (int py = 0; py < h; py++) {
        memcpy((uint8_t*)dst + (uint32_t)(y + py) * pitch + (uint32_t)x * 4,
               src32 + (uint32_t)py * BAR_CACHE_W + (uint32_t)x, (size_t)w * 4);
    }
    gfx_mark_dirty(x, y, w, h);
}

static void desk_cache_build(void) {
    if (!g_desk_cache) g_desk_cache = (uint32_t)kmalloc(800 * 600 * 4);
    if (!g_desk_cache) return;
    uint32_t saved = (uint32_t)gfx_draw_target();
    gfx_set_draw_target(g_desk_cache);
    wallpaper_blit();
    gfx_fill_alpha(0, 0, gfx_width(), STATUS_BAR_H, 0xFFFFFF, 40);
    gfx_set_draw_target(saved);
    g_desk_ready = 1;
    serial_printf(COM1, "[desk] cache built 0x%x\n", (uint32_t)g_desk_cache);
}

static void desk_blit_full(void) {
    if (!g_desk_ready) return;
    uint32_t* dst = gfx_draw_target();
    const uint32_t* src = (const uint32_t*)g_desk_cache;
    int pitch = gfx_pitch();
    int w = gfx_width() < 800 ? gfx_width() : 800;
    int h = gfx_height() < 600 ? gfx_height() : 600;
    for (int y = 0; y < h; y++) {
        memcpy((uint8_t*)dst + (uint32_t)y * pitch, src + (uint32_t)y * 800, (size_t)w * 4);
    }
    gfx_mark_dirty(0, 0, w, h);
}

// 从桌面缓存局部恢复矩形区域 (用于擦除旧光标残影)
static void desk_blit_rect(int x, int y, int w, int h) {
    if (!g_desk_ready || w <= 0 || h <= 0) return;
    int cw = gfx_width() < 800 ? gfx_width() : 800;
    int ch = gfx_height() < 600 ? gfx_height() : 600;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > cw) w = cw - x;
    if (y + h > ch) h = ch - y;
    if (w <= 0 || h <= 0) return;
    uint32_t* dst = gfx_draw_target();
    const uint32_t* src = (const uint32_t*)g_desk_cache;
    int pitch = gfx_pitch();
    for (int yy = 0; yy < h; yy++) {
        memcpy((uint8_t*)dst + (uint32_t)(y + yy) * pitch + (uint32_t)x * 4,
               src + (uint32_t)(y + yy) * 800 + (uint32_t)x, (size_t)w * 4);
    }
    gfx_mark_dirty(x, y, w, h);
}

static void render_desktop(void) {
    if (!g_desk_ready) desk_cache_build();
    if (desk_need_full) {
        desk_blit_full();
        desk_need_full = 0;
        icons_dirty = ~0ULL; // 全屏恢复背景后需重绘图标
        status_dirty = 1;    // 顶部/底部条被壁纸覆盖, 需重绘
        taskbar_dirty = 1;
        for (int wi = 0; wi < win_count; wi++) if (wins[wi].used) wins[wi].dirty = 1;
    }
    // 光标移动后: 先恢复上一帧光标区域背景 (消除移动残影), 窗口/任务栏等层随后重绘覆盖
    if (prev_cursor_x >= 0 && (prev_cursor_x != cursor_x || prev_cursor_y != cursor_y)) {
        // 用"上一帧->当前帧"的路径包围盒恢复, 覆盖一帧内跨越多格的中间位置, 避免拖尾
        int px0 = prev_cursor_x < cursor_x ? prev_cursor_x : cursor_x;
        int py0 = prev_cursor_y < cursor_y ? prev_cursor_y : cursor_y;
        int pdx = prev_cursor_x > cursor_x ? prev_cursor_x - cursor_x : cursor_x - prev_cursor_x;
        int pdy = prev_cursor_y > cursor_y ? prev_cursor_y - cursor_y : cursor_y - prev_cursor_y;
        int rx = px0 - 2, ry = py0 - 2, rw = pdx + 32, rh = pdy + 32;
        // 旧光标区域若与桌面图标相交, 恢复背景会擦掉图标, 需联动重绘
        for (int i = 0; i < DESKTOP_ICONS; i++) {
            int ix, iy, iw, ih;
            desk_icon_rect(i, &ix, &iy, &iw, &ih);
            if (rx < ix + iw && rx + rw > ix && ry < iy + ih && ry + rh > iy) { icons_dirty |= (1ULL << i); }
        }
        // 旧光标区域与窗口相交: 恢复壁纸会擦掉窗口像素, 联动重绘
        for (int wi = 0; wi < win_count; wi++) {
            if (!wins[wi].used || !wins[wi].visible) continue;
            int wxx = wins[wi].x, wyy = win_top(wi);
            if (rx < wxx + wins[wi].w && rx + rw > wxx &&
                ry < wyy + TITLE_H + wins[wi].h && ry + rh > wyy)
                wins[wi].dirty = 1;
        }
        // 旧光标区域与顶部/底部条相交: desk_blit_rect 只恢复壁纸层, 条内图标/文字需联动重绘
        // 顶部条: 空白区局部恢复 (memcpy), 触及设置图标/日期时间胶囊才整条重绘
        if (ry < STATUS_BAR_H) {
            if (g_bar_cache_ok) {
                int cx = rx < 0 ? 0 : rx;
                int cy = ry < 0 ? 0 : ry;
                int cw = rw - (cx - rx); if (cw > gfx_width() - cx) cw = gfx_width() - cx;
                int ch = STATUS_BAR_H - cy; if (ch > rh) ch = rh;
                int sbx = gfx_width() - 8 - (10 + 18 + 6 + (int)strlen(g_dt_label) * 8 + 10 + (int)strlen(g_dt_time) * 8 + 10);
                if (sbx < 0) sbx = 0;
                int sbw = (gfx_width() - sbx);
                if (cx < 40 && cx + cw > 2 && cy < 30 && cy + ch > 2) status_dirty = 1; // 设置图标区
                else if (cx < sbx + sbw && cx + cw > sbx) status_dirty = 1;             // 胶囊区
                else blit_bar_cache(g_status_cache, cx, cy, cw, ch);
            } else status_dirty = 1;
        }
        if (ry + rh > gfx_height() - TASKBAR_H) taskbar_dirty = 1;
        desk_blit_rect(rx, ry, rw, rh);
    }
    // 窗口拖动/移动: 恢复旧位置壁纸并标记重绘 (替代全屏恢复)
    for (int wi = 0; wi < win_count; wi++) {
        if (!wins[wi].used || !wins[wi].visible) continue;
        if (wins[wi].px != wins[wi].x || wins[wi].py != wins[wi].y) {
            int wtop = wins[wi].py - TITLE_H;
            desk_blit_rect(wins[wi].px, wtop, wins[wi].w, TITLE_H + wins[wi].h);
            wins[wi].px = wins[wi].x;
            wins[wi].py = wins[wi].y;
            wins[wi].dirty = 1;
        }
    }
    prev_cursor_x = cursor_x;
    prev_cursor_y = cursor_y;
    if (icons_dirty) {
        render_desktop_icons(icons_dirty);
        icons_dirty = 0;
    }
}

static void render_window_title(int i);
static void render_terminal(int i);
static void render_files(int i);
static void render_about(int i);
static void render_help(int i);
static void render_settings(int i);
static void render_manager(int i);
static void render_browser(int i);
static void render_translate(int i);
static void render_convert(int i);
static void render_shot(int i);
static void render_photo(int i);
static void render_video(int i);
static void render_pdf(int i);
static void render_music(int i);
static void render_calendar(int i);
static void render_editor(int i);
static void render_alarm(int i);
static void render_calc(int i);
static void render_store(int i);
static void render_zip(int i);
static void render_ide(int i);
static void render_taskmgr(int i);
static void render_disk(int i);
static void render_net(int i);
static void render_snake(int i);
static void render_g2048(int i);
static void render_mines(int i);
static void render_brick(int i);
static void render_paint(int i);
static void render_note(int i);
static void render_unit(int i);
static void render_stopw(int i);
static void render_hexview(int i);
static void render_window(int i) {
    if (!wins[i].used || !wins[i].visible) return;
    // 窗口阴影 (柔和统一)
    gfx_draw_shadow(wins[i].x + 2, win_top(i) + 2, wins[i].w, TITLE_H + wins[i].h, 3);
    render_window_title(i);
    // 内容区外框
    gfx_draw_round_rect(wins[i].x, wins[i].y, wins[i].w, wins[i].h, 10, 0xB8C6DA);
    switch (wins[i].type) {
    case WIN_TERMINAL:  render_terminal(i); break;
    case WIN_FILES:     render_files(i); break;
    case WIN_ABOUT:     render_about(i); break;
    case WIN_HELP:      render_help(i); break;
    case WIN_SETTINGS:  render_settings(i); break;
    case WIN_MANAGER:   render_manager(i); break;
    case WIN_BROWSER:   render_browser(i); break;
    case WIN_TRANSLATE: render_translate(i); break;
    case WIN_CONVERT:   render_convert(i); break;
    case WIN_SHOT:      render_shot(i); break;
    case WIN_PHOTO:     render_photo(i); break;
    case WIN_VIDEO:     render_video(i); break;
    case WIN_PDF:       render_pdf(i); break;
    case WIN_MUSIC:     render_music(i); break;
    case WIN_CALENDAR:  render_calendar(i); break;
    case WIN_EDITOR:    render_editor(i); break;
    case WIN_ALARM:     render_alarm(i); break;
    case WIN_CALC:      render_calc(i); break;
    case WIN_STORE:     render_store(i); break;
    case WIN_ZIP:       render_zip(i); break;
    case WIN_IDE:       render_ide(i); break;
    case WIN_TASKMGR:   render_taskmgr(i); break;
    case WIN_DISK:      render_disk(i); break;
    case WIN_NET:       render_net(i); break;
    case WIN_SNAKE:     render_snake(i); break;
    case WIN_2048:      render_g2048(i); break;
    case WIN_MINES:     render_mines(i); break;
    case WIN_BRICK:     render_brick(i); break;
    case WIN_PAINT:     render_paint(i); break;
    case WIN_NOTE:      render_note(i); break;
    case WIN_UNIT:      render_unit(i); break;
    case WIN_STOPW:     render_stopw(i); break;
    case WIN_HEXVIEW:   render_hexview(i); break;
    case WIN_TETRIS:    render_tetris(i); break;
    case WIN_TICTAC:    render_tictac(i); break;
    case WIN_MEMORY:    render_memory(i); break;
    case WIN_FIND:      render_find(i); break;
    case WIN_BASECONV:  render_baseconv(i); break;
    case WIN_RANDOM:    render_random(i); break;
    case WIN_TEXTSTATS: render_textstats(i); break;
    case WIN_SUDOKU:    render_sudoku(i); break;
    case WIN_TYPING:    render_typing(i); break;
    case WIN_POMODORO:  render_pomodoro(i); break;
    case WIN_GUESS:     render_guess(i); break;
    case WIN_DICE:      render_dice(i); break;
    case WIN_TODO:      render_todo(i); break;
    case WIN_PASSGEN:   render_passgen(i); break;
    case WIN_CLOCK:     render_clock(i); break;
    default: break;
    }
}

static void render_terminal(int i) {
    gfx_fill_rect(wins[i].x, wins[i].y, wins[i].w, wins[i].h, COL_BLACK);
    vga_render(wins[i].x, wins[i].y);
}

static void render_files(int i) {
    int wx = wins[i].x, wc_y = wins[i].y;
    int ww = wins[i].w, wh = wins[i].h;
    // 半透明渐变内容背景 (免逐帧模糊)
    gfx_fill_soft_gradient(wx, wc_y, ww, wh, 8, 0xF7FAFE, 0xDCE9F6, 70);
    // 路径栏 (浅灰圆角)
    gfx_fill_round_rect(wx + 6, wc_y + 5, ww - 12, 20, 6, 0xE8EFF8);
    gfx_draw_text(wx + 12, wc_y + 7, wins[i].file_dir, 0x103060, 0xE8EFF8);
    // 列表
    int list_y0 = wc_y + 30;
    int row_h = 18;
    int btn_y = wc_y + wh - 30;
    int max_rows = (btn_y - 6 - list_y0) / row_h;
    if (max_rows < 1) max_rows = 1;
    for (int r = 0; r < max_rows; r++) {
        int idx = wins[i].file_scroll + r;
        int ry = list_y0 + r * row_h;
        if (idx >= wins[i].entry_count) break;
        struct file_entry* e = &wins[i].entries[idx];
        uint32_t bg = (idx == wins[i].file_sel) ? 0xCFE4F8 : 0xFAFCFF;
        uint32_t fg = (idx == wins[i].file_sel) ? 0x103060 : 0x202020;
        gfx_fill_round_rect(wx + 4, ry, ww - 8, row_h - 2, 4, bg);
        char disp[FS_MAX_NAME + 8];
        uint32_t o = 0;
        disp[o++] = e->is_dir ? '[' : ' ';
        if (e->is_dir) disp[o++] = 'D';
        disp[o++] = ']';
        disp[o++] = ' ';
        const char* s = e->name;
        while (*s && o < sizeof(disp) - 12) disp[o++] = *s++;
        if (!e->is_dir) {
            disp[o++] = ' ';
            char num[16];
            uitoa(e->size, num, 10);
            const char* q = num;
            while (*q && o < sizeof(disp) - 1) disp[o++] = *q++;
            disp[o++] = 'B';
        }
        disp[o] = 0;
        gfx_draw_text(wx + 10, ry + 3, disp, fg, bg);
    }
    // 按钮行 (白底蓝字圆角)
    const char* labels[5] = {"Open", "Del", "Save", "Ref", "^"};
    int bx = wx + 8;
    int by = btn_y;
    for (int k = 0; k < 5; k++) {
        int hover = (cursor_x >= bx && cursor_x < bx + 44 && cursor_y >= by && cursor_y < by + 22);
        uint32_t bg = hover ? 0x3D7BD6 : 0xFFFFFF;
        uint32_t fg = hover ? 0xFFFFFF : 0x2A5A90;
        gfx_fill_round_rect(bx, by, 44, 22, 6, bg);
        gfx_draw_round_rect(bx, by, 44, 22, 6, hover ? 0x7FB4F0 : 0xA0B8D0);
        gfx_draw_text(bx + (44 - (int)strlen(labels[k]) * 8) / 2, by + 5,
                      labels[k], fg, bg);
        bx += 48;
    }
}

static void u32_to_str(uint32_t v, char* out) {
    char tmp[12];
    int n = 0;
    if (v == 0) {
        out[0] = '0';
        out[1] = 0;
        return;
    }
    while (v > 0 && n < 11) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (int k = 0; k < n; k++) out[k] = tmp[n - 1 - k];
    out[n] = 0;
}

static void render_about(int i) {
    int wx = wins[i].x, wc_y = wins[i].y;
    int w = wins[i].w, h = wins[i].h;
    // 半透明渐变内容背景
    gfx_fill_soft_gradient(wx, wc_y, w, h, 8, 0xF7FAFE, 0xDCE9F6, 70);
    char buf[24];

    // 白色简约横幅
    gfx_fill_round_rect(wx + 8, wc_y + 6, w - 16, 44, 8, 0xE8F0FA);
    gfx_draw_text(wx + 18, wc_y + 12, "EpochOS", 0x1E4E8C, 0xE8F0FA);
    gfx_draw_text(wx + 18, wc_y + 32, "From-Scratch x86 Operating System", 0x506070, 0xE8F0FA);
    gfx_draw_round_rect(wx + w - 84, wc_y + 12, 72, 22, 11, 0x3D7BD6);
    gfx_draw_text(wx + w - 66, wc_y + 16, "v1.2", 0xFFFFFF, 0x3D7BD6);

    int y = wc_y + 66;
    int lx = wx + 14;
    int vx = wx + 86;

    uint32_t used_kb = mm_used_pages() * (PAGE_SIZE / 1024) + mm_small_used_blocks() / 8;
    uint32_t total_mb = mm_total_pages() * (PAGE_SIZE / 1024) / 1024;
    gfx_draw_text(lx, y, "Memory", 0x506070, 0xFAFCFF);
    u32_to_str(total_mb, buf);
    gfx_draw_text(vx, y, buf, 0x1E4E8C, 0xFAFCFF);
    gfx_draw_text(vx + (int)strlen(buf) * 8 + 4, y, "MB total", 0x404040, 0xFAFCFF);
    u32_to_str(used_kb, buf);
    gfx_draw_text(vx + 96, y, buf, 0x1E4E8C, 0xFAFCFF);
    gfx_draw_text(vx + 96 + (int)strlen(buf) * 8 + 4, y, "KB used", 0x404040, 0xFAFCFF);
    y += 20;

    uint32_t disk_mb = (g_ata_master.sectors * SECTOR_SIZE) / (1024 * 1024);
    gfx_draw_text(lx, y, "Disk", 0x506070, 0xFAFCFF);
    u32_to_str(disk_mb, buf);
    gfx_draw_text(vx, y, buf, 0x1E4E8C, 0xFAFCFF);
    gfx_draw_text(vx + (int)strlen(buf) * 8 + 4, y, "MB  EPFS on ATA", 0x404040, 0xFAFCFF);
    y += 20;

    gfx_draw_text(lx, y, "CPU", 0x506070, 0xFAFCFF);
    gfx_draw_text(vx, y, "i686 32-bit protected mode", 0x202020, 0xFAFCFF);
    y += 20;

    gfx_draw_text(lx, y, "Display", 0x506070, 0xFAFCFF);
    gfx_draw_text(vx, y, "VBE 800x600x32", 0x202020, 0xFAFCFF);
    gfx_draw_text(vx + 112, y, "double-buffered", 0x404040, 0xFAFCFF);
    y += 20;

    gfx_draw_text(lx, y, "Uptime", 0x506070, 0xFAFCFF);
    u32_to_str(timer_get_uptime(), buf);
    gfx_draw_text(vx, y, buf, 0x202020, 0xFAFCFF);
    gfx_draw_text(vx + (int)strlen(buf) * 8 + 4, y, "s", 0x404040, 0xFAFCFF);
    y += 20;

    gfx_draw_round_rect(wx + 14, wc_y + h - 42, w - 28, 30, 8, 0xE8F0FA);
    gfx_draw_text(wx + 26, wc_y + h - 34, "100% From Scratch - Bootloader, Kernel, GUI, FS, Drivers", 0x1E4E8C, 0xE8F0FA);
}

static void render_help(int i) {
    int wx = wins[i].x, wc_y = wins[i].y;
    // 半透明渐变内容背景
    gfx_fill_soft_gradient(wx, wc_y, wins[i].w, wins[i].h, 8, 0xF7FAFE, 0xDCE9F6, 70);
    const char* lines[] = {
        "GUI shortcuts:",
        "  Click titlebar: drag window",
        "  [x]: close window",
        "  Taskbar: switch / hide window",
        "  Files window: click select,",
        "  double-click open, buttons op",
        "  Terminal: type commands",
        "  Save = write FS to ATA disk",
    };
    int y = wc_y + 8;
    for (int k = 0; k < 8; k++) {
        gfx_draw_text(wx + 10, y, lines[k], 0x202020, 0xFAFCFF);
        y += 18;
    }
}

static void render_settings(int i) {
    int wx = wins[i].x, wc_y = wins[i].y;
    int w = wins[i].w, h = wins[i].h;
    // 半透明渐变内容背景
    gfx_fill_soft_gradient(wx, wc_y, w, h, 8, 0xF7FAFE, 0xDCE9F6, 70);
    // 白色简约横幅
    gfx_fill_round_rect(wx + 8, wc_y + 6, w - 16, 38, 8, 0xE8F0FA);
    gfx_draw_text(wx + 18, wc_y + 11, "Settings", 0x1E4E8C, 0xE8F0FA);
    gfx_draw_text(wx + 18, wc_y + 28, "Desktop layout & system options", 0x506070, 0xE8F0FA);
    int y = wc_y + 56;
    int lx = wx + 14;
    int vx = wx + 130;
    // Appearance
    gfx_draw_text(lx, y, "Appearance", 0x1E4E8C, 0xFAFCFF);
    y += 20;
    gfx_draw_text(lx + 8, y, "Wallpaper", 0x506070, 0xFAFCFF);
    const char* wname = wallpaper_name(wallpaper_index);
    gfx_draw_text(vx, y, wname, 0x202020, 0xFAFCFF);
    y += 18;
    gfx_draw_text(lx + 8, y, "Theme", 0x506070, 0xFAFCFF);
    gfx_draw_text(vx, y, "White Minimal", 0x202020, 0xFAFCFF);
    // 壁纸切换按钮
    int byy = wc_y + 108;
    gfx_fill_round_rect(wx + 14, byy, 120, 24, 6, 0x3D7BD6);
    gfx_draw_text(wx + 26, byy + 5, "Next Wallpaper", 0xFFFFFF, 0x3D7BD6);
    y = byy + 34;
    // System
    gfx_draw_text(lx, y, "System", 0x1E4E8C, 0xFAFCFF);
    y += 20;
    gfx_draw_text(lx + 8, y, "Display", 0x506070, 0xFAFCFF);
    gfx_draw_text(vx, y, "VBE 800x600x32", 0x202020, 0xFAFCFF);
    y += 18;
    gfx_draw_text(lx + 8, y, "Power", 0x506070, 0xFAFCFF);
    gfx_draw_text(vx, y, "AC 100%", 0x202020, 0xFAFCFF);
    y += 18;
    gfx_draw_text(lx + 8, y, "Taskbar", 0x506070, 0xFAFCFF);
    gfx_draw_text(vx, y, "Bottom (always visible)", 0x202020, 0xFAFCFF);
    // 系统工具按钮: 垃圾清理 / 一键卸载 (距顶 224)
    int oy = wc_y + 224;
    gfx_fill_round_rect(wx + 14, oy, 130, 26, 6, 0x3D7BD6);
    gfx_draw_text(wx + 26, oy + 6, "Clean Garbage", 0xFFFFFF, 0x3D7BD6);
    gfx_fill_round_rect(wx + 154, oy, 92, 26, 6, 0xC0504D);
    gfx_draw_text(wx + 166, oy + 6, "Uninstall", 0xFFFFFF, 0xC0504D);
    // 状态/确认区 (距顶 258)
    int sy = wc_y + 258;
    gfx_fill_round_rect(wx + 14, sy, w - 28, 34, 8, 0xE8F0FA);
    if (settings_state == 1) {
        // 卸载确认
        gfx_draw_text(wx + 24, sy + 6, "Confirm uninstall?", 0xC0504D, 0xE8F0FA);
        gfx_fill_round_rect(wx + 160, sy + 5, 44, 24, 6, 0xC0504D);
        gfx_draw_text(wx + 173, sy + 11, "Yes", 0xFFFFFF, 0xC0504D);
        gfx_fill_round_rect(wx + 210, sy + 5, 44, 24, 6, 0x9AA8BC);
        gfx_draw_text(wx + 223, sy + 11, "No", 0xFFFFFF, 0x9AA8BC);
    } else if (settings_state == 2) {
        // 卸载完成
        gfx_draw_text(wx + 24, sy + 6, "EpochOS uninstalled. Close QEMU,", 0xC0504D, 0xE8F0FA);
        gfx_draw_text(wx + 24, sy + 18, "then delete epochos.img", 0xC0504D, 0xE8F0FA);
    } else {
        gfx_draw_text(wx + 24, sy + 11,
                      settings_info[0] ? settings_info : "System ready",
                      0x1E4E8C, 0xE8F0FA);
    }
}

// ---------------- 内置应用渲染 (Win11 毛玻璃风格) ----------------
// 通用: 内容区白色毛玻璃背景 + 顶部应用横幅
static void app_panel(int i, const char* subtitle) {
    int wx = wins[i].x, wc_y = wins[i].y;
    int w = wins[i].w, h = wins[i].h;
    gfx_fill_soft_gradient(wx, wc_y, w, h, 8, 0xF7FAFE, 0xDCE9F6, 70);
    gfx_fill_round_rect(wx + 8, wc_y + 6, w - 16, 30, 8, 0xE8F0FA);
    gfx_draw_text(wx + 18, wc_y + 11, wins[i].title, 0x1E4E8C, 0xE8F0FA);
    if (subtitle) gfx_draw_text(wx + 18, wc_y + 22, subtitle, 0x506070, 0xE8F0FA);
}

// 系统管家: 系统概览 + 一键优化
static void render_manager(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "System status & optimization");
    char buf[32];
    int y = wc_y + 48;
    int lx = wx + 14, vx = wx + 120;
    gfx_draw_text(lx, y, "CPU", 0x506070, 0xFAFCFF);
    gfx_draw_text(vx, y, "i686 32-bit @ PIT 100Hz", 0x202020, 0xFAFCFF);
    y += 18;
    gfx_draw_text(lx, y, "Memory", 0x506070, 0xFAFCFF);
    u32_to_str(mm_used_pages() * 4 / 1024, buf); strcat(buf, " / "); 
    char tmp[16]; u32_to_str(mm_total_pages() * 4 / 1024, tmp); strcat(buf, tmp); strcat(buf, " MB");
    gfx_draw_text(vx, y, buf, 0x202020, 0xFAFCFF);
    y += 18;
    gfx_draw_text(lx, y, "Tasks", 0x506070, 0xFAFCFF);
    u32_to_str(task_get_count(), buf);
    gfx_draw_text(vx, y, buf, 0x202020, 0xFAFCFF);
    y += 18;
    gfx_draw_text(lx, y, "Uptime", 0x506070, 0xFAFCFF);
    u32_to_str(timer_get_uptime(), buf); strcat(buf, " s");
    gfx_draw_text(vx, y, buf, 0x202020, 0xFAFCFF);
    y += 26;
    // 一键优化按钮
    gfx_fill_round_rect(wx + 14, y, 120, 26, 6, 0x3D7BD6);
    gfx_draw_text(wx + 26, y + 6, "Optimize", 0xFFFFFF, 0x3D7BD6);
    gfx_fill_round_rect(wx + 144, y, 120, 26, 6, 0x20B0A0);
    gfx_draw_text(wx + 156, y + 6, "Clean /tmp", 0xFFFFFF, 0x20B0A0);
    y += 40;
    gfx_fill_round_rect(wx + 14, y, w - 28, h - (y - wc_y) - 14, 8, 0xE8F0FA);
    gfx_draw_text(wx + 24, y + 6, wins[i].app_buf[0] ? wins[i].app_buf : "System healthy. Click Optimize to tune.",
                  0x1E4E8C, 0xE8F0FA);
}

// 浏览器: 本地站点 epoch://home / about / help / files
static void render_browser(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "EpochOS Web");
    const char* url = wins[i].app_buf[0] ? wins[i].app_buf : "epoch://home";
    int ay = wc_y + 44;
    gfx_fill_round_rect(wx + 8, ay, w - 16, 24, 6, 0xFFFFFF);
    gfx_draw_round_rect(wx + 8, ay, w - 16, 24, 6, wins[i].app_state ? 0x3D7BD6 : 0xA8B8CC);
    gfx_draw_text(wx + 16, ay + 5, url, 0x303030, 0xFFFFFF);
    // 页面区
    int py = ay + 34;
    gfx_fill_round_rect(wx + 8, py, w - 16, h - (py - wc_y) - 10, 8, 0xFAFCFF);
    int y = py + 12;
    if (strcmp(url, "epoch://about") == 0) {
        gfx_draw_text(wx + 20, y, "EpochOS v1.0 - from-scratch x86 OS", 0x1E4E8C, 0xFAFCFF);
        y += 20;
        gfx_draw_text(wx + 20, y, "CPU: i686 32-bit @ PIT 100Hz", 0x404040, 0xFAFCFF);
        y += 18;
        char buf[48]; u32_to_str(mm_used_pages() * 4 / 1024, buf);
        gfx_draw_text(wx + 20, y, "Memory: ", 0x404040, 0xFAFCFF);
        gfx_draw_text(wx + 88, y, buf, 0x404040, 0xFAFCFF);
        gfx_draw_text(wx + 88 + (int)strlen(buf) * 8, y, " MB used", 0x404040, 0xFAFCFF);
        y += 18;
        u32_to_str(task_get_count(), buf);
        gfx_draw_text(wx + 20, y, "Tasks: ", 0x404040, 0xFAFCFF);
        gfx_draw_text(wx + 88, y, buf, 0x404040, 0xFAFCFF);
        y += 18;
        u32_to_str(timer_get_uptime(), buf);
        gfx_draw_text(wx + 20, y, "Uptime: ", 0x404040, 0xFAFCFF);
        gfx_draw_text(wx + 88, y, buf, 0x404040, 0xFAFCFF);
        gfx_draw_text(wx + 88 + (int)strlen(buf) * 8, y, " s", 0x404040, 0xFAFCFF);
    } else if (strcmp(url, "epoch://help") == 0) {
        static const char* hl[] = {
            "GUI shortcuts:",
            "  Click titlebar: drag window",
            "  [x]: close window",
            "  Taskbar: switch / hide window",
            "  Files: click select, double-click open",
            "  Terminal: type commands",
            "  Desktop right-click: quick menu",
            "  Save FS = write to ATA disk",
            "Apps: Snake/2048/Mines/Brick/Paint/Notes",
            "UnitConv/Stopwatch/HexView in Store",
        };
        for (int k = 0; k < 10; k++) {
            gfx_draw_text(wx + 20, y, hl[k], 0x404040, 0xFAFCFF);
            y += 18;
        }
    } else if (strcmp(url, "epoch://files") == 0) {
        gfx_draw_text(wx + 20, y, "Files on ramfs (name size):", 0x1E4E8C, 0xFAFCFF);
        y += 18;
        static char br_fs[512];
        int n = fs_list(br_fs, sizeof(br_fs) - 1);
        if (n > 0) {
            char* tok = br_fs;
            while (tok && *tok && y < py + h - 24) {
                char* nl = strchr(tok, '\n');
                if (nl) *nl = 0;
                gfx_draw_text(wx + 20, y, tok, 0x404040, 0xFAFCFF);
                y += 16;
                tok = nl ? nl + 1 : 0;
            }
        } else {
            gfx_draw_text(wx + 20, y, "(empty)", 0x506070, 0xFAFCFF);
        }
    } else if (strcmp(url, "epoch://home") == 0) {
        gfx_draw_text(wx + 20, y, "Welcome to EpochOS Browser", 0x1E4E8C, 0xFAFCFF);
        y += 20;
        gfx_draw_text(wx + 20, y, "Type an epoch:// address and press Enter.", 0x404040, 0xFAFCFF);
        y += 16;
        gfx_draw_text(wx + 20, y, "Available: epoch://home  epoch://about", 0x404040, 0xFAFCFF);
        y += 16;
        gfx_draw_text(wx + 20, y, "epoch://help  epoch://files", 0x404040, 0xFAFCFF);
    } else {
        gfx_draw_text(wx + 20, y, "404 Not Found", 0xC05040, 0xFAFCFF);
        y += 18;
        gfx_draw_text(wx + 20, y, url, 0x404040, 0xFAFCFF);
        y += 18;
        gfx_draw_text(wx + 20, y, "Type epoch://home / about / help / files", 0x506070, 0xFAFCFF);
    }
}

// 翻译: 内置中英词条查表 (>=20)
static void render_translate(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "English <-> Chinese dictionary");
    // 输入框
    int y = wc_y + 48;
    gfx_fill_round_rect(wx + 10, y, w - 20, 26, 6, 0xFFFFFF);
    gfx_draw_round_rect(wx + 10, y, w - 20, 26, 6, 0xA8B8CC);
    gfx_draw_text(wx + 18, y + 6, wins[i].app_buf[0] ? wins[i].app_buf : "Type word, press Enter",
                  0x303030, 0xFFFFFF);
    // 结果区
    y += 38;
    gfx_fill_round_rect(wx + 10, y, w - 20, 56, 8, 0xE8F0FA);
    gfx_draw_text(wx + 20, y + 6, "Result:", 0x506070, 0xE8F0FA);
    if (wins[i].app_sel >= 0) {
        const char* wd = tr_words[wins[i].app_sel][0];
        gfx_draw_text(wx + 20, y + 24, wd, 0x1E4E8C, 0xE8F0FA);
        gfx_draw_text(wx + 20 + (int)strlen(wd) * 8 + 8, y + 24, "->", 0x506070, 0xE8F0FA);
        gfx_draw_text(wx + 20 + (int)strlen(wd) * 8 + 32, y + 24, tr_words[wins[i].app_sel][1], 0xC0504D, 0xE8F0FA);
    } else if (wins[i].app_sel == -2) {
        gfx_draw_text(wx + 20, y + 24, "not found: 未收录", 0xC05040, 0xE8F0FA);
    } else {
        gfx_draw_text(wx + 20, y + 24, "Enter an English word to look up.", 0x506070, 0xE8F0FA);
    }
    // 词条列表
    y += 70;
    gfx_fill_round_rect(wx + 10, y, w - 20, h - (y - wc_y) - 12, 8, 0xFAFCFF);
    gfx_draw_text(wx + 20, y + 6, "Built-in words:", 0x506070, 0xFAFCFF);
    int lx = wx + 20, ly = y + 24;
    int cnt = 0;
    for (int k = 0; k < 24; k++) {
        gfx_draw_text(lx, ly, tr_words[k][0], 0x303030, 0xFAFCFF);
        lx += (int)strlen(tr_words[k][0]) * 8 + 14;
        if (lx > wx + w - 60 || ++cnt % 6 == 0) { lx = wx + 20; ly += 16; }
    }
}

// ---- Converter 辅助: 真实 ramfs 格式转换 ----

// TXT -> MD: 非空行前加 "> " 引用标记, 空行保留
static int conv_txt2md(const uint8_t* in, int n, uint8_t* out, int max) {
    int oi = 0, pi = 0;
    while (pi < n && oi < max) {
        int e = pi;
        while (e < n && in[e] != '\n') e++;
        int len = e - pi;
        if (len > 0) {
            if (oi < max - len - 3) {
                out[oi++] = '>'; out[oi++] = ' ';
                for (int k = 0; k < len; k++) out[oi++] = in[pi + k];
            }
        }
        if (e < n && oi < max) out[oi++] = '\n';
        pi = e + 1;
    }
    return oi;
}

// MD -> HTML: # / ## 标题, - 列表, > 引用, 空行 <br>, 其他 <p>
static int conv_md2html(const uint8_t* in, int n, uint8_t* out, int max) {
    int oi = 0, pi = 0;
    while (pi < n && oi < max) {
        int e = pi;
        while (e < n && in[e] != '\n') e++;
        int len = e - pi;
        const char* open = "<p>"; const char* close = "</p>";
        int skip = 0;
        if (len >= 3 && in[pi] == '#' && in[pi+1] == '#' && in[pi+2] == ' ') { open = "<h2>"; close = "</h2>"; skip = 3; }
        else if (len >= 2 && in[pi] == '#' && in[pi+1] == ' ') { open = "<h1>"; close = "</h1>"; skip = 2; }
        else if (len >= 2 && in[pi] == '-' && in[pi+1] == ' ') { open = "<li>"; close = "</li>"; skip = 2; }
        else if (len >= 2 && in[pi] == '>' && in[pi+1] == ' ') { open = "<blockquote>"; close = "</blockquote>"; skip = 2; }
        else if (len == 0) { open = "<br>"; close = ""; }
        // 写开标签
        while (*open && oi < max) out[oi++] = (uint8_t)*open++;
        if (len > 0) {
            int body = len - skip;
            if (body > 0) {
                for (int k = 0; k < body && oi < max; k++) out[oi++] = in[pi + skip + k];
            }
        }
        while (*close && oi < max) out[oi++] = (uint8_t)*close++;
        if (e < n && oi < max) out[oi++] = '\n';
        pi = e + 1;
    }
    return oi;
}

// 文本转大写
static int conv_upper(const uint8_t* in, int n, uint8_t* out, int max) {
    int oi = 0;
    for (int k = 0; k < n && oi < max; k++) {
        uint8_t c = in[k];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[oi++] = c;
    }
    return oi;
}

// 文本转 HEX: 每字节两位十六进制 + 空格
static int conv_hex(const uint8_t* in, int n, uint8_t* out, int max) {
    static const char* hexd = "0123456789ABCDEF";
    int oi = 0;
    for (int k = 0; k < n && oi < max - 3; k++) {
        out[oi++] = (uint8_t)hexd[in[k] >> 4];
        out[oi++] = (uint8_t)hexd[in[k] & 15];
        out[oi++] = ' ';
    }
    return oi;
}

// 格式转换器: 基于 ramfs 的真实 TXT/MD 转换
static void render_convert(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "File format converter");
    int y = wc_y + 48;
    static const char* fmts[4] = {"TXT -> MD", "MD -> HTML", "TXT -> UPPER", "TXT -> HEX"};
    static const char* fsrc[4] = {"/doc.txt", "/doc.md", "/doc.txt", "/doc.txt"};
    static const char* fdst[4] = {"/doc.md", "/doc.html", "/doc.upper", "/doc.hex"};
    for (int k = 0; k < 4; k++) {
        int ry = y + k * 30;
        uint32_t bg = (wins[i].app_sel == k) ? 0xCFE4F8 : 0xFFFFFF;
        uint32_t fg = (wins[i].app_sel == k) ? 0x103060 : 0x202020;
        gfx_fill_round_rect(wx + 10, ry, w - 20, 26, 6, bg);
        gfx_draw_text(wx + 20, ry + 6, fmts[k], fg, bg);
        char info[48];
        if (fs_exists(fsrc[k])) {
            u32_to_str(fs_get_size(fsrc[k]), info);
            gfx_draw_text(wx + w - 130, ry + 6, info, 0x8090A0, bg);
            gfx_draw_text(wx + w - 130 + (int)strlen(info) * 8, ry + 6, "B", 0x8090A0, bg);
        } else {
            gfx_draw_text(wx + w - 90, ry + 6, "missing", 0x90A0B0, bg);
        }
    }
    // 转换按钮
    int by = wc_y + h - 44;
    gfx_fill_round_rect(wx + 10, by, 100, 28, 6, 0x3D7BD6);
    gfx_draw_text(wx + 26, by + 7, "Convert", 0xFFFFFF, 0x3D7BD6);
    gfx_fill_round_rect(wx + 120, by, w - 130, 28, 8, 0xE8F0FA);
    gfx_draw_text(wx + 130, by + 7, wins[i].app_buf[0] ? wins[i].app_buf : "Ready",
                  0x1E4E8C, 0xE8F0FA);
}

// 截图工具
static void render_shot(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Screen capture");
    int y = wc_y + 48;
    gfx_draw_text(wx + 16, y, "Capture the whole screen to /tmp/shot.img", 0x303030, 0xFAFCFF);
    y += 24;
    gfx_fill_round_rect(wx + 14, y, 120, 28, 6, 0x3D7BD6);
    gfx_draw_text(wx + 30, y + 7, "Capture", 0xFFFFFF, 0x3D7BD6);
    gfx_fill_round_rect(wx + 144, y, 120, 28, 6, 0x20B0A0);
    gfx_draw_text(wx + 158, y + 7, "Open Photo", 0xFFFFFF, 0x20B0A0);
    y += 44;
    gfx_fill_round_rect(wx + 14, y, w - 28, h - (y - wc_y) - 14, 8, 0xE8F0FA);
    gfx_draw_text(wx + 24, y + 8, wins[i].app_buf[0] ? wins[i].app_buf : "Click Capture to take a screenshot.",
                  0x1E4E8C, 0xE8F0FA);
}

// 照片查看器
static void render_photo(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Photo viewer");
    int y = wc_y + 44;
    char buf[64];
    char num[8];
    // 统计 /img 目录图片数
    int n = 0;
    char lbuf[1024];
    int r = fs_list_dir("/img", lbuf, sizeof(lbuf));
    if (r > 0) n = r;
    gfx_draw_text(wx + 16, y, "Photo gallery", 0x1E4E8C, 0xFAFCFF);
    y += 20;
    uitoa(n, num, 10);
    strcpy(buf, "Found "); strcat(buf, num); strcat(buf, " image(s) in /img");
    gfx_draw_text(wx + 16, y, buf, 0x404040, 0xFAFCFF);
    y += 26;
    // 操作按钮: 加载截图 / 清理
    gfx_fill_round_rect(wx + 14, y, 150, 26, 6, 0x3D7BD6);
    gfx_draw_text(wx + 28, y + 6, "Open snapshot", 0xFFFFFF, 0x3D7BD6);
    gfx_fill_round_rect(wx + 172, y, 120, 26, 6, 0xE08030);
    gfx_draw_text(wx + 186, y + 6, "Clear", 0xFFFFFF, 0xE08030);
    y += 36;
    // 显示区
    int vw = w - 28, vh = h - (y - wc_y) - 12;
    if (vh < 40) vh = 40;
    gfx_fill_round_rect(wx + 14, y, vw, vh, 8, 0x101418);
    if (photo_loaded) {
        // 最近邻缩放显示 800x600 -> vw x vh (保持比例)
        int dw = vw - 8, dh = vh - 8;
        if (dw > 800) dw = 800;
        if (dh > 600) dh = 600;
        int sw = 800, sh = 600;
        if (dw * sh > dh * sw) { sw = sw * dh / sh; sh = dh; }
        else { sh = sh * dw / sw; sw = dw; }
        int ox = wx + 14 + (vw - sw) / 2, oy = y + (vh - sh) / 2;
        int sy, sx;
        for (sy = 0; sy < sh; sy++) {
            const uint8_t* src = photo_raw + (uint32_t)(sy * 600 / sh) * 800 * 4;
            uint32_t* dst = (uint32_t*)gfx_draw_target() + (uint32_t)(oy + sy) * gfx_width() + (uint32_t)ox;
            for (sx = 0; sx < sw; sx++) {
                const uint8_t* p = src + (uint32_t)(sx * 800 / sw) * 4;
                dst[sx] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            }
        }
        gfx_draw_text(wx + 18, y + 4, "Snapshot loaded", 0x9FB8D0, 0x101418);
    } else {
        gfx_draw_glow_spot(wx + w/2, y + 60, 90, 0x3D7BD6, 60);
        gfx_draw_glow_spot(wx + w/2 + 50, y + 110, 70, 0x8A5AC0, 50);
        gfx_draw_text(wx + w/2 - 55, y + (vh)/2 - 8,
                      photo_err ? "No snapshot - use Screen Capture" : "No image loaded",
                      0xA0B0C0, 0x101418);
    }
}

// 视频播放器
static void render_video(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Video player");
    int y = wc_y + 44;
    uint32_t now = timer_get_ticks();
    if (wins[i].app_scroll == 0) wins[i].app_scroll = (int)now;
    uint32_t el = wins[i].app_state ? 0u : (now - (uint32_t)wins[i].app_scroll);
    uint32_t prog = (el / 30) % 100;
    // 视频区
    gfx_fill_round_rect(wx + 10, y, w - 20, h - 110, 8, 0x101418);
    if (!wins[i].app_state) {
        int gx = wx + 30 + (int)(prog * (uint32_t)(w - 100) / 100);
        gfx_draw_glow_spot(gx, y + 80, 90, 0x2A6FD6, 50);
        gfx_draw_glow_spot(wx + w - 70 - (int)(prog * (uint32_t)(w - 140) / 100), y + 130, 60, 0x8A5AC0, 40);
        gfx_draw_text(wx + w/2 - 50, y + (h - 110)/2 - 8, "EpochOS Demo Video", 0xA0B8D0, 0x101418);
    } else {
        gfx_draw_glow_spot(wx + w/2, y + 80, 110, 0x2A6FD6, 50);
        gfx_draw_text(wx + w/2 - 42, y + (h - 110)/2 - 8, "[ Paused ]", 0xA0B8D0, 0x101418);
    }
    // 控制条
    int cy = wc_y + h - 58;
    gfx_fill_round_rect(wx + 10, cy, w - 20, 40, 8, 0xE8F0FA);
    gfx_fill_round_rect(wx + 20, cy + 16, w - 80, 4, 2, 0xB0C0D4);
    if (prog > 0)
        gfx_fill_round_rect(wx + 20, cy + 16, (w - 80) * (int)prog / 100, 4, 2, 0x3D7BD6);
    gfx_fill_round_rect(wx + 22, cy + 14, 8, 8, 4, 0x3D7BD6);
    char st[32];
    if (!wins[i].app_state) {
        strcpy(st, "Playing ");
        uitoa(prog, st + 8, 10);
        strcat(st, "%");
    } else strcpy(st, "Paused");
    gfx_draw_text(wx + 26, cy + 6, st, 0x1E4E8C, 0xE8F0FA);
}

// PDF 阅读器
static void render_pdf(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "PDF reader");
    int y = wc_y + 48;
    gfx_fill_round_rect(wx + 10, y, w - 20, h - 70, 8, 0xFAFCFF);
    const char* lines[] = {
        "EpochOS User Manual",
        "",
        "1. Double-click icons to open apps.",
        "2. Double-click desktop icons to open apps.",
        "3. Files app browses the ramdisk.",
        "4. Terminal accepts shell commands.",
        "5. Settings switches wallpaper.",
        "6. Save in Files writes to disk.",
        "",
        "Page 1 / 3"
    };
    int ly = y + 10;
    for (int k = 0; k < 10; k++) {
        gfx_draw_text(wx + 22, ly, lines[k], 0x202020, 0xFAFCFF);
        ly += 18;
    }
}

// 音乐播放器
static void render_music(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Music player");
    const char* songs[5] = {"01 Startup Theme", "02 Desktop Flow", "03 Kernel Tune", "04 Shell Beats", "05 Goodbye"};
    int y = wc_y + 48;
    for (int k = 0; k < 5; k++) {
        int ry = y + k * 26;
        uint32_t bg = (wins[i].app_sel == k) ? 0xCFE4F8 : 0xFFFFFF;
        uint32_t fg = (wins[i].app_sel == k) ? 0x103060 : 0x202020;
        gfx_fill_round_rect(wx + 10, ry, w - 20, 22, 5, bg);
        gfx_draw_text(wx + 20, ry + 4, songs[k], fg, bg);
    }
    // 控制按钮
    int by = wc_y + h - 44;
    gfx_fill_round_rect(wx + 10, by, 60, 28, 6, 0x3D7BD6);
    gfx_draw_text(wx + 24, by + 7, "Play", 0xFFFFFF, 0x3D7BD6);
    gfx_fill_round_rect(wx + 78, by, 60, 28, 6, 0x8A5AC0);
    gfx_draw_text(wx + 92, by + 7, "Stop", 0xFFFFFF, 0x8A5AC0);
    gfx_fill_round_rect(wx + 146, by, w - 156, 28, 8, 0xE8F0FA);
    gfx_draw_text(wx + 156, by + 7, wins[i].app_buf[0] ? wins[i].app_buf : "Select a song", 0x1E4E8C, 0xE8F0FA);
}

// 日历
static void render_calendar(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Calendar");
    int y = wc_y + 48;
    gfx_draw_text(wx + 16, y, "August 2026", 0x1E4E8C, 0xFAFCFF);
    y += 22;
    const char* dow = "Mo Tu We Th Fr Sa Su";
    gfx_draw_text(wx + 16, y, dow, 0x506070, 0xFAFCFF);
    y += 20;
    // 2026-08-01 是周六: 前面 5 个空位
    int days = 31, start = 5;
    int dx = wx + 16, dy = y;
    for (int k = 0; k < start; k++) {
        gfx_draw_text(dx + k * 26, dy, "  ", 0xC0C8D4, 0xFAFCFF);
    }
    for (int d = 1; d <= days; d++) {
        int col = (start + d - 1) % 7;
        int row = (start + d - 1) / 7;
        char num[4]; uitoa(d, num, 10);
        uint32_t fg = (d == 20) ? 0xFFFFFF : 0x202020;
        uint32_t bg = (d == 20) ? 0x3D7BD6 : 0xFAFCFF;
        if (d == 20) {
            gfx_fill_round_rect(dx + col * 26 - 1, dy + row * 20 - 1, 20, 18, 4, bg);
        }
        gfx_draw_text(dx + col * 26, dy + row * 20, num, fg, bg);
    }
}

// 文本编辑器
static void render_editor(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Text editor");
    int y = wc_y + 44;
    // 编辑区 (白底)
    gfx_fill_round_rect(wx + 8, y, w - 16, h - 92, 6, 0xFFFFFF);
    gfx_draw_text(wx + 14, y + 6, wins[i].app_buf[0] ? wins[i].app_buf : "Type text... (keyboard)",
                  0x202020, 0xFFFFFF);
    // 底部状态
    int sy = wc_y + h - 40;
    gfx_fill_round_rect(wx + 8, sy, 80, 26, 6, 0x3D7BD6);
    gfx_draw_text(wx + 22, sy + 6, "Save", 0xFFFFFF, 0x3D7BD6);
    gfx_fill_round_rect(wx + 96, sy, w - 104, 26, 8, 0xE8F0FA);
    gfx_draw_text(wx + 106, sy + 6, wins[i].app_state ? "Saved to /doc.txt" : "Untitled", 0x1E4E8C, 0xE8F0FA);
}

// 闹钟
static void render_alarm(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Alarm clock");
    // 当前时间
    struct rtc_time rt;
    rtc_read(&rt);
    char buf[32];
    uitoa(rt.hour, buf, 10);
    if (rt.hour < 10) { buf[1] = buf[0]; buf[0] = '0'; buf[2] = 0; }
    strcat(buf, ":");
    char t2[8]; uitoa(rt.minute, t2, 10);
    if (rt.minute < 10) { t2[1] = t2[0]; t2[0] = '0'; t2[2] = 0; }
    strcat(buf, t2);
    strcat(buf, ":");
    uitoa(rt.second, t2, 10);
    if (rt.second < 10) { t2[1] = t2[0]; t2[0] = '0'; t2[2] = 0; }
    strcat(buf, t2);
    int y = wc_y + 54;
    gfx_draw_text(wx + w/2 - 40, y, buf, 0x1E4E8C, 0xFAFCFF);
    y += 40;
    gfx_draw_text(wx + 16, y, "Alarm set: 07:30", 0x404040, 0xFAFCFF);
    y += 22;
    gfx_fill_round_rect(wx + 14, y, 100, 26, 6, 0x3D7BD6);
    gfx_draw_text(wx + 26, y + 6, "Test Beep", 0xFFFFFF, 0x3D7BD6);
    y += 38;
    gfx_fill_round_rect(wx + 14, y, w - 28, h - (y - wc_y) - 12, 8, 0xE8F0FA);
    gfx_draw_text(wx + 24, y + 8, "Click Test Beep to play a tone.", 0x1E4E8C, 0xE8F0FA);
}

// 计算器

// ---------- 计算器: 定点(1e-3)四则表达式求值 (纯32位无溢出, 不依赖 libgcc) ----------
#define CALC_FIX 1000
typedef struct { unsigned hi, lo; } u64_t;
static u64_t u64_add(u64_t a, u64_t b) {
    u64_t r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo ? 1u : 0u);
    return r;
}
static u64_t u64_mul32(unsigned a, unsigned b) {
    unsigned a0 = a & 0xFFFF, a1 = a >> 16;
    unsigned b0 = b & 0xFFFF, b1 = b >> 16;
    u64_t r, t;
    r.lo = a0 * b0; r.hi = 0;
    t.lo = (a1 * b0) << 16; t.hi = (a1 * b0) >> 16; r = u64_add(r, t);
    t.lo = (a0 * b1) << 16; t.hi = (a0 * b1) >> 16; r = u64_add(r, t);
    t.lo = 0; t.hi = a1 * b1; r = u64_add(r, t);
    return r;
}
static unsigned u64_div_u32(u64_t a, unsigned d, unsigned* rem, unsigned* qhi) {
    unsigned qlo = 0, qh = 0, r = 0;
    int i;
    for (i = 31; i >= 0; i--) {
        r = (r << 1) | ((a.hi >> i) & 1);
        if (r >= d) { r -= d; qh |= (1u << i); }
    }
    for (i = 31; i >= 0; i--) {
        r = (r << 1) | ((a.lo >> i) & 1);
        if (r >= d) { r -= d; qlo |= (1u << i); }
    }
    if (rem) *rem = r;
    if (qhi) *qhi = qh;
    return qlo;
}
static int calc_mul_fix(long a, long b, long* out) {   // 返回 1=OK 0=溢出
    int neg = (a < 0) ^ (b < 0);
    unsigned ua = (a < 0) ? (unsigned)(-(long)a) : (unsigned)a;
    unsigned ub = (b < 0) ? (unsigned)(-(long)b) : (unsigned)b;
    u64_t p = u64_mul32(ua, ub);
    unsigned rem = 0, qhi = 0;
    unsigned q = u64_div_u32(p, CALC_FIX, &rem, &qhi);
    if (qhi != 0) return 0;
    long v = (long)q;
    *out = neg ? -v : v;
    return 1;
}
static int calc_div_fix(long a, long b, long* out) {   // 返回 1=OK 0=溢出 -1=除零
    if (b == 0) return -1;
    int neg = (a < 0) ^ (b < 0);
    unsigned ua = (a < 0) ? (unsigned)(-(long)a) : (unsigned)a;
    unsigned ub = (b < 0) ? (unsigned)(-(long)b) : (unsigned)b;
    if (ub == 0) return -1;
    u64_t p = u64_mul32(ua, CALC_FIX);
    unsigned rem = 0, qhi = 0;
    unsigned q = u64_div_u32(p, ub, &rem, &qhi);
    if (qhi != 0) return 0;
    long v = (long)q;
    *out = neg ? -v : v;
    return 1;
}
static const char* calc_src;
static int calc_pos;
static int calc_err;
static int calc_peek(void) {
    while (calc_src[calc_pos] == ' ') calc_pos++;
    return (unsigned char)calc_src[calc_pos];
}
static int calc_eat(int c) {
    if (calc_peek() == c) { calc_pos++; return 1; }
    return 0;
}
static long calc_expr(void);
static long calc_factor(void) {
    int c = calc_peek();
    if (c == '(') {
        long v;
        calc_pos++;
        v = calc_expr();
        if (!calc_eat(')')) calc_err = 1;
        return v;
    }
    if (c == '-') { calc_pos++; return -calc_factor(); }
    if (c == '+') { calc_pos++; return calc_factor(); }
    if (c >= '0' && c <= '9') {
        long v = 0;
        while (c >= '0' && c <= '9') {
            if (v > 200000) { calc_err = 3; return 0; }
            v = v * 10 + (c - '0');
            calc_pos++;
            c = calc_peek();
        }
        if (c == '.') {
            long frac = 0, scale = 1;
            calc_pos++;
            c = calc_peek();
            while (c >= '0' && c <= '9') {
                if (scale < CALC_FIX) { frac = frac * 10 + (c - '0'); scale *= 10; }
                calc_pos++;
                c = calc_peek();
            }
            v = v * CALC_FIX + frac * (CALC_FIX / scale);
        } else {
            v = v * CALC_FIX;
        }
        return v;
    }
    calc_err = 1;
    return 0;
}
static long calc_term(void) {
    long a = calc_factor();
    for (;;) {
        int c = calc_peek();
        if (c == '*') {
            long b, rr;
            calc_pos++;
            b = calc_factor();
            if (calc_err) return 0;
            if (!calc_mul_fix(a, b, &rr)) { calc_err = 3; return 0; }
            a = rr;
        } else if (c == '/') {
            long b, rr;
            int st;
            calc_pos++;
            b = calc_factor();
            if (calc_err) return 0;
            st = calc_div_fix(a, b, &rr);
            if (st < 0) { calc_err = 2; return 0; }
            if (st == 0) { calc_err = 3; return 0; }
            a = rr;
        } else break;
    }
    return a;
}
static long calc_expr(void) {
    long a = calc_term();
    for (;;) {
        int c = calc_peek();
        if (c == '+') {
            long b;
            calc_pos++;
            b = calc_term();
            if (calc_err) return 0;
            a += b;
        } else if (c == '-') {
            long b;
            calc_pos++;
            b = calc_term();
            if (calc_err) return 0;
            a -= b;
        } else break;
    }
    return a;
}
static void calc_fmt(long v, char* out) {
    if (v < 0) { *out++ = '-'; v = -v; }
    long ip = v / CALC_FIX, fp = v % CALC_FIX;
    uitoa((uint32_t)ip, out, 10);
    out += strlen(out);
    if (fp != 0) {
        int d3 = (int)(fp / 100), d2 = (int)((fp / 10) % 10), d1 = (int)(fp % 10);
        int keep = d1 ? 3 : (d2 ? 2 : (d3 ? 1 : 0));
        *out++ = '.';
        if (keep >= 3) *out++ = (char)('0' + d3);
        if (keep >= 2) *out++ = (char)('0' + d2);
        if (keep >= 1) *out++ = (char)('0' + d1);
        *out = 0;
    }
}
static int calc_eval(const char* expr, char* out, int outlen) {
    long v;
    calc_src = expr;
    calc_pos = 0;
    calc_err = 0;
    v = calc_expr();
    if (!calc_err && calc_peek() != 0) calc_err = 1;
    if (calc_err) {
        const char* m = (calc_err == 2) ? "Div by 0" : (calc_err == 3) ? "Overflow" : "Error";
        strncpy(out, m, (size_t)(outlen - 1));
        out[outlen - 1] = 0;
        return calc_err;
    }
    if (v > 1000000000L || v < -1000000000L) {
        strncpy(out, "Overflow", (size_t)(outlen - 1));
        out[outlen - 1] = 0;
        return 3;
    }
    calc_fmt(v, out);
    return 0;
}

static void render_calc(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Calculator");
    // 显示屏
    int dy = wc_y + 46;
    gfx_fill_round_rect(wx + 10, dy, w - 20, 40, 8, 0x20242C);
    char disp[32];
    if (wins[i].app_buf[0]) strcpy(disp, wins[i].app_buf);
    else { u32_to_str((uint32_t)wins[i].app_num, disp); }
    gfx_draw_text(wx + w - 60 - (int)strlen(disp) * 8, dy + 13, disp, 0xFFFFFF, 0x20242C);
    // 按键 4x5
    const char* keys[20] = {"7","8","9","/", "4","5","6","*", "1","2","3","-", "0","C","=","+", ".","(",")","Del"};
    int bw = (w - 28) / 4, bh = 26;
    int kx = wx + 10, ky = dy + 50;
    for (int k = 0; k < 20; k++) {
        int col = k % 4, row = k / 4;
        int bx = kx + col * (bw + 2), byy = ky + row * (bh + 2);
        if (byy + bh > wc_y + h - 8) break;
        uint32_t bg = (k >= 3 && k % 4 == 3) ? 0x3D7BD6 : ((k == 13) ? 0xE08030 : 0xE8F0FA);
        uint32_t fg = (k >= 3 && k % 4 == 3) ? 0xFFFFFF : 0x202020;
        gfx_fill_round_rect(bx, byy, bw, bh, 5, bg);
        gfx_draw_text(bx + (bw - (int)strlen(keys[k]) * 8) / 2, byy + (bh - 16) / 2 + 2, keys[k], fg, bg);
    }
}

// 应用商店: 真实安装/卸载 (fs 标记文件 + 桌面图标/窗口启用)
static void render_store(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "App store");
    int y = wc_y + 48;
    for (int k = 0; k < 12; k++) {
        if (y > wc_y + h - 70) break;
        int ry = y + k * 28;
        uint32_t bg = (wins[i].app_sel == k) ? 0xCFE4F8 : 0xFFFFFF;
        uint32_t fg = (wins[i].app_sel == k) ? 0x103060 : 0x202020;
        gfx_fill_round_rect(wx + 10, ry, w - 20, 24, 6, bg);
        gfx_draw_text(wx + 20, ry + 5, store_apps[k].name, fg, bg);
        int inst = app_is_installed(store_apps[k].type);
        gfx_draw_text(wx + 110, ry + 5, store_apps[k].desc, 0x506070, bg);
        gfx_draw_text(wx + w - 80, ry + 5, inst ? "Uninstall" : "Install", inst ? 0xC05040 : 0x3D7BD6, bg);
    }
    // 提示条
    int sy = wc_y + h - 36;
    gfx_fill_round_rect(wx + 10, sy, w - 20, 26, 8, 0xE8F0FA);
    gfx_draw_text(wx + 20, sy + 6, wins[i].app_buf[0] ? wins[i].app_buf : "Select an app, then click Install/Uninstall",
                  0x1E4E8C, 0xE8F0FA);
}

// 压缩工具
static void render_zip(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Archive manager");
    // 文件列表 (根目录)
    char lbuf[1024];
    int n = fs_list_dir("/", lbuf, sizeof(lbuf));
    int y = wc_y + 48;
    if (n < 0) {
        gfx_draw_text(wx + 16, y, "No files", 0x404040, 0xFAFCFF);
    } else {
        char* p = lbuf;
        int shown = 0;
        while (*p && shown < 6 && y < wc_y + h - 90) {
            char* line = p;
            while (*p && *p != '\n') p++;
            if (*p == '\n') *p++ = 0;
            if (line[0]) {
                gfx_draw_text(wx + 16, y, line, 0x202020, 0xFAFCFF);
                y += 18;
                shown++;
            }
        }
    }
    int by = wc_y + h - 44;
    gfx_fill_round_rect(wx + 10, by, 100, 28, 6, 0x3D7BD6);
    gfx_draw_text(wx + 24, by + 7, "Compress", 0xFFFFFF, 0x3D7BD6);
    gfx_fill_round_rect(wx + 120, by, 100, 28, 6, 0x20B0A0);
    gfx_draw_text(wx + 136, by + 7, "Extract", 0xFFFFFF, 0x20B0A0);
    gfx_fill_round_rect(wx + 230, by, w - 240, 28, 8, 0xE8F0FA);
    gfx_draw_text(wx + 240, by + 7, wins[i].app_buf[0] ? wins[i].app_buf : "Ready", 0x1E4E8C, 0xE8F0FA);
}

// IDE: 极简解释器 (print / 变量 / 算术 / if)
static void render_ide(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "EpochIDE - micro language");
    // 代码区
    int y = wc_y + 44;
    int code_h = h - 46 - 92;
    if (code_h < 60) code_h = 60;
    gfx_fill_round_rect(wx + 8, y, w - 16, code_h, 6, 0x101418);
    if (!wins[i].app_buf[0]) {
        // 预填示例代码 (真实可运行)
        strcpy(wins[i].app_buf,
               "x = 2 + 3 * 4\n"
               "print \"x = \", x\n"
               "if x > 10\n"
               "  print \"big\"\n"
               "endif\n"
               "print \"done\"");
    }
    int cy = y + 8;
    char* ln = wins[i].app_buf;
    int lno = 1;
    while (*ln && cy < y + code_h - 12) {
        char line[64]; int k = 0;
        while (*ln && *ln != '\n' && k < 63) line[k++] = *ln++;
        line[k] = 0;
        if (*ln == '\n') ln++;
        char nb[8]; itoa(lno, nb, 10);
        gfx_draw_text(wx + 16, cy, nb, 0x506070, 0x101418);
        gfx_draw_text(wx + 40, cy, line, 0xC8D4E0, 0x101418);
        cy += 16;
        lno++;
    }
    // 运行按钮
    int by = y + code_h + 8;
    gfx_fill_round_rect(wx + 10, by, 90, 28, 6, 0x3D7BD6);
    gfx_draw_text(wx + 30, by + 7, "Run", 0xFFFFFF, 0x3D7BD6);
    gfx_fill_round_rect(wx + 108, by, 120, 28, 6, 0x20B0A0);
    gfx_draw_text(wx + 126, by + 7, "Clear Out", 0xFFFFFF, 0x20B0A0);
    gfx_fill_round_rect(wx + 236, by, w - 246, 28, 8, 0xE8F0FA);
    gfx_draw_text(wx + 246, by + 7, ide_outn ? "Output below" : "Click Run to execute", 0x1E4E8C, 0xE8F0FA);
    // 输出区
    int oy = by + 34;
    gfx_fill_round_rect(wx + 8, oy, w - 16, h - (oy - wc_y) - 10, 6, 0x0E1418);
    if (ide_outn > 0) {
        int oy2 = oy + 6;
        char* s = ide_out + ide_out_scroll;
        while (*s && oy2 < oy + (h - (oy - wc_y) - 10) - 14) {
            char line[80]; int k = 0;
            while (*s && *s != '\n' && k < 79) line[k++] = *s++;
            line[k] = 0;
            if (*s == '\n') s++;
            gfx_draw_text(wx + 16, oy2, line, 0x9AD48A, 0x0E1418);
            oy2 += 16;
            if (!*s) break;
        }
    } else {
        gfx_draw_text(wx + 16, oy + 6, "(no output)", 0x506070, 0x0E1418);
    }
}

// 任务管理器
static void render_taskmgr(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Task manager (click a row to end task)");
    int y = wc_y + 48;
    // 内存统计 (实时)
    char mbuf[40];
    u32_to_str(mm_used_pages() * 4 / 1024, mbuf);
    strcat(mbuf, " / ");
    char tmp[16]; u32_to_str(mm_total_pages() * 4 / 1024, tmp); strcat(mbuf, tmp);
    strcat(mbuf, " MB");
    gfx_draw_text(wx + 14, y, "Memory", 0x506070, 0xFAFCFF);
    gfx_draw_text(wx + 90, y, mbuf, 0x202020, 0xFAFCFF);
    char cb[16]; u32_to_str(task_get_count(), cb);
    gfx_draw_text(wx + 200, y, "Tasks:", 0x506070, 0xFAFCFF);
    gfx_draw_text(wx + 258, y, cb, 0x202020, 0xFAFCFF);
    y += 20;
    gfx_draw_text(wx + 14, y, "ID  Name             State    Ticks   Action", 0x506070, 0xFAFCFF);
    y += 20;
    int n = task_get_count();
    if (n > TASK_MAX) n = TASK_MAX;
    for (int k = 0; k < n; k++) {
        const struct task* t = task_get(k);
        if (!t || t->state == TASK_EMPTY) continue;
        char buf[48];
        uitoa(t->id, buf, 10);
        strcat(buf, "   ");
        strcat(buf, t->name);
        int len = (int)strlen(buf);
        while (len < 24) { buf[len++] = ' '; }
        buf[len] = 0;
        if (t->state == TASK_RUNNING) strcat(buf, "running  ");
        else if (t->state == TASK_READY) strcat(buf, "ready    ");
        else strcat(buf, "exited   ");
        char tk[12]; uitoa(t->ticks_run, tk, 10);
        strcat(buf, tk);
        gfx_draw_text(wx + 14, y, buf, 0x202020, 0xFAFCFF);
        // Kill 按钮 (id>0 才可结束)
        if (t->id > 0) {
            int kx = wx + w - 62;
            uint32_t kb = 0xE85C5C, kf = 0xFFFFFF;
            if (cursor_x >= kx && cursor_x < kx + 52 &&
                cursor_y >= y - 1 && cursor_y < y + 15) { kb = 0xF08080; }
            gfx_fill_round_rect(kx, y - 1, 52, 16, 4, kb);
            gfx_draw_text(kx + 12, y + 1, "Kill", kf, kb);
        }
        y += 18;
    }
}

// 磁盘管理
static void render_disk(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Disk manager");
    int y = wc_y + 48;
    int lx = wx + 14, vx = wx + 120;
    gfx_draw_text(lx, y, "Disk", 0x506070, 0xFAFCFF);
    gfx_draw_text(vx, y, "ATA PIO (Primary Master)", 0x202020, 0xFAFCFF);
    y += 18;
    gfx_draw_text(lx, y, "Sectors", 0x506070, 0xFAFCFF);
    gfx_draw_text(vx, y, "LBA 0 - 4095 (2 MB)", 0x202020, 0xFAFCFF);
    y += 18;
    gfx_draw_text(lx, y, "FS region", 0x506070, 0xFAFCFF);
    gfx_draw_text(vx, y, "LBA 1024+ (ramdisk image)", 0x202020, 0xFAFCFF);
    y += 18;
    gfx_draw_text(lx, y, "Wallpaper", 0x506070, 0xFAFCFF);
    gfx_draw_text(vx, y, "LBA 1596+ (4 wallpapers)", 0x202020, 0xFAFCFF);
    y += 18;
    gfx_draw_text(lx, y, "Files", 0x506070, 0xFAFCFF);
    char buf[24]; uitoa(fs_list(buf, sizeof(buf)), buf, 10);
    gfx_draw_text(vx, y, buf, 0x202020, 0xFAFCFF);
    y += 26;
    gfx_fill_round_rect(wx + 14, y, 120, 26, 6, 0x3D7BD6);
    gfx_draw_text(wx + 26, y + 6, "Save FS", 0xFFFFFF, 0x3D7BD6);
    gfx_fill_round_rect(wx + 144, y, w - 158, 26, 8, 0xE8F0FA);
    gfx_draw_text(wx + 154, y + 6, wins[i].app_buf[0] ? wins[i].app_buf : "FS ready", 0x1E4E8C, 0xE8F0FA);
}

// 网络管理器
static void render_net(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Network manager");
    int y = wc_y + 48;
    int lx = wx + 14, vx = wx + 120;
    gfx_draw_text(lx, y, "Adapter", 0x506070, 0xFAFCFF);
    gfx_draw_text(vx, y, "EpochNET (loopback)", 0x202020, 0xFAFCFF);
    y += 18;
    gfx_draw_text(lx, y, "Status", 0x506070, 0xFAFCFF);
    gfx_draw_text(vx, y, wins[i].app_state ? "Disconnected" : "Connected", 0x202020, 0xFAFCFF);
    y += 18;
    gfx_draw_text(lx, y, "IP", 0x506070, 0xFAFCFF);
    gfx_draw_text(vx, y, "10.0.0.2 / 255.255.255.0", 0x202020, 0xFAFCFF);
    y += 18;
    gfx_draw_text(lx, y, "TX / RX", 0x506070, 0xFAFCFF);
    gfx_draw_text(vx, y, "0 B / 0 B", 0x202020, 0xFAFCFF);
    y += 26;
    gfx_fill_round_rect(wx + 14, y, 100, 26, 6, 0x3D7BD6);
    gfx_draw_text(wx + 26, y + 6, "Refresh", 0xFFFFFF, 0x3D7BD6);
    gfx_fill_round_rect(wx + 124, y, w - 138, 26, 8, 0xE8F0FA);
    gfx_draw_text(wx + 134, y + 6, "Loopback only (demo)", 0x1E4E8C, 0xE8F0FA);
}

// 白色简约毛玻璃标题栏: 圆角 + 浅蓝边框 + 标题文字 + 右上角 [关闭][最大化][最小化]
static void render_window_title(int i) {
    int wx = wins[i].x, wy = win_top(i);
    int ww = wins[i].w;
    int act = (i == active_win);
    uint32_t border = act ? 0x9CC4EE : 0xD0D8E4;
    // 标题栏流体渐变毛玻璃 (半透明渐变, 免逐帧模糊)
    gfx_fill_soft_gradient(wx, wy, ww, TITLE_H, 6, 0xF9FCFF, 0xD3E2F4, 66);
    gfx_fill_alpha(wx, wy, ww, 2, 0xFFFFFF, 70);
    gfx_draw_round_rect(wx, wy, ww, TITLE_H, 6, border);
    // 标题文字 (无背景发光, 保持玻璃通透)
    int tw = (int)strlen(wins[i].title) * 8;
    gfx_draw_text_glow(wx + (ww - tw) / 2, wy + 4, wins[i].title,
                       act ? 0x1E4E8C : 0x506070, 0xF2F7FC);
    // 右上角三个按钮: [最小化][最大化][关闭] (从右往左)
    int bx = wx + ww - 10;
    int byy = wy + 3;
    int bw = 16, bh = TITLE_H - 6;
    // 关闭 (红色 hover)
    gfx_fill_round_rect(bx - bw - 2, byy, bw, bh, 4, 0xE8EFF8);
    gfx_draw_text(bx - bw - 2 + 5, byy + 3, "x", 0xC05050, 0xE8EFF8);
    bx -= bw + 8;
    // 最大化
    gfx_fill_round_rect(bx - bw - 2, byy, bw, bh, 4, 0xE8EFF8);
    gfx_draw_text(bx - bw - 2 + 5, byy + 3, "+", 0x506070, 0xE8EFF8);
    bx -= bw + 8;
    // 最小化
    gfx_fill_round_rect(bx - bw - 2, byy, bw, bh, 4, 0xE8EFF8);
    gfx_fill_rect(bx - bw - 2 + 5, byy + bh / 2 + 1, 6, 2, 0x506070);
}

static void render_taskbar(void) {
    if (!taskbar_dirty) return;
    taskbar_dirty = 0;
    int w = gfx_width(), h = gfx_height();
    int y = h - TASKBAR_H;
    // 毛玻璃 Dock 栏 (预渲染缓存)
    if (g_bar_cache_ok)
        blit_bar_cache(g_taskbar_cache, 0, y, w, TASKBAR_H);
    else {
        gfx_fill_soft_gradient(0, y, w, TASKBAR_H, 0, 0xF4F8FC, 0xC9DCF0, 80);
    }
    gfx_fill_rect(0, y, w, 1, 0xC8D4E4);
    // 应用图标 (统一 22px, 活跃窗口底部圆点指示); 分页: 每页 TASKBAR_PER_PAGE 个
    int page_max = TASKBAR_PER_PAGE;
    int pages = (TASKBAR_APPS + page_max - 1) / page_max;
    if (taskbar_page >= pages) taskbar_page = pages - 1;
    if (taskbar_page < 0) taskbar_page = 0;
    // 右侧保留翻页按钮区 (多页时): [<] [>]
    int nav_w = (pages > 1) ? 44 : 0;
    int avail = w - 12 - nav_w;
    int count = TASKBAR_APPS - taskbar_page * page_max;
    if (count > page_max) count = page_max;
    int bw = (avail - (count - 1) * 4) / count;
    if (bw > 30) bw = 30;
    if (bw < 16) bw = 16;
    int bx = 6 + (avail - count * bw - (count - 1) * 4) / 2;
    int start = taskbar_page * page_max;
    for (int i = 0; i < count; i++) {
        int app = start + i + TASKBAR_OFF;
        int act = 0;
        for (int k = 0; k < win_count; k++)
            if (wins[k].used && wins[k].visible && wins[k].type == desk_types[app]) { act = (k == active_win); break; }
        int icx = bx + (bw - 22) / 2;
        if (desk_icons[app]) {
            gfx_draw_rgba_icon_scaled(icx, y + 2, 22, 22, ICON_SIZE, ICON_SIZE, desk_icons[app]->rgb, desk_icons[app]->alpha);
        } else {
            uint32_t ic = 0x7FA8E8;
            if (desk_types[app] == WIN_TERMINAL) ic = 0x10A050;
            else if (desk_types[app] == WIN_FILES) ic = 0x3D7BD6;
            else if (desk_types[app] == WIN_TASKMGR) ic = 0x5A6AC0;
            else if (desk_types[app] == WIN_DISK) ic = 0x4A6A8A;
            else if (desk_types[app] == WIN_SNAKE) ic = 0x2E9E4F;
            else if (desk_types[app] == WIN_2048) ic = 0xD08A2E;
            else if (desk_types[app] == WIN_MINES) ic = 0x6E7A8A;
            else if (desk_types[app] == WIN_BRICK) ic = 0xC0504D;
            else if (desk_types[app] == WIN_PAINT) ic = 0x8A5AC0;
            else if (desk_types[app] == WIN_NOTE) ic = 0xD0A02E;
            else if (desk_types[app] == WIN_UNIT) ic = 0x20A0A0;
            else if (desk_types[app] == WIN_STOPW) ic = 0x20B0A0;
            else if (desk_types[app] == WIN_HEXVIEW) ic = 0x2E5A8A;
            else if (desk_types[app] == WIN_TETRIS) ic = 0x3D6BD0;
            else if (desk_types[app] == WIN_TICTAC) ic = 0x8A5AC0;
            else if (desk_types[app] == WIN_MEMORY) ic = 0x2E9E4F;
            else if (desk_types[app] == WIN_FIND) ic = 0x3D7BD6;
            else if (desk_types[app] == WIN_BASECONV) ic = 0x4A7AA8;
            else if (desk_types[app] == WIN_RANDOM) ic = 0xC08A3E;
            else if (desk_types[app] == WIN_TEXTSTATS) ic = 0x8A6AC0;
            else if (desk_types[app] == WIN_SUDOKU) ic = 0x2E7BB0;
            else if (desk_types[app] == WIN_TYPING) ic = 0x20A050;
            else if (desk_types[app] == WIN_POMODORO) ic = 0xE06040;
            else if (desk_types[app] == WIN_GUESS) ic = 0xC08A20;
            else if (desk_types[app] == WIN_DICE) ic = 0x5050A0;
            else if (desk_types[app] == WIN_TODO) ic = 0x2E7BB0;
            else if (desk_types[app] == WIN_PASSGEN) ic = 0xC03A7E;
            else if (desk_types[app] == WIN_CLOCK) ic = 0x3576C8;
            gfx_fill_round_rect(icx + 4, y + 4, 14, 14, 4, ic);
        }
        if (act) {
            // 活跃指示: 毛玻璃圆角底 (半透明, Dock 通透) + 底部圆点
            gfx_fill_glass_round_rect(bx + 2, y + 2, bw - 4, TASKBAR_H - 4, 6, 1, 75);
            gfx_fill_round_rect(bx + bw / 2 - 2, y + TASKBAR_H - 5, 4, 4, 2, 0x3D7BD6);
        }
        bx += bw + 4;
    }
    // 翻页按钮
    if (pages > 1) {
        int ny = y + (TASKBAR_H - 18) / 2;
        int nx = w - 12 - 40;
        gfx_fill_round_rect(nx, ny, 18, 18, 4, 0xFFFFFF);
        gfx_fill_alpha(nx, ny, 18, 18, 0xFFFFFF, 150);
        gfx_draw_text(nx + 5, ny + 4, "<", 0x3D5A80, 0xFFFFFF);
        gfx_fill_round_rect(nx + 22, ny, 18, 18, 4, 0xFFFFFF);
        gfx_fill_alpha(nx + 22, ny, 18, 18, 0xFFFFFF, 150);
        gfx_draw_text(nx + 27, ny + 4, ">", 0x3D5A80, 0xFFFFFF);
        // 页码
        char pg[4];
        pg[0] = (char)('0' + taskbar_page + 1);
        pg[1] = '/';
        pg[2] = (char)('0' + pages);
        pg[3] = 0;
        gfx_draw_text(w - 12 - 40 - 34, y + 6, pg, 0x506070, 0xD0E0F4);
    }
}

// 状态栏时间缓存刷新 (每帧调用; 30 帧读一次 RTC, QEMU 下 RTC I/O 较慢)
static void status_time_tick(void) {
    if (g_dt_refresh > 0) { g_dt_refresh--; return; }
    g_dt_refresh = 30;
    struct rtc_time t;
    rtc_read(&t);
    const char* wdn = rtc_weekday_name(t.weekday);
    char date[32];
    rtc_format(date, sizeof(date));          // "YYYY-MM-DD HH:MM:SS"
    char dstr[24];
    int k = 0;
    dstr[0] = 0;
    if (date[5] && date[6] && date[8] && date[9]) {
        dstr[0] = date[5]; dstr[1] = date[6];
        dstr[2] = '-'; dstr[3] = date[8]; dstr[4] = date[9]; dstr[5] = 0;
    }
    k = 0;
    while (wdn[k] && k < 9) { g_dt_label[k] = wdn[k]; k++; }
    g_dt_label[k++] = ' ';
    for (int i = 0; dstr[i] && k < 20; i++) g_dt_label[k++] = dstr[i];
    g_dt_label[k] = 0;
    char old_time[6];
    for (int i = 0; i < 6; i++) old_time[i] = g_dt_time[i];
    g_dt_time[0] = date[11]; g_dt_time[1] = date[12];
    g_dt_time[2] = ':'; g_dt_time[3] = date[14]; g_dt_time[4] = date[15]; g_dt_time[5] = 0;
    for (int i = 0; i < 6; i++)
        if (g_dt_time[i] != old_time[i]) { status_clock_dirty = 1; break; }
}

// 右上角日期时间胶囊+电量绘制 (仅画笔刷, 不含背景恢复; 由整条/局部两条路径共用)
static void status_clock_paint(int w) {
    const char* label = g_dt_label;
    const char* tstr = g_dt_time;
    int tw1 = (int)strlen(label) * 8;
    int tw2 = (int)strlen(tstr) * 8;
    int pad = 10;
    int bw = pad + 18 + 6 + tw1 + 10 + tw2 + pad;
    int bx = w - bw - 8, by = 2, bh = STATUS_BAR_H - 4;
    // 日期时间胶囊 (毛玻璃半透明, 透出顶部状态栏壁纸)
    gfx_fill_glass_round_rect(bx, by, bw, bh, 10, 1, 70);
    // 电量图标: 静态电池造型, 填充约 80%
    int ex = bx + pad, ey = by + 7;
    gfx_draw_round_rect(ex, ey, 16, 14, 3, 0x506070);
    gfx_fill_rect(ex + 16, ey + 4, 2, 6, 0x506070);
    gfx_fill_round_rect(ex + 2, ey + 2, 12, 10, 2, 0x8FE8A0);   // 80% 填充
    gfx_fill_rect(ex + 2, ey + 2, 3, 10, 0x506070);              // 剩余 20% 留空
    // 日期(星期+月日)
    gfx_draw_text_glow(bx + pad + 24, by + 5, label, 0x2A3038, 0xA0C0F0);
    // 时间 (HH:MM)
    gfx_draw_text_glow(bx + pad + 24 + tw1 + 10, by + 5, tstr, 0x1E4E8C, 0xA0C0F0);
}

// 时钟变化: 仅局部重绘右上角胶囊区域 (不整条重绘, 缩小 swap 拷贝面积)
static void render_status_clock(void) {
    if (!status_clock_dirty) return;
    status_clock_dirty = 0;
    int w = gfx_width();
    const char* label = g_dt_label;
    const char* tstr = g_dt_time;
    int tw1 = (int)strlen(label) * 8;
    int tw2 = (int)strlen(tstr) * 8;
    int pad = 10;
    int bw = pad + 18 + 6 + tw1 + 10 + tw2 + pad;
    int bx = w - bw - 8;
    // 恢复胶囊周边毛玻璃, 再重画胶囊
    if (g_bar_cache_ok) {
        int rx0 = bx - 8; if (rx0 < 0) rx0 = 0;
        int rw0 = bw + 16; if (rx0 + rw0 > w) rw0 = w - rx0;
        blit_bar_cache(g_status_cache, rx0, 0, rw0, STATUS_BAR_H);
    } else {
        gfx_fill_glass_round_rect(bx - 8, 0, bw + 16, STATUS_BAR_H, 0, 1, 72);
        gfx_fill_alpha(bx - 8, 0, bw + 16, STATUS_BAR_H, 0xFFFFFF, 52);
    }
    status_clock_paint(w);
}

static void render_status_area(void) {
    if (!status_dirty) return;
    status_dirty = 0;
    int w = gfx_width();
    // 顶部状态栏: 毛玻璃半透明 (预渲染缓存, 避免每帧逐像素模糊), 左上角设置入口, 右上角日期/时间/电量
    if (g_bar_cache_ok)
        blit_bar_cache(g_status_cache, 0, 0, w, STATUS_BAR_H);
    else {
        gfx_fill_glass_round_rect(0, 0, w, STATUS_BAR_H, 0, 1, 72);
        gfx_fill_alpha(0, 0, w, STATUS_BAR_H, 0xFFFFFF, 52);
    }
    gfx_fill_rect(0, STATUS_BAR_H, w, 1, 0xC8D4E4);
    // 左上角: 设置齿轮图标 (毛玻璃圆角按钮, 透出顶部状态栏壁纸)
    int sx = 6, sy = (STATUS_BAR_H - 22) / 2;
    gfx_fill_glass_round_rect(sx - 4, sy - 4, 30, 30, 10, 1, 80);
    gfx_draw_rgba_icon_scaled(sx, sy, 22, 22, ICON_SIZE, ICON_SIZE, IC_SETTINGS, IC_SETTINGS_A);
    status_clock_paint(w);
}

static void render_cursor(void) {
    int x = cursor_x, y = cursor_y;
    // 使用用户提供的箭头光标素材 (背景已透明化), 尖端对齐光标坐标
    gfx_draw_rgba_icon_scaled(x, y, 28, 28, CURSOR_W, CURSOR_H, CURSOR_RGB, CURSOR_A);
}


static void gui_render(void) {
    uint32_t a0 = pit_now();
    static int dbg_rw = 0, dbg_rwt = -1;
    status_time_tick();          // 时间缓存按帧刷新 (30 帧读一次 RTC)
    // 任务栏窗口状态签名: 窗口激活/开关/类型变化时置位重绘
    int tb_sig = active_win + 1;
    for (int i = 0; i < win_count; i++)
        if (wins[i].used && wins[i].visible) tb_sig = tb_sig * 31 + wins[i].type;
    if (tb_sig != g_tb_sig) { g_tb_sig = tb_sig; taskbar_dirty = 1; }
    render_desktop();
    uint32_t a1 = pit_now();
    // 动画窗口: 每帧强制重绘
    tetris_tick();       // 俄罗斯方块下落计时
    mem_tick();          // 记忆翻牌翻回计时
    for (int i = 0; i < win_count; i++) {
        if (!wins[i].used || !wins[i].visible) continue;
        int tp = wins[i].type;
        if (tp == WIN_SNAKE) { if (snake_alive && !snake_paused) wins[i].dirty = 1; }
        else if (tp == WIN_BRICK) { if (brick_run) wins[i].dirty = 1; }
        else if (tp == WIN_STOPW) { if (stopw_run) wins[i].dirty = 1; }
        else if (tp == WIN_CLOCK) {
            // 时钟内容按秒变化: 秒表未运行时只在秒值跳变时重绘, 避免每帧整窗重建
            if (clk_sw_run) wins[i].dirty = 1;
            else {
                static int last_clk_sec = -1;
                struct rtc_time ct;
                rtc_read(&ct);
                if (ct.second != last_clk_sec) { last_clk_sec = ct.second; wins[i].dirty = 1; }
            }
        }
        else if (tp == WIN_POMODORO) { if (pom_run) wins[i].dirty = 1; }
        else if (tp == WIN_VIDEO) { wins[i].dirty = 1; }
        else if (tp == WIN_TETRIS) { if (tetris_running == 1) wins[i].dirty = 1; }
        else if (tp == WIN_MEMORY) { if (mem_wait) wins[i].dirty = 1; }
        else if (tp == WIN_PAINT) { if (g_mouse.buttons & MOUSE_LEFT_BUTTON) wins[i].dirty = 1; }
    }
    // 非激活窗口先画 (仅脏矩形), 激活窗口最后画 (置顶)
    for (int i = 0; i < win_count; i++) {
        if (wins[i].used && wins[i].visible && i != active_win && wins[i].dirty) { render_window(i); wins[i].dirty = 0; dbg_rw++; dbg_rwt = wins[i].type; }
    }
    if (active_win >= 0 && wins[active_win].used && wins[active_win].visible && wins[active_win].dirty) { render_window(active_win); wins[active_win].dirty = 0; dbg_rw++; dbg_rwt = wins[active_win].type; }
    uint32_t a2 = pit_now();
    render_taskbar();
    uint32_t a3 = pit_now();
    render_status_clock(); // 时钟变化时局部刷新右上角胶囊
    render_status_area();  // 桌面右上角状态区 (日期/电量)
    uint32_t a4 = pit_now();
    render_desktop_menu(); // 桌面右键菜单置顶
    uint32_t a5 = pit_now();
    render_cursor();
    uint32_t a6 = pit_now();
    gfx_swap();
    uint32_t a7 = pit_now();
    prof_desktop += (uint32_t)((a0 - a1) & 0xFFFF);
    prof_wins    += (uint32_t)((a1 - a2) & 0xFFFF);
    prof_task    += (uint32_t)((a2 - a3) & 0xFFFF);
    prof_status  += (uint32_t)((a3 - a4) & 0xFFFF);
    prof_menu    += (uint32_t)((a4 - a5) & 0xFFFF);
    prof_cur     += (uint32_t)((a5 - a6) & 0xFFFF);
    prof_swap    += (uint32_t)((a6 - a7) & 0xFFFF);
    prof_cnt++;
    if (prof_cnt >= 60) {
        struct rtc_time rt;
        rtc_read(&rt);
        int rt_sec = rt.second;
        serial_printf(COM1, "[prof] n=%d desk=%dus win=%dus task=%dus status=%dus menu=%dus cur=%dus swap=%dus avg=%dus rtc=%d rw=%d rwt=%d\n",
            (int)prof_cnt, (int)(prof_desktop/60*838/1000), (int)(prof_wins/60*838/1000),
            (int)(prof_task/60*838/1000), (int)(prof_status/60*838/1000),
            (int)(prof_menu/60*838/1000), (int)(prof_cur/60*838/1000),
            (int)(prof_swap/60*838/1000),
            (int)((prof_desktop+prof_wins+prof_task+prof_status+prof_menu+prof_cur+prof_swap)/60*838/1000), rt_sec,
            dbg_rw, dbg_rwt);
        prof_desktop=prof_wins=prof_task=prof_status=prof_menu=prof_cur=prof_swap=prof_cnt=0;
        dbg_rw = 0; dbg_rwt = -1;
    }
}

// ---------------- 窗口创建 ----------------

static int add_window(int type, const char* title, int x, int y, int w, int h) {
    if (win_count >= WIN_MAX) return -1;
    int i = win_count++;
    wins[i].used = 1;
    wins[i].dirty = 1;
    wins[i].px = x;
    wins[i].py = y;
    wins[i].type = type;
    strncpy(wins[i].title, title, 23);
    wins[i].title[23] = 0;
    wins[i].x = x;
    wins[i].y = y;
    wins[i].w = w;
    wins[i].h = h;
    wins[i].visible = 1;
    wins[i].maximized = 0;
    wins[i].r_x = x; wins[i].r_y = y; wins[i].r_w = w; wins[i].r_h = h;
    wins[i].file_scroll = 0;
    wins[i].file_sel = 0;
    strncpy(wins[i].file_dir, "/", FS_MAX_NAME - 1);
    wins[i].file_dir[FS_MAX_NAME - 1] = 0;
    wins[i].entry_count = 0;
    if (type == WIN_FILES) files_refresh(i);
    return i;
}

// ---------------- 主循环 ----------------

static void gui_welcome_anim(void) {
    int w = gfx_width(), h = gfx_height();
    // 开机画面: 直接显现, 无全屏黑盖淡入淡出 (消除"一闪一闪"观感)
    for (int t = 0; t < 6; t++) {
        // 渐变背景 + 双光晕
        gfx_fill_gradient_v(0, 0, w, h, 0x14263E, 0x081020);
        gfx_draw_glow_spot(w / 2, h / 2 - 20, 180, 0x2A6FD6, 42);
        gfx_draw_glow_spot(w / 2 + 40, h / 2 + 30, 120, 0x6A2BC4, 32);
        // 发光系统名 + 副标题
        gfx_draw_text_glow(w / 2 - 32, h / 2 - 44, "EpochOS", 0xFFFFFF, 0x4A8CE0);
        gfx_draw_text(w / 2 - 76, h / 2 - 12, "From-Scratch x86 OS", 0xA0C4E8, 0x0C1A30);
        gfx_swap();     // 双缓冲: 一次性提交整帧
        // 帧节流 ~33fps (nop 忙等+上限, 避免 timer 异常时 hlt 永久沉睡)
        uint32_t f0 = timer_get_ticks() + 3;
        uint32_t fg = 0;
        while ((int)(timer_get_ticks() - f0) < 0 && fg++ < 200000) {
            __asm__ __volatile__("nop");
        }
    }
}

void gui_run(void) {
    if (!gfx_available()) {
        // 无图形模式: 退回文本 Shell
        serial_write_str("[gui] no VBE, text shell fallback\n");
        shell_run();
        return;
    }
    // 创建窗口: 默认隐藏 Terminal, 默认显示 Files (桌面感更明显)
    int w_term = add_window(WIN_TERMINAL, "Terminal", 260, 30, 500, 350);
    int w_files = add_window(WIN_FILES, "Files", 240, 140, 520, 400);
    int w_about = add_window(WIN_ABOUT, "About", 150, 120, 380, 240);
    int w_help = add_window(WIN_HELP, "Help", 160, 130, 300, 180);
    int w_settings = add_window(WIN_SETTINGS, "Settings", 120, 90, 420, 300);
    wins[w_term].visible = 0;       // 终端默认隐藏
    wins[w_about].visible = 0;      // 关于默认隐藏
    wins[w_help].visible = 0;       // 帮助默认隐藏
    wins[w_settings].visible = 0;   // 设置默认隐藏
    wins[w_files].visible = 0;      // 桌面全屏显示: 文件浏览器默认隐藏, 双击图标再打开
    active_win = w_files;

    // 新增应用初始状态
    ts_clear();
    sd_load_puzzle(0);
    ty_load(0);
    pom_reset();
    gs_new();
    dc_side = 6;
    dc_result = 0;
    dc_hist_n = 0;

    cursor_x = gfx_width() / 2;
    cursor_y = gfx_height() / 2;

    // 双缓冲: 消除画面闪烁 (绘制写 backbuffer, 每帧 swap 到 LFB)
    gfx_backbuffer_init();
    // 预渲染状态栏/任务栏毛玻璃缓存 (backbuffer 需先有壁纸)
    wallpaper_blit();
    build_bar_caches();
    serial_printf(COM1, "[dbg] after backbuf ok\n");

    // 串口测试钩子: 接收 "c x y\n" 模拟点击, "s\n" 回执就绪 (供自动化验证)
    char test_line[48];
    int test_len = 0;
    serial_write(COM1, "[test] ready\n");

    // 开机欢迎动画
    gui_welcome_anim();

    uint32_t frame = timer_get_ticks();

    for (;;) {
        gui_test_poll();
        gui_handle_input();
        gui_render();   // 内部已提交 swap (脏矩形部分拷贝)
        // 帧节流 ~33fps (30ms; 优先 hlt 让出 CPU, 由 PIT 中断唤醒, 无中断时忙等兜底)
        frame += 3;
        uint32_t fg2 = 0;
        while ((int)(timer_get_ticks() - frame) < 0 && fg2++ < 200000) {
            __asm__ __volatile__("hlt");
        }
    }
}

// ================= 新增窗口: 小游戏与实用工具 =================

// 贪吃蛇
static void render_snake(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Snake - arrows move, R restart");
    uint32_t now = timer_get_ticks();
    if ((int)(now - snake_last) >= 9) { snake_last = now; snake_step(); }
    int ox = wx + 14, oy = wc_y + 46;
    int cw = (w - 28) / SNAKE_COLS, ch = 12;
    if (cw < 8) cw = 8;
    int bw = cw * SNAKE_COLS, bh = ch * SNAKE_ROWS;
    gfx_fill_round_rect(ox, oy, bw + 8, bh + 8, 6, 0xE8F0FA);
    gfx_fill_rect(ox + 4, oy + 4, bw, bh, 0x101418);
    // 食物
    gfx_fill_rect(ox + 4 + snake_fx * cw + 2, oy + 4 + snake_fy * ch + 2, cw - 4, ch - 4, 0xE05040);
    // 蛇身
    for (int k = 0; k < snake_len; k++) {
        uint32_t col = (k == 0) ? 0x40C060 : (0x208040 - k);
        gfx_fill_rect(ox + 4 + snake_body[k][0] * cw, oy + 4 + snake_body[k][1] * ch,
                      cw, ch, col);
    }
    // 得分 / 状态
    char sb[48];
    strcpy(sb, "Score: "); uitoa(snake_score, sb + 7, 10);
    gfx_draw_text(wx + 20, wc_y + 46 + bh + 16, sb, 0x1E4E8C, 0xFAFCFF);
    if (!snake_alive) {
        gfx_draw_text(wx + 20, wc_y + 46 + bh + 32, "Game Over - press R to restart", 0xC05040, 0xFAFCFF);
    } else if (snake_paused) {
        gfx_draw_text(wx + 20, wc_y + 46 + bh + 32, "Paused - press P to resume", 0x506070, 0xFAFCFF);
    }
}

// 2048
static void render_g2048(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "2048 - arrows merge, R restart");
    char sb[48]; strcpy(sb, "Score: "); uitoa(g2048_score, sb + 7, 10);
    gfx_draw_text(wx + 16, wc_y + 44, sb, 0x1E4E8C, 0xFAFCFF);
    int ox = wx + 16, oy = wc_y + 66;
    int cw = (w - 40) / 4, ch = (h - 118) / 4;
    if (cw < 20) cw = 20; if (ch < 20) ch = 20;
    static const uint32_t gcols[12] = {
        0xECE8E0, 0xE8D8A0, 0xE0C080, 0xD8A860,
        0xD09050, 0xC87840, 0xC06030, 0xB04830,
        0xA04028, 0x903820, 0x803018, 0x702810
    };
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int v = g2048[r][c];
            int px = ox + c * (cw + 4), py = oy + r * (ch + 4);
            uint32_t bg = v == 0 ? 0xD8D4CC : gcols[v - 1];
            gfx_fill_round_rect(px, py, cw, ch, 6, bg);
            if (v) {
                char num[8]; uitoa(1 << v, num, 10);
                gfx_draw_text(px + (cw - (int)strlen(num) * 8) / 2, py + (ch - 16) / 2,
                              num, 0xFFFFFF, bg);
            }
        }
    }
    if (g2048_over) {
        gfx_draw_text(wx + 16, wc_y + h - 30, "Game Over - press R to restart", 0xC05040, 0xFAFCFF);
    }
}

// 扫雷
static void render_mines(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Mines - left open, right flag");
    int ox = wx + 14, oy = wc_y + 46;
    int cw = 18, ch = 18;
    if (cw * MINES_N + 8 > w - 28) cw = (w - 36) / MINES_N;
    if (ch * MINES_M + 8 > h - 92) ch = (h - 100) / MINES_M;
    for (int y = 0; y < MINES_M; y++) {
        for (int x = 0; x < MINES_N; x++) {
            int px = ox + x * (cw + 1), py = oy + y * (ch + 1);
            uint32_t bg = mines_v[x][y] ? 0xD8D8D0 : 0xB8C6DA;
            gfx_fill_rect(px, py, cw, ch, bg);
            if (mines_f[x][y]) {
                gfx_draw_text(px + (cw - 8) / 2, py + (ch - 16) / 2, "F", 0xC05040, bg);
            } else if (mines_v[x][y]) {
                if (mines_g[x][y]) {
                    gfx_fill_rect(px, py, cw, ch, 0xC04040);
                } else {
                    int cnt = mines_count(x, y);
                    if (cnt > 0) {
                        char d[2]; d[0] = '0' + cnt; d[1] = 0;
                        gfx_draw_text(px + (cw - 8) / 2, py + (ch - 16) / 2, d, 0x1E4E8C, bg);
                    }
                }
            }
        }
    }
    // 状态行
    char st[64];
    if (mines_over == 2) strcpy(st, "Boom! R to restart");
    else if (mines_over == 1) strcpy(st, "You win! R to restart");
    else { strcpy(st, "Flags: "); uitoa(mines_flags, st + 7, 10); }
    gfx_draw_text(wx + 16, oy + MINES_M * (ch + 1) + 4, st, 0x1E4E8C, 0xFAFCFF);
}

// 打砖块
static void render_brick(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Brick - move paddle with mouse");
    // 挡板跟随鼠标 (窗口内容坐标)
    if (active_win == i) {
        int px = cursor_x - wx - 35;
        if (px < 4) px = 4;
        if (px + 70 > w - 8) px = w - 8 - 70;
        brick_paddle = px;
    }
    brick_step();
    // 砖块
    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            if (!brick_g[r][c]) continue;
            uint32_t col = (r % 2) ? 0xE0A050 : 0x5A9AD6;
            gfx_fill_round_rect(wx + 10 + c * (BRICK_CELL_W + 4),
                                wc_y + 50 + r * (BRICK_CELL_H + 4),
                                BRICK_CELL_W, BRICK_CELL_H, 5, col);
        }
    }
    // 挡板
    gfx_fill_round_rect(wx + brick_paddle, wc_y + 316, 70, 12, 5, 0x3D7BD6);
    // 球
    gfx_fill_rect(wx + (brick_bx >> 4), wc_y + (brick_by >> 4), 8, 8, 0xE05040);
    // 信息
    char sb[64]; strcpy(sb, "Score: "); uitoa(brick_score, sb + 7, 10);
    gfx_draw_text(wx + 14, wc_y + 340, sb, 0x1E4E8C, 0xFAFCFF);
    if (brick_over == 1) gfx_draw_text(wx + 100, wc_y + 340, "You win! R restart", 0x20B0A0, 0xFAFCFF);
    if (brick_over == 2) gfx_draw_text(wx + 100, wc_y + 340, "Lost! R restart", 0xC05040, 0xFAFCFF);
}

// 绘图板
static void render_paint(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Paint - drag to draw, C clear");
    static const uint32_t pal[6] = {0x202020, 0xD03030, 0x20A060, 0x3060D0, 0xD0A020, 0xE0E0E0};
    // 调色板
    int py = wc_y + 44;
    for (int k = 0; k < 6; k++) {
        int px = wx + 10 + k * 30;
        gfx_fill_round_rect(px, py, 26, 26, 5, pal[k]);
        if (k == paint_cur_col) gfx_draw_round_rect(px - 2, py - 2, 30, 30, 6, 0x3D7BD6);
    }
    // Clear 按钮
    int bxx = wx + 10 + 6 * 30 + 16;
    gfx_fill_round_rect(bxx, py, 64, 26, 6, 0xE06050);
    gfx_draw_text(bxx + 10, py + 6, "Clear", 0xFFFFFF, 0xE06050);
    // 画布区
    int cy = py + 32;
    int cw = w - 24, chh = h - (cy - wc_y) - 14;
    if (cw < 40) cw = 40; if (chh < 40) chh = 40;
    gfx_fill_round_rect(wx + 10, cy, cw, chh, 6, 0xFFFFFF);
    gfx_draw_round_rect(wx + 10, cy, cw, chh, 6, 0xA8B8CC);
    // 已绘制的线段 (画布坐标 = 窗口内容坐标)
    for (int k = 0; k < paint_n; k++) {
        gfx_draw_line(wx + paint_x0[k], cy + paint_y0[k],
                      wx + paint_x1[k], cy + paint_y1[k], paint_col[k]);
    }
    // 实时绘制
    if ((g_mouse.buttons & MOUSE_LEFT_BUTTON) && active_win == i) {
        int mx = cursor_x, my = cursor_y;
        int in = (mx >= wx + 10 && mx < wx + 10 + cw && my >= cy && my < cy + chh);
        if (in) {
            if (!paint_drawing && paint_n < PAINT_MAX - 1) {
                paint_x0[paint_n] = mx - wx;
                paint_y0[paint_n] = my - cy;
                paint_x1[paint_n] = mx - wx;
                paint_y1[paint_n] = my - cy;
                paint_col[paint_n] = pal[paint_cur_col];
                paint_n++;
                paint_drawing = 1;
            } else if (paint_drawing && paint_n > 0) {
                paint_x1[paint_n - 1] = mx - wx;
                paint_y1[paint_n - 1] = my - cy;
            }
        } else {
            paint_drawing = 0;
        }
        if (paint_n > 0) {
            gfx_draw_line(wx + paint_x0[paint_n - 1], cy + paint_y0[paint_n - 1],
                          wx + paint_x1[paint_n - 1], cy + paint_y1[paint_n - 1],
                          paint_col[paint_n - 1]);
        }
    } else if (paint_drawing) {
        paint_drawing = 0;
    }
    gfx_draw_text(wx + 16, cy + chh - 18, wins[i].app_buf[0] ? wins[i].app_buf : "",
                  0x506070, 0xFFFFFF);
}

// 便签
static void render_note(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Notes - type, S save, L load");
    int y = wc_y + 48;
    gfx_fill_round_rect(wx + 8, y, w - 16, h - 84, 6, 0xFFF8E0);
    gfx_draw_round_rect(wx + 8, y, w - 16, h - 84, 6, 0xE0C878);
    int cy = y + 6;
    char* ln = wins[i].app_buf;
    int shown = 0;
    while (*ln && shown < 14 && cy < y + h - 84 - 12) {
        char line[64]; int k = 0;
        while (*ln && *ln != '\n' && k < 63) line[k++] = *ln++;
        line[k] = 0;
        if (*ln == '\n') ln++;
        gfx_draw_text(wx + 14, cy, line, 0x403020, 0xFFF8E0);
        cy += 16;
        shown++;
    }
    int by = wc_y + h - 32;
    gfx_fill_round_rect(wx + 10, by, 64, 24, 6, 0x3D7BD6);
    gfx_draw_text(wx + 24, by + 5, "Save", 0xFFFFFF, 0x3D7BD6);
    gfx_fill_round_rect(wx + 82, by, 64, 24, 6, 0x20B0A0);
    gfx_draw_text(wx + 96, by + 5, "Load", 0xFFFFFF, 0x20B0A0);
    gfx_fill_round_rect(wx + 154, by, 60, 24, 6, 0xE08030);
    gfx_draw_text(wx + 166, by + 5, "Clear", 0xFFFFFF, 0xE08030);
    gfx_fill_round_rect(wx + 222, by, w - 232, 24, 8, 0xE8F0FA);
    gfx_draw_text(wx + 232, by + 5, wins[i].app_state ? "Saved" : "", 0x1E4E8C, 0xE8F0FA);
}

// 64/32 除法 (避免 libgcc __divdi3; 仅处理非负)
static int ll_div32(long long a, int b) {
    unsigned long long ua = (unsigned long long)a;
    unsigned long long ub = (unsigned long long)b;
    unsigned long long q = 0;
    for (int bit = 63; bit >= 0; bit--) {
        q = (q << 1) | ((ua >> bit) & 1ULL);
        if (q >= ub) q -= ub;
    }
    return (int)q;
}

// 单位换算
static void render_unit(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Unit converter");
    int y = wc_y + 48;
    // 分类按钮
    static const char* cats[3] = {"Length", "Weight", "Temp"};
    for (int k = 0; k < 3; k++) {
        int px = wx + 10 + k * 90;
        uint32_t bg = (unit_cat == k) ? 0x3D7BD6 : 0xE8F0FA;
        uint32_t fg = (unit_cat == k) ? 0xFFFFFF : 0x1E4E8C;
        gfx_fill_round_rect(px, y, 84, 24, 6, bg);
        gfx_draw_text(px + 12, y + 5, cats[k], fg, bg);
    }
    // 单位选项
    static const char* len_u[4] = {"mm", "cm", "m", "km"};
    static const char* wgt_u[4] = {"mg", "g", "kg", "t"};
    static const char* tmp_u[3] = {"C", "F", "K"};
    int un = (unit_cat == 2) ? 3 : 4;
    const char** uu = (unit_cat == 2) ? (const char**)tmp_u : ((unit_cat == 0) ? (const char**)len_u : (const char**)wgt_u);
    int y2 = y + 34;
    for (int k = 0; k < un; k++) {
        int px = wx + 10 + k * 84;
        uint32_t bg = (k == unit_from) ? 0x20B0A0 : 0xE8F0FA;
        uint32_t fg = (k == unit_from) ? 0xFFFFFF : 0x1E4E8C;
        gfx_fill_round_rect(px, y2, 78, 24, 6, bg);
        gfx_draw_text(px + 12, y2 + 5, uu[k], fg, bg);
    }
    gfx_draw_text(wx + 14, y2 + 8, "->", 0x506070, 0xFAFCFF);
    int y3 = y2 + 34;
    for (int k = 0; k < un; k++) {
        int px = wx + 10 + k * 84;
        uint32_t bg = (k == unit_to) ? 0xD08040 : 0xE8F0FA;
        uint32_t fg = (k == unit_to) ? 0xFFFFFF : 0x1E4E8C;
        gfx_fill_round_rect(px, y3, 78, 24, 6, bg);
        gfx_draw_text(px + 12, y3 + 5, uu[k], fg, bg);
    }
    // 输入与结果
    int y4 = y3 + 40;
    gfx_fill_round_rect(wx + 10, y4, w - 20, 28, 6, 0xFFFFFF);
    gfx_draw_round_rect(wx + 10, y4, w - 20, 28, 6, 0xA8B8CC);
    gfx_draw_text(wx + 18, y4 + 6, wins[i].app_buf[0] ? wins[i].app_buf : "0", 0x303030, 0xFFFFFF);
    int y5 = y4 + 38;
    // 换算
    char res[48]; res[0] = 0;
    if (wins[i].app_buf[0]) {
        int val = atoi(wins[i].app_buf);
        if (unit_cat == 2) {
            // 温度: 先转 C
            int c;
            if (unit_from == 0) c = val;
            else if (unit_from == 1) c = (val - 32) * 5 / 9;
            else c = val - 273;
            int out;
            if (unit_to == 0) out = c;
            else if (unit_to == 1) out = c * 9 / 5 + 32;
            else out = c + 273;
            itoa(out, res, 10);
        } else {
            const int* tab = (unit_cat == 0) ? unit_l : unit_w;
            long long v = (long long)val * tab[unit_from];
            itoa(ll_div32(v, tab[unit_to]), res, 10);
        }
        gfx_draw_text(wx + 14, y5, "=", 0x506070, 0xFAFCFF);
        gfx_draw_text(wx + 30, y5, res, 0x1E4E8C, 0xFAFCFF);
    } else {
        gfx_draw_text(wx + 14, y5, "Type a number above", 0x506070, 0xFAFCFF);
    }
}

// 秒表
static void render_stopw(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Stopwatch");
    uint32_t el = stopw_acc;
    if (stopw_run) el += timer_get_ticks() - stopw_start;
    char t[16];
    uint32_t cs = el % 100; el /= 100;
    uint32_t sec = el % 60; el /= 60;
    uint32_t min = el % 60;
    itoa((int)min, t, 10);
    if (min < 10) { t[1] = t[0]; t[0] = '0'; t[2] = 0; }
    char t2[8]; itoa((int)sec, t2, 10);
    if (sec < 10) { t2[1] = t2[0]; t2[0] = '0'; t2[2] = 0; }
    strcat(t, ":"); strcat(t, t2);
    strcat(t, ".");
    itoa((int)cs, t2, 10);
    if (cs < 10) { t2[1] = t2[0]; t2[0] = '0'; t2[2] = 0; }
    strcat(t, t2);
    gfx_draw_text(wx + w / 2 - 56, wc_y + 60, t, 0x1E4E8C, 0xFAFCFF);
    int by = wc_y + 120;
    gfx_fill_round_rect(wx + 14, by, 90, 28, 6, stopw_run ? 0xE08030 : 0x20B0A0);
    gfx_draw_text(wx + 30, by + 7, stopw_run ? "Stop" : "Start", 0xFFFFFF, stopw_run ? 0xE08030 : 0x20B0A0);
    gfx_fill_round_rect(wx + 112, by, 90, 28, 6, 0x3D7BD6);
    gfx_draw_text(wx + 128, by + 7, "Reset", 0xFFFFFF, 0x3D7BD6);
}

// 十六进制查看器
static void render_hexview(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Hex viewer");
    int y = wc_y + 46;
    gfx_fill_round_rect(wx + 8, y, w - 16, 26, 6, 0xFFFFFF);
    gfx_draw_text(wx + 16, y + 6, hex_name[0] ? hex_name : "/doc.txt", 0x303030, 0xFFFFFF);
    gfx_fill_round_rect(wx + w - 90, y, 80, 26, 6, 0x3D7BD6);
    gfx_draw_text(wx + w - 78, y + 6, "Load", 0xFFFFFF, 0x3D7BD6);
    // 内容区
    int cy = y + 34;
    int cw = w - 16, chh = h - (cy - wc_y) - 8;
    gfx_fill_round_rect(wx + 8, cy, cw, chh, 6, 0x101418);
    int rows = (hex_len + 15) / 16;
    int vis = chh / 16;
    if (hex_len == 0) {
        gfx_draw_text(wx + 18, cy + 8, "(no file loaded - click Load)", 0x506070, 0x101418);
    } else {
        int start = wins[i].app_scroll;
        if (start < 0) start = 0;
        if (start > rows - vis) start = rows - vis;
        if (start < 0) start = 0;
        for (int r = 0; r < vis && r + start < rows; r++) {
            int off = (r + start) * 16;
            char line[96]; line[0] = 0;
            char nb[16];
            // 偏移
            u32_to_str((uint32_t)off, nb);
            int nl = (int)strlen(nb);
            while (nl < 8) { line[nl] = '0'; nl++; }
            line[nl] = 0;
            strcat(line, "  ");
            // hex
            for (int b = 0; b < 16; b++) {
                if (off + b < hex_len) {
                    char hx[4];
                    const char* hexd = "0123456789ABCDEF";
                    hx[0] = hexd[hex_data[off + b] >> 4];
                    hx[1] = hexd[hex_data[off + b] & 15];
                    hx[2] = ' ';
                    hx[3] = 0;
                    strcat(line, hx);
                } else {
                    strcat(line, "   ");
                }
                if (b == 7) strcat(line, " ");
            }
            strcat(line, " |");
            for (int b = 0; b < 16 && off + b < hex_len; b++) {
                char c = hex_data[off + b];
                char cc[2]; cc[0] = (c >= 32 && c < 127) ? c : '.'; cc[1] = 0;
                strcat(line, cc);
            }
            strcat(line, "|");
            gfx_draw_text(wx + 16, cy + 4 + r * 16, line, 0xC8D4E0, 0x101418);
        }
    }
}

// ============ 伪随机数 (xorshift32, 无堆) ============
static uint32_t rng_next(void); /* 已在上方通用工具区定义 */
static int str_contains(const char* hay, const char* needle) {
    if (!needle[0]) return 1;
    int nl = (int)strlen(needle);
    for (const char* p = hay; *p; p++) {
        int k = 0;
        while (k < nl && p[k] && p[k] == needle[k]) k++;
        if (k == nl) return 1;
    }
    return 0;
}

static void rng_gen(void) {
    int lo = atoi(rng_min), hi = atoi(rng_max);
    if (hi <= lo) hi = lo + 1;
    uint32_t span = (uint32_t)(hi - lo) + 1;
    int v = lo + (int)(rng_next() % span);
    uitoa((uint32_t)v, rng_out, 10);
}
static void render_random(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Random number generator");
    int y = wc_y + 48;
    gfx_draw_text(wx + 14, y, "Min", 0x506070, 0xFAFCFF);
    gfx_fill_round_rect(wx + 70, y - 4, 90, 26, 6, 0xFFFFFF);
    gfx_fill_alpha(wx + 70, y - 4, 90, 26, 0xFFFFFF, 90);
    gfx_draw_text(wx + 78, y + 2, rng_min[0] ? rng_min : "0", 0x1E4E8C, 0xFFFFFF);
    y += 30;
    gfx_draw_text(wx + 14, y, "Max", 0x506070, 0xFAFCFF);
    gfx_fill_round_rect(wx + 70, y - 4, 90, 26, 6, 0xFFFFFF);
    gfx_fill_alpha(wx + 70, y - 4, 90, 26, 0xFFFFFF, 90);
    gfx_draw_text(wx + 78, y + 2, rng_max[0] ? rng_max : "100", 0x1E4E8C, 0xFFFFFF);
    y += 36;
    gfx_fill_round_rect(wx + 14, y, 96, 28, 6, 0x3D7BD6);
    gfx_draw_text(wx + 28, y + 7, "Generate", 0xFFFFFF, 0x3D7BD6);
    y += 44;
    gfx_fill_round_rect(wx + 14, y, w - 28, 44, 8, 0xE8F0FA);
    gfx_draw_text(wx + 24, y + 13, rng_out[0] ? rng_out : "-", 0x1E4E8C, 0xE8F0FA);
}

// ---------------- TextStats: 文本统计 ----------------
static void render_textstats(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Type text, stats update live");
    int y = wc_y + 48;
    gfx_fill_round_rect(wx + 14, y, w - 28, 90, 8, 0xFFFFFF);
    gfx_fill_alpha(wx + 14, y, w - 28, 90, 0xFFFFFF, 90);
    gfx_draw_text(wx + 22, y + 8, ts_text[0] ? ts_text : "Type text here...", 0x1E4E8C, 0xFFFFFF);
    y += 104;
    int chars = 0, words = 0, lines = 1, digits = 0, in_word = 0;
    for (const char* p = ts_text; *p; p++) {
        chars++;
        if (*p == '\n') lines++;
        if (*p >= '0' && *p <= '9') digits++;
        if (*p == ' ' || *p == '\n' || *p == '\t') in_word = 0;
        else if (!in_word) { in_word = 1; words++; }
    }
    if (!ts_text[0]) { chars = 0; words = 0; lines = 0; digits = 0; }
    char lb[32], vb[16];
    int cw2 = (w - 28) / 2 - 5;
    const char* labels[4] = {"Characters", "Words", "Lines", "Digits"};
    int vals[4] = {chars, words, lines, digits};
    for (int k = 0; k < 4; k++) {
        int cx = wx + 14 + (k % 2) * (cw2 + 10);
        int cy = y + (k / 2) * 66;
        gfx_fill_round_rect(cx, cy, cw2, 56, 8, 0xE8F0FA);
        gfx_draw_text(cx + 12, cy + 8, labels[k], 0x506070, 0xE8F0FA);
        uitoa((uint32_t)vals[k], vb, 10);
        gfx_draw_text(cx + 12, cy + 28, vb, 0x1E4E8C, 0xE8F0FA);
    }
    int by = y + 136;
    gfx_fill_round_rect(wx + 14, by, 96, 28, 6, 0x3D7BD6);
    gfx_draw_text(wx + 30, by + 7, "Clear", 0xFFFFFF, 0x3D7BD6);
    (void)lb; (void)h;
}

// ---------------- Sudoku: 数独 ----------------
static void render_sudoku(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "1-9 fill, arrows move, R new, C check");
    if (sd_cur == 0 && sd_puzzle[0] == 0) sd_load_puzzle(0);
    int ox = wx + 14, oy = wc_y + 48;
    int cell = (w - 28) / 9; if (cell > 34) cell = 34;
    int gw = cell * 9, gh = cell * 9;
    gfx_fill_round_rect(ox - 2, oy - 2, gw + 4, gh + 4, 6, 0xE8F0FA);
    gfx_fill_rect(ox, oy, gw, gh, 0xFFFFFF);
    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 9; c++) {
            int idx = r * 9 + c;
            if (sd_sel == idx) gfx_fill_rect(ox + c * cell, oy + r * cell, cell, cell, 0xDDECFF);
            int v = sd_puzzle[idx];
            if (v) {
                uint32_t col = sd_fixed[idx] ? 0x1E3A5C : 0x1E8C50;
                if (sd_msg == 2 && v != sd_solution[idx]) col = 0xC04040;
                char dig[2]; dig[0] = '0' + v; dig[1] = 0;
                gfx_draw_text(ox + c * cell + cell / 2 - 4, oy + r * cell + cell / 2 - 6, dig, col, 0xFFFFFF);
            }
        }
    }
    for (int k = 0; k <= 9; k++) {
        int thick = (k % 3 == 0) ? 3 : 1;
        gfx_fill_rect(ox + k * cell - thick / 2, oy, thick, gh, 0x506070);
        gfx_fill_rect(ox, oy + k * cell - thick / 2, gw, thick, 0x506070);
    }
    if (sd_msg == 1) gfx_draw_text(wx + 14, oy + gh + 12, "Correct! Press R for a new puzzle", 0x1E8C50, 0xFAFCFF);
    else if (sd_msg == 2) gfx_draw_text(wx + 14, oy + gh + 12, "Some cells are wrong (red)", 0xC04040, 0xFAFCFF);
    else gfx_draw_text(wx + 14, oy + gh + 12, "Click a cell, then type 1-9", 0x506070, 0xFAFCFF);
    (void)h;
}

// ---------------- TypingTest: 打字测试 ----------------
static void render_typing(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Type the text, R/Space switches text");
    if (!ty_texts[ty_idx]) ty_load(0);
    const char* s = ty_texts[ty_idx];
    int y = wc_y + 48;
    gfx_fill_round_rect(wx + 14, y, w - 28, 150, 8, 0xFFFFFF);
    gfx_fill_alpha(wx + 14, y, w - 28, 150, 0xFFFFFF, 90);
    int x = wx + 22, cy = y + 12;
    int slen = (int)strlen(s);
    for (int k = 0; k < slen; k++) {
        char ch[2]; ch[0] = s[k]; ch[1] = 0;
        uint32_t col;
        if (k < ty_pos) col = 0x1E8C50;
        else if (k == ty_pos) col = 0xC04040;
        else col = 0x8899AA;
        gfx_draw_text(x, cy, ch, col, 0xFFFFFF);
        x += 8;
        if (x > wx + w - 32) { x = wx + 22; cy += 14; }
    }
    char sb[64];
    if (ty_done) {
        uint32_t el = timer_get_uptime() - ty_start; if (el < 1) el = 1;
        int acc = 100 - (ty_err * 100) / (ty_pos + ty_err > 0 ? ty_pos + ty_err : 1);
        strcpy(sb, "Done! accuracy ");
        uitoa((uint32_t)acc, sb + strlen(sb), 10);
        strcat(sb, "%, ");
        uitoa(ty_pos, sb + strlen(sb), 10);
        strcat(sb, " chars in ");
        uitoa(el, sb + strlen(sb), 10);
        strcat(sb, "s");
        gfx_draw_text(wx + 14, y + 164, sb, 0x1E8C50, 0xFAFCFF);
    } else if (ty_started) {
        uint32_t el = timer_get_uptime() - ty_start; if (el < 1) el = 1;
        strcpy(sb, "Progress ");
        uitoa((uint32_t)ty_pos, sb + strlen(sb), 10);
        strcat(sb, "/");
        uitoa((uint32_t)slen, sb + strlen(sb), 10);
        strcat(sb, "  errors ");
        uitoa((uint32_t)ty_err, sb + strlen(sb), 10);
        strcat(sb, "  speed ");
        uitoa((uint32_t)(ty_pos * 60 / el), sb + strlen(sb), 10);
        strcat(sb, " cpm");
        gfx_draw_text(wx + 14, y + 164, sb, 0x1E4E8C, 0xFAFCFF);
    } else {
        gfx_draw_text(wx + 14, y + 164, "Start typing to begin...", 0x506070, 0xFAFCFF);
    }
    (void)h;
}

// ---------------- Pomodoro: 番茄钟 ----------------
static void render_pomodoro(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, pom_work ? "Focus session 25min" : "Short break 5min");
    if (!pom_total) pom_reset();
    if (pom_run) {
        uint32_t up = timer_get_uptime();
        if (up >= pom_deadline) {
            if (pom_work) { pom_sessions++; pom_work = 0; pom_total = 5 * 60; }
            else { pom_work = 1; pom_total = 25 * 60; }
            pom_left = pom_total;
            pom_deadline = up + pom_left;
        } else {
            pom_left = pom_deadline - up;
        }
    }
    char tb[16];
    uitoa(pom_left / 60, tb, 10);
    strcat(tb, ":");
    if (pom_left % 60 < 10) strcat(tb, "0");
    char ss[4]; uitoa(pom_left % 60, ss, 10); strcat(tb, ss);
    gfx_draw_text(wx + 24, wc_y + 66, tb, 0x1E4E8C, 0xFAFCFF);
    int prog = (int)((pom_total - pom_left) * 100 / (pom_total ? pom_total : 1));
    if (prog > 100) prog = 100;
    gfx_fill_round_rect(wx + 14, wc_y + 116, w - 28, 14, 7, 0xE0E8F0);
    if (prog > 0) gfx_fill_round_rect(wx + 14, wc_y + 116, (w - 28) * prog / 100, 14, 7, 0xE08030);
    int by = wc_y + 146;
    gfx_fill_round_rect(wx + 14, by, 90, 28, 6, pom_run ? 0xE08030 : 0x20B0A0);
    gfx_draw_text(wx + 24, by + 7, pom_run ? "Pause" : "Start", 0xFFFFFF, pom_run ? 0xE08030 : 0x20B0A0);
    gfx_fill_round_rect(wx + 114, by, 90, 28, 6, 0x8A9AB0);
    gfx_draw_text(wx + 126, by + 7, "Reset", 0xFFFFFF, 0x8A9AB0);
    char cb[48];
    strcpy(cb, "Focus sessions done: ");
    uitoa((uint32_t)pom_sessions, cb + strlen(cb), 10);
    gfx_draw_text(wx + 14, by + 44, cb, 0x506070, 0xFAFCFF);
    gfx_draw_text(wx + 14, by + 64, "Space toggles, R resets", 0x506070, 0xFAFCFF);
    (void)h;
}

// ---------------- GuessNumber: 猜数字 ----------------
static void render_guess(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Guess the number 1-100");
    if (!gs_target) gs_new();
    int y = wc_y + 48;
    gfx_fill_round_rect(wx + 14, y, 130, 30, 6, 0xFFFFFF);
    gfx_fill_alpha(wx + 14, y, 130, 30, 0xFFFFFF, 90);
    gfx_draw_text(wx + 22, y + 7, gs_buf[0] ? gs_buf : "...", 0x1E4E8C, 0xFFFFFF);
    gfx_fill_round_rect(wx + 154, y, 80, 30, 6, 0x3D7BD6);
    gfx_draw_text(wx + 176, y + 7, "New", 0xFFFFFF, 0x3D7BD6);
    y += 46;
    if (gs_over) {
        char mb[48];
        strcpy(mb, "Correct! ");
        uitoa((uint32_t)gs_tries, mb + strlen(mb), 10);
        strcat(mb, " tries");
        gfx_draw_text(wx + 14, y, mb, 0x1E8C50, 0xFAFCFF);
    } else if (gs_msg == 1) {
        gfx_draw_text(wx + 14, y, "Too high!", 0xC04040, 0xFAFCFF);
    } else if (gs_msg == 2) {
        gfx_draw_text(wx + 14, y, "Too low!", 0xC04040, 0xFAFCFF);
    } else {
        gfx_draw_text(wx + 14, y, "Enter a number 1-100, press Enter", 0x506070, 0xFAFCFF);
    }
    y += 26;
    char tb[48];
    strcpy(tb, "Tries: ");
    uitoa((uint32_t)gs_tries, tb + strlen(tb), 10);
    gfx_draw_text(wx + 14, y, tb, 0x202020, 0xFAFCFF);
    (void)h;
}

// ---------------- DiceRoller: 掷骰子 ----------------
static void render_dice(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Pick a die, then Roll (Space)");
    if (!dc_side) dc_side = 6;
    static const int sides[7] = {4, 6, 8, 10, 12, 20, 100};
    int by = wc_y + 48;
    for (int k = 0; k < 7; k++) {
        int x = wx + 14 + k * 56;
        int act = (dc_side == sides[k]);
        gfx_fill_round_rect(x, by, 50, 26, 6, act ? 0x3D7BD6 : 0xAAB8CC);
        char sb[8]; uitoa((uint32_t)sides[k], sb, 10);
        char lab[8]; lab[0] = 'D'; strcpy(lab + 1, sb);
        gfx_draw_text(x + 13, by + 6, lab, 0xFFFFFF, act ? 0x3D7BD6 : 0xAAB8CC);
    }
    int ry = by + 44;
    gfx_fill_round_rect(wx + 14, ry, w - 28, 90, 10, 0xE8F0FA);
    if (dc_result) {
        char rb[8]; uitoa((uint32_t)dc_result, rb, 10);
        gfx_draw_text(wx + w / 2 - 10, ry + 30, rb, 0x1E4E8C, 0xE8F0FA);
    } else {
        gfx_draw_text(wx + w / 2 - 44, ry + 30, "Click Roll", 0x506070, 0xE8F0FA);
    }
    int rby = ry + 104;
    gfx_fill_round_rect(wx + 14, rby, 96, 28, 6, 0xE08030);
    gfx_draw_text(wx + 34, rby + 7, "Roll", 0xFFFFFF, 0xE08030);
    gfx_draw_text(wx + 14, rby + 42, "History:", 0x506070, 0xFAFCFF);
    for (int k = 0; k < dc_hist_n; k++) {
        char hb[8]; uitoa((uint32_t)dc_hist[k], hb, 10);
        gfx_draw_text(wx + 84 + k * 40, rby + 42, hb, 0x202020, 0xFAFCFF);
    }
    (void)h;
}

// ---------------- TodoList: 待办清单 ----------------
static void render_todo(int i) {
    int wx = wins[i].x, wy = wins[i].y;
    int w = wins[i].w, h = wins[i].h;
    app_panel(i, "Type task & Enter  ·  click box toggles done  ·  x deletes");
    // 输入行: [输入框][Add]
    int inw = w - 124;
    gfx_fill_round_rect(wx + 14, wy + 48, inw, 30, 6, 0xFFFFFF);
    char* ibuf = wins[i].app_buf;
    int ilen = (int)strlen(ibuf);
    static char ibuf2[64];
    int imaxc = (inw - 18) / 8; if (imaxc > 60) imaxc = 60;
    int idis = 0;
    while (ibuf[idis] && idis < imaxc) { ibuf2[idis] = ibuf[idis]; idis++; }
    ibuf2[idis] = 0;
    gfx_draw_text(wx + 22, wy + 55, ilen ? ibuf2 : "new task...",
                  ilen ? 0x1E4E8C : 0x9AAEC4, 0xFFFFFF);
    gfx_fill_round_rect(wx + w - 102, wy + 48, 88, 30, 6, 0x2E7BB0);
    gfx_draw_text(wx + w - 84, wy + 55, "Add", 0xFFFFFF, 0x2E7BB0);
    // 操作行: [Clear done][计数] [^] [v]
    gfx_fill_round_rect(wx + 14, wy + 86, 96, 26, 6, 0xD06050);
    gfx_draw_text(wx + 30, wy + 93, "Clear done", 0xFFFFFF, 0xD06050);
    char cnt[24]; uitoa((uint32_t)todo_n, cnt, 10);
    gfx_draw_text(wx + 126, wy + 93, cnt, 0x506070, 0xFAFCFF);
    gfx_draw_text(wx + 138, wy + 93, "/ 16", 0x90A0B4, 0xFAFCFF);
    gfx_fill_round_rect(wx + w - 90, wy + 86, 34, 26, 6, 0xAAB8CC);
    gfx_draw_char(wx + w - 77, wy + 91, '^', 0xFFFFFF, 0xAAB8CC);
    gfx_fill_round_rect(wx + w - 48, wy + 86, 34, 26, 6, 0xAAB8CC);
    gfx_draw_char(wx + w - 35, wy + 91, 'v', 0xFFFFFF, 0xAAB8CC);
    // 列表
    int vis = todo_vis_rows(h);
    int mxr = todo_n - vis; if (mxr < 0) mxr = 0;
    if (todo_scroll > mxr) todo_scroll = mxr;
    if (todo_scroll < 0) todo_scroll = 0;
    int top = wy + 122;
    for (int r = 0; r < vis; r++) {
        int idx = todo_scroll + r;
        int ry = top + r * 28;
        int bg = (r & 1) ? 0xF0F6FE : 0xFFFFFF;
        if (idx >= todo_n) {                 // 空槽: 画背景覆盖残影
            gfx_fill_round_rect(wx + 14, ry, w - 28, 26, 5, bg);
            continue;
        }
        gfx_fill_round_rect(wx + 14, ry, w - 28, 26, 5, bg);
        int done = todo_done[idx];
        if (done) {
            gfx_fill_round_rect(wx + 24, ry + 5, 16, 16, 3, 0x2E7BB0);
            gfx_draw_line(wx + 27, ry + 12, wx + 31, ry + 16, 0xFFFFFF);
            gfx_draw_line(wx + 31, ry + 16, wx + 37, ry + 7, 0xFFFFFF);
        } else {
            gfx_draw_round_rect(wx + 24, ry + 5, 16, 16, 3, 0x7A8CA8);
        }
        int maxc = (w - 90) / 8;
        if (maxc > TODO_LEN - 1) maxc = TODO_LEN - 1;
        if (maxc < 1) maxc = 1;
        static char tbuf[64];
        int tl = 0;
        while (todo_items[idx][tl] && tl < maxc) { tbuf[tl] = todo_items[idx][tl]; tl++; }
        tbuf[tl] = 0;
        gfx_draw_text(wx + 48, ry + 5, tbuf, done ? 0x8A96A6 : 0x202A38, bg);
        if (done) gfx_fill_rect(wx + 48, ry + 14, tl * 8 + 2, 1, 0x8A96A6);
        gfx_fill_round_rect(wx + w - 32, ry + 3, 20, 20, 4, 0xEFE6E4);
        gfx_draw_char(wx + w - 26, ry + 5, 'x', 0xB04838, 0xEFE6E4);
    }
}

// ============ 随机密码生成器: 逻辑 ============
static void pgen_gen(void) {
    char pool[96];
    int pn = 0;
    if (pgen_opt[0]) for (char ch = 'A'; ch <= 'Z'; ch++) pool[pn++] = ch;
    if (pgen_opt[1]) for (char ch = 'a'; ch <= 'z'; ch++) pool[pn++] = ch;
    if (pgen_opt[2]) for (char ch = '0'; ch <= '9'; ch++) pool[pn++] = ch;
    if (pgen_opt[3]) { const char* s = "!@#$%^&*-_+=?"; while (*s) pool[pn++] = *s++; }
    int len = pgen_lens[pgen_len_i];
    if (pn == 0) { pgen_out[0] = 0; strcpy(pgen_msg, "Select at least one charset"); return; }
    int n = 0;
    // 每个已勾选字符集至少出现一次
    for (int o = 0; o < 4 && n < len; o++) {
        if (!pgen_opt[o]) continue;
        if (o == 0) pgen_out[n++] = (char)('A' + rng_next() % 26);
        else if (o == 1) pgen_out[n++] = (char)('a' + rng_next() % 26);
        else if (o == 2) pgen_out[n++] = (char)('0' + rng_next() % 10);
        else { const char* s = "!@#$%^&*-_+=?"; pgen_out[n++] = s[rng_next() % 14]; }
    }
    while (n < len) pgen_out[n++] = pool[rng_next() % (uint32_t)pn];
    // Fisher-Yates 洗牌, 避免"每类开头"可预测
    for (int j = n - 1; j > 0; j--) {
        int kk = rng_next() % (uint32_t)(j + 1);
        char t = pgen_out[j]; pgen_out[j] = pgen_out[kk]; pgen_out[kk] = t;
    }
    pgen_out[n] = 0;
    strcpy(pgen_msg, "Generated ");
    uitoa((uint32_t)n, pgen_msg + 10, 10);
    strcat(pgen_msg, " chars");
}

static void pgen_copy(void) {
    if (!pgen_out[0]) return;
    strcpy(pgen_clip, pgen_out);
    strcpy(pgen_msg, "Copied to clipboard: ");
    strcat(pgen_msg, pgen_clip);
}

static void pgen_term(void) {
    if (!pgen_out[0]) return;
    int have_term = -1;
    for (int k = 0; k < win_count; k++)
        if (wins[k].type == WIN_TERMINAL) { have_term = k; break; }
    if (have_term < 0) open_window_by_type(WIN_TERMINAL);
    for (int w = 0; w < win_count; w++)
        if (wins[w].type == WIN_TERMINAL) { wins[w].dirty = 1; break; }
    for (const char* p = pgen_out; *p; p++) shell_process_char(*p);
    shell_process_char('\r');
    strcpy(pgen_msg, "Typed into terminal (");
    uitoa((uint32_t)strlen(pgen_out), pgen_msg + 20, 10);
    strcat(pgen_msg, " chars)");
}

// ============ 随机密码生成器: 渲染 ============
static void render_passgen(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Secure password (kernel rng, no disk)");
    int y0 = wc_y + 48;
    // Length row
    gfx_draw_text(wx + 14, y0 + 5, "Length", 0x506070, 0xFAFCFF);
    for (int k = 0; k < 4; k++) {
        int bx = wx + 74 + k * 54;
        char lb[4]; int len = pgen_lens[k];
        lb[0] = (char)('0' + len / 10); lb[1] = (char)('0' + len % 10); lb[2] = 0;
        if (lb[0] == '0') { lb[0] = lb[1]; lb[1] = 0; }
        uint32_t fg = (pgen_len_i == k) ? 0xFFFFFF : 0x1E4E8C;
        uint32_t bg = (pgen_len_i == k) ? 0x3D7BD6 : 0xE8F0FA;
        gfx_fill_round_rect(bx, y0, 48, 24, 6, bg);
        gfx_draw_text(bx + (48 - (int)strlen(lb) * 8) / 2, y0 + 5, lb, fg, bg);
    }
    // Charset row
    gfx_draw_text(wx + 14, y0 + 39, "Chars", 0x506070, 0xFAFCFF);
    static const char* pg_lab[4] = {"A-Z", "a-z", "0-9", "!@#"};
    for (int c = 0; c < 4; c++) {
        int bx = wx + 74 + c * 64;
        uint32_t fg = pgen_opt[c] ? 0xFFFFFF : 0x202A38;
        uint32_t bg = pgen_opt[c] ? 0x3D7BD6 : 0xE8F0FA;
        gfx_fill_round_rect(bx, y0 + 34, 58, 22, 6, bg);
        gfx_draw_text(bx + (58 - (int)strlen(pg_lab[c]) * 8) / 2, y0 + 39, pg_lab[c], fg, bg);
    }
    // Action row
    gfx_fill_round_rect(wx + 14, y0 + 64, 108, 26, 7, 0x3D7BD6);
    gfx_draw_text(wx + 26, y0 + 70, "Generate", 0xFFFFFF, 0x3D7BD6);
    gfx_fill_round_rect(wx + 134, y0 + 64, 76, 26, 7, 0x4A9E60);
    gfx_draw_text(wx + 150, y0 + 70, "Copy", 0xFFFFFF, 0x4A9E60);
    gfx_fill_round_rect(wx + 222, y0 + 64, 92, 26, 7, 0x5A6AC0);
    gfx_draw_text(wx + 235, y0 + 70, "Terminal", 0xFFFFFF, 0x5A6AC0);
    // Password box
    int pbx = wx + 14, pby = y0 + 102, pbw = w - 28, pbh = 48;
    gfx_fill_round_rect(pbx, pby, pbw, pbh, 8, 0xFFFFFF);
    gfx_draw_round_rect(pbx, pby, pbw, pbh, 8, 0xB8CCE0);
    if (pgen_out[0]) {
        int tw = (int)strlen(pgen_out) * 8;
        int tx = pbx + (pbw - tw) / 2; if (tx < pbx + 8) tx = pbx + 8;
        gfx_draw_text(tx, pby + (pbh - 16) / 2 + 1, pgen_out, 0x103060, 0xFFFFFF);
    } else {
        gfx_draw_text(pbx + 14, pby + (pbh - 16) / 2 + 1, "Press Generate (or G) to create", 0x9AA8BC, 0xFFFFFF);
    }
    // Status message + hint
    gfx_draw_text(wx + 16, pby + pbh + 8,
                  pgen_msg[0] ? pgen_msg : "Generated passwords stay in RAM only", 0x1E4E8C, 0xFAFCFF);
    gfx_draw_text(wx + 16, pby + pbh + 28,
                  "Keys: 1-4 length | G generate | C copy | T terminal", 0x6A7A90, 0xFAFCFF);
    (void)h;
}

// 时钟/秒表: 实时时间 (CMOS RTC) + 秒表 Start/Stop/Reset
static void render_clock(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Local time (CMOS RTC) + stopwatch");
    int y0 = wc_y + 48;

    // ---- 实时时间面板 ----
    struct rtc_time t;
    rtc_read(&t);
    char date[32];
    rtc_format(date, sizeof(date));          // "YYYY-MM-DD HH:MM:SS"
    char dstr[12];
    for (int k = 0; k < 10; k++) dstr[k] = date[k];
    dstr[10] = 0;
    char tstr[10];
    for (int k = 0; k < 8; k++) tstr[k] = date[11 + k];
    tstr[8] = 0;
    static const char* wd_en[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const char* wd = (t.weekday >= 1 && t.weekday <= 7) ? wd_en[t.weekday - 1] : "---";

    gfx_fill_round_rect(wx + 14, y0, w - 28, 78, 10, 0x0E2A44);
    gfx_draw_text(wx + 24, y0 + 8, "Local time (RTC)", 0x8FB8D8, 0x0E2A44);
    int dw = 13 * 8;
    gfx_draw_text(wx + (w - dw) / 2, y0 + 28, dstr, 0xCFE8FF, 0x0E2A44);
    gfx_draw_text(wx + (w - dw) / 2 + 88, y0 + 28, wd, 0x7FA8C8, 0x0E2A44);
    gfx_draw_text_glow(wx + (w - 64) / 2, y0 + 52, tstr, 0x7FE0FF, 0x1E6A9E);

    // ---- 秒表面板 ----
    int sy = y0 + 92;
    gfx_draw_text(wx + 16, sy, "Stopwatch", 0x1E4E8C, 0xFAFCFF);
    gfx_draw_text(wx + w - 108, sy, clk_sw_run ? "RUNNING" : "STOPPED",
                  clk_sw_run ? 0x2A8040 : 0x808890, 0xFAFCFF);
    uint32_t el = clk_sw_elapsed();
    char sw[16], b[8];
    uint32_t cs = el % 100; el /= 100;
    uint32_t sec = el % 60; el /= 60;
    uint32_t min = el % 100;
    itoa((int)min, b, 10);
    if (min < 10) { b[1] = b[0]; b[0] = '0'; b[2] = 0; }
    strcpy(sw, b); strcat(sw, ":");
    itoa((int)sec, b, 10);
    if (sec < 10) { b[1] = b[0]; b[0] = '0'; b[2] = 0; }
    strcat(sw, b); strcat(sw, ".");
    itoa((int)cs, b, 10);
    if (cs < 10) { b[1] = b[0]; b[0] = '0'; b[2] = 0; }
    strcat(sw, b);
    int swy = sy + 20;
    gfx_fill_round_rect(wx + 14, swy, w - 28, 46, 10, 0x101820);
    gfx_draw_text_glow(wx + (w - 64) / 2, swy + 14, sw, 0xA8F0C0, 0x1E6A4E);

    // ---- 按钮行 (与 gui_click_content 命中一致: y = wy + 216) ----
    int by = wc_y + 216;
    gfx_fill_round_rect(wx + 14, by, 90, 28, 7, clk_sw_run ? 0xE08030 : 0x2A8040);
    gfx_draw_text(wx + 34, by + 7, clk_sw_run ? "Stop" : "Start",
                  0xFFFFFF, clk_sw_run ? 0xE08030 : 0x2A8040);
    gfx_fill_round_rect(wx + 112, by, 90, 28, 7, 0x3D7BD6);
    gfx_draw_text(wx + 132, by + 7, "Reset", 0xFFFFFF, 0x3D7BD6);

    // ---- 说明与开机时长 ----
    gfx_draw_text(wx + 16, by + 40, "Keys: Space/Enter start-stop | R reset", 0x6A7A90, 0xFAFCFF);
    char up[32];
    uint32_t us = timer_get_uptime();
    uitoa(us, up, 10);
    strcat(up, " s");
    gfx_draw_text(wx + 16, by + 60, "Uptime: ", 0x6A7A90, 0xFAFCFF);
    gfx_draw_text(wx + 16 + 64, by + 60, up, 0x1E4E8C, 0xFAFCFF);
    (void)h;
}

static int bc_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static void bc_convert(void) {
    bc_bin[0] = bc_oct[0] = bc_hex[0] = bc_dec[0] = 0;
    if (!bc_input[0]) return;
    if (bc_mode == 0) {
        int v = atoi(bc_input);
        uitoa((uint32_t)v, bc_bin, 2);
        uitoa((uint32_t)v, bc_oct, 8);
        uitoa((uint32_t)v, bc_hex, 16);
        uitoa((uint32_t)v, bc_dec, 10);
    } else {
        uint32_t v = 0;
        for (const char* p = bc_input; *p; p++) {
            int d = bc_hexval(*p);
            if (d < 0) { bc_dec[0] = 0; return; }
            v = v * 16 + (uint32_t)d;
        }
        uitoa(v, bc_dec, 10);
    }
}
static void render_baseconv(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, bc_mode ? "Hex input -> Decimal" : "Decimal input -> 2/8/16");
    int y = wc_y + 48;
    gfx_fill_round_rect(wx + 14, y, w - 28, 30, 6, 0xFFFFFF);
    gfx_fill_alpha(wx + 14, y, w - 28, 30, 0xFFFFFF, 90);
    gfx_draw_text(wx + 22, y + 7, bc_input[0] ? bc_input : "type number...", 0x1E4E8C, 0xFFFFFF);
    y += 40;
    gfx_fill_round_rect(wx + 14, y, 88, 26, 6, bc_mode == 0 ? 0x3D7BD6 : 0xAAB8CC);
    gfx_draw_text(wx + 24, y + 6, "Dec", 0xFFFFFF, bc_mode == 0 ? 0x3D7BD6 : 0xAAB8CC);
    gfx_fill_round_rect(wx + 110, y, 88, 26, 6, bc_mode == 1 ? 0x3D7BD6 : 0xAAB8CC);
    gfx_draw_text(wx + 118, y + 6, "Hex", 0xFFFFFF, bc_mode == 1 ? 0x3D7BD6 : 0xAAB8CC);
    y += 40;
    gfx_draw_text(wx + 16, y, "BIN", 0x506070, 0xFAFCFF);
    gfx_draw_text(wx + 70, y, bc_bin, 0x202020, 0xFAFCFF);
    y += 22;
    gfx_draw_text(wx + 16, y, "OCT", 0x506070, 0xFAFCFF);
    gfx_draw_text(wx + 70, y, bc_oct, 0x202020, 0xFAFCFF);
    y += 22;
    gfx_draw_text(wx + 16, y, "HEX", 0x506070, 0xFAFCFF);
    gfx_draw_text(wx + 70, y, bc_hex, 0x202020, 0xFAFCFF);
    y += 22;
    gfx_draw_text(wx + 16, y, "DEC", 0x506070, 0xFAFCFF);
    gfx_draw_text(wx + 70, y, bc_dec, 0x202020, 0xFAFCFF);
}

static void tictac_reset(void) {
    memset(tictac, 0, 9);
    tictac_turn = 0; tictac_over = 0; tictac_win = 0;
}
static int tictac_win_line(int p) {
    static const int L[8][3] = {{0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}};
    for (int k = 0; k < 8; k++)
        if (tictac[L[k][0]] == p && tictac[L[k][1]] == p && tictac[L[k][2]] == p) return 1;
    return 0;
}
static int tictac_draw(void) { for (int k = 0; k < 9; k++) if (!tictac[k]) return 0; return 1; }
static void tictac_ai(void) {
    if (tictac_over || tictac_turn != 1) return;
    int best = -1;
    for (int k = 0; k < 9 && best < 0; k++) if (!tictac[k]) { tictac[k] = 2; if (tictac_win_line(2)) best = k; tictac[k] = 0; }
    for (int k = 0; k < 9 && best < 0; k++) if (!tictac[k]) { tictac[k] = 1; if (tictac_win_line(1)) best = k; tictac[k] = 0; }
    if (best < 0 && !tictac[4]) best = 4;
    if (best < 0) { static const int C[4] = {0,2,6,8}; for (int k = 0; k < 4 && best < 0; k++) if (!tictac[C[k]]) best = C[k]; }
    if (best < 0) { for (int k = 0; k < 9; k++) if (!tictac[k]) { best = k; break; } }
    if (best >= 0) {
        tictac[best] = 2; tictac_turn = 0;
        if (tictac_win_line(2)) { tictac_over = 1; tictac_win = 2; }
        else if (tictac_draw()) { tictac_over = 1; tictac_win = 3; }
    }
}
static void render_tictac(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "TicTacToe - you are X");
    int gx = wx + 14, gy = wc_y + 48, gs = 78, gap = 6;
    int total = gs * 3 + gap * 2;
    gfx_fill_round_rect(gx - 4, gy - 4, total + 8, total + 8, 8, 0xE0E8F2);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            int x = gx + c * (gs + gap), y = gy + r * (gs + gap);
            gfx_fill_round_rect(x, y, gs, gs, 8, 0xFFFFFF);
            int v = tictac[r * 3 + c];
            if (v == 1) {  // X
                gfx_draw_line(x + 10, y + 10, x + gs - 10, y + gs - 10, 0x2E5A8A);
                gfx_draw_line(x + gs - 10, y + 10, x + 10, y + gs - 10, 0x2E5A8A);
            } else if (v == 2) {  // O
                gfx_draw_round_rect(x + 10, y + 10, gs - 20, gs - 20, 8, 0xC06030);
                gfx_draw_round_rect(x + 14, y + 14, gs - 28, gs - 28, 6, 0xC06030);
            }
        }
    }
    int ty = gy + total + 14;
    char st[24];
    if (tictac_over) {
        if (tictac_win == 1) strcpy(st, "You win! (R to restart)");
        else if (tictac_win == 2) strcpy(st, "AI wins! (R to restart)");
        else strcpy(st, "Draw! (R to restart)");
    } else {
        strcpy(st, tictac_turn == 0 ? "Your turn (X)" : "AI thinking...");
    }
    gfx_draw_text(gx, ty, st, 0x1E4E8C, 0xFAFCFF);
}

static void mem_reset(void) {
    for (int i = 0; i < 16; i++) { mem_cards[i] = (uint8_t)(i / 2); mem_flip[i] = 0; mem_state[i] = 0; }
    for (int i = 15; i > 0; i--) {
        int j = (int)(rng_next() % (uint32_t)(i + 1));
        uint8_t t = mem_cards[i]; mem_cards[i] = mem_cards[j]; mem_cards[j] = t;
    }
    mem_open = -1; mem_moves = 0; mem_matched = 0; mem_over = 0; mem_wait = 0;
}
static void mem_tick(void) {
    if (mem_wait && timer_get_ticks() - mem_wait_last >= 45) {
        mem_flip[mem_open] = 0;
        mem_flip[mem_wait - 1] = 0;
        mem_open = -1; mem_wait = 0;
    }
}
static void render_memory(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Memory - match pairs");
    int cw = 44, gap = 8, ox = wx + 14, oy = wc_y + 48;
    for (int k = 0; k < 16; k++) {
        int x = ox + (k % 4) * (cw + gap), y = oy + (k / 4) * (cw + gap);
        if (mem_state[k]) {
            gfx_fill_round_rect(x, y, cw, cw, 8, 0x8FE8A0);
            gfx_draw_text(x + 16, y + 15, "OK", 0x207030, 0x8FE8A0);
        } else if (mem_flip[k]) {
            gfx_fill_round_rect(x, y, cw, cw, 8, 0xF4C860);
            char nb[2]; nb[0] = (char)('1' + mem_cards[k]); nb[1] = 0;
            gfx_draw_text(x + 17, y + 13, nb, 0x604010, 0xF4C860);
        } else {
            gfx_fill_round_rect(x, y, cw, cw, 8, 0x3D7BD6);
            gfx_draw_text(x + 15, y + 13, "?", 0xFFFFFF, 0x3D7BD6);
        }
    }
    char st[32];
    st[0] = 0;
    strcat(st, "Moves: "); char nb[8]; uitoa((uint32_t)mem_moves, nb, 10); strcat(st, nb);
    strcat(st, "  Pairs: "); uitoa((uint32_t)mem_matched, nb, 10); strcat(st, nb);
    gfx_draw_text(wx + 14, oy + 4 * (cw + gap) + 8, st, 0x1E4E8C, 0xFAFCFF);
    if (mem_over) gfx_draw_text(wx + 14, oy + 4 * (cw + gap) + 24, "All matched!", 0x1E8C40, 0xFAFCFF);
    gfx_fill_round_rect(wx + 14, oy + 4 * (cw + gap) + 36, 96, 26, 6, 0x3D7BD6);
    gfx_draw_text(wx + 26, oy + 4 * (cw + gap) + 42, "New Game", 0xFFFFFF, 0x3D7BD6);
}

static void find_do(void) {
    find_out[0] = 0;
    int n = fs_list(find_tmp, 3299);
    if (n <= 0) { strcpy(find_out, "(no files)"); return; }
    int olen = 0;
    const char* p = find_tmp;
    while (*p && olen < 3280) {
        const char* nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        char line[96];
        int cp = len < 90 ? len : 90;
        for (int k = 0; k < cp; k++) line[k] = p[k];
        line[cp] = 0;
        // 行 = "name size" ; 匹配 name 部分 (到空格或/为止)
        char name[96];
        int nn = 0;
        while (line[nn] && line[nn] != ' ' && line[nn] != '/' && nn < 90) { name[nn] = line[nn]; nn++; }
        name[nn] = 0;
        if (str_contains(name, find_query)) {
            for (int k = 0; line[k] && olen < 3280; k++) { find_out[olen++] = line[k]; }
            find_out[olen++] = '\n';
        }
        if (!nl) break;
        p = nl + 1;
    }
    if (olen == 0) strcpy(find_out, "(no match)");
    find_out[olen] = 0;
    find_scroll = 0;
}
static void render_find(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    app_panel(i, "Search files by name (ramfs)");
    int y = wc_y + 48;
    gfx_fill_round_rect(wx + 14, y, w - 120, 30, 6, 0xFFFFFF);
    gfx_fill_alpha(wx + 14, y, w - 120, 30, 0xFFFFFF, 90);
    gfx_draw_text(wx + 22, y + 7, find_query[0] ? find_query : "file name...", 0x1E4E8C, 0xFFFFFF);
    gfx_fill_round_rect(wx + w - 96, y, 82, 30, 6, 0x3D7BD6);
    gfx_draw_text(wx + w - 82, y + 7, "Search", 0xFFFFFF, 0x3D7BD6);
    int cy = y + 38;
    int chh = h - (cy - wc_y) - 8;
    gfx_fill_round_rect(wx + 8, cy, w - 16, chh, 6, 0x101418);
    int vis = chh / 16;
    if (find_out[0] == 0) {
        gfx_draw_text(wx + 18, cy + 8, "(enter keyword, click Search)", 0x506070, 0x101418);
    } else {
        int start = find_scroll;
        if (start < 0) start = 0;
        int lines = 0;
        for (const char* q = find_out; *q; q++) if (*q == '\n') lines++;
        if (start > lines - vis) start = lines - vis;
        if (start < 0) start = 0;
        int li = 0, ypos = cy + 4;
        const char* p = find_out;
        while (*p) {
            const char* nl = strchr(p, '\n');
            int len = nl ? (int)(nl - p) : (int)strlen(p);
            if (li >= start && li < start + vis && len < 90) {
                char line[96];
                for (int k = 0; k < len; k++) line[k] = p[k];
                line[len] = 0;
                gfx_draw_text(wx + 16, ypos, line, 0xC8D4E0, 0x101418);
                ypos += 16;
            }
            li++;
            if (!nl) break;
            p = nl + 1;
        }
    }
}

static void tetris_cells(int piece, int rot, int* ox, int* oy) {
    for (int k = 0; k < 4; k++) {
        int x = tetris_shape[piece][k * 2];
        int y = tetris_shape[piece][k * 2 + 1];
        for (int r = 0; r < rot; r++) { int t = x; x = -y; y = t; }
        ox[k] = x; oy[k] = y;
    }
}
static int tetris_hit(int px, int py, int piece, int rot) {
    int ox[4], oy[4];
    tetris_cells(piece, rot, ox, oy);
    for (int k = 0; k < 4; k++) {
        int x = px + ox[k], y = py + oy[k];
        if (x < 0 || x >= TETRIS_W || y < 0 || y >= TETRIS_H) return 1;
        if (tetris_board[y * TETRIS_W + x]) return 1;
    }
    return 0;
}
static void tetris_freeze(void) {
    int ox[4], oy[4];
    tetris_cells(tetris_piece, tetris_rot, ox, oy);
    for (int k = 0; k < 4; k++) {
        int x = tetris_px + ox[k], y = tetris_py + oy[k];
        if (y < 0) { tetris_running = 2; return; }
        tetris_board[y * TETRIS_W + x] = (uint8_t)(tetris_piece + 1);
    }
    // 消行
    for (int y = TETRIS_H - 1; y >= 0; y--) {
        int full = 1;
        for (int x = 0; x < TETRIS_W; x++) if (!tetris_board[y * TETRIS_W + x]) { full = 0; break; }
        if (full) {
            for (int yy = y; yy > 0; yy--)
                for (int x = 0; x < TETRIS_W; x++) tetris_board[yy * TETRIS_W + x] = tetris_board[(yy - 1) * TETRIS_W + x];
            for (int x = 0; x < TETRIS_W; x++) tetris_board[x] = 0;
            tetris_score += 100;
            y++;
        }
    }
    tetris_spawn();
}
static void tetris_spawn(void) {
    tetris_piece = (int)(rng_next() % 7);
    tetris_rot = 0;
    tetris_px = 3; tetris_py = 0;
    if (tetris_hit(tetris_px, tetris_py, tetris_piece, tetris_rot)) tetris_running = 2;
}
static void tetris_reset(void) {
    memset(tetris_board, 0, TETRIS_W * TETRIS_H);
    tetris_score = 0; tetris_running = 1; tetris_paused = 0; tetris_last = timer_get_ticks();
    tetris_spawn();
}
static void tetris_tick(void) {
    if (!tetris_running || tetris_running == 2 || tetris_paused) return;
    uint32_t now = timer_get_ticks();
    uint32_t interval = 26;  // ~260ms/格
    if (now - tetris_last < interval) return;
    tetris_last = now;
    if (!tetris_hit(tetris_px, tetris_py + 1, tetris_piece, tetris_rot)) tetris_py++;
    else tetris_freeze();
}
static void tetris_move(int dx) {
    if (!tetris_running || tetris_running == 2 || tetris_paused) return;
    if (!tetris_hit(tetris_px + dx, tetris_py, tetris_piece, tetris_rot)) tetris_px += dx;
}
static void tetris_rotate(void) {
    if (!tetris_running || tetris_running == 2 || tetris_paused) return;
    int nr = (tetris_rot + 1) & 3;
    if (!tetris_hit(tetris_px, tetris_py, tetris_piece, nr)) tetris_rot = nr;
}
static void tetris_drop(void) {
    if (!tetris_running || tetris_running == 2 || tetris_paused) return;
    while (!tetris_hit(tetris_px, tetris_py + 1, tetris_piece, tetris_rot)) tetris_py++;
    tetris_freeze();
    tetris_last = timer_get_ticks();
}
static void render_tetris(int i) {
    int wx = wins[i].x, wc_y = wins[i].y, w = wins[i].w, h = wins[i].h;
    char sub[24];
    sub[0] = 0; strcat(sub, "Score: "); char nb[12]; uitoa((uint32_t)tetris_score, nb, 10); strcat(sub, nb);
    app_panel(i, sub);
    int cell = 18;
    int ox = wx + 14, oy = wc_y + 52;
    // 背景网格
    gfx_fill_round_rect(ox - 4, oy - 4, TETRIS_W * cell + 8, TETRIS_H * cell + 8, 6, 0x101418);
    for (int yy = 0; yy < TETRIS_H; yy++)
        for (int xx = 0; xx < TETRIS_W; xx++) {
            uint8_t v = tetris_board[yy * TETRIS_W + xx];
            uint32_t col = 0x202830;
            if (v) col = 0x3D7BD6 + (uint32_t)v * 0x040404;
            gfx_fill_rect(ox + xx * cell + 1, oy + yy * cell + 1, cell - 2, cell - 2, col);
        }
    // 当前块
    if (tetris_running == 1) {
        int oxx[4], oyy[4];
        tetris_cells(tetris_piece, tetris_rot, oxx, oyy);
        uint32_t col = 0xE0A030;
        for (int k = 0; k < 4; k++) {
            int xx = tetris_px + oxx[k], yy = tetris_py + oyy[k];
            if (xx >= 0 && xx < TETRIS_W && yy >= 0 && yy < TETRIS_H)
                gfx_fill_rect(ox + xx * cell + 1, oy + yy * cell + 1, cell - 2, cell - 2, col);
        }
    }
    // 侧栏信息
    int sx = ox + TETRIS_W * cell + 16;
    gfx_draw_text(sx, oy + 4, "Left/Right", 0x506070, 0xFAFCFF);
    gfx_draw_text(sx, oy + 20, "Up=Rotate", 0x506070, 0xFAFCFF);
    gfx_draw_text(sx, oy + 36, "Space=Drop", 0x506070, 0xFAFCFF);
    gfx_draw_text(sx, oy + 52, "P=Pause", 0x506070, 0xFAFCFF);
    gfx_draw_text(sx, oy + 68, "R=Restart", 0x506070, 0xFAFCFF);
    if (tetris_running == 0 || tetris_running == 2) {
        gfx_draw_text(sx, oy + 92, tetris_running == 2 ? "GAME OVER" : "Press R to start", 0xC04030, 0xFAFCFF);
    }
    if (tetris_paused && tetris_running == 1) gfx_draw_text(sx, oy + 92, "PAUSED", 0xE0A030, 0xFAFCFF);
}
