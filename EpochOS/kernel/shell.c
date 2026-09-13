// ============================================================
// EpochOS - 命令行 Shell
// 支持: help about clear echo uptime meminfo ls touch cat rm
//       write date calc ver sysinfo reboot ps mkdir rmdir mv cd
//       pwd hexdump sleep whoami shutdown color 等
// ============================================================
#include "shell.h"
#include "vga.h"
#include "keyboard.h"
#include "timer.h"
#include "mm.h"
#include "fs.h"
#include "task.h"
#include "cmos.h"
#include "string.h"
#include "ata.h"
#include "mouse.h"
#include "speaker.h"
#include "serial.h"
#include "elf.h"
#include "ring3_test_bin.h"

#define CMDLINE_MAX 128

// ---- 命令函数前置声明 ----
static void cmd_help(int argc, char** argv);
static void cmd_about(int argc, char** argv);
static void cmd_clear(int argc, char** argv);
static void cmd_echo(int argc, char** argv);
static void cmd_uptime(int argc, char** argv);
static void cmd_meminfo(int argc, char** argv);
static void cmd_ver(int argc, char** argv);
static void cmd_sysinfo(int argc, char** argv);
static void cmd_calc(int argc, char** argv);
static void cmd_date(int argc, char** argv);
static void cmd_reboot(int argc, char** argv);
static void cmd_ls(int argc, char** argv);
static void cmd_touch(int argc, char** argv);
static void cmd_write(int argc, char** argv);
static void cmd_cat(int argc, char** argv);
static void cmd_rm(int argc, char** argv);
static void cmd_ps(int argc, char** argv);
static void cmd_mkdir(int argc, char** argv);
static void cmd_rmdir(int argc, char** argv);
static void cmd_mv(int argc, char** argv);
static void cmd_cd(int argc, char** argv);
static void cmd_pwd(int argc, char** argv);
static void cmd_hexdump(int argc, char** argv);
static void cmd_sleep(int argc, char** argv);
static void cmd_whoami(int argc, char** argv);
static void cmd_shutdown(int argc, char** argv);
static void cmd_color(int argc, char** argv);
static void cmd_diskinfo(int argc, char** argv);
static void cmd_diskread(int argc, char** argv);
static void cmd_diskwrite(int argc, char** argv);
static void cmd_mouse(int argc, char** argv);
static void cmd_beep(int argc, char** argv);
static void cmd_serial(int argc, char** argv);
static void cmd_ring3(int argc, char** argv);
static void cmd_elf(int argc, char** argv);
static void cmd_exec(int argc, char** argv);
static void cmd_wait(int argc, char** argv);
static int wait_ring3_exit(int slot);

// ---- 命令表 ----
static struct command {
    const char* name;
    void (*fn)(int, char**);
} commands[] = {
    {"help",    cmd_help},
    {"about",   cmd_about},
    {"clear",   cmd_clear},
    {"echo",    cmd_echo},
    {"uptime",  cmd_uptime},
    {"meminfo", cmd_meminfo},
    {"ver",     cmd_ver},
    {"sysinfo", cmd_sysinfo},
    {"calc",    cmd_calc},
    {"date",    cmd_date},
    {"reboot",  cmd_reboot},
    {"ls",      cmd_ls},
    {"touch",   cmd_touch},
    {"write",   cmd_write},
    {"cat",     cmd_cat},
    {"rm",      cmd_rm},
    {"ps",      cmd_ps},
    {"mkdir",   cmd_mkdir},
    {"rmdir",   cmd_rmdir},
    {"mv",      cmd_mv},
    {"cd",      cmd_cd},
    {"pwd",     cmd_pwd},
    {"hexdump", cmd_hexdump},
    {"sleep",   cmd_sleep},
    {"whoami",  cmd_whoami},
    {"shutdown",cmd_shutdown},
    {"color",   cmd_color},
    {"diskinfo",cmd_diskinfo},
    {"diskread",cmd_diskread},
    {"diskwrite",cmd_diskwrite},
    {"mouse",   cmd_mouse},
    {"beep",    cmd_beep},
    {"serial",  cmd_serial},
    {"ring3",   cmd_ring3},
    {"elf",     cmd_elf},
    {"exec",    cmd_exec},
    {"wait",    cmd_wait},
    {0, 0}
};

