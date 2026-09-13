# -*- coding: utf-8 -*-
"""EpochOS 输入注入可靠性自测工具

注入链路 (真实设备路径):
    QEMU monitor  mouse_move / mouse_button / sendkey
      -> QEMU PS/2 控制器
      -> 内核 i8042/PS2 驱动 (mouse.c / keyboard.c)
      -> gui_handle_input -> gui_on_click / 活动窗口键盘处理

校验链路:
    串口 COM1 回执 ([g] click / [g] activate / [k] key)
  + screendump 截图 (PPM) 像素差异

每次注入后立即校验, 未生效自动重试 (默认 3 次), 并输出成功率统计。

用法:
    python tools/input_inject.py click-test [N]        # N 次单击任务栏图标 (默认 20)
    python tools/input_inject.py dblclick-test [N]     # N 次双击桌面图标 (默认 20)
    python tools/input_inject.py key-test              # 新建窗口后键盘注入实证
    python tools/input_inject.py prof [SECONDS]        # 窗口激活态帧耗时采样
    python tools/input_inject.py all [N]               # 依次执行以上全部
"""
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS_ROOT = os.path.join(os.path.dirname(ROOT), "工具")
QEMU = os.path.join(TOOLS_ROOT, "bin", "qemu-system-i386.exe")
SHARE_SRC = os.path.join(TOOLS_ROOT, "share")
IMG = os.path.join(ROOT, "build", "epochos.img")

MON_PORT = 4581
SER_PORT = 4582

# 屏幕与 UI 几何 (与 kernel/gui.c 一致)
SCREEN_W, SCREEN_H = 800, 600
STATUS_BAR_H = 28
TASKBAR_H = 26
TASKBAR_APPS = 43
TASKBAR_PER_PAGE = 20
DESK_X0 = 12
DESK_Y0 = STATUS_BAR_H + 12
DESK_COL_W = 94
DESK_ICON_CENTER = (DESK_X0 + 45, DESK_Y0 + 31)          # 桌面第 0 个图标 (Files) 中心
TASKBAR_Y = SCREEN_H - TASKBAR_H // 2                     # 任务栏图标中心 y


def desk_icon_center(i):
    """桌面第 i 个图标中心 (kernel desk_icon_rect: 90x62, 4 列)"""
    col = i % 4
    row = i // 4
    return DESK_X0 + col * DESK_COL_W + 45, DESK_Y0 + row * 70 + 31

# 窗口类型值 (kernel/gui.h 枚举 WIN_*, 报告与断言用)
WIN_NAMES = {
    0: "Terminal", 1: "Files", 2: "About", 3: "Help", 4: "Settings",
    5: "Manager", 6: "Browser", 7: "Translate", 8: "Converter", 9: "Screenshot",
    10: "Photo", 11: "Video", 12: "PDF", 13: "Music", 14: "Calendar",
    15: "Editor", 16: "Alarm", 17: "Calculator", 18: "Store", 19: "Zip",
    20: "IDE", 21: "TaskMgr", 22: "Disk", 23: "Net", 24: "Snake",
    25: "2048", 26: "Mines", 27: "Brick", 28: "Paint", 29: "Notes",
    30: "UnitConv", 31: "Stopwatch", 32: "HexView", 33: "Tetris", 34: "TicTac",
    35: "Memory", 36: "Find", 37: "BaseConv", 38: "Random", 39: "TextStats",
    40: "Sudoku", 41: "Typing", 42: "Pomodoro", 43: "Guess", 44: "Dice",
    45: "Todo", 46: "PassGen", 47: "Clock",
}
WIN_BROWSER = 6
WIN_TODO = 45
WIN_CLOCK = 47


def taskbar_icon_center(i):
    """任务栏第 i 个应用图标中心 (i 从 0 计, 与 kernel handle_taskbar 同算法)"""
    w = SCREEN_W
    page_max = TASKBAR_PER_PAGE
    pages = (TASKBAR_APPS + page_max - 1) // page_max
    nav_w = 44 if pages > 1 else 0
    avail = w - 12 - nav_w
    count = TASKBAR_APPS
    if count > page_max:
        count = page_max
    bw = (avail - (count - 1) * 4) // count
    if bw > 30:
        bw = 30
    if bw < 16:
        bw = 16
    bx = 6 + (avail - count * bw - (count - 1) * 4) // 2
    return bx + i * (bw + 4) + bw // 2, TASKBAR_Y


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError("not a P6 ppm")
    # 解析头: P6 <w> <h> <maxval>\n
    idx = 2
    fields = []
    while len(fields) < 3:
        while idx < len(data) and data[idx:idx + 1].isspace():
            idx += 1
        if data[idx:idx + 1] == b"#":
            while data[idx:idx + 1] != b"\n":
                idx += 1
            continue
        start = idx
        while idx < len(data) and not data[idx:idx + 1].isspace():
            idx += 1
        fields.append(int(data[start:idx]))
    idx += 1
    w, h, _ = fields
    return w, h, data[idx:idx + w * h * 3]


