; ============================================================
; MarvisOS - Bootloader
; 引导扇区: 实模式 -> 保护模式 -> 加载内核到 1MB 处 -> 跳转
; NASM 编译: nasm -f bin boot.asm -o boot.bin
; ============================================================

BITS 16

; 关键: org 声明代码段起始地址, 使所有 label 按实际加载地址 0x7C00 计算
ORG 0x7C00

; 内核加载地址（物理 1MB 处，即 0x100000）
KERNEL_OFFSET equ 0x1000
KERNEL_SEGMENT equ 0x1000    ; 段 0x1000 << 4 = 0x10000

; ============ 启动入口 ============
start:
    cli
    ; 初始化段寄存器
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    ; 保存引导驱动器号
    mov [BOOT_DRIVE], dl

    ; 显示启动信息
    mov si, MSG_BOOT
    call print_string

    ; 加载内核到内存
    call load_kernel

    ; 进入保护模式
    call switch_to_pm

    ; 不会返回到这里
    jmp $

; ============ 字符串打印（实模式 BIOS） ============
print_string:
    pusha
    mov ah, 0x0E
.loop:
    lodsb
    or al, al
    jz .done
    int 0x10
    jmp .loop
.done:
    popa
    ret

; ============ 磁盘读取 ============
; 使用 BIOS int 13h ah=02h 逐扇区 CHS 读取 (兼容性最好, 支持跨磁头/柱面)
; 内核从第 2 扇区开始, 读 KERNEL_SECTORS 个扇区到 KERNEL_SEGMENT:0
load_kernel:
    pusha
    mov bx, KERNEL_SEGMENT
    mov es, bx
    xor bx, bx
    mov di, KERNEL_SECTORS   ; 剩余扇区数
    mov ch, 0                ; 柱面 0
    mov dh, 0                ; 磁头 0
    mov cl, 2                ; 起始扇区 2 (跳过引导扇区)
.read_loop:
    mov ah, 0x02
    mov al, 1                ; 每次读 1 扇区
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc disk_error
    add bx, 512              ; 缓冲区前进 1 扇区
    jnc .bx_ok
    mov ax, es
    add ax, 0x1000           ; BX 回绕 (64KB), 段寄存器进位 64KB/16
    mov es, ax
.bx_ok:
    ; 推进 CHS: 扇区 -> 磁头 -> 柱面
    inc cl
    cmp cl, 19               ; 每磁道 18 扇区
    jb .next_sector
    mov cl, 1
    inc dh
    cmp dh, 2                ; 每柱面 2 磁头
    jb .next_sector
    mov dh, 0
    inc ch
.next_sector:
    dec di
    jnz .read_loop
    popa
    ret

disk_error:
    mov si, MSG_DISK_ERROR
    call print_string
    jmp $

; ============ 切换到保护模式 ============
switch_to_pm:
    cli
    ; 开启 A20 地址线（通过键盘控制器）
    call enable_a20

    ; 加载 GDT
    lgdt [gdt_descriptor]

    ; 设置 CR0 保护模式位
    mov eax, cr0
    or eax, 0x1
    mov cr0, eax

    ; 远跳转刷新流水线
    jmp CODE_SEG:init_pm

enable_a20:
    ; 等待键盘控制器空闲
    call wait_kbc
    mov al, 0xAD        ; 禁用键盘
    out 0x64, al

    call wait_kbc
    mov al, 0xD0        ; 读取输出端口
    out 0x64, al

    call wait_kbc_input
    in al, 0x60
    or al, 0x02         ; 设置 A20 位
    mov ah, al

    call wait_kbc
    mov al, 0xD1        ; 写输出端口
    out 0x64, al

    call wait_kbc
    mov al, ah
    out 0x60, al

    call wait_kbc
    mov al, 0xAE        ; 重新启用键盘
    out 0x64, al

    call wait_kbc
    ret

wait_kbc:
    in al, 0x64
    test al, 0x02       ; 输入缓冲满?
    jnz wait_kbc
    ret

wait_kbc_input:
    in al, 0x64
    test al, 0x01       ; 输出缓冲有数据?
    jz wait_kbc_input
    ret

; ============ 保护模式入口 ============
BITS 32
init_pm:
    ; 设置数据段寄存器
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000    ; 栈顶

    ; 跳转到内核（物理地址 0x100000）
    ; KERNEL_SEGMENT:0 -> 0x10000 需要修正，这里直接用绝对地址跳转
    ; 内核被加载到 0x1000:0x0000 = 0x10000，不是 1MB。
    ; 使用间接跳转到 0x10000
    mov eax, 0x10000
    jmp eax

; ============ 数据 ============
BOOT_DRIVE db 0
MSG_BOOT db 'MarvisOS Bootloader v0.1', 0x0D, 0x0A, 0
MSG_DISK_ERROR db 'Disk read error!', 0x0D, 0x0A, 0

; GDT 表
KERNEL_SECTORS equ 256       ; 最多读取 256 扇区 (128KB) 内核

gdt_start:
    ; 空描述符
    dq 0x0
; 代码段描述符: 基址 0, 限长 4GB, 32 位
gdt_code:
    dw 0xFFFF       ; limit 15:0
    dw 0x0000       ; base 15:0
    db 0x00         ; base 23:16
    db 10011010b    ; access: present, ring0, code, exec/read
    db 11001111b    ; granularity: 4KB, 32bit, limit 19:16 = F
    db 0x00         ; base 31:24
; 数据段描述符
gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b    ; access: present, ring0, data, read/write
    db 11001111b
    db 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

; 填充到 510 字节
times 510-($-$$) db 0
dw 0xAA55