static char cmdline[CMDLINE_MAX];
static int cmd_len = 0;

// 当前目录 (ramfs 路径, "/" 为根)
static char cwd[FS_MAX_NAME] = "/";

// 解析: 把命令行按空白拆成 argc/argv
static int parse_cmd(char* line, char** argv, int max_argc) {
    int argc = 0;
    char* p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') *p++ = 0;
        if (*p == 0) break;
        if (argc < max_argc) {
            argv[argc++] = p;
        }
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return argc;
}

static void print_prompt(void) {
    vga_write_color("epochos:", COLOR_LIGHT_GREEN, COLOR_BLACK);
    vga_write_color(cwd, COLOR_LIGHT_CYAN, COLOR_BLACK);
    vga_write_color("# ", COLOR_LIGHT_GREEN, COLOR_BLACK);
}

// ---- 命令实现 ----

static void cmd_help(int argc, char** argv) {
    vga_write("EpochOS Shell - available commands:\n");
    vga_write("  help            show this help\n");
    vga_write("  about           about EpochOS\n");
    vga_write("  clear           clear screen\n");
    vga_write("  echo <text>     print text\n");
    vga_write("  uptime          system uptime\n");
    vga_write("  meminfo         memory usage\n");
    vga_write("  ver             kernel version\n");
    vga_write("  sysinfo         CPU & system info\n");
    vga_write("  calc <a> <op> <b>   simple calculator\n");
    vga_write("  date            real time (CMOS RTC)\n");
    vga_write("  reboot          reboot machine\n");
    vga_write("  shutdown        power off machine\n");
    vga_write("  ls              list files\n");
    vga_write("  cd <dir>        change directory\n");
    vga_write("  pwd             print working directory\n");
    vga_write("  mkdir <name>    create directory\n");
    vga_write("  rmdir <name>    remove empty directory\n");
    vga_write("  touch <name>    create empty file\n");
    vga_write("  write <name> <text>  write file\n");
    vga_write("  cat <name>      show file content\n");
    vga_write("  mv <old> <new>  rename file/dir\n");
    vga_write("  rm <name>       delete file\n");
    vga_write("  hexdump <name>  hex view file\n");
    vga_write("  sleep <sec>     busy-wait seconds\n");
    vga_write("  whoami          current user\n");
    vga_write("  color <fg>      set text color (0-15)\n");
    vga_write("  ps              list tasks (pid/state/exit code)\n");
    vga_write("  diskinfo        show ATA hard disk info\n");
    vga_write("  diskread <lba> <count>  read disk sectors\n");
    vga_write("  diskwrite <lba> <count> <value>  fill sectors\n");
    vga_write("  mouse           show mouse status\n");
    vga_write("  beep [freq] [ms]  beep speaker\n");
    vga_write("  serial <text>   send text to COM1\n");
    vga_write("  elf <path> [args...]  run ELF user program (epoch-libc)\n");
    vga_write("  exec <path> [args...]  alias of elf\n");
    vga_write("  wait <slot>     wait process exit and show its exit code\n");
    (void)argc; (void)argv;
}

static void cmd_about(int argc, char** argv) {
    vga_write_color("============================================\n", COLOR_LIGHT_CYAN, COLOR_BLACK);
    vga_write_color("  EpochOS - From Scratch x86 Operating System\n", COLOR_LIGHT_GREEN, COLOR_BLACK);
    vga_write_color("  Version 0.2  |  32-bit Protected Mode\n", COLOR_LIGHT_GREEN, COLOR_BLACK);
    vga_write_color("  Built with NASM + i686-elf-gcc, zero deps\n", COLOR_LIGHT_GREEN, COLOR_BLACK);
    vga_write_color("============================================\n", COLOR_LIGHT_CYAN, COLOR_BLACK);
    (void)argc; (void)argv;
}

static void cmd_clear(int argc, char** argv) {
    vga_clear(0x0A);
    (void)argc; (void)argv;
}

static void cmd_echo(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        vga_write(argv[i]);
        if (i < argc - 1) vga_write(" ");
    }
    vga_newline();
}

