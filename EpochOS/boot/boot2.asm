; ============================================================
; EpochOS - boot2 二级引导 (16KB, LBA 1-32)
; 任务:
;   1. 枚举 VBE 模式列表, 设置 800x600x32 LFB 图形模式
;   2. 把 bootinfo 写入 0x5000 供内核读取
;   3. 读内核 (LBA 33, 762 扇区) 到 0x10000
;   4. 开启保护模式, 跳转内核
; ============================================================
[org 0x7E00]
[bits 16]

start:
    mov [boot_drive], dl

    ; ---- 先读内核 (LBA=33 -> 0x10000), 避免 VBE 切换影响磁盘访问 ----
    ; SeaBIOS 扩展读单次上限 127 扇区, 故分块: 127*6 = 762 扇区 (390KB)
    ; kernel 当前 514 扇区 (263KB), 每块均不超过 127 上限
    mov si, kernel_dap
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc disk_error
    mov si, kernel_dap2
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc disk_error
    mov si, kernel_dap3
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc disk_error
    mov si, kernel_dap4
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc disk_error
    mov si, kernel_dap5
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc disk_error
    mov si, kernel_dap6
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc disk_error

    ; ---- VBE 探测: 0x4F00 获取控制器信息, 枚举模式列表找 800x600x32 ----
    call serial_init
    mov ax, 0x4F00
    mov di, vbe_info
    int 0x10
    cmp ax, 0x004F
    jne no_vbe
    cmp dword [vbe_info], 0x41534556      ; "VESA"
    jne no_vbe
    ; 保存模式列表指针 (VBE Info: +0x0E=偏移, +0x10=段)
    mov ax, [vbe_info + 0x10]
    mov [list_seg], ax
    mov ax, [vbe_info + 0x0E]
    mov [list_off], ax
    ; 调试: 打印签名低16位与列表指针
    mov si, dbg_vbe
    call serial_str
    mov ax, [vbe_info]
    call serial_hex16
    mov si, dbg_space
    call serial_str
    mov ax, [list_seg]
    call serial_hex16
    mov si, dbg_colon
    call serial_str
    mov ax, [list_off]
    call serial_hex16
    mov si, dbg_nl
    call serial_str

.vbe_loop:
    ; 读模式号
    mov ax, [list_seg]
    mov es, ax
    mov di, [list_off]
    mov cx, [es:di]
    cmp cx, 0xFFFF
    je no_vbe
    add word [list_off], 2
    mov [cur_mode], cx
    ; 0x4F01 查询 mode_info (ES=0 -> 0x6200)
    mov ax, 0x4F01
    mov di, mode_info
    push cx
    push es
    xor bx, bx
    mov es, bx
    int 0x10
    pop es
    pop cx
    cmp ax, 0x004F
    jne .vbe_loop
    ; 匹配 800x600x32
    cmp word [mode_info + 0x12], 800
    jne .vbe_loop
    cmp word [mode_info + 0x14], 600
    jne .vbe_loop
    cmp byte [mode_info + 0x19], 32
    jne .vbe_loop
    mov bx, [mode_info]             ; 属性: bit0 模式支持, bit7 LFB
    test bx, 0x01
    jz .vbe_loop
    test bx, 0x80
    jz .vbe_loop
    ; 命中: 设置模式 cur_mode | LFB(bit14)
    mov cx, [cur_mode]
    mov ax, 0x4F02
    mov bx, cx
    or bx, 0x4000
    push es
    xor cx, cx
    mov es, cx
    int 0x10
    pop es
    cmp ax, 0x004F
    jne no_vbe
    ; 设置成功后再查询一次, 取真实参数
    mov cx, [cur_mode]
    mov ax, 0x4F01
    mov di, mode_info
    push es
    xor bx, bx
    mov es, bx
    int 0x10
    pop es
    cmp ax, 0x004F
    jne no_vbe
    jmp .fill_bootinfo

.fill_bootinfo:
    ; ---- 填充 bootinfo (全部取自 mode_info 真实值) ----
    movzx eax, word [mode_info + 0x12]
    mov dword [bootinfo + 0x00], eax        ; width
    movzx eax, word [mode_info + 0x14]
    mov dword [bootinfo + 0x04], eax        ; height
    movzx eax, byte [mode_info + 0x19]
    mov dword [bootinfo + 0x08], eax        ; bpp
    movzx eax, word [mode_info + 0x10]
    mov dword [bootinfo + 0x0C], eax        ; pitch (bytes per line)
    mov eax, [mode_info + 0x28]
    mov dword [bootinfo + 0x10], eax        ; LFB 物理地址
    mov dword [bootinfo + 0x14], 1          ; vbe_ok
    jmp kernel_load

