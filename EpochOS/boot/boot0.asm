; ============================================================
; EpochOS - boot0 引导扇区 (512B)
; 任务: 从硬盘读取 boot2 (LBA 1, 32 扇区) 到 0x7E00 并跳转
; 使用 int 13h AH=42 扩展读 (QEMU 支持)
; ============================================================
[org 0x7C00]
[bits 16]

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [boot_drive], dl

    ; ---- 读 boot2: LBA=1, 32 扇区 -> 0x0000:0x7E00 ----
    mov si, dap
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc disk_error

    ; ---- 跳转 boot2 ----
    jmp 0x0000:0x7E00

disk_error:
    mov si, err_msg
.print:
    lodsb
    or al, al
    jz .hang
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp .print
.hang:
    hlt
    jmp .hang

dap:
    db 0x10          ; DAP 大小
    db 0x00          ; 保留
    dw 32            ; 扇区数
    dw 0x7E00        ; 偏移
    dw 0x0000        ; 段
    dd 1             ; LBA 低 32 位
    dd 0             ; LBA 高 32 位

boot_drive: db 0
err_msg: db "EpochOS boot0: boot2 load FAILED", 0

times 510-($-$$) db 0
dw 0xAA55
