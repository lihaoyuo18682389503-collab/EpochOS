// ============================================================
// EpochOS - 进程/任务调度器 (抢占式 round-robin + ring3 用户进程)
// PIT 定时器中断驱动上下文切换, 每个任务独立内核栈
// 进程模型扩展: pid / ring / CR3(地址空间) / 用户入口与栈 / 退出码
// ============================================================
#ifndef EPOCHOS_TASK_H
#define EPOCHOS_TASK_H

#include "types.h"

#define TASK_MAX        8
#define TASK_STACK_SIZE 8192      // 每个任务独立内核栈 (2 页)
#define TASK_NAME_MAX   16

// 任务状态
#define TASK_EMPTY     0
#define TASK_READY     1
#define TASK_RUNNING   2
#define TASK_EXITED    3

// 特权级
#define TASK_RING0     0          // 内核任务
#define TASK_RING3     3          // 用户态进程

struct task {
    uint32_t esp;                 // 保存的栈指针 (指向中断帧顶)
    uint8_t  state;
    uint8_t  id;
    char     name[TASK_NAME_MAX];
    uint8_t* stack_base;          // 任务栈底 (用于释放)
    uint32_t ticks_run;           // 运行累计 tick 数
    // ---- 进程模型扩展 ----
    uint32_t pid;                 // 进程号 (全局自增)
    uint32_t ring;                // 0=内核任务, 3=用户进程
    uint32_t cr3;                 // 页目录物理地址 (用户进程独立地址空间)
    uint32_t user_entry;          // 用户入口虚拟地址
    uint32_t user_stack;          // 用户栈虚拟地址
    uint32_t user_exit_ip;        // 用户代码尾部 halt 循环 (exit 后 eip 指向)
    int      exit_code;           // 退出码 (-1 = 未退出)
    uint32_t code_page;           // 用户代码物理页
    uint32_t stack_page;          // 用户栈物理页
    // ---- ELF 进程扩展 (阶段3) ----
#define TASK_MAX_TRACKED_PAGES 64
    uint32_t elf_pages[TASK_MAX_TRACKED_PAGES];  // 用户进程全部物理页 (段页/栈页/页目录/mmap页), 退出时回收
    uint32_t elf_page_count;      // elf_pages 有效数量
    uint32_t user_brk;            // 用户堆 break 地址 (SYS_brk), 初始 = ELF 最高段页对齐尾
    uint32_t mmap_base;           // 用户 mmap 区当前底 (向下生长), 初始 = 栈基 - 1 页; SYS_mmap 用之分配匿名/文件映射页
};

// 初始化调度器并注册当前执行流为任务 0 (pid=1)
void task_init(void);

// 创建内核任务: 入口函数 + 名称, 返回任务 id, 失败返回 -1
int task_create(const char* name, void (*entry)(void));

// 创建 ring3 用户进程: 拷贝用户代码到独立页表空间并进入用户态
// code 为原始机器码, code_len 必须 <= 4096; 返回 slot id, 失败返回 -1
int process_create_user(const char* name, const uint8_t* code, uint32_t code_len);

// 创建 ring3 用户进程 (ELF 版, 阶段3/4): 解析 ELF32 的 PT_LOAD 段,
// 分配物理页并映射到独立地址空间, 建立用户栈 (argc/argv/env/auxv), 跳转 entry
// argc>=1, argv[0]=进程名; 返回 slot id, 失败返回 -1
int process_create_elf(const char* name, const uint8_t* elf, uint32_t elf_len,
                       const void* ehdr, const void* phdrs, int phnum,
                       int argc, const char* const* argv);

// exec (阶段5): 在当前进程 slot 上重载新 ELF 段/栈 (pid/cr3/内核栈保持不变),
// 成功后释放旧用户物理页并切换页目录, 回填 *out_esp (新用户栈初始 esp);
// 失败返回 -1 (原进程完好可继续运行)
struct elf32_ehdr;
struct elf32_phdr;
int process_exec_reload(struct task* t, const uint8_t* elf, uint32_t elf_len,
                        const struct elf32_ehdr* eh,
                        const struct elf32_phdr* ph, int phnum,
                        int argc, const char* const* argv, uint32_t* out_esp);

// 记录物理页到任务 (ELF 进程统一回收, 供 syscall/brk 扩展堆页使用)
// 返回 0 成功, -1 表示跟踪表已满
int process_track_page(struct task* t, uint32_t phys);
// 从物理页跟踪表移除一页 (munmap / mmap 回滚释放后调用, 防止退出时二次释放)
void process_untrack_page(struct task* t, uint32_t phys);

// 当前任务主动退出 (入口函数返回或调用此函数)
void task_exit(void);

// 让出 CPU (协作式调度, 立即切换)
void task_yield(void);

// 调度器: 由中断 stub 调用, 切换上下文
void scheduler_switch(void);

// 调度器辅助: 切换 CR3 (并更新 TSS.esp0) —— 由 scheduler_switch 汇编调用
void scheduler_load_cr3(struct task* next);

// 查询任务数量 / 指定任务信息 (供 ps 命令)
int  task_get_count(void);
const struct task* task_get(int id);
uint32_t task_get_current_id(void);
struct task* task_get_current(void);
uint32_t task_get_current_pid(void);

// 结束指定任务 (id>0): 标记 EXITED 不再被调度; 成功返回 0, 失败返回 -1
int task_kill(int id);

#endif
