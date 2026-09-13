# -*- coding: utf-8 -*-
"""EpochOS 一键启动器
功能: 1) 若镜像不存在则自动构建  2) 复制 QEMU share/镜像到 ASCII 临时目录
      3) 启动 QEMU 图形窗口运行 EpochOS
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = r"E:\Assets\操作系统"
OS_DIR = os.path.join(ROOT, "EpochOS")
TOOLS = os.path.join(ROOT, "工具")
TOOLS_BIN = os.path.join(TOOLS, "bin")
TOOLS_SHARE = os.path.join(TOOLS, "share")
IMG = os.path.join(OS_DIR, "build", "epochos.img")
BUILD_PY = os.path.join(OS_DIR, "build.py")
QEMU = os.path.join(TOOLS_BIN, "qemu-system-i386.exe")

def need_tool(name, path):
    if not os.path.isfile(path):
        print(f"[错误] 缺少 {name}: {path}")
        return False
    return True

def main():
    if not need_tool("QEMU", QEMU):
        sys.exit(1)
    if not os.path.isdir(TOOLS_SHARE):
        print(f"[错误] 缺少 QEMU share 目录: {TOOLS_SHARE}")
        sys.exit(1)

    # 镜像不存在则自动构建
    if not os.path.isfile(IMG):
        print("[提示] 镜像不存在, 正在自动构建...")
        if not os.path.isfile(BUILD_PY):
            print(f"[错误] 缺少构建脚本: {BUILD_PY}")
            sys.exit(1)
        py = sys.executable
        r = subprocess.run([py, BUILD_PY], cwd=OS_DIR)
        if r.returncode != 0 or not os.path.isfile(IMG):
            print("[错误] 构建失败, 请查看上方输出")
            sys.exit(1)

    # 复制到 ASCII 临时目录 (QEMU 无法加载中文路径下的 BIOS share)
    work = os.path.join(tempfile.gettempdir(), "epochos_launch")
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    share_dst = os.path.join(work, "share")
    shutil.copytree(TOOLS_SHARE, share_dst)
    img_dst = os.path.join(work, "epochos.img")
    shutil.copy2(IMG, img_dst)

    print("[启动] QEMU 窗口即将打开, 关闭窗口即退出...")
    # q35 自带 ICH9 SATA(AHCI), 内核仅支持 IDE PIO, 需附加 piix3-ide 控制器挂载磁盘
    subprocess.run([QEMU, "-L", share_dst, "-machine", "q35",
                    "-drive", f"file={img_dst},format=raw,if=none,id=disk",
                    "-device", "piix3-ide,id=ide",
                    "-device", "ide-hd,drive=disk,bus=ide.0",
                    "-vga", "std",
                    "-display", "sdl",
                    "-no-reboot", "-no-shutdown"])
    print("[退出] EpochOS 已关闭")

if __name__ == "__main__":
    main()
