/* ============================================================
 * EpochOS - 中断 Stub (GNU as)
 * 每个异常/IRQ 一个入口, 统一压入中断号后跳到公共处理
 * ============================================================ */

.section .text

/* 异常 0-31: 部分 CPU 自动压入错误码 */
.macro ISR_NOERR n
.global isr\n
isr\n:
    cli
    pushl $0
    pushl $\n
    jmp isr_common_stub
.endm

.macro ISR_ERR n
.global isr\n
isr\n:
    cli
    pushl $\n
    jmp isr_common_stub
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_NOERR 17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

/* 0x80: syscall gate (int 0x80 from ring3) */
ISR_NOERR 128

/* 异常公共处理: 保存全部寄存器后调用 isr_handler */
isr_common_stub:
    pusha
    pushl %ds
    pushl %es
    pushl %fs
    pushl %gs
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    pushl %esp
    call isr_handler
    addl $4, %esp
    popl %gs
    popl %fs
    popl %es
    popl %ds
    popa
    addl $8, %esp
    iret

/* IRQ 0-15 -> 中断号 32-47 */
.macro IRQ_STUB n
.global irq\n
irq\n:
    cli
    pushl $0
    pushl $(32 + \n)
    jmp irq_common_stub
.endm

IRQ_STUB 0
IRQ_STUB 1
IRQ_STUB 2
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12
IRQ_STUB 13
IRQ_STUB 14
IRQ_STUB 15

/* IRQ 公共处理 */
irq_common_stub:
    pusha
    pushl %ds
    pushl %es
    pushl %fs
    pushl %gs
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    pushl %esp
    call irq_handler
    addl $4, %esp
    /* 任务调度切换点: 进入 scheduler_switch 保存/加载 esp,
       然后跳回 irq_common_stub_restore 恢复新任务的寄存器 */
    jmp scheduler_switch
.global irq_common_stub_restore
irq_common_stub_restore:
    popl %gs
    popl %fs
    popl %es
    popl %ds
    popa
    addl $8, %esp
    iret

/* ��ȡ CR2 �Ĵ��� (ҳ�����ַ) */
.global read_cr2
read_cr2:
    movl %cr2, %eax
    ret
