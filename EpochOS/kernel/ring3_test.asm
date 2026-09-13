; ============================================================
; EpochOS - ring3 用户态测试程序 (阶段2: 进程模型)
; 功能: eax=1 (syscall exit), ebx=42 (退出码), int 0x80 陷入内核
; 验证: ring3 进入 -> 特权级切换 -> syscall 分发 -> 退出回收
; 编译: nasm -f bin ring3_test.asm -o ring3_test.bin
;       由 build.py 编译并生成 ring3_test_bin.h 嵌入内核
; 布局: 代码映射到虚拟 0x08048000 (Linux ELF 默认入口风格),
;       尾部 jmp $ 供 exit 后停驻 (下一次 PIT 中断被调度器跳过)
; ============================================================
bits 32
org 0x08048000

global _start
_start:
    mov eax, 1          ; syscall nr = 1 (exit)
    mov ebx, 42         ; exit status = 42
    int 0x80            ; ring3 -> ring0 syscall gate
    jmp $               ; halt loop (退出后停驻, 等待调度切走)
