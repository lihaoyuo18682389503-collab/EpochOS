/* ============================================================
 * EpochOS - ELF32 用户测试程序链接脚本
 * 紧凑布局, 避免默认脚本 4KB 对齐空洞 (ramfs 单文件 <= 4096B)
 * 入口 0x08048000 (与 EpochOS ELF loader 支持的用户地址一致)
 * ============================================================ */
OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

SECTIONS
{
    . = 0x08048000;

    .text : {
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    .bss : {
        *(.bss)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