static void cmd_uptime(int argc, char** argv) {
    uint32_t up = timer_get_uptime();
    vga_printf("Uptime: %u seconds (%u ticks)\n", up, timer_get_ticks());
    (void)argc; (void)argv;
}

static void cmd_meminfo(int argc, char** argv) {
    uint32_t total = mm_total_pages() * MM_PAGE_SIZE;
    uint32_t used = (mm_total_pages() - mm_free_pages()) * MM_PAGE_SIZE;
    vga_printf("Memory: %u KB total, %u KB used, %u KB free\n",
               total / 1024, used / 1024, (total - used) / 1024);
    vga_printf("Pages: %u total, %u free\n", mm_total_pages(), mm_free_pages());
    (void)argc; (void)argv;
}

static void cmd_ver(int argc, char** argv) {
    vga_write("EpochOS 0.2 (i686, 2026-08-17)\n");
    (void)argc; (void)argv;
}

static void cmd_sysinfo(int argc, char** argv) {
    vga_write("CPU: x86 (i686) 32-bit Protected Mode\n");
    vga_write("Timer: PIT 8254 @ 100Hz\n");
    vga_write("Keyboard: PS/2 8042, IRQ1\n");
    vga_write("Display: VGA text mode 80x25, 0xB8000\n");
    vga_write("Disk: Floppy (virtual, 1.44MB)\n");
    vga_write("FS: ramfs (in-memory filesystem)\n");
    (void)argc; (void)argv;
}

static void cmd_calc(int argc, char** argv) {
    if (argc < 4) {
        vga_write("Usage: calc <a> <op> <b>  (op: + - * / %)\n");
        return;
    }
    int a = atoi(argv[1]);
    int b = atoi(argv[3]);
    char op = argv[2][0];
    int result = 0;
    switch (op) {
    case '+': result = a + b; break;
    case '-': result = a - b; break;
    case '*': result = a * b; break;
    case '/':
        if (b == 0) { vga_write("Error: divide by zero\n"); return; }
        result = a / b;
        break;
    case '%':
        if (b == 0) { vga_write("Error: modulo by zero\n"); return; }
        result = a % b;
        break;
    default:
        vga_write("Unknown operator (use + - * / %)\n");
        return;
    }
    vga_printf("%d %c %d = %d\n", a, op, b, result);
}

static void cmd_date(int argc, char** argv) {
    struct rtc_time t;
    char buf[32];
    rtc_read(&t);
    rtc_format(buf, sizeof(buf));
    vga_write("Real time (CMOS RTC): ");
    vga_write(buf);
    vga_write("  ");
    vga_write(rtc_weekday_name(t.weekday));
    vga_newline();
    uint32_t up = timer_get_uptime();
    vga_printf("Uptime: %u seconds\n", up);
    (void)argc; (void)argv;
}

static void cmd_reboot(int argc, char** argv) {
    vga_write("Rebooting...\n");
    outb(0x64, 0xFE);
    for (;;) { __asm__ __volatile__("hlt"); }
    (void)argc; (void)argv;
}

static void cmd_ls(int argc, char** argv) {
    char buf[512];
    int n;
    if (argc >= 2) {
        // ls <dir> 列出指定目录
        n = fs_list_dir(argv[1], buf, sizeof(buf));
        if (n < 0) {
            vga_printf("Error: not a directory: %s\n", argv[1]);
            return;
        }
        vga_printf("%s (%d entries):\n", argv[1], n);
    } else {
        n = fs_list_dir(cwd, buf, sizeof(buf));
        if (n < 0) {
            vga_write("Error: cannot list current dir\n");
            return;
        }
        vga_printf("%s (%d entries):\n", cwd, n);
    }
    if (n == 0) {
        vga_write("(empty)\n");
        return;
    }
    vga_write(buf);
}

static void cmd_touch(int argc, char** argv) {
    if (argc < 2) { vga_write("Usage: touch <name>\n"); return; }
    if (fs_create(argv[1]) == 0) {
        vga_printf("Created: %s\n", argv[1]);
    } else {
        vga_write("Error: cannot create file\n");
    }
}

