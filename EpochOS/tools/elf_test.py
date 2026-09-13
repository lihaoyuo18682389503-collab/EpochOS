# -*- coding: utf-8 -*-
"""EpochOS 用户程序 (ELF/ring3) 自动化测试

通过串口 TCP 与 QEMU 中的内核交互, 借助内核自带的测试钩子
(open:/key:/click:) 打开终端并注入命令, 从而验证用户态 ELF 能否
正确加载、运行、系统调用、退出。

用法: python tools/elf_test.py
"""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(os.path.dirname(ROOT), "工具")
QEMU = os.path.join(TOOLS, "bin", "qemu-system-i386.exe")
SHARE_SRC = os.path.join(TOOLS, "share")
IMG = os.path.join(ROOT, "build", "epochos.img")
PORT = 4455


class QemuSerial:
    """通过 TCP 与 QEMU 的客户机串口双向通信

    QEMU 命令行用 "-serial tcp:127.0.0.1:PORT,server,nowait",
    即 QEMU 侧是 TCP 服务端, 本类作为客户端连上去。
    """

    def __init__(self, host, port, timeout=0.2, connect_timeout=25):
        self.buf = b""
        self.sock = None
        self.host = host
        self.port = port
        self.timeout = timeout
        self.connect_timeout = connect_timeout

    def accept(self):
        """重试连接 QEMU 的串口服务端"""
        deadline = time.time() + self.connect_timeout
        last = None
        while time.time() < deadline:
            try:
                s = socket.create_connection((self.host, self.port), timeout=2)
                s.settimeout(self.timeout)
                self.sock = s
                return s
            except OSError as e:
                last = e
                time.sleep(0.5)
        raise RuntimeError(f"无法连接 QEMU 串口 {self.host}:{self.port}: {last}")

    def send(self, text):
        self.sock.sendall(text.encode("utf-8", errors="replace"))

    def pump(self, wait=0.0):
        """读取当前可用的所有数据, 返回新增文本"""
        if wait:
            time.sleep(wait)
        got = b""
        while True:
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                break
            except OSError:
                break
            if not chunk:
                break
            got += chunk
        self.buf += got
        return got.decode("utf-8", errors="replace")

    def wait_for(self, needle, timeout=15.0):
        deadline = time.time() + timeout
        text = self.buf.decode("utf-8", errors="replace")
        while needle not in text:
            if time.time() > deadline:
                return False, text
            self.pump(0.25)
            text = self.buf.decode("utf-8", errors="replace")
        return True, text

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


def main():
    if not os.path.isfile(IMG):
        print("[错误] 找不到镜像, 请先运行 build.py:", IMG)
        return 1

    work = os.path.join(tempfile.gettempdir(), "epochos_elftest")
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    share = os.path.join(work, "share")
    shutil.copytree(SHARE_SRC, share)
    img_dst = os.path.join(work, "epochos.img")
    shutil.copy2(IMG, img_dst)

    ser = QemuSerial("127.0.0.1", PORT)
    proc = subprocess.Popen(
        [QEMU, "-L", share, "-machine", "q35",
         "-drive", f"file={img_dst},format=raw,if=none,id=disk",
         "-device", "piix3-ide,id=ide",
         "-device", "ide-hd,drive=disk,bus=ide.0",
         "-vga", "std", "-display", "none",
         "-serial", f"tcp:127.0.0.1:{PORT},server,nowait",
         "-no-reboot", "-no-shutdown"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    results = []
    try:
        ser.accept()
        # [test] ready 是内核启动早期的一次性输出; 串口 TCP 服务端用 nowait,
        # 客户端常在它之后才连上 -> 该消息被丢弃。改用反复可用的
        # "echo" -> "[test] pong" 作为就绪握手: 既验证 GUI 主循环在跑,
        # 也验证内核确实收到了我们发过去的串口输入。
        ser.send("echo\n")
        ok, text = ser.wait_for("[test] pong", timeout=25)
        if not ok:
            print("[失败] 内核未就绪/未响应 echo, 串口输出:")
            print(text[-3000:])
            return 1
        print("[ok] 内核就绪, GUI 主循环运行中 (echo->pong 握手成功)")

        # 打开终端窗口 (WIN_TERMINAL = 0)
        ser.send("open:0\n")
        ser.pump(0.8)

        # 依次运行几个用户态程序
        # 注意: shell_process_char 以 '\r' 作为回车 (见 kernel/shell.c),
        # 串口 key: 协议会剥掉行尾的 '\n', 因此命令必须以 '\r' 结尾才能触发执行。
        # pass_marker: 该程序自我判定 PASS 时打印的标记 (用于 elf-test 之类自带断言的程序)
        cases = [
            ("argecho", "elf /bin/argecho hello 42\r\n", None),
            ("hello_c", "elf /bin/hello_c\r\n", None),
            ("fib", "elf /bin/fib\r\n", None),
            ("syscheck", "elf /bin/syscheck\r\n", "syscheck: PASS all"),
            ("badptr", "elf /bin/badptr\r\n", "badptr: PASS all"),  # EFAULT 防护验证
            ("linuxabi", "elf /bin/linuxabi\r\n", "linuxabi: PASS all"),  # 阶段6 Linux 兼容 ABI
        ]
        for name, cmd, pass_marker in cases:
            before = len(ser.buf)
            ser.send("key:" + cmd)
            ser.pump(3.0)
            out = ser.buf[before:].decode("utf-8", errors="replace")
            # 用户程序输出经由内核 vga/serial, 进程创建会打 [proc]/[elf] 日志
            created = "[proc] create" in out or "[elf] loaded" in out
            exited = "exit(" in out
            passed = (pass_marker is None) or (pass_marker in out and "FAIL" not in out)
            ok_case = created and exited and passed
            print(f"\n--- {name}: 命令 {cmd.strip()!r}")
            print(f"    进程创建={'是' if created else '否'}  退出={'是' if exited else '否'}"
                  + (f"  断言={'通过' if passed else '未通过'}" if pass_marker else ""))
            if out.strip():
                for line in out.splitlines():
                    if any(k in line for k in
                           ("[proc]", "[elf]", "[syscall]", "exit(", "hello", "fib",
                            "badptr", "syscheck")):
                        print("    |", line)
            results.append((name, ok_case))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
        ser.close()

    print("\n" + "=" * 56)
    ok_all = all(c for _, c in results)
    for name, ok_case in results:
        print(f"  {name:12s} {'通过' if ok_case else '未通过'}")
    print("=" * 56)
    print("结果:", "用户程序全部加载/断言成功" if ok_all else "存在未通过的程序")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
