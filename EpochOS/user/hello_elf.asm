; ============================================================
; EpochOS - ELF loader 测试程序 1 (纯汇编)
; Linux x86 syscall 子集: write(1,...) + exit(42) via int 0x80
; 编译: nasm -f elf32 -> i686-elf-ld -m elf_i386 -Ttext 0x08048000 -e _start
; 静态 ET_EXEC, 无动态链接, 由 EpochOS ELF loader 映射运行
; ============================================================
BITS 32

section .text
global _start

_start:
    ; SYS_write(4): fd=1 stdout, buf=msg, len=msg_len
    mov eax, 4
    mov ebx, 1
    mov ecx, msg
    mov edx, msg_len
    int 0x80

    ; SYS_exit(1): exit(42)
    mov eax, 1
    mov ebx, 42
    int 0x80

.self_loop:                 ; 保护: syscall exit 不会修改 eip,
    jmp .self_loop          ; iret 回到此处自旋, PIT 后调度器跳过

section .rodata
msg:     db "hello_elf: write syscall OK from EpochOS ring3!", 10
msg_len: equ $ - msg