static void cmd_write(int argc, char** argv) {
    if (argc < 3) { vga_write("Usage: write <name> <text>\n"); return; }
    char content[256];
    content[0] = 0;
    int used = 0;
    const int cap = (int)sizeof(content) - 1;   // 留 1 字节给 NUL
    for (int i = 2; i < argc; i++) {
        if (i > 2 && used < cap) content[used++] = ' ';
        const char* a = argv[i];
        while (*a && used < cap) content[used++] = *a++;
        content[used] = 0;
    }
    if (fs_write(argv[1], (const uint8_t*)content, (uint32_t)strlen(content)) == 0) {
        vga_printf("Written %u bytes to %s\n", (uint32_t)strlen(content), argv[1]);
    } else {
        vga_write("Error: write failed\n");
    }
}

static void cmd_cat(int argc, char** argv) {
    if (argc < 2) { vga_write("Usage: cat <name>\n"); return; }
    uint8_t buf[256];
    int n = fs_read(argv[1], buf, sizeof(buf) - 1);
    if (n < 0) {
        vga_printf("Error: file not found: %s\n", argv[1]);
        return;
    }
    buf[n] = 0;
    vga_write((char*)buf);
    vga_newline();
}

static void cmd_rm(int argc, char** argv) {
    if (argc < 2) { vga_write("Usage: rm <name>\n"); return; }
    if (fs_delete(argv[1]) == 0) {
        vga_printf("Deleted: %s\n", argv[1]);
    } else {
        vga_printf("Error: file not found: %s\n", argv[1]);
    }
}

// 显示用户进程列表: pid/名称/状态/退出码/用户栈/IP
static void cmd_ps(int argc, char** argv) {
    vga_printf("Tasks (%d/%d):\n", task_get_count(), TASK_MAX);
    serial_printf(COM1, "[ps] Tasks (%d/%d)\r\n", task_get_count(), TASK_MAX);
    for (int i = 0; i < task_get_count(); i++) {
        const struct task* t = task_get(i);
        if (!t || t->state == TASK_EMPTY) continue;
        const char* state_name = "?";
        switch (t->state) {
        case TASK_RUNNING: state_name = "RUNNING"; break;
        case TASK_READY:   state_name = "READY"; break;
        case TASK_EXITED:  state_name = "EXITED"; break;
        default: break;
        }
        if (t->ring == TASK_RING3) {
            vga_printf("  [%d] pid=%u ", i, t->pid);
            vga_write(t->name);
            int pad = 12 - (int)strlen(t->name);
            if (pad > 0) { for (int k = 0; k < pad; k++) vga_write(" "); }
            vga_printf("%s exit=%d esp=0x%x ticks=%u\n",
                       state_name, t->exit_code,
                       t->user_stack, t->ticks_run);
            serial_printf(COM1, "[ps] [%d] pid=%u %s %s exit=%d esp=0x%x ticks=%u\r\n",
                          i, t->pid, t->name, state_name, t->exit_code,
                          t->user_stack, t->ticks_run);
        } else {
            vga_printf("  [%d] pid=%u ", i, t->pid);
            vga_write(t->name);
            int pad = 12 - (int)strlen(t->name);
            if (pad > 0) { for (int k = 0; k < pad; k++) vga_write(" "); }
            vga_printf("%s (ring0) ticks=%u\n", state_name, t->ticks_run);
            serial_printf(COM1, "[ps] [%d] pid=%u %s %s (ring0) ticks=%u\r\n",
                          i, t->pid, t->name, state_name, t->ticks_run);
        }
    }
    (void)argc; (void)argv;
}

// ring3 用户态测试: 创建用户进程运行内嵌汇编测试程序 (mov eax,1;mov ebx,42;int 0x80)
static void cmd_ring3(int argc, char** argv) {
    int slot = process_create_user("ring3test", ring3_test_bin, ring3_test_len);
    if (slot < 0) {
        vga_write("ring3: failed to create user process\n");
        serial_write(COM1, "[shell] ring3: create failed\r\n");
        return;
    }
    uint32_t pid = task_get(slot) ? task_get(slot)->pid : 0;
    vga_printf("ring3: user process created slot=%d pid=%u (expect exit=42)\n", slot, pid);
    serial_printf(COM1, "[shell] ring3: slot=%d pid=%u created, waiting exit...\r\n", slot, pid);
    (void)argc; (void)argv;
}

