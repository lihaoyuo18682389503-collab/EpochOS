// ============================================================
// EpochOS - 抢占式多任务调度器实现 (含 ring3 用户进程)
// 依赖 PIT 定时器中断 (100Hz), 每个 IRQ 保存/恢复时切换上下文
// ============================================================
#include "task.h"
#include "mm.h"
#include "paging.h"
#include "gdt.h"
#include "string.h"
#include "serial.h"
#include "elf.h"

static uint32_t g_next_pid = 1;   // 进程号分配器 (task0=pid1)

// ---- 中断帧布局 (与 interrupts.S irq_common_stub 对应) ----
// 从栈顶 (esp 指向) 向下:
//   gs fs es ds | edi esi ebp esp ebx edx ecx eax | int_no err_code | eip cs eflags
#define FRAME_GS_OFF      0
#define FRAME_FS_OFF      4
#define FRAME_ES_OFF      8
#define FRAME_DS_OFF      12
#define FRAME_EDI_OFF     16
#define FRAME_ESI_OFF     20
#define FRAME_EBP_OFF     24
#define FRAME_ESP_OFF     28
#define FRAME_EBX_OFF     32
#define FRAME_EDX_OFF     36
#define FRAME_ECX_OFF     40
#define FRAME_EAX_OFF     44
#define FRAME_INTNO_OFF   48
#define FRAME_ERR_OFF     52
#define FRAME_EIP_OFF     56
#define FRAME_CS_OFF      60
#define FRAME_EFLAGS_OFF  64
#define FRAME_SIZE        68

static struct task tasks[TASK_MAX];
static struct task* current_task = 0;
static uint32_t multitasking_enabled = 0;

// 前置声明 (ELF 进程辅助, 定义在文件后部)
static void reap_exited_processes(void);
int process_track_page(struct task* t, uint32_t phys);

// 内核代码段/数据段选择子 (与 GDT 一致)
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10

void task_init(void) {
    for (int i = 0; i < TASK_MAX; i++) {
        tasks[i].state = TASK_EMPTY;
        tasks[i].stack_base = 0;
        tasks[i].ticks_run = 0;
        tasks[i].esp = 0;
        tasks[i].pid = 0;
        tasks[i].ring = TASK_RING0;
        tasks[i].cr3 = 0;
        tasks[i].user_entry = 0;
        tasks[i].user_stack = 0;
        tasks[i].user_exit_ip = 0;
        tasks[i].exit_code = -1;
        tasks[i].code_page = 0;
        tasks[i].stack_page = 0;
    }
    // 任务 0: 当前执行流 (内核主线程/shell), pid=1, 使用内核页目录
    tasks[0].id = 0;
    tasks[0].state = TASK_RUNNING;
    tasks[0].pid = g_next_pid++;
    tasks[0].ring = TASK_RING0;
    tasks[0].cr3 = (uint32_t)kernel_page_directory;
    strcpy(tasks[0].name, "main");
    current_task = &tasks[0];
    multitasking_enabled = 1;
}

// 选择下一个可运行任务 (round-robin, 从当前之后开始找)
static struct task* scheduler_pick_next(void) {
    int cur = (int)(current_task - tasks);
    for (int i = 1; i <= TASK_MAX; i++) {
        int idx = (cur + i) % TASK_MAX;
        if (tasks[idx].state == TASK_READY || tasks[idx].state == TASK_RUNNING) {
            return &tasks[idx];
        }
    }
    // 没有其他任务, 继续当前任务
    return current_task;
}

// 汇编切换点: 由 irq_common_stub 在恢复寄存器前 jmp 进入
// 保存当前 esp 到 current_task, 选择 next, 切换 CR3 (进程地址空间),
// 加载 next 的 esp, 然后跳回恢复序列
__asm__(
    ".globl scheduler_switch\n"
    "scheduler_switch:\n"
    "    cmpl $0, multitasking_enabled\n"
    "    je 1f\n"
    "    movl current_task, %eax\n"
    "    movl %esp, (%eax)\n"
    "    call scheduler_pick_next\n"
    "    movl %eax, current_task\n"
    "    pushl %eax\n"
    "    call scheduler_load_cr3\n"
    "    addl $4, %esp\n"
    "    movl current_task, %eax\n"
    "    movl (%eax), %esp\n"
    "1:  jmp irq_common_stub_restore\n"
);

