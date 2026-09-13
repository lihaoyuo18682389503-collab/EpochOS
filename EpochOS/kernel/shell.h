// ============================================================
// EpochOS - 命令行 Shell 头文件
// ============================================================
#ifndef EPOCHOS_SHELL_H
#define EPOCHOS_SHELL_H

void shell_init(void);
void shell_run(void);
void shell_process_char(char c);   // GUI 模式下逐字符喂入 Shell

#endif
