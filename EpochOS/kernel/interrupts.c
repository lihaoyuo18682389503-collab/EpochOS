// ============================================================
// EpochOS - IDT 初始化与中断分发
// ============================================================
#include "interrupts.h"
#include "syscall.h"

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idtp;

// 中断处理函数表 (IRQ 0-15)
static void* irq_routines[16] = { 0, 0, 0, 0, 0, 0, 0, 0,
                                  0, 0, 0, 0, 0, 0, 0, 0 };

// 异常名称表
static const char* const exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",
    "Coprocessor Fault",
    "Alignment Check",
    "Machine Check",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved"
};

static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_lo = base & 0xFFFF;
    idt[num].base_hi = (base >> 16) & 0xFFFF;
    idt[num].sel = sel;
    idt[num].always0 = 0;
    idt[num].flags = flags;
}

static void idt_flush(void) {
    __asm__ __volatile__("lidt %0" : : "m"(idtp));
}

void idt_init(void) {
    idtp.limit = sizeof(struct idt_entry) * IDT_ENTRIES - 1;
    idtp.base = (uint32_t)&idt[0];

    // 清零全部表项
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    // 注册异常 0-31 (ring0, 32-bit interrupt gate)
    idt_set_gate(0, (uint32_t)isr0, 0x08, 0x8E);
    idt_set_gate(1, (uint32_t)isr1, 0x08, 0x8E);
    idt_set_gate(2, (uint32_t)isr2, 0x08, 0x8E);
    idt_set_gate(3, (uint32_t)isr3, 0x08, 0x8E);
    idt_set_gate(4, (uint32_t)isr4, 0x08, 0x8E);
    idt_set_gate(5, (uint32_t)isr5, 0x08, 0x8E);
    idt_set_gate(6, (uint32_t)isr6, 0x08, 0x8E);
    idt_set_gate(7, (uint32_t)isr7, 0x08, 0x8E);
    idt_set_gate(8, (uint32_t)isr8, 0x08, 0x8E);
    idt_set_gate(9, (uint32_t)isr9, 0x08, 0x8E);
    idt_set_gate(10, (uint32_t)isr10, 0x08, 0x8E);
    idt_set_gate(11, (uint32_t)isr11, 0x08, 0x8E);
    idt_set_gate(12, (uint32_t)isr12, 0x08, 0x8E);
    idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E);
    idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E);
    idt_set_gate(15, (uint32_t)isr15, 0x08, 0x8E);
    idt_set_gate(16, (uint32_t)isr16, 0x08, 0x8E);
    idt_set_gate(17, (uint32_t)isr17, 0x08, 0x8E);
    idt_set_gate(18, (uint32_t)isr18, 0x08, 0x8E);
    idt_set_gate(19, (uint32_t)isr19, 0x08, 0x8E);
    idt_set_gate(20, (uint32_t)isr20, 0x08, 0x8E);
    idt_set_gate(21, (uint32_t)isr21, 0x08, 0x8E);
    idt_set_gate(22, (uint32_t)isr22, 0x08, 0x8E);
    idt_set_gate(23, (uint32_t)isr23, 0x08, 0x8E);
    idt_set_gate(24, (uint32_t)isr24, 0x08, 0x8E);
    idt_set_gate(25, (uint32_t)isr25, 0x08, 0x8E);
    idt_set_gate(26, (uint32_t)isr26, 0x08, 0x8E);
    idt_set_gate(27, (uint32_t)isr27, 0x08, 0x8E);
    idt_set_gate(28, (uint32_t)isr28, 0x08, 0x8E);
    idt_set_gate(29, (uint32_t)isr29, 0x08, 0x8E);
    idt_set_gate(30, (uint32_t)isr30, 0x08, 0x8E);
    idt_set_gate(31, (uint32_t)isr31, 0x08, 0x8E);

    // 注册 0x80 系统调用中断门: DPL=3 (0xEE), 允许用户态 int 0x80 陷入
    idt_set_gate(0x80, (uint32_t)isr128, 0x08, 0xEE);

    // 注册 IRQ 0-15 -> 中断号 32-47
    idt_set_gate(32, (uint32_t)irq0, 0x08, 0x8E);
    idt_set_gate(33, (uint32_t)irq1, 0x08, 0x8E);
    idt_set_gate(34, (uint32_t)irq2, 0x08, 0x8E);
    idt_set_gate(35, (uint32_t)irq3, 0x08, 0x8E);
    idt_set_gate(36, (uint32_t)irq4, 0x08, 0x8E);
    idt_set_gate(37, (uint32_t)irq5, 0x08, 0x8E);
    idt_set_gate(38, (uint32_t)irq6, 0x08, 0x8E);
    idt_set_gate(39, (uint32_t)irq7, 0x08, 0x8E);
    idt_set_gate(40, (uint32_t)irq8, 0x08, 0x8E);
    idt_set_gate(41, (uint32_t)irq9, 0x08, 0x8E);
    idt_set_gate(42, (uint32_t)irq10, 0x08, 0x8E);
    idt_set_gate(43, (uint32_t)irq11, 0x08, 0x8E);
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E);
    idt_set_gate(45, (uint32_t)irq13, 0x08, 0x8E);
    idt_set_gate(46, (uint32_t)irq14, 0x08, 0x8E);
    idt_set_gate(47, (uint32_t)irq15, 0x08, 0x8E);

    idt_flush();
}

// 异常/系统调用处理 (由 isr_common_stub 调用)
void isr_handler(struct regs* r) {
    if (r->int_no == 0x80) {
        // 用户态系统调用 (int 0x80)
        syscall_handler(r);
        return;
    }
    if (r->int_no < 32) {
        // 输出异常信息到 VGA (由 kernel.c 提供)
        extern void panic_screen(uint32_t int_no, uint32_t err, const char* msg, uint32_t eip);
        panic_screen(r->int_no, r->err_code, exception_messages[r->int_no], r->eip);
        for (;;) {
            __asm__ __volatile__("hlt");
        }
    }
}

// IRQ 处理 (由 irq_common_stub 调用)
void irq_handler(struct regs* r) {
    uint8_t irq = r->int_no - 32;

    // 通知 PIC 中断结束
    if (irq >= 8) {
        outb(0xA0, 0x20);   // 从片
    }
    outb(0x20, 0x20);       // 主片

    // 分发到已注册的处理函数
    if (irq_routines[irq] != 0) {
        void (*handler)(struct regs*) = irq_routines[irq];
        handler(r);
    }
}

void irq_install_handler(uint8_t irq, void (*handler)(struct regs* r)) {
    if (irq < 16) {
        irq_routines[irq] = (void*)handler;
    }
}

void irq_uninstall_handler(uint8_t irq) {
    if (irq < 16) {
        irq_routines[irq] = 0;
    }
}

// 8259 PIC 初始化
void pic_init(void) {
    outb(0x20, 0x11);   // ICW1: 初始化, 级联, 边沿触发
    outb(0xA0, 0x11);
    outb(0x21, 0x20);   // ICW2: 主片中断号 0x20-0x27
    outb(0xA1, 0x28);   // ICW2: 从片中断号 0x28-0x2F
    outb(0x21, 0x04);   // ICW3: 主片从片接 IRQ2
    outb(0xA1, 0x02);   // ICW3: 从片级联到主片 IRQ2
    outb(0x21, 0x01);   // ICW4: 8086 模式
    outb(0xA1, 0x01);
    outb(0x21, 0x0);    // 屏蔽全开 (允许所有 IRQ)
    outb(0xA1, 0x0);
}
