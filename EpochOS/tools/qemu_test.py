# -*- coding: utf-8 -*-
"""启动 EpochOS 并截图验证 (QEMU 不支持中文路径, 全部走 ASCII 临时目录)"""
import os
import shutil
import socket
import subprocess
import time
import sys

QEMU = r"E:\Assets\操作系统\工具\bin\qemu-system-i386.exe"
QEMU_SHARE_SRC = r"E:\Assets\操作系统\工具\share"

WORK = os.path.join(os.environ["TEMP"], "epochos_build")
IMG = os.path.join(WORK, "epochos.img")
SHOT = os.path.join(os.environ["TEMP"], "epochos_screen.ppm")
SHARE_DST = os.path.join(WORK, "qemu_share")

PORT = 4444

def main():
    # 复制 QEMU 数据文件到 ASCII 路径 (BIOS 等)
    if os.path.exists(SHARE_DST):
        shutil.rmtree(SHARE_DST)
    shutil.copytree(QEMU_SHARE_SRC, SHARE_DST)
    print("SHARE_READY:", os.path.exists(os.path.join(SHARE_DST, "bios-256k.bin")))

    # 启动 QEMU, monitor 监听 tcp
    proc = subprocess.Popen(
        [QEMU, "-L", SHARE_DST, "-fda", IMG, "-boot", "a", "-display", "none",
         "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
         "-no-reboot", "-no-shutdown"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("QEMU PID:", proc.pid)
    time.sleep(5)

    # 连接 monitor
    s = None
    for attempt in range(5):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=3)
            break
        except Exception as e:
            print(f"connect attempt {attempt}: {e}")
            time.sleep(1)
    if s is None:
        print("FAILED_TO_CONNECT")
        proc.kill()
        sys.exit(1)

    def send(cmd):
        s.sendall((cmd + "\n").encode())
        time.sleep(0.5)

    send("screendump " + SHOT)
    send("quit")
    s.close()
    proc.wait(timeout=5)
    print("SCREEN_EXISTS:", os.path.exists(SHOT))
    if os.path.exists(SHOT):
        print("SIZE:", os.path.getsize(SHOT))

if __name__ == "__main__":
    main()