// ELF 用户程序加载: 从 ramfs 读取 ELF32 静态程序 -> 解析 -> 创建 ring3 进程
// 用法: elf <path> [arg1 arg2 ...]  (示例: elf /bin/argecho hello 42)
static void cmd_elf(int argc, char** argv) {
    const char* path = "/bin/hello_elf";
    if (argc >= 2) path = argv[1];
    if (!path[0]) {
        vga_write("Usage: elf <path> [args...]\n");
        return;
    }
    // 进程名取路径最后一段 (截断到 TASK_NAME_MAX-1)
    const char* base = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    char name[TASK_NAME_MAX];
    strncpy(name, base, TASK_NAME_MAX - 1);
    name[TASK_NAME_MAX - 1] = 0;

    // 用户进程 argv: argv[0]=进程名, argv[1..]=命令行剩余参数
    const char* uargv[16];
    int uargc = 1;
    uargv[0] = name;
    for (int i = 2; i < argc && uargc < 16; i++) {
        uargv[uargc++] = argv[i];
    }

    vga_printf("elf: loading '%s' (%d arg%s) ...\n", path, uargc,
               uargc > 1 ? "s" : "");
    int slot = elf_load_from_fs(path, name, uargc, uargv);
    if (slot < 0) {
        vga_write("elf: load failed (check path / ELF32 static ET_EXEC / magic)\n");
        serial_printf(COM1, "[shell] elf: load '%s' failed\r\n", path);
        return;
    }
    uint32_t pid = task_get(slot) ? task_get(slot)->pid : 0;
    vga_printf("elf: process created slot=%d pid=%u (ELF ring3 running)\n", slot, pid);
    serial_printf(COM1, "[shell] elf: slot=%d pid=%u loaded, waiting exit...\r\n", slot, pid);
    // 运行 ELF 后自动等待退出并显示退出码 (阶段5: elf 命令一站式回显)
    wait_ring3_exit(slot);
    (void)argc;
}

// exec 命令 = elf 的别名 (Linux 风格入口, 同样支持传参)
static void cmd_exec(int argc, char** argv) {
    cmd_elf(argc, argv);
}

// 忙等 slot 上的 ring3 进程退出, 并回显其退出码
// 返回: 0=已回显退出码; 1=进程不存在/已被 reap (无退出码可读)
static int wait_ring3_exit(int slot) {
    const struct task* t = task_get(slot);
    if (!t || t->state == TASK_EMPTY) return 1;
    // 忙等 (shell 为内核任务; 时钟中断驱动调度, 需主动让出)
    // 注意: 退出忙等后须保持中断开启, 否则 GUI 主循环 idle(hlt) 再无法被唤醒
    for (;;) {
        t = task_get(slot);
        if (!t || t->state == TASK_EMPTY || t->state == TASK_EXITED) break;
        __asm__ __volatile__("sti; hlt");
    }
    if (!t || t->state == TASK_EMPTY) return 1;
    vga_printf("elf: '%s' (pid=%u) exit code: %d\n",
               t->name, t->pid, t->exit_code);
    serial_printf(COM1, "[shell] elf: pid=%u '%s' exit code: %d\r\n",
                  t->pid, t->name, t->exit_code);
    return 0;
}

// wait <slot|pid>: 等待某 ring3 进程退出并显示其 exit code
// 用法: wait <slot>   (slot 为 ps 输出中的任务槽位)
static void cmd_wait(int argc, char** argv) {
    if (argc < 2) {
        vga_write("Usage: wait <slot>\n");
        return;
    }
    int slot = atoi(argv[1]);
    const struct task* t = task_get(slot);
    if (!t || t->state == TASK_EMPTY) {
        vga_printf("wait: slot %d not exists\n", slot);
        return;
    }
    if (t->ring != TASK_RING3) {
        vga_printf("wait: slot %d is not a ring3 process\n", slot);
        return;
    }
    vga_printf("wait: waiting pid=%u '%s' ...\n", t->pid, t->name);
    // 忙等 (shell 为内核任务; 时钟中断驱动调度, 需主动让出)
    // 注意: 退出忙等后须保持中断开启 (同 cmd_elf), 否则主循环 idle 冻结
    for (;;) {
        t = task_get(slot);
        if (!t || t->state == TASK_EMPTY || t->state == TASK_EXITED) break;
        __asm__ __volatile__("sti; hlt");
    }
    if (!t || t->state == TASK_EMPTY) {
        vga_write("wait: process was reaped (no exit code)\n");
        return;
    }
    vga_printf("wait: '%s' exited with code %d\n", t->name, t->exit_code);
    serial_printf(COM1, "[shell] wait: pid=%u '%s' exit_code=%d\r\n",
                  t->pid, t->name, t->exit_code);
}

