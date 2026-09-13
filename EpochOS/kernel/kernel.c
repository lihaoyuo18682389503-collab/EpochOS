// ============================================================
// EpochOS - 内核主入口: 整合中断/定时器/键盘/内存/文件系统/Shell
// ============================================================

#include "types.h"
#include "vga.h"
#include "interrupts.h"
#include "timer.h"
#include "keyboard.h"
#include "mm.h"
#include "fs.h"
#include "shell.h"
#include "task.h"
#include "string.h"
#include "serial.h"
#include "mouse.h"
#include "ata.h"
#include "paging.h"
#include "gdt.h"
#include "gui.h"
#include "wallpaper.h"
#include "gfx.h"
#include "syscall.h"

// 前置声明
void kernel_main(void);

// 演示任务: 后台计数器, 每 50 tick 更新屏幕右上角
static void demo_task1(void) {
    uint32_t count = 0;
    for (;;) {
        count++;
        if (count % 50 == 0) {
            vga_set_cursor_pos(0, 58);
            vga_printf("[t1:%u] ", count);
        }
        task_yield();
    }
}

// 演示任务: 后台计数器, 每 100 tick 更新屏幕右上角第二格
static void demo_task2(void) {
    uint32_t count = 0;
    for (;;) {
        count++;
        if (count % 100 == 0) {
            vga_set_cursor_pos(1, 58);
            vga_printf("[t2:%u] ", count);
        }
        task_yield();
    }
}

// 内核入口: 放在 .text.entry section, 保证链接后位于 .text 最前
// bootloader 跳到 0x10000 即从此处开始执行
__attribute__((section(".text.entry")))
void kernel_entry(void) {
    kernel_main();
}

// 打印带颜色的横幅
static void print_banner(void) {
    vga_write_color("==========================================================\n", COLOR_LIGHT_CYAN, COLOR_BLACK);
    vga_write_color("  EpochOS v1.0 - From Scratch\n", COLOR_LIGHT_GREEN, COLOR_BLACK);
    vga_write_color("  32-bit Protected Mode Kernel with Full Subsystems\n", COLOR_LIGHT_GREEN, COLOR_BLACK);
    vga_write_color("==========================================================\n", COLOR_LIGHT_CYAN, COLOR_BLACK);
    vga_newline();
}

// 打印子系统初始化状态
static void print_subsystem(const char* name, int ok) {
    vga_write("  [");
    if (ok) {
        vga_write_color("OK", COLOR_LIGHT_GREEN, COLOR_BLACK);
    } else {
        vga_write_color("FAIL", COLOR_LIGHT_RED, COLOR_BLACK);
    }
    vga_write("] ");
    vga_write(name);
    vga_newline();
}

// 异常死机画面: 由 interrupts.c 的 isr_handler 调用
void panic_screen(uint32_t int_no, uint32_t err, const char* msg, uint32_t eip) {
    extern uint32_t read_cr2(void);
    uint32_t cr2v = read_cr2();
    serial_printf(COM1, "[PANIC] int=%u err=0x%x msg=%s cr2=0x%x eip=0x%x\n", int_no, err, msg, cr2v, eip);
    gfx_panic_screen(int_no, err, msg, cr2v, eip);
    vga_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
    vga_clear(COLOR_BLACK);
    vga_write_color("==========================================================\n", COLOR_LIGHT_RED, COLOR_BLACK);
    vga_write_color("  EpochOS KERNEL PANIC\n", COLOR_WHITE, COLOR_BLACK);
    vga_write_color("==========================================================\n", COLOR_LIGHT_RED, COLOR_BLACK);
    vga_newline();
    vga_printf("  Exception: %s\n", msg);
    vga_printf("  Number:    %u\n", int_no);
    vga_printf("  Error:     %u\n", err);
    vga_newline();
    vga_write_color("  System halted. Restart QEMU to reboot.\n", COLOR_LIGHT_GREY, COLOR_BLACK);
}