// 调度器辅助: 切换 next 进程的页目录 (CR3), 并同步 TSS.esp0
// 用户进程拥有独立地址空间; 内核任务使用内核页目录
void scheduler_load_cr3(struct task* next) {
    if (!next) return;
    if (next->cr3) {
        __asm__ __volatile__("mov %0, %%cr3" : : "r"(next->cr3) : "memory");
    }
    if (next->ring == TASK_RING3 && next->stack_base) {
        // int 0x80 / 中断从 ring3 陷入时, CPU 从 TSS 取内核栈
        g_tss.esp0 = (uint32_t)(next->stack_base + TASK_STACK_SIZE);
    }
}

// 创建任务: 分配独立内核栈并伪造中断帧, 首次调度时 iret 进入入口
int task_create(const char* name, void (*entry)(void)) {
    if (!multitasking_enabled || !entry) {
        return -1;
    }
    int slot = -1;
    for (int i = 1; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_EMPTY) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }

    uint8_t* stack = (uint8_t*)kmalloc(TASK_STACK_SIZE);
    if (!stack) {
        return -1;
    }
    uint32_t* frame = (uint32_t*)(stack + TASK_STACK_SIZE - FRAME_SIZE);

    frame[FRAME_GS_OFF / 4]      = KERNEL_DS;
    frame[FRAME_FS_OFF / 4]      = KERNEL_DS;
    frame[FRAME_ES_OFF / 4]      = KERNEL_DS;
    frame[FRAME_DS_OFF / 4]      = KERNEL_DS;
    frame[FRAME_EDI_OFF / 4]     = 0;
    frame[FRAME_ESI_OFF / 4]     = 0;
    frame[FRAME_EBP_OFF / 4]     = 0;
    frame[FRAME_ESP_OFF / 4]     = 0;
    frame[FRAME_EBX_OFF / 4]     = 0;
    frame[FRAME_EDX_OFF / 4]     = 0;
    frame[FRAME_ECX_OFF / 4]     = 0;
    frame[FRAME_EAX_OFF / 4]     = 0;
    frame[FRAME_INTNO_OFF / 4]   = 0;
    frame[FRAME_ERR_OFF / 4]     = 0;
    frame[FRAME_EIP_OFF / 4]     = (uint32_t)entry;
    frame[FRAME_CS_OFF / 4]      = KERNEL_CS;
    frame[FRAME_EFLAGS_OFF / 4]  = 0x202;   // IF=1 开中断

    tasks[slot].id = slot;
    tasks[slot].state = TASK_READY;
    tasks[slot].esp = (uint32_t)frame;
    tasks[slot].stack_base = stack;
    tasks[slot].ticks_run = 0;
    tasks[slot].pid = g_next_pid++;
    tasks[slot].ring = TASK_RING0;
    tasks[slot].cr3 = (uint32_t)kernel_page_directory;
    tasks[slot].user_entry = 0;
    tasks[slot].user_stack = 0;
    tasks[slot].user_exit_ip = 0;
    tasks[slot].exit_code = -1;
    tasks[slot].code_page = 0;
    tasks[slot].stack_page = 0;
    tasks[slot].mmap_base = 0;
    strncpy(tasks[slot].name, name, TASK_NAME_MAX - 1);
    tasks[slot].name[TASK_NAME_MAX - 1] = 0;
    return slot;
}

// ============================================================
// ring3 用户进程 (阶段2: 最小进程模型)
// 布局: 用户代码 @ 0x08048000 (1 页), 用户栈 @ 0x40000000 (1 页)
// 页目录 = 内核页目录副本 (低地址内核区/高地址 0xC0000000 保持 supervisor),
//         用户页另加 PAGE_USER, 实现地址空间隔离
// 首次调度: scheduler_switch -> irq_common_stub_restore -> iret (ring3)
// ============================================================
#define USER_CS           0x18
#define USER_DS           0x20
#define USER_CODE_ADDR    0x08048000
#define USER_STACK_ADDR   0x40000000
#define USER_STACK_TOP    (USER_STACK_ADDR + 0x1000)
#define USER_FRAME_SIZE   76     // 含 iret 到 ring3 所需的 user_esp/ss

