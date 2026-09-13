# ============================================================
# EpochOS epoch-libc crt0 (用户态程序入口, 阶段4)
# 栈顶布局 (EpochOS ELF loader 建立, Linux i386 风格):
#   esp -> [argc][argv[0] ptr][argv[1] ptr]...[argv[argc-1] ptr][NULL]
#          [envp NULL][auxv AT_NULL]...
# 本文件定义 _start (ELF entry), 收集栈指针后调 C 入口 epoch_c_entry,
# 由 epoch_c_entry 解析 argc/argv 并调用 main, main 返回后调 exit。
# ============================================================
    .section .text
    .globl _start
_start:
    xorl %ebp, %ebp            # 帧指针清零 (Linux crt 惯例)
    movl %esp, %eax            # eax = &argc (栈顶)
    pushl %eax                 # cdecl 传参: epoch_c_entry(&argc)
    call epoch_c_entry
    # epoch_c_entry 内已调用 exit, 正常不返回; 保护性自旋
_hang:
    jmp _hang

    .section .note.GNU-stack,"",@progbits
