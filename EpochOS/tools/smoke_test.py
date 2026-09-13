# -*- coding: utf-8 -*-
"""EpochOS 无头冒烟测试
启动 QEMU (无显示), 串口 COM1 输出到文件, 等待若干秒后退出,
返回启动日志。用于验证内核能否正常引导到 GUI。

用法: python tools/smoke_test.py [等待秒数]
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(os.path.dirname(ROOT), "工具")
QEMU = os.path.join(TOOLS, "bin", "qemu-system-i386.exe")
SHARE_SRC = os.path.join(TOOLS, "share")
IMG = os.path.join(ROOT, "build", "epochos.img")


def main():
    wait = int(sys.argv[1]) if len(sys.argv) > 1 else 8

    if not os.path.isfile(QEMU):
        print("[错误] 找不到 QEMU:", QEMU)
        return 1
    if not os.path.isfile(IMG):
        print("[错误] 找不到镜像, 请先运行 build.py:", IMG)
        return 1

    # QEMU 无法加载中文路径下的 BIOS/share, 复制到 ASCII 临时目录
    work = os.path.join(tempfile.gettempdir(), "epochos_smoke")
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    share = os.path.join(work, "share")
    shutil.copytree(SHARE_SRC, share)
    img_dst = os.path.join(work, "epochos.img")
    shutil.copy2(IMG, img_dst)
    log = os.path.join(work, "serial.log")

    proc = subprocess.Popen(
        [QEMU, "-L", share, "-machine", "q35",
         "-drive", f"file={img_dst},format=raw,if=none,id=disk",
         "-device", "piix3-ide,id=ide",
         "-device", "ide-hd,drive=disk,bus=ide.0",
         "-vga", "std", "-display", "none",
         "-serial", f"file:{log}",
         "-no-reboot", "-no-shutdown"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        time.sleep(wait)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)

    if not os.path.isfile(log):
        print("[失败] 没有产生串口日志")
        return 1

    with open(log, "r", errors="replace") as f:
        text = f.read()

    print("=" * 60)
    print(f"串口日志 ({len(text)} 字节, 等待 {wait}s)")
    print("=" * 60)
    print(text)

    # 判定: 出现 GUI 就绪标志即认为引导成功
    ok = "[test] ready" in text
    if "PANIC" in text or "KERNEL PANIC" in text:
        ok = False
    print("=" * 60)
    print("结果:", "引导成功 (进入 GUI)" if ok else "引导失败/未完成")
    print("=" * 60)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