int process_create_user(const char* name, const uint8_t* code, uint32_t code_len) {
    if (!multitasking_enabled || !code || code_len == 0 || code_len > 4096) {
        return -1;
    }
    reap_exited_processes();
    int slot = -1;
    for (int i = 1; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_EMPTY) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }

    // 先声明并置零, 便于统一走 fail: 释放 (goto 不会跨越未初始化的变量)
    uint8_t* stack = 0;
    uint32_t code_page = 0, stack_page = 0, pd_phys = 0;

    stack = (uint8_t*)kmalloc(TASK_STACK_SIZE);
    if (!stack) return -1;
    code_page = alloc_page();
    if (!code_page) goto fail;
    stack_page = alloc_page();
    if (!stack_page) goto fail;
    pd_phys = alloc_page();
    if (!pd_phys) goto fail;

    // 用户代码拷入物理页 (内核通过恒等映射访问)
    memcpy((void*)code_page, code, code_len);

    // 用户页目录 = 内核页目录副本 (低地址内核区 + 高地址内核映射保持 supervisor)
    page_entry_t* pd = (page_entry_t*)pd_phys;
    memcpy(pd, kernel_page_directory, 1024 * sizeof(uint32_t));

    // 映射用户代码/用户栈到用户地址空间 (USER 标志)
    if (paging_map_page_in(pd, USER_CODE_ADDR, code_page,
                           PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER)) goto fail;
    if (paging_map_page_in(pd, USER_STACK_ADDR, stack_page,
                           PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER)) goto fail;

    // 伪造 ring3 中断帧: iret 时 CPU 弹 eip/cs/eflags/user_esp/ss
    // 注意: CS/SS 必须带 RPL=3 (|3), 否则 iret 不切换特权级 -> GPF(err=0x18)
    uint32_t* frame = (uint32_t*)(stack + TASK_STACK_SIZE - USER_FRAME_SIZE);
    frame[FRAME_GS_OFF / 4]      = USER_DS | 3;
    frame[FRAME_FS_OFF / 4]      = USER_DS | 3;
    frame[FRAME_ES_OFF / 4]      = USER_DS | 3;
    frame[FRAME_DS_OFF / 4]      = USER_DS | 3;
    frame[FRAME_EDI_OFF / 4]     = 0;
    frame[FRAME_ESI_OFF / 4]     = 0;
    frame[FRAME_EBP_OFF / 4]     = 0;
    frame[FRAME_ESP_OFF / 4]     = 0;
    frame[FRAME_EBX_OFF / 4]     = 0;
    frame[FRAME_EDX_OFF / 4]     = 0;
    frame[FRAME_ECX_OFF / 4]     = 0;
    frame[FRAME_EAX_OFF / 4]     = 0;
    frame[FRAME_INTNO_OFF / 4]   = 0;
    frame[FRAME_ERR_OFF / 4]     = 0;
    frame[FRAME_EIP_OFF / 4]     = USER_CODE_ADDR;
    frame[FRAME_CS_OFF / 4]      = USER_CS | 3;   // 0x1B
    frame[FRAME_EFLAGS_OFF / 4]  = 0x202;   // IF=1
    frame[17] = USER_STACK_TOP;             // user_esp
    frame[18] = USER_DS | 3;                // ss = 0x23

    tasks[slot].id = slot;
    tasks[slot].state = TASK_READY;
    tasks[slot].esp = (uint32_t)frame;
    tasks[slot].stack_base = stack;
    tasks[slot].ticks_run = 0;
    tasks[slot].pid = g_next_pid++;
    tasks[slot].ring = TASK_RING3;
    tasks[slot].cr3 = pd_phys;
    tasks[slot].user_entry = USER_CODE_ADDR;
    tasks[slot].user_stack = USER_STACK_ADDR;
    tasks[slot].user_exit_ip = USER_CODE_ADDR + (code_len - 2);  // 尾部 jmp $
    tasks[slot].exit_code = -1;
    tasks[slot].code_page = code_page;
    tasks[slot].stack_page = stack_page;
    tasks[slot].user_brk = USER_CODE_ADDR + 0x1000;  // 初始 break = 代码页尾
    tasks[slot].mmap_base = USER_STACK_ADDR - PAGE_SIZE;  // mmap 区底 (向下生长)
    tasks[slot].elf_page_count = 0;   // 记录物理页以便退出时回收
    process_track_page(&tasks[slot], code_page);
    process_track_page(&tasks[slot], stack_page);
    process_track_page(&tasks[slot], pd_phys);
    strncpy(tasks[slot].name, name, TASK_NAME_MAX - 1);
    tasks[slot].name[TASK_NAME_MAX - 1] = 0;

    // 特权级切换 (int 0x80 / PIT) 时 CPU 从 TSS.esp0 取内核栈
    g_tss.esp0 = (uint32_t)(stack + TASK_STACK_SIZE);

    serial_printf(COM1, "[proc] create pid=%u '%s' ring=%u entry=0x%x "
                        "ustack=0x%x pd=0x%x code=%uB\n",
                  tasks[slot].pid, tasks[slot].name, tasks[slot].ring,
                  USER_CODE_ADDR, USER_STACK_TOP, pd_phys, code_len);
    return slot;