// ---- 路径工具 ----
// 把 cwd + arg 拼接为完整路径, 处理 ".." 与 "."
static void join_path(const char* cwd_in, const char* arg, char* out, uint32_t max) {
    char tmp[FS_MAX_NAME];
    uint32_t o = 0;
    if (arg[0] == '/') {
        // 绝对路径, 去掉开头 '/' (ramfs 内部不带开头 '/')
        const char* p = arg;
        while (*p == '/') p++;
        while (*p && o < max - 1) tmp[o++] = *p++;
        tmp[o] = 0;
    } else {
        // 相对路径: cwd + '/' + arg
        const char* p = cwd_in;
        if (p[0] == '/') p++;
        while (*p && o < max - 1) tmp[o++] = *p++;
        if (o > 0) tmp[o++] = '/';
        const char* q = arg;
        while (*q && o < max - 1) tmp[o++] = *q++;
        tmp[o] = 0;
    }
    // 规范化: 处理 ".." 与 "."
    char stack[FS_MAX_NAME][64];
    int depth = 0;
    char* tok = tmp;
    while (*tok) {
        char* seg = tok;
        while (*tok && *tok != '/') tok++;
        int is_last = (*tok == 0);
        if (*tok == '/') { *tok = 0; tok++; }
        if (seg[0] == 0 || strcmp(seg, ".") == 0) {
            // 忽略
        } else if (strcmp(seg, "..") == 0) {
            if (depth > 0) depth--;
        } else {
            strncpy(stack[depth], seg, 63);
            stack[depth][63] = 0;
            depth++;
        }
        if (is_last) break;
    }
    o = 0;
    if (depth == 0) {
        out[o++] = '/';
        out[o] = 0;
        return;
    }
    for (int i = 0; i < depth; i++) {
        out[o++] = '/';
        const char* s = stack[i];
        while (*s && o < max - 1) out[o++] = *s++;
    }
    out[o] = 0;
}

static void cmd_mkdir(int argc, char** argv) {
    if (argc < 2) { vga_write("Usage: mkdir <name>\n"); return; }
    char full[FS_MAX_NAME];
    join_path(cwd, argv[1], full, sizeof(full));
    if (fs_mkdir(full) == 0) {
        vga_printf("Created directory: %s\n", full);
    } else {
        vga_printf("Error: cannot create directory %s (exists? parent missing?)\n", full);
    }
}

static void cmd_rmdir(int argc, char** argv) {
    if (argc < 2) { vga_write("Usage: rmdir <name>\n"); return; }
    char full[FS_MAX_NAME];
    join_path(cwd, argv[1], full, sizeof(full));
    if (fs_rmdir(full) == 0) {
        vga_printf("Removed directory: %s\n", full);
    } else {
        vga_printf("Error: cannot remove %s (not dir / not empty / not found)\n", full);
    }
}

static void cmd_mv(int argc, char** argv) {
    if (argc < 3) { vga_write("Usage: mv <old> <new>\n"); return; }
    char oldfull[FS_MAX_NAME], newfull[FS_MAX_NAME];
    join_path(cwd, argv[1], oldfull, sizeof(oldfull));
    join_path(cwd, argv[2], newfull, sizeof(newfull));
    if (fs_rename(oldfull, newfull) == 0) {
        vga_printf("Renamed: %s -> %s\n", oldfull, newfull);
    } else {
        vga_printf("Error: rename failed (%s -> %s)\n", oldfull, newfull);
    }
}

static void cmd_cd(int argc, char** argv) {
    if (argc < 2) {
        strncpy(cwd, "/", sizeof(cwd));
        cwd[1] = 0;
        return;
    }
    char full[FS_MAX_NAME];
    join_path(cwd, argv[1], full, sizeof(full));
    if (fs_is_dir(full)) {
        strncpy(cwd, full, sizeof(cwd) - 1);
        cwd[sizeof(cwd) - 1] = 0;
    } else {
        vga_printf("Error: not a directory: %s\n", full);
    }
}

