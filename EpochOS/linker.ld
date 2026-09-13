/* MarvisOS 内核链接脚本 */
ENTRY(kernel_main)

SECTIONS
{
    . = 0x10000;              /* 内核加载地址: 物理 0x10000 */

    .text : {
        *(.text.entry)     /* 内核入口最先 */
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    /* .bss 移到 1MB 处: 避开 VGA 内存区 (0xA0000-0xBFFFF) 与 mm 管理区 (0x300000+)
       0xA0000-0xBFFFF 在 VBE 图形模式下被 VGA 硬件接管, 写入会被吞噬/覆盖
       注意: 不可再用 0x200000 —— .text/.rodata/.data 已连续增长到 ~0x47DF0,
       固定 0x200000 会使 .bss 与 .rodata/.data 尾部重叠, 运行时清零覆盖静态数据 */
    . = 0x100000;
    .bss : {
        __bss_start = .;
        *(COMMON)
        *(.bss*)
    }
    __bss_end = .;
}