fail:
    // 统一回收本函数已分配的资源 (变量均为 0 初始化, 未分配到的项 free_page 会跳过)
    if (code_page)  free_page(code_page);
    if (stack_page) free_page(stack_page);
    if (pd_phys)    free_page(pd_phys);
    if (stack)      kfree(stack);
    serial_write_str("[proc] create_user failed, resources freed\n");
    return -1;
}

// 当前任务退出: 标记退出并停在这里 (不再被调度)
void task_exit(void) {
    current_task->state = TASK_EXITED;
    __asm__ __volatile__("cli; hlt");
    for (;;) {}
}

// 协作式让出: 触发一次 PIT 中断走调度
void task_yield(void) {
    __asm__ __volatile__("int $0x20");
}

int task_get_count(void) {
    int n = 0;
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state != TASK_EMPTY) {
            n++;
        }
    }
    return n;
}

const struct task* task_get(int id) {
    if (id < 0 || id >= TASK_MAX || tasks[id].state == TASK_EMPTY) {
        return 0;
    }
    return &tasks[id];
}

uint32_t task_get_current_id(void) {
    return current_task ? (uint32_t)(current_task - tasks) : 0;
}

struct task* task_get_current(void) {
    return current_task;
}

uint32_t task_get_current_pid(void) {
    return current_task ? current_task->pid : 0;
}

// 结束任务: 仅允许结束非内核主线程 (id>0), 置 EXITED 后调度器不再选择
int task_kill(int id) {
    if (id <= 0 || id >= TASK_MAX) return -1;
    if (tasks[id].state == TASK_EMPTY || tasks[id].state == TASK_EXITED) return -1;
    tasks[id].state = TASK_EXITED;
    return 0;
}

// ---- ELF 进程 (阶段3) ----

// 回收已退出的 ring3 进程: 释放用户物理页/页目录/内核栈 (惰性, 在创建新进程前调用)
static void reap_exited_processes(void) {
    for (int i = 1; i < TASK_MAX; i++) {
        struct task* t = &tasks[i];
        if (t->state != TASK_EXITED || t->ring != TASK_RING3) continue;
        // 用户进程物理页 (段页/栈页/页目录)
        for (uint32_t p = 0; p < t->elf_page_count; p++) {
            if (t->elf_pages[p]) free_page(t->elf_pages[p]);
        }
        // 内核栈 (页对齐, kmalloc 分配)
        if (t->stack_base) kfree(t->stack_base);
        serial_printf(COM1, "[task] reaped pid=%u '%s' (%u pages freed)\r\n",
                      t->pid, t->name, t->elf_page_count);
        memset(t, 0, sizeof(*t));
        t->state = TASK_EMPTY;
        t->exit_code = -1;
    }
}

// 记录物理页到任务 (统一回收)
// 返回 0 成功; 跟踪表已满返回 -1 (调用方必须据此释放该页, 否则泄漏)
int process_track_page(struct task* t, uint32_t phys) {
    if (!t || !phys) return -1;
    if (t->elf_page_count < TASK_MAX_TRACKED_PAGES) {
        t->elf_pages[t->elf_page_count++] = phys;
        return 0;
    }
    serial_write_str("[task] page tracking table full, page leaked!\n");
    return -1;
}

// 从跟踪表移除一页 (munmap/mmap 回滚后物理页已 free, 防止退出时二次释放)
void process_untrack_page(struct task* t, uint32_t phys) {
    if (!t || !phys) return;
    for (uint32_t i = 0; i < t->elf_page_count; i++) {
        if (t->elf_pages[i] == phys) {
            for (uint32_t j = i; j + 1 < t->elf_page_count; j++)
                t->elf_pages[j] = t->elf_pages[j + 1];
            t->elf_page_count--;
            t->elf_pages[t->elf_page_count] = 0;
            return;
        }
    }
}

