// ============================================================
// EpochOS - 图形用户界面 (GUI)
// 桌面 + 任务栏 + 窗口管理 + 终端窗口 + 文件浏览器 + 鼠标交互
// ============================================================
#ifndef EPOCHOS_GUI_H
#define EPOCHOS_GUI_H

// 窗口类型
#define WIN_TERMINAL  0    // 终端 (渲染 VGA 虚拟缓冲 80x25)
#define WIN_FILES     1    // 文件浏览器
#define WIN_ABOUT     2    // 关于
#define WIN_HELP      3    // 帮助
#define WIN_SETTINGS  4    // 设置
#define WIN_MANAGER   5    // 系统管家
#define WIN_BROWSER   6    // 浏览器
#define WIN_TRANSLATE 7    // 翻译
#define WIN_CONVERT   8    // 格式转换器
#define WIN_SHOT      9    // 截图工具
#define WIN_PHOTO     10   // 照片
#define WIN_VIDEO     11   // 视频
#define WIN_PDF       12   // PDF 阅读器
#define WIN_MUSIC     13   // 音乐
#define WIN_CALENDAR  14   // 日历
#define WIN_EDITOR    15   // 文本编辑器
#define WIN_ALARM     16   // 闹钟
#define WIN_CALC      17   // 计算器
#define WIN_STORE     18   // 应用商店
#define WIN_ZIP       19   // 压缩工具
#define WIN_IDE       20   // 内置 IDE
#define WIN_TASKMGR   21   // 任务管理器
#define WIN_DISK      22   // 磁盘管理
#define WIN_NET       23   // 网络管理器
#define WIN_SNAKE     24   // 贪吃蛇
#define WIN_2048      25   // 2048
#define WIN_MINES     26   // 扫雷
#define WIN_BRICK     27   // 打砖块
#define WIN_PAINT     28   // 绘图板
#define WIN_NOTE      29   // 便签
#define WIN_UNIT      30   // 单位换算
#define WIN_STOPW     31   // 秒表
#define WIN_HEXVIEW   32   // 十六进制查看器
#define WIN_TETRIS    33   // 俄罗斯方块
#define WIN_TICTAC    34   // 井字棋
#define WIN_MEMORY    35   // 记忆翻牌
#define WIN_FIND      36   // 文件搜索
#define WIN_BASECONV  37   // 进制转换
#define WIN_RANDOM    38   // 随机数生成器
#define WIN_TEXTSTATS 39   // 文本统计
#define WIN_SUDOKU    40   // 数独
#define WIN_TYPING    41   // 打字测试
#define WIN_POMODORO  42   // 番茄钟
#define WIN_GUESS     43   // 猜数字
#define WIN_DICE      44   // 掷骰子
#define WIN_TODO      45   // 待办清单
#define WIN_PASSGEN    46   // random password generator
#define WIN_CLOCK     47   // 时钟/秒表 (RTC 时间 + stopwatch)
#define WIN_TOTAL     48   // 窗口类型总数

// 初始化桌面并进入 GUI 主循环 (永不返回)
void gui_run(void);

#endif