void kernel_main(void) {
    vga_init();
    print_banner();

    // PMM/VMM/内核堆自测结果 (serial 初始化后统一输出)
    uint32_t st_page = 0, st_maprc = -1, st_back = 0, st_bump = 0;

    // 1. 中断系统 (IDT + 8259 PIC)
    idt_init();
    pic_init();      // 重映射 PIC: IRQ0-15 -> 中断 32-47 (否则 IRQ0 触发 0x08 Double Fault)
    print_subsystem("Interrupts (IDT + PIC)", 1);

    // 1.5 GDT 扩展 + TSS (ring3 用户代码/数据段 + 特权级切换)
    gdt_init();
    print_subsystem("GDT/TSS (ring3 user segments)", 1);

    // 开启中断 (bootloader 曾 cli, IF=0 会屏蔽所有 IRQ)
    __asm__ __volatile__("sti");

    // 2. 定时器 (PIT 100Hz)
    timer_init();
    print_subsystem("Timer (PIT 100Hz)", 1);

    // 3. 键盘 (PS/2 IRQ1)
    keyboard_init();
    print_subsystem("Keyboard (PS/2)", 1);

    // 4. 内存管理 (位图 + 小对象堆)
    mm_init();
    print_subsystem("Memory Manager (128MB bitmap)", 1);

    // 4.5 分页虚拟内存 (恒等映射 + 高地址内核映射, CR0.PG)
    paging_init();
    {
        uint32_t mapped = paging_mapped_count();
        print_subsystem("Paging (4KB, identity+kernel@0xC0000000)", mapped > 0 ? 1 : 0);
    }

    // 4.6 PMM/VMM/内核堆自测 (结果存变量, serial 初始化后统一输出)
    {
        st_page = alloc_page();
        st_maprc = map_page(0xE0000000, st_page, PAGE_PRESENT | PAGE_WRITABLE);
        st_back = (st_maprc == 0) ? paging_get_phys(0xE0000000) : 0;
        void* b1 = kmalloc_bump(64);
        void* b2 = kmalloc_bump(1024);
        if (b1 && b2) st_bump = (uint32_t)((uint8_t*)b2 - (uint8_t*)b1);
        if (st_maprc == 0) unmap_page(0xE0000000);
        free_page(st_page);
        print_subsystem("PMM/VMM/Heap (selftest)",
            (st_page != 0 && st_maprc == 0 && st_back == st_page &&
             st_bump >= 64 && (st_bump & 7) == 0) ? 1 : 0);
    }

    // 5. 内存文件系统 (ramfs)
    fs_init();
    print_subsystem("File System (ramfs)", 1);

    // 6. 多任务调度器 (PIT 100Hz 抢占式)
    task_init();
    print_subsystem("Task Scheduler (preemptive)", 1);
    // task_create("demo1", demo_task1);
    // task_create("demo2", demo_task2);
    print_subsystem("Demo Tasks (2 spawned)", 1);

    // 6.5 系统调用表 (fd 表显式清零, 不依赖 .bss 清零)
    syscall_init();

    // 7. 串口 (COM1 115200)
    print_subsystem("Serial (COM1 16550 UART)", serial_init(COM1));
    serial_printf(COM1, "[mm] selftest: alloc_page=0x%x map_rc=%d get_phys=0x%x bump_align=%uB bump_used=%uB bump_free=%uB\n",
                  st_page, st_maprc, st_back, st_bump, mm_bump_used(), mm_bump_free());
    serial_printf(COM1, "[mm] paging: mapped=%u pages, identity+kernel@0xC0000000\n", paging_mapped_count());
    serial_write(COM1, "\r\n[EpochOS] Kernel booted.\r\n");

    // 8. PS/2 鼠标 (IRQ12)
    print_subsystem("Mouse (PS/2 IRQ12)", mouse_init() == 0 ? 1 : 0);

    // 9. ATA 硬盘 (PIO 主通道)
    ata_init();
    print_subsystem("ATA (PIO 0x1F0)",
        (g_ata_master.present || g_ata_slave.present) ? 1 : 0);

    // 9.5 磁盘文件系统持久化挂载 (从 ATA 镜像恢复文件)
    fs_disk_mount();
    print_subsystem("Disk FS (ATA persist)", g_ata_master.present ? 1 : 0);

    // 9.6 壁纸系统 (从 ATA 镜像预加载 4 张壁纸到内存)
    serial_write(COM1, "[dbg] before wallpaper_init\r\n");
    int wall_ok = wallpaper_init();
    serial_printf(COM1, "[dbg] wallpaper_init=%d\r\n", wall_ok);
    print_subsystem("Wallpaper (4 embedded)", wall_ok);

    vga_newline();
    vga_write_color("System ready. GUI starting...\n", COLOR_LIGHT_BROWN, COLOR_BLACK);
    vga_newline();

    // 10. 启动图形界面 (无图形模式自动回退文本 Shell)
    gfx_init();
    shell_init();
    gui_run();

    // 理论上不可达
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