// 用户栈标准布局 (Linux i386): 从高地址向下
//   [高] argv 字符串区 (argv[0]..argv[argc-1])
//        auxv[] (AT_NULL 结束, 仅占位)
//        envp[] (NULL 结束, 空)
//        argv[] (argv[0]..argv[argc-1], NULL)
//   esp -> argc
// 返回 init_esp; 字符串超界时打印并返回 0
static uint32_t build_user_stack(uint8_t* stack_phys, uint32_t stack_pages,
                                 uint32_t stack_vaddr, int argc,
                                 const char* const* argv) {
    uint32_t top = stack_vaddr + stack_pages * PAGE_SIZE;
    uint32_t esp = top;
    uint32_t base_phys = (uint32_t)(uint32_t)stack_phys;

    if (argc < 1) argc = 1;
    if (argc > 16) argc = 16;                 // 上限 16 个参数

    // 字符串区 (高地址向下): argv[argc-1] .. argv[0]
    uint32_t astr[16];
    for (int i = argc - 1; i >= 0; i--) {
        const char* s = argv[i] ? argv[i] : "";
        uint32_t n = (uint32_t)strlen(s) + 1;
        esp -= n;
        if (esp < stack_vaddr + 0x80) {       // 防溢出 (预留栈底结构区)
            serial_write_str("[proc] user stack overflow (argv too long)\n");
            return 0;
        }
        memcpy((void*)(uint32_t)(esp - stack_vaddr + base_phys), s, n);
        astr[i] = esp;
    }
    esp &= ~3u;                               // 4 字节对齐

    // auxv[0] = AT_NULL (type=0, value=0)
    esp -= 8;
    *(uint32_t*)(uint32_t)(esp - stack_vaddr + base_phys) = 0;
    *(uint32_t*)(uint32_t)(esp + 4 - stack_vaddr + base_phys) = 0;
    // envp[0] = NULL
    esp -= 4;
    *(uint32_t*)(uint32_t)(esp - stack_vaddr + base_phys) = 0;
    // argv[] 指针数组 + NULL
    esp -= (uint32_t)(argc + 1) * 4;
    for (int i = 0; i < argc; i++) {
        *(uint32_t*)(uint32_t)(esp + (uint32_t)i * 4 - stack_vaddr + base_phys) =
            astr[i];
    }
    *(uint32_t*)(uint32_t)(esp + (uint32_t)argc * 4 - stack_vaddr + base_phys) = 0;
    // argc
    esp -= 4;
    *(uint32_t*)(uint32_t)(esp - stack_vaddr + base_phys) = (uint32_t)argc;
    return esp;
}

// ELF 用户进程布局:
//   ELF 段: vaddr 由 phdr 指定 (通常 .text @ 0x08048000), 逐页映射 USER 标志
//   用户栈: 0x40000000 起, 默认 1 页 (向下生长, 栈顶含 argc/argv/env/auxv)
//   注意: 物理页单独 alloc_page 不保证连续, 故栈只取 1 页, 由 build_user_stack
//         在单页内写入 argc/argv/env/auxv (小程序栈深 4KB 足够)
#define ELF_USER_STACK_ADDR 0x40000000
#define ELF_USER_STACK_PAGES 1

// 用户空间虚拟地址下限 —— 关键隔离约束, 不要随意下调!
// 用户页目录是内核页目录的副本 (memcpy), 而内核页目录对 [0, MEMORY_END)
// 做的是恒等映射: 该区间的 PDE 直接指向**内核自己的页表**(共享同一张表)。
// 因此若把用户页映射进 [0, MEMORY_END), paging_map_page_in 会改写内核的 PTE:
//   - 内核对该虚拟地址的恒等映射被替换成用户页 -> 内核自身访问踩空/数据错乱
//   - 新 PTE 带 PAGE_USER -> 用户进程可直接读写内核正在使用的物理页 (提权)
// 故用户地址一律限制在 [MEMORY_END, ELF_USER_STACK_ADDR)。
// 注: SYS_brk 的起点 user_brk 来自本文件的段尾, 恒 >= MEMORY_END 且只增不减,
//     因此天然落在该安全区间内。
#define USER_MIN_VADDR  MEMORY_END