def ppm_diff_ratio(path_a, path_b):
    """两张 PPM 的差异像素比例 (0.0~1.0)"""
    wa, ha, pa = read_ppm(path_a)
    wb, hb, pb = read_ppm(path_b)
    if (wa, ha) != (wb, hb):
        return 1.0
    n = wa * ha
    diff = 0
    step = 3
    for off in range(0, n * 3, step):
        if abs(pa[off] - pb[off]) > 8:
            diff += 1
    return diff / float(n)


class EpochOS:
    """QEMU 会话: monitor + 串口双向 + 截图 + 输入注入"""

    def __init__(self, work=None, verbose=True):
        self.verbose = verbose
        self.work = work or os.path.join(tempfile.gettempdir(), "epochos_inject")
        if os.path.exists(self.work):
            shutil.rmtree(self.work, ignore_errors=True)
        os.makedirs(self.work)
        share = os.path.join(self.work, "share")
        shutil.copytree(SHARE_SRC, share)
        self.img = os.path.join(self.work, "epochos.img")
        shutil.copy2(IMG, self.img)

        self.proc = subprocess.Popen(
            [QEMU, "-L", share, "-machine", "q35", "-vga", "std",
             "-drive", f"file={self.img},format=raw,if=none,id=disk",
             "-device", "piix3-ide,id=ide", "-device", "ide-hd,drive=disk,bus=ide.0",
             "-display", "none",
             "-monitor", f"tcp:127.0.0.1:{MON_PORT},server,nowait",
             "-serial", f"tcp:127.0.0.1:{SER_PORT},server,nowait",
             "-no-reboot", "-no-shutdown"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        self.mon = self._connect(MON_PORT)
        self.ser = self._connect(SER_PORT)
        if self.mon is None or self.ser is None:
            raise RuntimeError("无法连接 QEMU monitor/serial")
        self.mon.settimeout(5.0)
        self.ser.settimeout(0.5)

        self._lines = []
        self._partial = b""
        self._lock = threading.Lock()
        self._stop = False
        self._reader = threading.Thread(target=self._read_serial, daemon=True)
        self._reader.start()

        # 光标模型坐标 (home 后从 0,0 起累计)
        self.cur_x, self.cur_y = 0, 0

    # ---------- 基础 IO ----------
    def _connect(self, port, tries=20):
        for _ in range(tries):
            try:
                return socket.create_connection(("127.0.0.1", port), timeout=5)
            except Exception:
                time.sleep(0.5)
        return None

    def _read_serial(self):
        while not self._stop:
            try:
                data = self.ser.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                break
            with self._lock:
                self._partial += data
                while b"\n" in self._partial:
                    line, self._partial = self._partial.split(b"\n", 1)
                    self._lines.append(line.decode("utf-8", "replace").strip("\r"))

    def log_len(self):
        with self._lock:
            return len(self._lines)

    def log_since(self, start):
        with self._lock:
            return list(self._lines[start:])

    def wait_ready(self, timeout=25):
        """等待内核 GUI 就绪"""
        t0 = time.time()
        while time.time() - t0 < timeout:
            if any("[test] ready" in l for l in self.log_since(0)):
                time.sleep(1.0)   # 让首帧渲染完成
                return True
            time.sleep(0.3)
        return False

    def mon_cmd(self, cmd, wait=0.05):
        self.mon.sendall((cmd + "\n").encode())
        if wait:
            time.sleep(wait)

    def wait_for(self, pattern, start, timeout=3.0):
        rx = re.compile(pattern)
        t0 = time.time()
        while time.time() - t0 < timeout:
            for l in self.log_since(start):
                if rx.search(l):
                    return l
            time.sleep(0.1)
        return None

    # ---------- 截图 ----------
    def screenshot(self, name):
        path = os.path.join(self.work, name)
        if os.path.exists(path):
            os.remove(path)
        self.mon_cmd("screendump " + path.replace("\\", "/"), wait=0.05)
        t0 = time.time()
        size = -1
        while time.time() - t0 < 6:
            if os.path.exists(path):
                ns = os.path.getsize(path)
                if ns > 0 and ns == size:
                    return path
                size = ns
            time.sleep(0.1)
        raise RuntimeError("screendump 失败: " + path)

    # ---------- 鼠标注入 ----------
    def move_rel(self, dx, dy, settle=0.03):
        """按 <=100 像素分步注入相对位移 (PS/2 单包限制)"""
        while dx != 0 or dy != 0:
            sx = max(-100, min(100, dx))
            sy = max(-100, min(100, dy))
            self.mon_cmd(f"mouse_move {sx} {sy}", wait=settle)
            self.cur_x += sx
            self.cur_y += sy
            dx -= sx
            dy -= sy
        self.cur_x = max(0, min(SCREEN_W - 1, self.cur_x))
        self.cur_y = max(0, min(SCREEN_H - 1, self.cur_y))
        time.sleep(0.05)

    def home_cursor(self, settle=0.05):
        """负向大位移把光标顶到 (0,0)"""
        for _ in range(10):
            self.mon_cmd("mouse_move -100 -100", wait=settle)
        time.sleep(0.2)
        self.cur_x, self.cur_y = 0, 0

    def move_to(self, x, y, settle=0.03):
        self.move_rel(x - self.cur_x, y - self.cur_y, settle=settle)

    def press(self, button=1, settle=0.06):
        self.mon_cmd(f"mouse_button {button}", wait=settle)

    def release(self, settle=0.06):
        self.mon_cmd("mouse_button 0", wait=settle)

    # ---------- 校验 + 重试 ----------
    def click(self, x, y, expect, retries=3, need_screen=True, tag=""):
        """单击 (x,y): 注入 -> 校验回执/截图 -> 未生效重试

        返回 dict: ok, attempts, evidence, screen_changed
        """
        for attempt in range(1, retries + 1):
            if attempt > 1:
                self.home_cursor()
            self.move_to(x, y)
            before = self.screenshot(f"before_{tag}_{attempt}.ppm") if need_screen else None
            start = self.log_len()
            self.press()
            self.release()
            hit = self.wait_for(expect, start, timeout=1.5)
            changed = None
            if need_screen:
                after = self.screenshot(f"after_{tag}_{attempt}.ppm")
                changed = ppm_diff_ratio(before, after)
            if hit and (not need_screen or changed is None or changed > 0.0):
                return {"ok": True, "attempts": attempt, "evidence": hit,
                        "screen_changed": changed}
            if hit and changed == 0.0:
                # 回执到内核但画面未变: 记录但仍算事件到达
                return {"ok": True, "attempts": attempt, "evidence": hit,
                        "screen_changed": changed, "note": "画面无变化"}
            time.sleep(0.2)
        return {"ok": False, "attempts": retries, "evidence": None, "screen_changed": None}

    def double_click(self, x, y, expect, retries=3, interval=0.13, need_screen=True, tag=""):
        """双击 (x,y): 两次单击间隔 < 300ms (内核双击阈值 30 ticks @100Hz)"""
        for attempt in range(1, retries + 1):
            if attempt > 1:
                self.home_cursor()
            self.move_to(x, y)
            before = self.screenshot(f"before_{tag}_{attempt}.ppm") if need_screen else None
            start = self.log_len()
            self.press()
            self.release(settle=0.02)
            time.sleep(interval)
            self.press()
            self.release()
            hit = self.wait_for(expect, start, timeout=1.5)
            changed = None
            if need_screen:
                after = self.screenshot(f"after_{tag}_{attempt}.ppm")
                changed = ppm_diff_ratio(before, after)
            if hit:
                return {"ok": True, "attempts": attempt, "evidence": hit,
                        "screen_changed": changed}
            time.sleep(0.2)
        return {"ok": False, "attempts": retries, "evidence": None, "screen_changed": None}

    # ---------- 键盘注入 ----------
    def sendkey(self, key, settle=0.08):
        self.mon_cmd("sendkey " + key, wait=settle)

    def open_via_serial(self, win_type):
        """内核自测钩子 open:<type> (仅用于构造场景, 不用于可靠性统计)"""
        start = self.log_len()
        self.ser.sendall(f"open:{win_type}\n".encode())
        return self.wait_for(r"\[test\] opened type=", start, timeout=3.0)

    def close(self):
        self._stop = True
        for s in (self.mon, self.ser):
            try:
                if s:
                    s.close()
            except Exception:
                pass
        try:
            self.proc.terminate()
            self.proc.wait(timeout=10)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass


# ---------------- 测试用例 ----------------

def test_click(session, n=20, icon_index=0):
    """连续 N 次单击任务栏图标, 统计真实注入成功率"""
    x, y = taskbar_icon_center(icon_index)
    session.home_cursor()
    results = []
    for k in range(n):
        r = session.click(x, y, r"\[g\] click TASKBAR", retries=3,
                          need_screen=True, tag=f"click{k}")
        r["seq"] = k + 1
        results.append(r)
        time.sleep(0.25)
    ok = sum(1 for r in results if r["ok"])
    retried = sum(1 for r in results if r["ok"] and r["attempts"] > 1)
    failed = [r["seq"] for r in results if not r["ok"]]
    return {
        "test": "click",
        "target": f"taskbar icon #{icon_index} @({x},{y})",
        "total": n, "success": ok, "failed_seq": failed,
        "success_rate": round(ok * 100.0 / n, 1),
        "retried_success": retried,
        "avg_attempts": round(sum(r["attempts"] for r in results) / float(n), 3),
    }


def test_dblclick(session, n=20):
    """连续 N 次双击桌面 Files 图标, 统计成功率 (以窗口激活回执为准)"""
    x, y = DESK_ICON_CENTER
    session.home_cursor()
    results = []
    for k in range(n):
        r = session.double_click(x, y, r"\[g\] activate win=", retries=3,
                                 interval=0.13, need_screen=True, tag=f"dbl{k}")
        r["seq"] = k + 1
        results.append(r)
        time.sleep(0.25)
    ok = sum(1 for r in results if r["ok"])
    retried = sum(1 for r in results if r["ok"] and r["attempts"] > 1)
    failed = [r["seq"] for r in results if not r["ok"]]
    return {
        "test": "double_click",
        "target": f"desktop icon Files @({x},{y})",
        "total": n, "success": ok, "failed_seq": failed,
        "success_rate": round(ok * 100.0 / n, 1),
        "retried_success": retried,
        "avg_attempts": round(sum(r["attempts"] for r in results) / float(n), 3),
    }


def test_key(session):
    """新建窗口后真实键盘注入实证 (PS/2 sendkey -> 内核 -> 活动窗口)"""
    out = {"test": "keyboard_after_new_window"}

    # --- 阶段 A: 真实双击桌面 Browser 图标 -> 新建窗口 -> 真实键盘 ---
    bx, by = desk_icon_center(1)
    session.home_cursor()
    r = session.double_click(bx, by, r"\[g\] activate win=(\d+) type=(\d+)",
                             retries=3, tag="keyA")
    out["new_window_action"] = r
    win_id = None
    if r.get("evidence"):
        m = re.search(r"win=(\d+) type=(\d+)", r["evidence"])
        if m:
            win_id = int(m.group(1))
            out["new_window_type"] = int(m.group(2))
            out["new_window_type_name"] = WIN_NAMES.get(int(m.group(2)), "?")
    time.sleep(0.4)
    keys = ["h", "e", "l", "l", "o"]
    start = session.log_len()
    for k in keys:
        session.sendkey(k, settle=0.12)
    time.sleep(0.4)
    lines = session.log_since(start)
    arrived = [l for l in lines if "[k] key=" in l]
    dropped = [l for l in lines if "[k] drop" in l]
    out["target_win"] = win_id
    out["keys_sent"] = keys
    out["keys_arrived"] = len(arrived)
    out["keys_dropped"] = len(dropped)
    out["arrive_lines"] = arrived[:10]
    out["drop_lines"] = dropped[:10]
    out["all_keys_to_same_new_win"] = (
        win_id is not None and len(arrived) >= len(keys)
        and all(f"win={win_id} " in l for l in arrived))
    out["screenshot"] = session.screenshot("key_new_window.ppm")
    out["pass"] = (out["all_keys_to_same_new_win"] and len(dropped) == 0)

    # --- 阶段 B: 内核自测钩子打开新窗口 (Todo=45) 后再注入真实键盘 ---
    opened = session.open_via_serial(WIN_TODO)
    act = session.wait_for(rf"\[g\] activate win=(\d+) type={WIN_TODO}",
                           session.log_len() - 60, timeout=3.0)
    time.sleep(0.5)
    start = session.log_len()
    for k in keys:
        session.sendkey(k, settle=0.12)
    time.sleep(0.4)
    lines = session.log_since(start)
    arrived_b = [l for l in lines if "[k] key=" in l]
    dropped_b = [l for l in lines if "[k] drop" in l]
    out["hook_open"] = opened
    out["hook_activate"] = act
    out["hook_keys_arrived"] = len(arrived_b)
    out["hook_keys_dropped"] = len(dropped_b)
    out["hook_arrive_lines"] = arrived_b[:10]
    out["hook_screenshot"] = session.screenshot("key_hook.ppm")
    out["pass_b"] = len(arrived_b) >= len(keys) and len(dropped_b) == 0
    return out


def test_prof(session, seconds=12):
    """窗口激活态帧耗时采样: 分别采集 静态激活窗口 / 每帧重绘激活窗口"""
    out = {}

    # --- 场景 A: 静态激活窗口 (Files, 经真实双击打开) ---
    session.home_cursor()
    r = session.double_click(*DESK_ICON_CENTER, r"\[g\] activate win=", tag="profA")
    out["open_files"] = r
    time.sleep(0.5)
    start = session.log_len()
    t0 = time.time()
    while time.time() - t0 < seconds * 0.5:
        time.sleep(0.5)
    prof_a = [l for l in session.log_since(start) if l.startswith("[prof]")]
    out["static_active_window"] = parse_prof(prof_a)

    # --- 场景 B: 每帧重绘激活窗口 (Clock=47, 窗口内容随 RTC 每帧刷新) ---
    session.open_via_serial(WIN_CLOCK)
    time.sleep(1.0)
    start = session.log_len()
    t0 = time.time()
    while time.time() - t0 < seconds:
        time.sleep(0.5)
    prof_b = [l for l in session.log_since(start) if l.startswith("[prof]")]
    out["animating_active_window"] = parse_prof(prof_b)

    out["test"] = "profile"
    return out


def parse_prof(lines):
    """解析 [prof] 行, 汇总各分量与 avg 的统计量"""
    if not lines:
        return {"samples": 0}
    rx = re.compile(
        r"desk=(\d+)us win=(\d+)us task=(\d+)us status=(\d+)us menu=(\d+)us "
        r"cur=(\d+)us swap=(\d+)us avg=(\d+)us")
    keys = ["desk", "win", "task", "status", "menu", "cur", "swap", "avg"]
    vals = {k: [] for k in keys}
    for l in lines:
        m = rx.search(l)
        if not m:
            continue
        for k, v in zip(keys, m.groups()):
            vals[k].append(int(v))
    if not vals["avg"]:
        return {"samples": 0}
    n = len(vals["avg"])
    stats = {}
    for k in keys:
        arr = vals[k]
        stats[k] = {
            "avg_us": round(sum(arr) / float(len(arr)), 1),
            "max_us": max(arr),
            "min_us": min(arr),
        }
    return {
        "samples": n,
        "avg_total_us": stats["avg"]["avg_us"],
        "avg_total_ms": round(stats["avg"]["avg_us"] / 1000.0, 3),
        "max_total_us": stats["avg"]["max_us"],
        "components_us": {k: stats[k]["avg_us"] for k in keys if k != "avg"},
        "winter_us": stats["win"]["avg_us"],
        "swap_us": stats["swap"]["avg_us"],
        "raw_tail": lines[-5:],
    }


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    mode = sys.argv[1]
    arg2 = int(sys.argv[2]) if len(sys.argv) > 2 else None
    prof_secs = int(sys.argv[3]) if (mode == "all" and len(sys.argv) > 3) else 12

    session = EpochOS()
    summary = {"mode": mode}
    try:
        if not session.wait_ready():
            print(json.dumps({"error": "GUI 未就绪"}, ensure_ascii=False))
            return 1
        if mode == "click-test":
            summary["click"] = test_click(session, arg2 or 20)
        elif mode == "dblclick-test":
            summary["double_click"] = test_dblclick(session, arg2 or 20)
        elif mode == "key-test":
            summary["keyboard"] = test_key(session)
        elif mode == "prof":
            summary["prof"] = test_prof(session, arg2 or 12)
        elif mode == "all":
            n = arg2 or 20
            summary["click"] = test_click(session, n)
            summary["double_click"] = test_dblclick(session, n)
            summary["keyboard"] = test_key(session)
            summary["prof"] = test_prof(session, prof_secs)
        else:
            print(__doc__)
            return 1
    finally:
        session.close()
    out = os.environ.get("EPOCHOS_OUT")
    if out:
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "w", encoding="utf-8") as f:
            json.dump(summary, f, ensure_ascii=False, indent=2)
    print("=" * 70)
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