static void cmd_pwd(int argc, char** argv) {
    vga_write(cwd);
    vga_newline();
    (void)argc; (void)argv;
}

static void cmd_hexdump(int argc, char** argv) {
    if (argc < 2) { vga_write("Usage: hexdump <name>\n"); return; }
    char full[FS_MAX_NAME];
    join_path(cwd, argv[1], full, sizeof(full));
    uint8_t buf[256];
    int n = fs_read(full, buf, sizeof(buf));
    if (n < 0) {
        vga_printf("Error: file not found: %s\n", full);
        return;
    }
    vga_printf("hexdump %s (%d bytes):\n", full, n);
    for (int i = 0; i < n; i += 16) {
        vga_printf("%04x: ", i);
        for (int j = 0; j < 16; j++) {
            if (i + j < n) {
                vga_printf("%02x ", buf[i + j]);
            } else {
                vga_write("   ");
            }
        }
        vga_write(" |");
        for (int j = 0; j < 16 && i + j < n; j++) {
            char c = (char)buf[i + j];
            vga_putchar((c >= 32 && c < 127) ? c : '.');
        }
        vga_write("|\n");
    }
}

static void cmd_sleep(int argc, char** argv) {
    if (argc < 2) { vga_write("Usage: sleep <seconds>\n"); return; }
    uint32_t sec = (uint32_t)atoi(argv[1]);
    uint32_t start = timer_get_uptime();
    while (timer_get_uptime() - start < sec) {
        __asm__ __volatile__("hlt");
    }
    vga_printf("Slept %u seconds\n", sec);
}

static void cmd_whoami(int argc, char** argv) {
    vga_write("root\n");
    (void)argc; (void)argv;
}

static void cmd_shutdown(int argc, char** argv) {
    vga_write("Shutting down...\n");
    // QEMU/bochs 标准 ACPI 关机端口
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    for (;;) { __asm__ __volatile__("hlt"); }
    (void)argc; (void)argv;
}

static void cmd_color(int argc, char** argv) {
    if (argc < 2) {
        vga_write("Usage: color <fg 0-15>\n");
        vga_write("  0 black 1 blue 2 green 3 cyan 4 red 5 magenta\n");
        vga_write("  6 brown 7 grey 8 dgrey 9 lblue 10 lgreen 11 lcyan\n");
        vga_write("  12 lred 13 lmagenta 14 yellow 15 white\n");
        return;
    }
    int c = atoi(argv[1]);
    if (c < 0 || c > 15) {
        vga_write("Color must be 0-15\n");
        return;
    }
    vga_set_color((uint8_t)c, COLOR_BLACK);
    vga_printf("Text color set to %d\n", c);
}

static void cmd_diskinfo(int argc, char** argv) {
    (void)argc; (void)argv;
    vga_write("=== ATA Disk Info ===\n");
    if (g_ata_master.present) {
        vga_printf("Master: %s, sectors=%u, size=%u MB\n",
                   g_ata_master.model, g_ata_master.sectors,
                   (unsigned)((uint64_t)g_ata_master.sectors * 512 / 1048576));
    } else {
        vga_write("Master: not present\n");
    }
    if (g_ata_slave.present) {
        vga_printf("Slave: %s, sectors=%u, size=%u MB\n",
                   g_ata_slave.model, g_ata_slave.sectors,
                   (unsigned)((uint64_t)g_ata_slave.sectors * 512 / 1048576));
    } else {
        vga_write("Slave: not present\n");
    }
}

static void cmd_diskread(int argc, char** argv) {
    if (argc < 3) {
        vga_write("Usage: diskread <lba> <count>\n");
        return;
    }
    uint32_t lba = (uint32_t)atoi(argv[1]);
    int count = atoi(argv[2]);
    if (count < 1 || count > 8) {
        vga_write("count must be 1-8\n");
        return;
    }
    if (!g_ata_master.present) {
        vga_write("No ATA disk present\n");
        return;
    }
    uint8_t buf[512 * 8];
    int ok = ata_read_sectors(0, lba, count, buf);  // 0 = master drive
    if (ok != 0) {
        vga_write("Read error\n");
        return;
    }
    vga_printf("Read %d sector(s) from LBA %u:\n", count, lba);
    for (int s = 0; s < count; s++) {
        vga_printf("--- sector %d ---\n", s);
        for (int row = 0; row < 16; row++) {
            vga_printf("%04x: ", s * 512 + row * 16);
            for (int col = 0; col < 16; col++) {
                vga_printf("%02x ", buf[s * 512 + row * 16 + col]);
            }
            vga_write("\n");
        }
    }
}