no_vbe:
    ; 无 VBE: 文本模式 80x25 兜底
    mov si, dbg_novbe
    call serial_str
    mov dword [bootinfo + 0x00], 80
    mov dword [bootinfo + 0x04], 25
    mov dword [bootinfo + 0x08], 0
    mov dword [bootinfo + 0x0C], 160
    mov dword [bootinfo + 0x10], 0xB8000
    mov dword [bootinfo + 0x14], 0
    ; 继续加载内核 (文本模式)

kernel_load:
    ; ---- 开启 A20 (内核 .bss 在 1MB 处, 需要 1MB+ 地址线) ----
    call enable_a20
    ; ---- 进入保护模式 ----
    cli
    lgdt [gdt_ptr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pmode

[bits 32]
pmode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    ; ---- 清零内核 .bss ----
    ; .bss 起点 = __bss_start (linker.ld, 0x100000)。
    ; 注意: 清零范围必须 >= 实际 .bss 大小, 否则超出部分的静态变量会带着
    ;       RAM 里的随机值启动 (C 语义要求静态变量零初始化), 表现为随机性故障。
    ;       历史上这里只清 1MB, 而 .bss 随 photo_raw(1.92MB)/g_fds/files 等
    ;       增长到约 2.83MB, 导致 fd 表等整块未清零。
    ; 现按 BSS_ZERO_LIMIT = 4MB 清零 (0x100000-0x500000), 留足余量;
    ; build.py 会在链接后断言 .bss 确实落在该范围内, 超限直接构建失败。
    ; 该区间不含 .text/.rodata/.data (尾部 0x5D894 以下) 与 VGA 区, 安全。
    mov ecx, (0x400000 / 4)         ; 4MB / 4 = 1048576 dwords
    mov edi, 0x100000
    xor eax, eax
    rep stosd
    ; ---- 跳转内核 (bootinfo 在 0x5000) ----
    jmp 0x08:0x10000

[bits 16]
disk_error:
    ; 直接写 0xB8000 显示错误码 (绕过 int 0x10, 文本模式必可见)
    push es
    push ax
    push bx
    push di
    mov ax, 0xB800
    mov es, ax
    xor di, di
    mov si, err_msg
.print:
    lodsb
    or al, al
    jz .err_code
    mov [es:di], al
    inc di
    mov byte [es:di], 0x07
    inc di
    jmp .print
.err_code:
    ; 显示 " AH=XX"
    mov word [es:di], 0x0741   ; 'A'
    add di, 2
    mov word [es:di], 0x0748   ; 'H'
    add di, 2
    mov word [es:di], 0x073D   ; '='
    add di, 2
    mov al, ah
    shr al, 4
    call .hex_digit
    mov al, ah
    and al, 0x0F
    call .hex_digit
    pop di
    pop bx
    pop ax
    pop es
.hang:
    hlt
    jmp .hang
.hex_digit:
    cmp al, 10
    jb .num
    add al, 'A' - 10
    jmp .put
.num:
    add al, '0'
.put:
    mov [es:di], al
    inc di
    mov byte [es:di], 0x07
    inc di
    ret

; ---- 串口调试例程 (0x3F8, QEMU -serial file) ----

; ---- 开启 A20 (快速门 + 键盘控制器双保险) ----
enable_a20:
    push ax
    ; 快速 A20 门 (0x92), bit0 保留不清 (避免误触发复位)
    in al, 0x92
    or al, 0x02
    and al, 0xFE
    out 0x92, al
    ; 键盘控制器法 (保险, 对部分模拟器/主板必须)
    call .wait_kbc
    mov al, 0xAD
    out 0x64, al
    call .wait_kbc
    mov al, 0xD0
    out 0x64, al
    call .wait_kbc_in
    in al, 0x60
    or al, 0x02
    mov ah, al
    call .wait_kbc
    mov al, 0xD1
    out 0x64, al
    call .wait_kbc
    mov al, ah
    out 0x60, al
    call .wait_kbc
    mov al, 0xAE
    out 0x64, al
    call .wait_kbc
    pop ax
    ret
.wait_kbc:
    in al, 0x64
    test al, 0x02
    jnz .wait_kbc
    ret
.wait_kbc_in:
    in al, 0x64
    test al, 0x01
    jz .wait_kbc_in
    ret

serial_init:
    push ax
    push dx
    mov dx, 0x3FB
    mov al, 0x80
    out dx, al              ; DLAB=1 (LCR)
    mov dx, 0x3F8
    mov al, 0x01
    out dx, al              ; 波特率低字节 (38400)
    mov dx, 0x3F9
    mov al, 0x00
    out dx, al              ; 波特率高字节
    mov dx, 0x3FB
    mov al, 0x03
    out dx, al              ; 8N1, DLAB=0
    mov dx, 0x3F9
    mov al, 0x00
    out dx, al              ; 关中断
    pop dx
    pop ax
    ret

serial_str:
    push ax
    push bx
    push dx
.loop:
    lodsb
    or al, al
    jz .done
    mov bx, ax
.wait_tx:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20
    jz .wait_tx
    mov dx, 0x3F8
    mov al, bl
    out dx, al
    jmp .loop
.done:
    pop dx
    pop bx
    pop ax
    ret

serial_hex16:
    push ax
    push bx
    push cx
    push dx
    mov cx, 4
.next:
    rol ax, 4
    mov bx, ax
    and bx, 0x0F
    cmp bl, 10
    jb .num
    add bl, 'A' - 10
    jmp .put
.num:
    add bl, '0'
.put:
    push ax             ; 保护 rol 后的 ax, 避免 mov al,bl 污染
    mov dx, 0x3FD
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    mov dx, 0x3F8
    mov al, bl
    out dx, al
    pop ax              ; 恢复 ax
    loop .next
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ---- 数据 ----
kernel_dap:
    db 0x10
    db 0x00
    dw 127          ; 第1块: SeaBIOS 扩展读单次上限 (kernel 需 514 扇区)
    dw 0x0000        ; 偏移
    dw 0x1000        ; 段 (0x10000)
    dd 33            ; LBA 低 (boot0=0, boot2=1..32, kernel=33..544)
    dd 0
kernel_dap2:
    db 0x10
    db 0x00
    dw 127           ; 第2块: 127 扇区
    dw 0x0000        ; 偏移
    dw 0x1FE0        ; 段 (0x1FE00 = 0x10000 + 127*512)
    dd 160           ; LBA 低 (33 + 127)
    dd 0
kernel_dap3:
    db 0x10
    db 0x00
    dw 127           ; 第3块: 127 扇区
    dw 0x0000        ; 偏移
    dw 0x2FC0        ; 段 (0x2FC00 = 0x1FE00 + 127*512)
    dd 287           ; LBA 低 (160 + 127)
    dd 0
kernel_dap4:
    db 0x10
    db 0x00
    dw 127           ; 第4块: 127 扇区
    dw 0x0000        ; 偏移
    dw 0x3FA0        ; 段 (0x3FA00 = 0x2FC00 + 127*512)
    dd 414           ; LBA 低 (287 + 127)
    dd 0
kernel_dap5:
    db 0x10
    db 0x00
    dw 127           ; 第5块: 127 扇区
    dw 0x0000        ; 偏移
    dw 0x4F80        ; 段 (0x4F800 = 0x3FA00 + 127*512)
    dd 541           ; LBA 低 (414 + 127)
    dd 0
kernel_dap6:
    db 0x10
    db 0x00
    dw 127           ; 第6块: 127 扇区
    dw 0x0000        ; 偏移
    dw 0x5F60        ; 段 (0x5F600 = 0x4F800 + 127*512)
    dd 668           ; LBA 低 (541 + 127)
    dd 0

boot_drive: db 0
cur_mode: dw 0
list_seg: dw 0
list_off: dw 0
err_msg: db "EpochOS boot2: kernel load FAILED", 0
dbg_vbe: db "[b2] vbe sig=", 0
dbg_colon: db ":", 0
dbg_space: db " ", 0
dbg_nl: db 13, 10, 0
dbg_novbe: db "[b2] NO VBE", 13, 10, 0

; ---- GDT ----
align 4
gdt:
    dq 0x0000000000000000                    ; null
    dw 0xFFFF, 0x0000, 0x9A00, 0x00CF        ; code 0x08
    dw 0xFFFF, 0x0000, 0x9200, 0x00CF        ; data 0x10
gdt_ptr:
    dw 23
    dd gdt

; ---- bootinfo 输出区 (内核读取) ----
; 实际写入地址 0x5000, 此处仅作汇编引用
bootinfo equ 0x5000
vbe_info equ 0x6000
mode_info equ 0x6200

times 16384-($-$$) db 0