int process_create_elf(const char* name, const uint8_t* elf, uint32_t elf_len,
                       const void* ehdr_v, const void* phdrs_v, int phnum,
                       int argc, const char* const* argv) {
    if (!multitasking_enabled || !elf || !ehdr_v || !phdrs_v || phnum <= 0) {
        return -1;
    }
    const struct elf32_ehdr* eh = (const struct elf32_ehdr*)ehdr_v;
    const struct elf32_phdr* ph = (const struct elf32_phdr*)phdrs_v;

    reap_exited_processes();

    int slot = -1;
    for (int i = 1; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_EMPTY) { slot = i; break; }
    }
    if (slot < 0) return -1;

    uint8_t* stack = (uint8_t*)kmalloc(TASK_STACK_SIZE);
    if (!stack) return -1;
    uint32_t pd_phys = alloc_page();
    if (!pd_phys) return -1;

    // 用户页目录 = 内核页目录副本 (低地址内核区 + 高地址内核映射保持 supervisor)
    page_entry_t* pd = (page_entry_t*)pd_phys;
    memcpy(pd, kernel_page_directory, 1024 * sizeof(uint32_t));

    struct task* t = &tasks[slot];
    memset(t, 0, sizeof(*t));
    t->elf_page_count = 0;
    process_track_page(t, pd_phys);

    // ---- 映射 ELF PT_LOAD 段 ----
    uint32_t max_brk_vaddr = 0;
    for (int i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint32_t vaddr = ph[i].p_vaddr;
        uint32_t remain = ph[i].p_filesz;   // 待复制文件字节
        uint32_t memsz  = ph[i].p_memsz;
        // 覆盖范围: [vaddr, vaddr+memsz), 页对齐
        uint32_t start = vaddr & ~(PAGE_SIZE - 1);
        uint32_t end   = (vaddr + memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (start < USER_MIN_VADDR || end > ELF_USER_STACK_ADDR) {
            serial_printf(COM1, "[elf] segment vaddr out of range: 0x%x-0x%x "
                                "(allowed 0x%x-0x%x)\n",
                          start, end, USER_MIN_VADDR, ELF_USER_STACK_ADDR);
            goto fail;
        }
        for (uint32_t va = start; va < end; va += PAGE_SIZE) {
            uint32_t phys = alloc_page();
            if (!phys) { serial_write_str("[elf] alloc_page failed\n"); goto fail; }
            if (paging_map_page_in(pd, va, phys,
                                   PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER)) {
                free_page(phys);
                serial_write_str("[elf] map failed\n");
                goto fail;
            }
            // 跟踪表容量有限: 大 .bss 的程序可能超出, 此时放弃加载而不是
            // 留下无法回收的孤儿页 (否则进程退出后这些页永久泄漏)
            if (process_track_page(t, phys) != 0) {
                free_page(phys);
                serial_write_str("[elf] page tracking table full\n");
                goto fail;
            }
            // 本页相对段起始的偏移 (首页可能不在页边界)
            uint32_t page_off = (va == start) ? (vaddr & (PAGE_SIZE - 1)) : 0;
            // 复制 filesz 剩余部分
            uint32_t copy_len = 0;
            if (remain > 0) {
                copy_len = PAGE_SIZE - page_off;
                if (copy_len > remain) copy_len = remain;
                memcpy((void*)(uint32_t)(phys + page_off),
                       elf + ph[i].p_offset + (ph[i].p_filesz - remain), copy_len);
                remain -= copy_len;
            }
            // 本页剩余清零 (bss: memsz - filesz)
            if (PAGE_SIZE - page_off > copy_len) {
                memset((void*)(uint32_t)(phys + page_off + copy_len), 0,
                       PAGE_SIZE - page_off - copy_len);
            }
        }
        uint32_t brk_end = (vaddr + memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (brk_end > max_brk_vaddr) max_brk_vaddr = brk_end;
    }

    // ---- 映射用户栈 (0x40000000 起, 2 页) ----
    uint32_t stack_vaddr = ELF_USER_STACK_ADDR;
    uint32_t stack_phys0 = 0;
    for (uint32_t i = 0; i < ELF_USER_STACK_PAGES; i++) {
        uint32_t phys = alloc_page();
        if (!phys) { serial_write_str("[elf] stack alloc failed\n"); goto fail; }
        if (paging_map_page_in(pd, stack_vaddr + i * PAGE_SIZE, phys,
                               PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER)) {
            free_page(phys);
            goto fail;
        }
        if (process_track_page(t, phys) != 0) {
            free_page(phys);
            serial_write_str("[elf] page tracking table full (stack)\n");
            goto fail;
        }
        if (i == 0) stack_phys0 = phys;
    }
    // 栈内容 (argc/argv/env/auxv) 写入第一物理页
    uint32_t init_esp = build_user_stack((uint8_t*)(uint32_t)stack_phys0,
                                         ELF_USER_STACK_PAGES, stack_vaddr,
                                         argc, argv);
    if (init_esp == 0) {                      // argv 过长溢出, 释放后失败
        for (uint32_t p = 0; p < t->elf_page_count; p++) {
            if (t->elf_pages[p]) free_page(t->elf_pages[p]);
        }
        if (stack) kfree(stack);
        return -1;
    }

    // ---- 伪造 ring3 中断帧 ----
    uint32_t* frame = (uint32_t*)(stack + TASK_STACK_SIZE - USER_FRAME_SIZE);
    frame[FRAME_GS_OFF / 4]      = USER_DS | 3;
    frame[FRAME_FS_OFF / 4]      = USER_DS | 3;
    frame[FRAME_ES_OFF / 4]      = USER_DS | 3;
    frame[FRAME_DS_OFF / 4]      = USER_DS | 3;
    frame[FRAME_EDI_OFF / 4]     = 0;
    frame[FRAME_ESI_OFF / 4]     = 0;
    frame[FRAME_EBP_OFF / 4]     = 0;
    frame[FRAME_ESP_OFF / 4]     = 0;
    frame[FRAME_EBX_OFF / 4]     = 0;
    frame[FRAME_EDX_OFF / 4]     = 0;
    frame[FRAME_ECX_OFF / 4]     = 0;
    frame[FRAME_EAX_OFF / 4]     = 0;
    frame[FRAME_INTNO_OFF / 4]   = 0;
    frame[FRAME_ERR_OFF / 4]     = 0;
    frame[FRAME_EIP_OFF / 4]     = eh->e_entry;
    frame[FRAME_CS_OFF / 4]      = USER_CS | 3;   // 0x1B
    frame[FRAME_EFLAGS_OFF / 4]  = 0x202;         // IF=1
    frame[17] = init_esp;                          // user_esp
    frame[18] = USER_DS | 3;                       // ss = 0x23

    t->id = slot;
    t->state = TASK_READY;
    t->esp = (uint32_t)frame;
    t->stack_base = stack;
    t->ticks_run = 0;
    t->pid = g_next_pid++;
    t->ring = TASK_RING3;
    t->cr3 = pd_phys;
    t->user_entry = eh->e_entry;
    t->user_stack = stack_vaddr;
    t->user_exit_ip = 0;        // 用户程序 exit 后自循环, 调度器跳过
    t->exit_code = -1;
    t->code_page = 0;
    t->stack_page = 0;
    t->user_brk = max_brk_vaddr;
    t->mmap_base = stack_vaddr - PAGE_SIZE;   // mmap 区底 (向下生长), 与 process_create_user 一致
    strncpy(t->name, name, TASK_NAME_MAX - 1);
    t->name[TASK_NAME_MAX - 1] = 0;

    // 特权级切换 (int 0x80 / PIT) 时 CPU 从 TSS.esp0 取内核栈
    g_tss.esp0 = (uint32_t)(stack + TASK_STACK_SIZE);

    serial_printf(COM1, "[proc] create pid=%u '%s' ring=%u entry=0x%x "
                        "ustack=0x%x esp=0x%x pd=0x%x elf=%uB pages=%u argc=%d\r\n",
                  t->pid, t->name, t->ring, eh->e_entry,
                  stack_vaddr, init_esp, pd_phys, elf_len, t->elf_page_count, argc);
    return slot;

fail:
    // 释放已分配资源
    for (uint32_t p = 0; p < t->elf_page_count; p++) {
        if (t->elf_pages[p]) free_page(t->elf_pages[p]);
    }
    if (stack) kfree(stack);
    return -1;
}

// ============================================================
// exec 重载 (阶段5): 当前进程调用 execve -> 释放旧 ELF 段/栈物理页,
// 在同一个 task slot / pid / 内核栈上映射新 ELF, 切换到新页目录
// 调用前提: do_execve (syscall.c) 已把新 ELF 读入内核缓冲并完成 elf_parse;
//           argv 指向内核侧字符串 (用户栈字符串已在 exec 前拷贝)
// 失败时不触碰旧进程任何资源; 成功后旧用户页全部释放, cr3 指向新目录
// ============================================================
int process_exec_reload(struct task* t, const uint8_t* elf, uint32_t elf_len,
                        const struct elf32_ehdr* eh,
                        const struct elf32_phdr* ph, int phnum,
                        int argc, const char* const* argv, uint32_t* out_esp) {
    if (!t || !eh || !ph || phnum <= 0 || !out_esp) return -1;
    if (t->ring != TASK_RING3) return -1;
    (void)elf_len;

    // 必须与 struct task 的 elf_pages[] 等长: 否则超出的页既无法被跟踪,
    // 也不会在进程退出/再次 exec 时回收 -> 每次 exec 静默泄漏物理页
    uint32_t new_pages[TASK_MAX_TRACKED_PAGES];
    int npc = 0;

    uint32_t new_pd = alloc_page();
    if (!new_pd) {
        serial_write_str("[exec] alloc pd failed\n");
        return -1;
    }
    new_pages[npc++] = new_pd;
    page_entry_t* pd = (page_entry_t*)new_pd;
    memcpy(pd, kernel_page_directory, 1024 * sizeof(uint32_t));

    // ---- 映射新 ELF PT_LOAD 段 (与 process_create_elf 同逻辑) ----
    uint32_t max_brk_vaddr = 0;
    for (int i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint32_t vaddr = ph[i].p_vaddr;
        uint32_t remain = ph[i].p_filesz;
        uint32_t memsz  = ph[i].p_memsz;
        uint32_t start = vaddr & ~(PAGE_SIZE - 1);
        uint32_t end   = (vaddr + memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (start < USER_MIN_VADDR || end > ELF_USER_STACK_ADDR) {
            serial_printf(COM1, "[exec] segment out of range: 0x%x-0x%x "
                                "(allowed 0x%x-0x%x)\n",
                          start, end, USER_MIN_VADDR, ELF_USER_STACK_ADDR);
            goto rollback;
        }
        for (uint32_t va = start; va < end; va += PAGE_SIZE) {
            uint32_t phys = alloc_page();
            if (!phys) { serial_write_str("[exec] alloc seg page failed\n"); goto rollback; }
            if (npc >= TASK_MAX_TRACKED_PAGES) { free_page(phys); serial_write_str("[exec] too many pages\n"); goto rollback; }
            if (paging_map_page_in(pd, va, phys,
                                   PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER)) {
                free_page(phys);
                serial_write_str("[exec] map seg failed\n");
                goto rollback;
            }
            new_pages[npc++] = phys;
            uint32_t page_off = (va == start) ? (vaddr & (PAGE_SIZE - 1)) : 0;
            uint32_t copy_len = 0;
            if (remain > 0) {
                copy_len = PAGE_SIZE - page_off;
                if (copy_len > remain) copy_len = remain;
                memcpy((void*)(uint32_t)(phys + page_off),
                       elf + ph[i].p_offset + (ph[i].p_filesz - remain), copy_len);
                remain -= copy_len;
            }
            if (PAGE_SIZE - page_off > copy_len) {
                memset((void*)(uint32_t)(phys + page_off + copy_len), 0,
                       PAGE_SIZE - page_off - copy_len);
            }
        }
        uint32_t brk_end = (vaddr + memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (brk_end > max_brk_vaddr) max_brk_vaddr = brk_end;
    }

    // ---- 新用户栈 (0x40000000 起 1 页) ----
    uint32_t stack_vaddr = ELF_USER_STACK_ADDR;
    uint32_t stack_phys0 = 0;
    for (uint32_t i = 0; i < ELF_USER_STACK_PAGES; i++) {
        uint32_t phys = alloc_page();
        if (!phys) { serial_write_str("[exec] stack alloc failed\n"); goto rollback; }
        if (npc >= TASK_MAX_TRACKED_PAGES) { free_page(phys); serial_write_str("[exec] stack pages full\n"); goto rollback; }
        if (paging_map_page_in(pd, stack_vaddr + i * PAGE_SIZE, phys,
                               PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER)) {
            free_page(phys);
            serial_write_str("[exec] stack map failed\n");
            goto rollback;
        }
        new_pages[npc++] = phys;
        if (i == 0) stack_phys0 = phys;
    }
    uint32_t init_esp = build_user_stack((uint8_t*)(uint32_t)stack_phys0,
                                         ELF_USER_STACK_PAGES, stack_vaddr,
                                         argc, argv);
    if (init_esp == 0) {
        serial_write_str("[exec] build_user_stack overflow\n");
        goto rollback;
    }

    // ---- 全部成功: 切页目录 -> 释放旧用户页 -> 更新任务 ----
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(new_pd) : "memory");
    for (uint32_t p = 0; p < t->elf_page_count; p++) {
        if (t->elf_pages[p]) free_page(t->elf_pages[p]);
    }
    // 上面每次分配都已保证 npc <= TASK_MAX_TRACKED_PAGES, 故此处不会截断
    for (int k = 0; k < TASK_MAX_TRACKED_PAGES; k++) t->elf_pages[k] = 0;
    for (int k = 0; k < npc; k++) t->elf_pages[k] = new_pages[k];
    t->elf_page_count = (uint32_t)npc;
    t->cr3 = new_pd;
    t->user_entry = eh->e_entry;
    t->user_stack = stack_vaddr;
    t->user_exit_ip = 0;
    t->exit_code = -1;
    t->state = TASK_READY;
    t->code_page = 0;
    t->stack_page = 0;
    t->user_brk = max_brk_vaddr;
    t->mmap_base = stack_vaddr - PAGE_SIZE;   // mmap 区底 (向下生长), exec 后重置
    g_tss.esp0 = (uint32_t)(t->stack_base + TASK_STACK_SIZE);
    *out_esp = init_esp;

    serial_printf(COM1, "[exec] pid=%u reload '%s' entry=0x%x esp=0x%x pd=0x%x pages=%d argc=%d\r\n",
                  t->pid, t->name, eh->e_entry, init_esp, new_pd, npc, argc);
    return 0;

rollback:
    for (int k = 0; k < npc; k++) {
        if (new_pages[k]) free_page(new_pages[k]);
    }
    return -1;
}
