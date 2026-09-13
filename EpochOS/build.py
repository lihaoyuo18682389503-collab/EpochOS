# -*- coding: utf-8 -*-
"""
EpochOS Build Script (Python) - 两阶段引导 + 硬盘镜像
用法: python build.py
说明: NASM 3.01 不支持中文路径, 故在 ASCII 临时目录编译, 产物复制回项目目录
镜像布局 (硬盘 -hda, 16MB):
  LBA 0        : boot0 (512B 引导扇区)
  LBA 1-32     : boot2 (16KB, VBE 图形模式设置)
  LBA 33-794   : kernel (最多 762 扇区 = 390KB)
  LBA 1020     : EPFS super block
  LBA 1024+    : EPFS 文件槽位 (每文件 9 扇区: 1 header + 8 data)
"""
import os
import shutil
import sys
import subprocess
import tempfile
import struct

try:
    from PIL import Image
    HAVE_PIL = True
except Exception:
    HAVE_PIL = False

ROOT = os.path.dirname(os.path.abspath(__file__))          # EpochOS 根目录
BOOT_DIR = os.path.join(ROOT, "boot")
KERNEL_DIR = os.path.join(ROOT, "kernel")
USER_DIR = os.path.join(ROOT, "user")
BUILD_DIR = os.path.join(ROOT, "build")

TOOLS_DIR = os.path.join(os.path.dirname(ROOT), "工具")     # E:\Assets\操作系统\工具
TOOLS_BIN = os.path.join(TOOLS_DIR, "bin")

# 工具链候选路径: 工具目录 -> 环境变量 -> WinGet 默认位置
TOOL_BIN_CANDIDATES = [
    TOOLS_BIN,
    r"C:\Users\ThinkPad\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin",
]
QEMU_BIN_CANDIDATES = [
    TOOLS_BIN,
    r"C:\Program Files\qemu",
]

def find_tool(name, candidates):
    for d in candidates:
        p = os.path.join(d, name)
        if os.path.isfile(p):
            return p
    which = shutil.which(name)
    if which:
        return which
    return None

NASM = find_tool("nasm.exe", TOOL_BIN_CANDIDATES)
# 优先使用 i686-elf 交叉工具链 (裸机内核), 其次 MinGW
GCC = find_tool("i686-elf-gcc.exe", TOOL_BIN_CANDIDATES) or find_tool("gcc.exe", TOOL_BIN_CANDIDATES)
LD = find_tool("i686-elf-ld.exe", TOOL_BIN_CANDIDATES) or find_tool("ld.exe", TOOL_BIN_CANDIDATES)
OBJCOPY = find_tool("i686-elf-objcopy.exe", TOOL_BIN_CANDIDATES) or find_tool("objcopy.exe", TOOL_BIN_CANDIDATES)
OBJDUMP = find_tool("i686-elf-objdump.exe", TOOL_BIN_CANDIDATES) or find_tool("objdump.exe", TOOL_BIN_CANDIDATES)
QEMU = find_tool("qemu-system-i386.exe", QEMU_BIN_CANDIDATES)

# 镜像布局常量 (与 boot0.asm / boot2.asm / fs.c 严格一致)
BOOT0_LBA = 0
BOOT2_LBA = 1
BOOT2_SECTORS = 32          # boot2 = 16KB
KERNEL_LBA = 33
KERNEL_MAX_SECTORS = 762    # 390KB (boot2 6 块 DAP 可读上限, LBA 33-794)
IMAGE_SECTORS = 32768       # 16MB

FS_SUPER_LBA = 1020
FS_BASE_LBA = 1024
FS_SLOT_SECTORS = 9         # 1 header + 8 data
FS_MAX_NAME = 24

# boot2 清零 .bss 的范围 (必须与 boot/boot2.asm 中的 BSS 清零循环严格一致)
# 起点固定 0x100000 (linker.ld __bss_start), 长度 4MB
BSS_ZERO_BASE = 0x100000
BSS_ZERO_LIMIT = 0x400000      # 4MB