static void cmd_diskwrite(int argc, char** argv) {
    if (argc < 4) {
        vga_write("Usage: diskwrite <lba> <count> <value 0-255>\n");
        return;
    }
    uint32_t lba = (uint32_t)atoi(argv[1]);
    int count = atoi(argv[2]);
    int val = atoi(argv[3]);
    if (count < 1 || count > 8) {
        vga_write("count must be 1-8\n");
        return;
    }
    if (val < 0 || val > 255) {
        vga_write("value must be 0-255\n");
        return;
    }
    if (!g_ata_master.present) {
        vga_write("No ATA disk present\n");
        return;
    }
    uint8_t buf[512 * 8];
    for (int i = 0; i < 512 * count; i++) buf[i] = (uint8_t)val;
    int ok = ata_write_sectors(0, lba, count, buf);  // 0 = master drive
    if (ok != 0) {
        vga_write("Write error\n");
        return;
    }
    vga_printf("Wrote %d sector(s) with value %d at LBA %u\n", count, val, lba);
}

static void cmd_mouse(int argc, char** argv) {
    (void)argc; (void)argv;
    vga_printf("Mouse: x=%d y=%d buttons=%d\n",
               g_mouse.x, g_mouse.y, g_mouse.buttons);
}

static void cmd_beep(int argc, char** argv) {
    int freq = 880;
    int dur = 200;
    if (argc >= 2) freq = atoi(argv[1]);
    if (argc >= 3) dur = atoi(argv[2]);
    if (freq < 20 || freq > 5000) {
        vga_write("freq must be 20-5000 Hz\n");
        return;
    }
    vga_printf("Beep: %d Hz for %d ms\n", freq, dur);
    speaker_beep((uint32_t)freq, (uint32_t)dur);
}

static void cmd_serial(int argc, char** argv) {
    if (argc < 2) {
        vga_write("Usage: serial <text>\n");
        return;
    }
    for (int i = 1; i < argc; i++) {
        serial_write(COM1, argv[i]);
        if (i < argc - 1) serial_putc(COM1, ' ');
    }
    serial_putc(COM1, '\n');
    vga_write("Sent to COM1\n");
}

// ---- 执行 ----
static void execute(const char* line) {
    char buf[CMDLINE_MAX];
    strncpy(buf, line, CMDLINE_MAX - 1);
    buf[CMDLINE_MAX - 1] = 0;

    char* argv[16];
    int argc = parse_cmd(buf, argv, 16);
    if (argc == 0) return;

    for (struct command* c = commands; c->name; c++) {
        if (strcmp(c->name, argv[0]) == 0) {
            c->fn(argc, argv);
            return;
        }
    }
    vga_printf("Command not found: %s (try 'help')\n", argv[0]);
}

void shell_init(void) {
    cmd_len = 0;
    cmdline[0] = 0;
}

void shell_process_char(char c) {
    if (c == '\n' || c == '\r') {   // '\r' 供串口测试注入 (key 协议用 \r 回车)
        vga_putchar('\n');
        cmdline[cmd_len] = 0;
        execute(cmdline);
        cmd_len = 0;
        cmdline[0] = 0;
        print_prompt();
    } else if (c == '\b') {
        if (cmd_len > 0) {
            cmd_len--;
            vga_putchar('\b');
        }
    } else if (cmd_len < CMDLINE_MAX - 1) {
        cmdline[cmd_len++] = c;
        vga_putchar(c);
    }
}

void shell_run(void) {
    print_prompt();
    for (;;) {
        int ch = keyboard_getchar();
        if (ch < 0) {
            __asm__ __volatile__("hlt");
            continue;
        }
        shell_process_char((char)ch);
    }
}