# ---- 壁纸区域 (与 kernel/wallpaper.h 严格一致) ----
WALL_COUNT = 4
WALL_W = 800
WALL_H = 600
WALL_DIR_LBA = 1596
WALL_DATA_LBA = 1600
WALL_RGB_SIZE = WALL_W * WALL_H * 3
WALL_SECTORS = (WALL_RGB_SIZE + 511) // 512
WALL_NAMES = ["Sunset", "Ocean", "Mountain", "Meadow"]

# 壁纸源 PNG (用户指定) - 蓝色渐变毛玻璃抽象壁纸, 4 槽位均嵌入同一张
WALL_SRC_DIR = r"E:\文件\系统壁纸"
WALL_SRC_FILES = [
    "生成操作系统壁纸 (10).png",
]

def run(cmd, cwd=None):
    print(">>", " ".join(cmd))
    proc = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if proc.stdout:
        print(proc.stdout)
    if proc.stderr:
        print(proc.stderr, file=sys.stderr)
    if proc.returncode != 0:
        raise RuntimeError(f"Command failed: {' '.join(cmd)} (code {proc.returncode})")
    return proc

def read_section(elf_path, name):
    """用 objdump 读取 ELF 段的 (addr, size), 失败返回 None"""
    proc = subprocess.run([OBJDUMP, "-h", elf_path],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        return None
    for line in proc.stdout.splitlines():
        parts = line.split()
        # 形如: "  4 .bss          002d3708  00100000  00100000  0004f000  2**12"
        if len(parts) >= 3 and parts[1] == name:
            try:
                return int(parts[2], 16), int(parts[3], 16)
            except ValueError:
                return None
    return None


def check_bss_fits(elf_path):
    """断言 .bss 完全落在 boot2 清零范围内

    boot2.asm 用固定长度清零 .bss; 若 .bss 超出该范围, 多出的静态变量会
    带着 RAM 随机值启动 (C 语义要求零初始化), 表现为极难排查的随机故障。
    此处在构建期直接失败, 而不是留到运行时发作。
    """
    info = read_section(elf_path, ".bss")
    if info is None:
        print("[warn] 无法读取 .bss 段信息, 跳过清零范围断言")
        return
    size, addr = info
    end = addr + size
    print(f"[bss] .bss = 0x{addr:X} + 0x{size:X} -> 0x{end:X} "
          f"({size / 1048576:.2f} MB), boot2 清零到 0x{BSS_ZERO_BASE + BSS_ZERO_LIMIT:X}")
    if addr != BSS_ZERO_BASE:
        raise RuntimeError(
            f".bss 起点 0x{addr:X} 与 linker.ld/boot2 约定 0x{BSS_ZERO_BASE:X} 不一致")
    if end > BSS_ZERO_BASE + BSS_ZERO_LIMIT:
        raise RuntimeError(
            f".bss 超出 boot2 清零范围: 结束 0x{end:X} > 上限 "
            f"0x{BSS_ZERO_BASE + BSS_ZERO_LIMIT:X} (超出 {end - BSS_ZERO_BASE - BSS_ZERO_LIMIT} 字节)。\n"
            f"       请增大 boot/boot2.asm 中 BSS 清零循环的 ecx (当前 {BSS_ZERO_LIMIT} 字节), "
            f"并同步 build.py 的 BSS_ZERO_LIMIT。")


# ---- EPFS 文件表构造 (与 kernel/fs.c 槽位格式一致) ----
def build_epfs(entries):
    """
    entries: list of (name, is_dir, data_bytes)
    返回: 文件表区域字节 (从 LBA 1020 起, super + 64 槽位)
    """
    out = bytearray()
    # super (1 扇区)
    super_sec = bytearray(512)
    super_sec[0:4] = b"EPFS"
    struct.pack_into("<I", super_sec, 4, 1)   # version
    out += bytes(super_sec)

    slots = [None] * 64
    for idx, (name, is_dir, data) in enumerate(entries):
        if idx >= 64:
            break
        slots[idx] = (name, is_dir, data)

    for i in range(64):
        sec = bytearray(512)
        e = slots[i]
        if e is not None:
            name, is_dir, data = e
            bname = name.encode("utf-8")[:FS_MAX_NAME - 1]
            sec[0:len(bname)] = bname
            struct.pack_into("<I", sec, 24, len(data) if data else 0)   # size
            struct.pack_into("<I", sec, 28, 1 if is_dir else 0)         # flags
        out += bytes(sec)
        data_secs = bytearray(8 * 512)
        if e is not None and not e[1] and e[2]:
            data_secs[0:len(e[2])] = e[2]
        out += bytes(data_secs)
    return bytes(out)

# ---- 壁纸区域构建: 将源 PNG 缩放为 800x600 RGB888 写入镜像固定 LBA ----
def build_wallpaper_area(img):
    if not HAVE_PIL:
        print("[wall] PIL not available, skip wallpaper area")
        return 0
    datas = []
    for name in WALL_SRC_FILES:
        src = os.path.join(WALL_SRC_DIR, name)
        if not os.path.isfile(src):
            print(f"[wall] source missing: {src}, skip wallpaper area")
            return 0
        im = Image.open(src).convert("RGB")
        tw, th = WALL_W, WALL_H
        w, h = im.size
        scale = max(tw / w, th / h)
        nw, nh = int(w * scale + 0.5), int(h * scale + 0.5)
        im2 = im.resize((nw, nh), Image.LANCZOS)
        left = (nw - tw) // 2
        top = (nh - th) // 2
        im3 = im2.crop((left, top, left + tw, top + th))
        rgb = im3.tobytes()
        if len(rgb) != WALL_RGB_SIZE:
            print(f"[wall] bad size for {name}: {len(rgb)}")
            return 0
        datas.append(rgb)
    # 目录区: 4 x 128B 记录 (magic 'WAL' + 名称长度 + 名称)
    dir_sec = bytearray(512)
    for i in range(WALL_COUNT):
        base = i * 128
        dir_sec[base + 0] = ord('W'); dir_sec[base + 1] = ord('A'); dir_sec[base + 2] = ord('L')
        bname = WALL_NAMES[i].encode("utf-8")[:23]
        dir_sec[base + 3] = len(bname)
        dir_sec[base + 4:base + 4 + len(bname)] = bname
    img[WALL_DIR_LBA * 512:WALL_DIR_LBA * 512 + 512] = bytes(dir_sec)
    for i in range(WALL_COUNT):
        lba = WALL_DATA_LBA + i * WALL_SECTORS
        data = datas[i % len(datas)]
        img[lba * 512:lba * 512 + len(data)] = data
    print(f"[wall] {WALL_COUNT} wallpapers embedded @LBA {WALL_DATA_LBA}+ ({WALL_SECTORS} sectors each)")
    return 1

def build_image(boot0, boot2, kernel, out_path, user_elfs=None):
    img = bytearray(IMAGE_SECTORS * 512)
    # boot0 -> LBA 0
    img[BOOT0_LBA * 512:BOOT0_LBA * 512 + len(boot0)] = boot0
    # boot2 -> LBA 1..32
    img[BOOT2_LBA * 512:BOOT2_LBA * 512 + len(boot2)] = boot2
    # kernel -> LBA 33..
    img[KERNEL_LBA * 512:KERNEL_LBA * 512 + len(kernel)] = kernel

    # EPFS 文件表: 预置示例文件
    examples = [
        ("welcome.txt", False, b"Welcome to EpochOS v1.1 GUI!\n\nThis is the graphical edition with:\n - VBE 800x600x32 desktop\n - Window manager (terminal/files/about/help)\n - Mouse + keyboard interaction\n - ramfs with ATA disk persistence\n\nTry: type 'help' in the terminal window.\n"),
        ("readme.txt", False, b"EpochOS quick guide:\n  ls / cd / cat / touch / write\n  mkdir / rmdir / mv / rm\n  date / meminfo / ps / sysinfo\n  save files via GUI Files -> Save\n"),
        ("hello.txt", False, b"Hello, world from EpochOS!\n"),
        ("doc.txt", False, b"EpochOS sample document.\nThis line is a second paragraph.\nA third line for conversion test.\n"),
        ("doc.md", False, b"# EpochOS Markdown\n\n- item one\n- item two\n\n> quoted note\n"),
        ("docs", True, b""),
        ("docs/notes.txt", False, b"EpochOS development notes:\n - boot: boot0 + boot2 two-stage\n - kernel: 32-bit protected mode + paging\n - GUI: VBE LFB + window manager\n - FS: ramfs + ATA persistence\n"),
        ("docs/todo.txt", False, b"TODO:\n - more GUI apps\n - terminal scrollback\n - process list window\n"),
        # 商店应用安装标记: 预置 /apps/paint 使绘图板默认已安装 (与 store_marker_for_type 对应)
        ("apps", True, b""),
        ("apps/paint", True, b""),
        ("apps/todo", True, b""),
        ("apps/passgen", True, b""),
        ("apps/clock", True, b""),
    ]
    # 打入 ELF 用户测试程序 (阶段3): 挂到 bin/ 目录下
    if user_elfs:
        if not any(e[0] == "bin" for e in examples):
            examples.append(("bin", True, b""))
        for name, data in user_elfs.items():
            examples.append((name, False, data))
    epfs = build_epfs(examples)
    # super block -> LBA 1020 (FS_SUPER_LBA)
    img[FS_SUPER_LBA * 512:FS_SUPER_LBA * 512 + 512] = epfs[0:512]
    # 槽位区 -> LBA 1024 起 (FS_BASE_LBA, 与内核 fs_disk_mount/save 一致)
    slots = epfs[512:]
    img[FS_BASE_LBA * 512:FS_BASE_LBA * 512 + len(slots)] = slots

    # 壁纸区域 (固定 LBA, 与内核 wallpaper.h 一致)
    build_wallpaper_area(img)

    with open(out_path, "wb") as f:
        f.write(bytes(img))

def main():
    for name, path in [("NASM", NASM), ("GCC", GCC), ("LD", LD)]:
        if not path:
            raise RuntimeError(f"Tool not found: {name}. Run 工具\\scripts\\setup_tools.bat first.")

    os.makedirs(BUILD_DIR, exist_ok=True)

    # 在 ASCII 临时目录构建 (NASM 3.01 不支持中文路径)
    work = os.path.join(tempfile.gettempdir(), "epochos_build")
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    boot_work = os.path.join(work, "boot")
    kernel_work = os.path.join(work, "kernel")
    user_work = os.path.join(work, "user")
    os.makedirs(boot_work)
    os.makedirs(kernel_work)
    os.makedirs(user_work)

    # 复制源码到工作目录
    for f in ["boot0.asm", "boot2.asm"]:
        shutil.copy2(os.path.join(BOOT_DIR, f), os.path.join(boot_work, f))
    for f in os.listdir(KERNEL_DIR):
        if f.endswith((".c", ".h", ".S")):
            shutil.copy2(os.path.join(KERNEL_DIR, f), os.path.join(kernel_work, f))
    # ring3 用户测试程序 (汇编, 供 NASM 编译)
    shutil.copy2(os.path.join(KERNEL_DIR, "ring3_test.asm"), os.path.join(kernel_work, "ring3_test.asm"))
    # ELF 用户测试程序源码 (阶段3 hello + 阶段4 epoch-libc 与测试程序)
    for f in os.listdir(USER_DIR):
        if f.endswith((".c", ".h", ".s", ".asm", ".ld")):
            shutil.copy2(os.path.join(USER_DIR, f), os.path.join(user_work, f))
    shutil.copy2(os.path.join(ROOT, "linker.ld"), os.path.join(work, "linker.ld"))

    print("=" * 50)
    print("EpochOS Build (two-stage boot + GUI + ring3)")
    print("=" * 50)

    # 1. 编译 bootloader
    print("[1/4] Assembling bootloaders...")
    boot0_bin = os.path.join(work, "boot0.bin")
    run([NASM, "-f", "bin", os.path.join(boot_work, "boot0.asm"), "-o", boot0_bin])
    boot2_bin = os.path.join(work, "boot2.bin")
    run([NASM, "-f", "bin", os.path.join(boot_work, "boot2.asm"), "-o", boot2_bin])

    # 1.5 编译 ring3 用户测试程序 (阶段2): asm -> bin -> C 头文件嵌入内核
    print("[1.5] Assembling ring3 user test program...")
    ring3_asm = os.path.join(kernel_work, "ring3_test.asm")
    ring3_bin = os.path.join(work, "ring3_test.bin")
    run([NASM, "-f", "bin", ring3_asm, "-o", ring3_bin])
    with open(ring3_bin, "rb") as f:
        ring3_data = f.read()
    ring3_lines = []
    for i in range(0, len(ring3_data), 12):
        chunk = ring3_data[i:i + 12]
        ring3_lines.append(", ".join("0x%02X" % b for b in chunk))
    ring3_hdr = (
        "// Auto-generated by build.py from ring3_test.asm - DO NOT EDIT\n"
        "#ifndef EPOCHOS_RING3_TEST_BIN_H\n"
        "#define EPOCHOS_RING3_TEST_BIN_H\n"
        "#include \"types.h\"\n"
        "static const uint8_t ring3_test_bin[] = {\n" +
        ",\n".join(ring3_lines) +
        "\n};\n"
        "static const uint32_t ring3_test_len = sizeof(ring3_test_bin);\n"
        "#endif\n"
    )
    with open(os.path.join(kernel_work, "ring3_test_bin.h"), "w") as f:
        f.write(ring3_hdr)
    print(f"[ring3] test program {len(ring3_data)} bytes -> ring3_test_bin.h")

    # 1.6 编译 ELF 用户测试程序 (阶段3 hello + 阶段4 epoch-libc 程序)
    #     生成 ELF32 ET_EXEC, 打入镜像 /bin/ (ramfs 单文件上限 4096B)
    print("[1.6] Building ELF user test programs (epoch-libc)...")
    hello_elf_o = os.path.join(work, "hello_elf.o")
    run([NASM, "-f", "elf32", os.path.join(user_work, "hello_elf.asm"), "-o", hello_elf_o])
    hello_elf_bin = os.path.join(work, "hello_elf")
    run([LD, "-m", "elf_i386", "-N", "-T", os.path.join(user_work, "elf32.ld"),
         hello_elf_o, "-o", hello_elf_bin])
    hello_c_o = os.path.join(work, "hello_c.o")
    run([GCC, "-m32", "-ffreestanding", "-fno-pie", "-fno-stack-protector",
         "-fno-builtin", "-nostdlib", "-c",
         os.path.join(user_work, "hello_c.c"), "-o", hello_c_o])
    hello_c_bin = os.path.join(work, "hello_c")
    run([LD, "-m", "elf_i386", "-N", "-T", os.path.join(user_work, "elf32.ld"),
         hello_c_o, "-o", hello_c_bin])

    def check_elf32(path, label):
        with open(path, "rb") as f:
            head = f.read(20)
        if head[0:4] != b"\x7fELF":
            raise RuntimeError(f"{label}: bad magic")
        etype, emach = struct.unpack_from("<HH", head, 16)
        if etype != 2:
            raise RuntimeError(f"{label}: not ET_EXEC (type={etype})")
        if emach != 3:
            raise RuntimeError(f"{label}: not EM_386 (machine={emach})")
        print(f"[elf] {label}: OK ({os.path.getsize(path)} bytes, ET_EXEC EM_386)")

    check_elf32(hello_elf_bin, "hello_elf")
    check_elf32(hello_c_bin, "hello_c")

    # ---- epoch-libc (阶段4): crt0 + epoch.o 一次编译, 各程序共享 ----
    LIB_FLAGS = ["-m32", "-ffreestanding", "-fno-pie", "-fno-stack-protector",
                 "-fno-builtin", "-nostdlib", "-Os", "-fomit-frame-pointer",
                 "-ffunction-sections", "-fdata-sections"]
    crt0_o = os.path.join(work, "crt0.o")
    run([GCC, "-m32", "-ffreestanding", "-nostdlib", "-c",
         os.path.join(user_work, "crt0.s"), "-o", crt0_o])
    epoch_o = os.path.join(work, "epoch.o")
    run([GCC] + LIB_FLAGS + ["-c", os.path.join(user_work, "epoch.c"),
                             "-o", epoch_o])

    def build_libc_app(src, label):
        obj = os.path.join(work, label + ".o")
        run([GCC] + LIB_FLAGS + ["-c", os.path.join(user_work, src), "-o", obj])
        out = os.path.join(work, label)
        run([LD, "-m", "elf_i386", "-N", "--gc-sections",
             "-T", os.path.join(user_work, "elf32.ld"),
             crt0_o, epoch_o, obj, "-o", out])
        check_elf32(out, label)
        with open(out, "rb") as f:
            data = f.read()
        if len(data) > 4096:
            raise RuntimeError(f"{label}: ELF {len(data)}B > 4096 (ramfs limit)")
        print(f"[elf] {label}: fits ramfs ({len(data)}/4096 B)")
        return out, data

    argecho_bin, argecho_data = build_libc_app("argecho.c", "argecho")
    calc_bin, calc_data = build_libc_app("calc.c", "calc")
    fileio_bin, fileio_data = build_libc_app("fileio.c", "fileio")
    fib_bin, fib_data = build_libc_app("fib.c", "fib")
    cp_bin, cp_data = build_libc_app("cp.c", "cp")
    strtest_bin, strtest_data = build_libc_app("strtest.c", "strtest")
    syscheck_bin, syscheck_data = build_libc_app("syscheck.c", "syscheck")
    badptr_bin, badptr_data = build_libc_app("badptr.c", "badptr")
    linuxabi_bin, linuxabi_data = build_libc_app("linuxabi.c", "linuxabi")

    with open(hello_elf_bin, "rb") as f:
        hello_elf_data = f.read()
    with open(hello_c_bin, "rb") as f:
        hello_c_data = f.read()
    if len(hello_elf_data) > 4096 or len(hello_c_data) > 4096:
        raise RuntimeError("ELF test program too large for ramfs (max 4096 B)")
    user_elfs = {
        "bin/hello_elf": hello_elf_data,
        "bin/hello_c": hello_c_data,
        "bin/argecho": argecho_data,
        "bin/calc": calc_data,
        "bin/fileio": fileio_data,
        "bin/fib": fib_data,
        "bin/cp": cp_data,
        "bin/strtest": strtest_data,
        "bin/syscheck": syscheck_data,
        "bin/badptr": badptr_data,
        "bin/linuxabi": linuxabi_data,
    }
    # 打包各 ELF 到 build/ (供 QEMU 自测与检视)
    for name in ["hello_elf", "hello_c", "argecho", "calc", "fileio",
                 "fib", "cp", "strtest", "syscheck", "badptr", "linuxabi"]:
        shutil.copy2(os.path.join(work, name), os.path.join(BUILD_DIR, name))

    # 2. 编译内核 (所有 .c 与 .S)
    print("[2/4] Compiling kernel...")
    # 内核编译: -Werror 让 "excess elements in array initializer" 之类的问题
    # 直接失败, 而不是被静默丢弃 (desk_icons 曾多写一项被 GCC 悄悄截断)
    KERNEL_CFLAGS = ["-m32", "-ffreestanding", "-fno-pie", "-fno-stack-protector",
                     "-fno-builtin", "-nostdlib", "-Werror", "-c"]
    kernel_o_list = []
    for src in sorted(os.listdir(kernel_work)):
        if src.endswith(".c"):
            obj = os.path.join(work, src.replace(".c", ".o"))
            run([GCC] + KERNEL_CFLAGS +
                [os.path.join(kernel_work, src), "-o", obj])
            kernel_o_list.append(obj)
        elif src.endswith(".S"):
            obj = os.path.join(work, src.replace(".S", "_asm.o"))
            run([GCC, "-m32", "-ffreestanding", "-c",
                 os.path.join(kernel_work, src), "-o", obj])
            kernel_o_list.append(obj)

    # 3. 链接内核为 ELF 再转纯二进制
    print("[3/4] Linking kernel...")
    kernel_elf = os.path.join(work, "kernel.elf")
    run([LD, "-T", os.path.join(work, "linker.ld"),
         "-o", kernel_elf] + kernel_o_list)
    # 断言 .bss 落在 boot2 清零范围内 (超出会导致静态变量未零初始化)
    check_bss_fits(kernel_elf)
    kernel_bin = os.path.join(work, "kernel.bin")
    # 只输出前三个段: .bss 位于 2MB 处 (NOBITS, 运行时由 boot2 清零),
    # 若整体输出会把 .data 与 .bss 之间的空洞 (约 1.8MB) 填零进 kernel.bin
    run([OBJCOPY, "-O", "binary",
         "--only-section=.text", "--only-section=.rodata", "--only-section=.data",
         kernel_elf, kernel_bin])

    # 4. 生成硬盘镜像
    print("[4/4] Creating disk image...")
    with open(boot0_bin, "rb") as f:
        boot0_data = f.read()
    with open(boot2_bin, "rb") as f:
        boot2_data = f.read()
    with open(kernel_bin, "rb") as f:
        kernel_data = f.read()

    if len(boot0_data) != 512:
        raise RuntimeError(f"boot0 must be 512 bytes, got {len(boot0_data)}")
    if len(boot2_data) != BOOT2_SECTORS * 512:
        raise RuntimeError(f"boot2 must be {BOOT2_SECTORS*512} bytes, got {len(boot2_data)}")
    if len(kernel_data) > KERNEL_MAX_SECTORS * 512:
        raise RuntimeError(f"Kernel too large: {len(kernel_data)} > {KERNEL_MAX_SECTORS*512}")

    img = os.path.join(BUILD_DIR, "epochos.img")
    build_image(boot0_data, boot2_data, kernel_data, img, user_elfs=user_elfs)
    shutil.copy2(boot0_bin, os.path.join(BUILD_DIR, "boot0.bin"))
    shutil.copy2(boot2_bin, os.path.join(BUILD_DIR, "boot2.bin"))
    shutil.copy2(kernel_bin, os.path.join(BUILD_DIR, "kernel.bin"))
    shutil.copy2(hello_elf_bin, os.path.join(BUILD_DIR, "hello_elf"))
    shutil.copy2(hello_c_bin, os.path.join(BUILD_DIR, "hello_c"))

    print()
    print(f"Build OK! boot0={len(boot0_data)} boot2={len(boot2_data)} kernel={len(kernel_data)}")
    print(f"Image: {img} ({IMAGE_SECTORS*512//1048576} MB, -hda)")
    print()
    if QEMU:
        print("Run with QEMU (q35 必需, 规避 11.1 i440fx 显示表面刷新花屏; 附加 piix3-ide 供内核 IDE PIO 读盘):")
        print(f'  "{QEMU}" -machine q35 -vga std -drive file="{img}",format=raw,if=none,id=disk -device piix3-ide,id=ide -device ide-hd,drive=disk,bus=ide.0 -display sdl')
    else:
        print("QEMU not found, install it to run.")

if __name__ == "__main__":
    main()
