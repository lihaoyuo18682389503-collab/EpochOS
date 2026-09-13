// ============================================================
// EpochOS - 最小系统调用实现 (int 0x80, Linux 兼容号, 阶段3/4/5)
// 实现: 1=exit, 2=getpid, 3=read, 4=write, 5=open(含O_CREAT), 6=close,
//       11=execve, 19=lseek, 20=writev, 45=brk, 54=ioctl(TCGETS), 141=sysinfo
// 说明: fd 0/1/2 = 控制台 (stdin 无输入源, read 返回 0)
//       fd >= 3 = ramfs 文件描述符 (open 时把文件载入内核缓冲)
//       write 到文件 = 追加 (ramfs 全量覆写, 单文件 <= 4KB)
//       brk 只支持增长, 上限为 ELF 用户栈底 0x40000000
//       execve: 同进程 pid/cr3 替换 ELF 段/栈 (Linux execve 语义)
// ============================================================
#include "syscall.h"
#include "task.h"
#include "serial.h"
#include "vga.h"
#include "fs.h"
#include "string.h"
#include "mm.h"
#include "paging.h"
#include "elf.h"
#include "timer.h"

#define FD_TABLE_MAX  8
#define ELF_USER_STACK_BASE 0x40000000   // 与 task.c ELF_USER_STACK_ADDR 一致

// Linux open flags (最小子集)
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0x40
#define O_TRUNC     0x200

// Linux ioctl request (TCGETS 最小实现)
#define TCGETS_REQ  0x5401

struct fd_entry {
    uint8_t  used;
    char     name[FS_MAX_NAME];
    uint8_t  buf[FS_MAX_SIZE];   // open 时整文件载入 (ramfs 单文件 <= 4KB)
    uint32_t size;
    uint32_t pos;
};

static struct fd_entry g_fds[FD_TABLE_MAX];

// 内核栈无初值保障, 启动时清零 fd 表
void syscall_init(void) {
    memset(g_fds, 0, sizeof(g_fds));
}

static void console_out_char(char c) {
    if (c == '\n') {
        vga_putchar('\r');
        serial_putc(COM1, '\r');
    }
    vga_putchar(c);
    serial_putc(COM1, c);
}

// ---- 1. exit ----
static void do_exit(int code) {
    struct task* cur = task_get_current();
    if (!cur) return;
    cur->state = TASK_EXITED;
    cur->exit_code = code;
    serial_printf(COM1, "[syscall] pid=%u exit(%d) ring3-ok\n", cur->pid, code);
}

// ---- 4. write ----
static int do_write(int fd, const char* buf, int count) {
    if (count < 0) return -1;
    if (fd == 0) return 0;                       // stdin 不可写
    if (fd <= 2) {                               // stdout/stderr -> 控制台
        if (count == 0) return 0;
        if (!paging_user_access_ok((uint32_t)buf, (uint32_t)count)) return -1;  // EFAULT
        int n = 0;
        for (int i = 0; i < count; i++) {
            if (buf[i] == 0) break;              // 防御: 遇 NUL 停止
            console_out_char(buf[i]);
            n++;
        }
        return n;
    }
    if (fd >= FD_TABLE_MAX || !g_fds[fd].used) return -1;
    // 写入已打开文件: 追加到 ramfs 文件 (先落 fd 缓冲再写回文件)
    struct fd_entry* e = &g_fds[fd];
    int can = FS_MAX_SIZE - (int)e->size;
    if (can > count) can = count;
    if (can > 0) {
        if (!paging_user_access_ok((uint32_t)buf, (uint32_t)can)) return -1;    // EFAULT
        memcpy(e->buf + e->size, buf, (uint32_t)can);
        e->size += (uint32_t)can;
        fs_write(e->name, e->buf, e->size);      // ramfs 全量覆写
    }
    return can;
}

// ---- 3. read ----
static int do_read(int fd, char* buf, int count) {
    if (count < 0) return -1;
    if (fd == 0) return 0;                       // stdin 无输入源
    if (fd >= FD_TABLE_MAX || !g_fds[fd].used) return -1;
    struct fd_entry* e = &g_fds[fd];
    int avail = (int)e->size - (int)e->pos;
    if (avail > count) avail = count;
    if (avail > 0) {
        if (!paging_user_access_ok((uint32_t)buf, (uint32_t)avail)) return -1;  // EFAULT
        memcpy(buf, e->buf + e->pos, (uint32_t)avail);
        e->pos += (uint32_t)avail;
    }
    return avail;
}

// 前向声明: 从用户空间安全拷贝字符串 (定义见下方 copy_user_str)
static int copy_user_str(const char* us, char* kbuf, uint32_t max);

// ---- 5. open (支持 O_CREAT: 文件不存在时创建空文件) ----
static int do_open(const char* path, int flags) {
    char kpath[FS_MAX_NAME];
    if (copy_user_str(path, kpath, FS_MAX_NAME) < 0) return -1;  // 含用户指针校验
    if (kpath[0] == 0) return -1;
    if (fs_exists(kpath)) {
        if (fs_is_dir(kpath)) return -1;
    } else {
        if (!(flags & O_CREAT)) return -1;      // 不存在且未要求创建 -> 失败
        if (fs_create(kpath) != 0) return -1;   // 创建空文件
        fs_mark_dirty();
    }
    int fd = -1;
    for (int i = 3; i < FD_TABLE_MAX; i++) {
        if (!g_fds[i].used) { fd = i; break; }
    }
    if (fd < 0) return -1;
    struct fd_entry* e = &g_fds[fd];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, kpath, FS_MAX_NAME - 1);
    e->name[FS_MAX_NAME - 1] = 0;
    e->size = fs_read(kpath, e->buf, sizeof(e->buf));
    if ((int)e->size < 0) {
        e->used = 0;
        return -1;
    }
    if (flags & O_TRUNC) {                      // O_TRUNC: 清空已有内容
        e->size = 0;
        fs_write(e->name, e->buf, 0);
        fs_mark_dirty();
    }
    e->pos = 0;
    e->used = 1;
    return fd;
}

// ---- 6. close ----
static int do_close(int fd) {
    if (fd <= 2 || fd >= FD_TABLE_MAX || !g_fds[fd].used) return -1;
    memset(&g_fds[fd], 0, sizeof(g_fds[fd]));
    return 0;
}

// ---- 19. lseek (Linux: whence 0=SET 1=CUR 2=END) ----
static int do_lseek(int fd, int offset, int whence) {
    if (fd <= 2 || fd >= FD_TABLE_MAX || !g_fds[fd].used) return -1;
    struct fd_entry* e = &g_fds[fd];
    int base = 0;
    if (whence == 1) base = (int)e->pos;
    else if (whence == 2) base = (int)e->size;
    else if (whence != 0) return -1;
    int np = base + offset;
    if (np < 0) return -1;
    if ((uint32_t)np > e->size) return -1;    // 超文件尾 (不支持 extend 写)
    e->pos = (uint32_t)np;
    return np;
}

// ---- 45. brk ----
static int do_brk(uint32_t addr) {
    struct task* cur = task_get_current();
    if (!cur) return -1;
    if (addr == 0) return (int)cur->user_brk;    // 查询当前 break

    if (addr < cur->user_brk) return (int)cur->user_brk;   // 暂不支持收缩
    if (addr > ELF_USER_STACK_BASE - PAGE_SIZE) {
        serial_printf(COM1, "[syscall] pid=%u brk(0x%x) beyond stack base\n",
                      cur->pid, addr);
        return (int)cur->user_brk;
    }

    // 从当前 break 页对齐位置向上逐页映射 USER 页
    uint32_t va = cur->user_brk;
    uint32_t need_end = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t pd = cur->cr3;
    while (va < need_end) {
        page_entry_t* pde = &((page_entry_t*)pd)[(va >> 22) & 0x3FF];
        // 防御: 该页已映射则跳过 (正常情况下 brk 起点从未映射, 仅防 loader/bss 尾页重叠)
        int mapped = 0;
        if ((*pde & PAGE_PRESENT)) {
            page_entry_t* pt = (page_entry_t*)(uint32_t)(*pde & ~0xFFFu);
            if ((pt[(va >> 12) & 0x3FF] & PAGE_PRESENT)) mapped = 1;
        }
        if (!mapped) {
            uint32_t phys = alloc_page();
            if (!phys) {
                serial_write_str("[syscall] brk alloc_page failed\n");
                return (int)cur->user_brk;
            }
            if (paging_map_page_in((page_entry_t*)pd, va, phys,
                                   PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER)) {
                free_page(phys);
                serial_write_str("[syscall] brk map failed\n");
                return (int)cur->user_brk;
            }
            // 跟踪表满则撤销本次映射并回收页, 否则该页在进程退出时无法回收
            if (process_track_page(cur, phys) != 0) {
                page_entry_t* pde2 = &((page_entry_t*)pd)[(va >> 22) & 0x3FF];
                if (*pde2 & PAGE_PRESENT) {
                    page_entry_t* pt2 = (page_entry_t*)(uint32_t)(*pde2 & ~0xFFFu);
                    pt2[(va >> 12) & 0x3FF] = 0;
                    __asm__ __volatile__("invlpg (%0)" : : "r"(va) : "memory");
                }
                free_page(phys);
                serial_write_str("[syscall] brk page tracking full\n");
                return (int)cur->user_brk;
            }
        }
        va += PAGE_SIZE;
    }
    cur->user_brk = addr;
    return (int)cur->user_brk;
}

// ---- 2. getpid ----
static int do_getpid(void) {
    struct task* cur = task_get_current();
    return cur ? (int)cur->pid : -1;
}

// 从用户空间安全拷贝字符串 (中断/系统调用上下文: 当前 cr3 仍映射用户页)
static int copy_user_str(const char* us, char* kbuf, uint32_t max) {
    if (!us || !kbuf || max == 0) return -1;
    /* 逐字节安全拷贝: 仅跨页时校验下一页可访问。整段预校验 max 字节
       会在 argv 字符串贴近用户栈页尾时误判 EFAULT (常见于多参数启动) */
    uint32_t i = 0;
    while (i + 1 < max) {
        uint32_t va = (uint32_t)us + i;
        if ((va & 0xFFFu) == 0 && !paging_user_access_ok(va, 1)) return -1;
        char c = *(const volatile char*)(us + i);
        kbuf[i] = c;
        if (c == 0) return (int)i;
        i++;
    }
    kbuf[i] = 0;
    return (int)i;
}

// ---- 20. writev (Linux i386: writev(fd, iov, iovcnt)) ----
// 用户侧 iovec 结构: { uint32_t base; uint32_t len; } 与 Linux i386 布局一致
static int do_writev(int fd, const void* iov_user, int iovcnt) {
    if (!iov_user || iovcnt < 0) return -1;
    if (iovcnt > 16) iovcnt = 16;
    const uint8_t* p = (const uint8_t*)iov_user;
    int total = 0;
    for (int i = 0; i < iovcnt; i++) {
        uint32_t slot = (uint32_t)p + (uint32_t)i * 8;
        if (!paging_user_access_ok(slot, 8)) return -1;   // iovec 结构本身须在用户空间
        uint32_t base = *(const uint32_t*)(p + (uint32_t)i * 8);
        uint32_t len  = *(const uint32_t*)(p + (uint32_t)i * 8 + 4);
        if (len == 0) continue;
        if (len > 4096) len = 4096;
        int n = do_write(fd, (const char*)(uint32_t)base, (int)len);  // base 在 do_write 内校验
        if (n < 0) {
            if (total > 0) return total;
            return -1;
        }
        total += n;
        if (n < (int)len) break;               // 控制台遇 NUL / 文件写满
    }
    return total;
}

// ---- 54. ioctl (仅 TCGETS: 控制台视为 tty) ----
static int do_ioctl(int fd, int request, void* argp) {
    (void)argp;
    if (fd < 0 || fd > 2) return -1;           // 仅标准流
    if (request == TCGETS_REQ) return 0;       // isatty 语义: 是终端
    return -1;
}

// ---- 141. sysinfo (EpochOS 子集: 64 字节, 与 epoch.h 用户结构一致) ----
// 布局: uptime(秒) | totalram | freeram | procs | mem_unit | pad[11]
static int do_sysinfo(void* info_user) {
    if (!info_user) return -1;
    if (!paging_user_access_ok((uint32_t)info_user, 16 * sizeof(uint32_t))) return -1;  // EFAULT
    uint32_t* p = (uint32_t*)info_user;
    uint32_t total = mm_total_pages() * PAGE_SIZE;
    uint32_t free  = mm_free_pages() * PAGE_SIZE;
    p[0] = timer_get_uptime();                 // uptime 秒
    p[1] = total;
    p[2] = free;
    p[3] = (uint32_t)task_get_count();         // procs (含内核主线程)
    p[4] = 1;                                  // mem_unit = 1 字节
    for (int i = 5; i < 16; i++) p[i] = 0;
    return 0;
}

// ============================================================
// 阶段6: 拓宽的 POSIX 系统调用 (兼容经适配工具链编译的 Linux 程序)
// ============================================================

// 内核侧 stat 结构 (与 user/epoch.h struct stat 严格一致: 13 个 uint32)
struct epoch_stat {
    uint32_t st_dev, st_ino, st_mode, st_nlink, st_uid, st_gid, st_rdev,
             st_size, st_blksize, st_blocks, st_atime, st_mtime, st_ctime;
};

// 在进程自己的页目录 (pd) 中取消单页映射, 返回被释放的物理页 (0=未映射)
// 注意: 不能调 paging_unmap_page (它操作内核页目录), 必须直接走进程 PD。
static uint32_t proc_unmap_one(page_entry_t* pd, uint32_t va) {
    uint32_t pd_idx = (va >> 22) & 0x3FF;
    if (!(pd[pd_idx] & PAGE_PRESENT)) return 0;
    page_entry_t* pt = (page_entry_t*)(pd[pd_idx] & 0xFFFFF000);
    uint32_t e = pt[(va >> 12) & 0x3FF];
    uint32_t phys = e & 0xFFFFF000;
    pt[(va >> 12) & 0x3FF] = 0;
    __asm__ __volatile__("invlpg (%0)" : : "r"(va) : "memory");
    return phys;
}

// ---- 90. mmap (i386 6 参: ebx=addr ecx=len edx=prot esi=flags edi=fd ebp=offset) ----
// 支持匿名映射 (MAP_ANONYMOUS) 与只读私有文件映射 (fd>=3, ramfs 文件, <=4KB)。
// 地址取自进程 mmap_base 向下生长; 严格限制在 [MEMORY_END, 栈基) 用户区间,
// 且不得落在内核高映射 [KERNEL_VIRT_BASE, ...) 或覆盖 brk 堆。
static void* do_mmap(void* addr, uint32_t length, int prot, int flags,
                     int fd, uint32_t offset) {
    (void)prot;
    struct task* cur = task_get_current();
    if (!cur || cur->ring != TASK_RING3) return (void*)-1;
    if (length == 0) return (void*)-1;

    uint32_t npages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    if (npages == 0) return (void*)-1;
    if (npages > (TASK_MAX_TRACKED_PAGES - cur->elf_page_count))
        return (void*)-1;       // 余下跟踪槽不足 -> 拒绝, 避免孤儿页泄漏

    int fixed = (flags & 0x10) != 0;   // MAP_FIXED = 0x10
    uint32_t base;
    if (addr && fixed) {
        base = (uint32_t)addr & ~(PAGE_SIZE - 1);
    } else {
        base = (cur->mmap_base - npages * PAGE_SIZE) & ~(PAGE_SIZE - 1);
    }
    uint32_t top = base + npages * PAGE_SIZE;
    if (base < MEMORY_END || base >= KERNEL_VIRT_BASE) return (void*)-1;
    if (top > ELF_USER_STACK_BASE - PAGE_SIZE) return (void*)-1;   // 不侵占用户栈
    if (base < cur->user_brk) return (void*)-1;                    // 不与 brk 堆重叠

    // 只读私有文件映射: 取 ramfs 文件内容
    const uint8_t* filedata = 0;
    uint32_t filesize = 0;
    if (!(flags & 0x20) && fd >= 0 && fd < FD_TABLE_MAX && g_fds[fd].used) {
        filedata = g_fds[fd].buf;
        filesize = g_fds[fd].size;
    }

    page_entry_t* pd = (page_entry_t*)(cur->cr3 & 0xFFFFF000);
    uint32_t i = 0;
    for (; i < npages; i++) {
        uint32_t va = base + i * PAGE_SIZE;
        uint32_t phys = alloc_page();
        if (!phys) goto rollback;
        if (paging_map_page_in(pd, va, phys,
                               PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER)) {
            free_page(phys);
            goto rollback;
        }
        if (process_track_page(cur, phys) != 0) {
            proc_unmap_one(pd, va);
            free_page(phys);
            goto rollback;
        }
        memset((void*)phys, 0, PAGE_SIZE);          // 匿名页必须清零
        if (filedata) {                              // 文件映射: 拷贝内容 (从 offset 起)
            uint32_t foff = offset + i * PAGE_SIZE;
            if (foff < filesize) {
                uint32_t cpy = filesize - foff;
                if (cpy > PAGE_SIZE) cpy = PAGE_SIZE;
                memcpy((void*)phys, filedata + foff, cpy);
            }
        }
    }
    cur->mmap_base = base;          // 区底下移, 下次 mmap 继续向下
    return (void*)base;

rollback:
    for (uint32_t j = 0; j < i; j++) {
        uint32_t p = proc_unmap_one(pd, base + j * PAGE_SIZE);
        if (p) { free_page(p); process_untrack_page(cur, p); }
    }
    return (void*)-1;
}

// ---- 91. munmap ----
static int do_munmap(void* addr, uint32_t length) {
    struct task* cur = task_get_current();
    if (!cur || !addr || length == 0) return -1;
    uint32_t start = (uint32_t)addr & ~(PAGE_SIZE - 1);
    uint32_t end   = (uint32_t)addr + length;
    if (end < start) return -1;
    end = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (start < MEMORY_END || end > ELF_USER_STACK_BASE) return -1;
    page_entry_t* pd = (page_entry_t*)(cur->cr3 & 0xFFFFF000);
    for (uint32_t va = start; va < end; va += PAGE_SIZE) {
        uint32_t p = proc_unmap_one(pd, va);
        if (p) { free_page(p); process_untrack_page(cur, p); }
    }
    return 0;
}

// ---- 78. gettimeofday ----
static int do_gettimeofday(void* tv, void* tz) {
    (void)tz;
    if (!tv) return 0;
    if (!paging_user_access_ok((uint32_t)tv, 8)) return -1;
    uint32_t* p = (uint32_t*)tv;
    uint32_t ticks = timer_get_ticks();
    p[0] = timer_get_uptime();                  // 秒
    p[1] = (ticks % 100) * 10000;               // 微秒 (100Hz -> 每 tick 10ms)
    return 0;
}

// ---- 162. nanosleep (自旋等待; 抢占式调度下其他任务仍运行) ----
static int do_nanosleep(const void* req, void* rem) {
    (void)rem;
    if (!req) return -1;
    if (!paging_user_access_ok((uint32_t)req, 8)) return -1;
    uint32_t sec  = ((const uint32_t*)req)[0];
    uint32_t nsec = ((const uint32_t*)req)[1];
    uint32_t start = timer_get_ticks();
    uint32_t want = sec * 100 + (nsec / 10000000u);   // 目标 tick 数 (100Hz)
    if (want == 0 && (sec || nsec)) want = 1;
    // int 0x80 是中断门 (进入时 IF=0); 若不开中断, PIT 不触发, 自旋将永远等不到 tick
    __asm__ __volatile__("sti");
    while (timer_get_ticks() - start < want) { __asm__ __volatile__("pause"); }
    __asm__ __volatile__("cli");
    return 0;
}

// ---- 252. exit_group (单线程模型下等同 exit) ----
static void do_exit_group(int code) {
    do_exit(code);
}

// ---- 108. fstat ----
static int do_fstat(int fd, struct epoch_stat* st) {
    if (!st) return -1;
    if (!paging_user_access_ok((uint32_t)st, sizeof(struct epoch_stat))) return -1;
    memset(st, 0, sizeof(*st));
    if (fd >= 0 && fd <= 2) {
        st->st_mode = 0x2000;            // S_IFCHR
        st->st_ino  = (uint32_t)fd + 1;
        return 0;
    }
    if (fd < 0 || fd >= FD_TABLE_MAX || !g_fds[fd].used) return -1;
    st->st_mode = 0x8000;                // S_IFREG
    st->st_size = g_fds[fd].size;
    st->st_ino  = (uint32_t)fd + 1;
    return 0;
}

// ---- 106. stat ----
static int do_stat(const char* path, struct epoch_stat* st) {
    char kpath[FS_MAX_NAME];
    if (copy_user_str(path, kpath, FS_MAX_NAME) < 0) return -1;
    if (!st || !paging_user_access_ok((uint32_t)st, sizeof(struct epoch_stat))) return -1;
    memset(st, 0, sizeof(*st));
    if (!fs_exists(kpath)) return -1;
    if (fs_is_dir(kpath)) {
        st->st_mode = 0x4000;            // S_IFDIR
        st->st_ino  = 1;
        return 0;
    }
    st->st_mode = 0x8000;                 // S_IFREG
    st->st_size = fs_get_size(kpath);
    st->st_ino  = 1;
    return 0;
}

// ---- 11. execve (path, argv, envp) ----
// 语义: 读取新 ELF -> 在当前任务上重载 (pid 不变), 成功后改写中断帧跳转新入口
// argv/envp 为 ring3 用户指针 (当前 cr3 映射, 中断上下文可直接读取)
// 返回值: 1=已接管帧(不再返回用户旧程序), 0=失败(继续原进程)
static int do_execve(struct regs* r, const char* path_user,
                     char** argv_user, char** envp_user) {
    struct task* cur = task_get_current();
    if (!cur || cur->ring != TASK_RING3) return 0;
    (void)envp_user;

    // 1. 拷贝 path 到内核缓冲
    char path[FS_MAX_NAME];
    if (copy_user_str(path_user, path, FS_MAX_NAME) < 0) return 0;

    // 2. 读取 ELF 到内核缓冲并解析
    uint8_t buf[FS_MAX_SIZE];
    int n = fs_read(path, buf, sizeof(buf));
    if (n < 0) {
        serial_printf(COM1, "[syscall] execve: fs_read failed %s\n", path);
        return 0;
    }
    struct elf32_ehdr eh;
    struct elf32_phdr ph[ELF_MAX_PHDR];
    int phnum = elf_parse(buf, (uint32_t)n, &eh, ph, ELF_MAX_PHDR);
    if (phnum < 0) {
        serial_printf(COM1, "[syscall] execve: bad ELF %s\n", path);
        return 0;
    }

    // 3. 拷贝 argv 字符串到内核 (argv 用户指针数组; 上限 16 个, 每串 63 字符)
    char kargv[16][64];
    int argc = 0;
    if (argv_user) {
        for (; argc < 15; argc++) {
            uint32_t slot = (uint32_t)argv_user + (uint32_t)argc * 4;
            if (!paging_user_access_ok(slot, 4)) break;        // 数组槽本身须在用户空间
            const char* s = *(const char* const*)slot;
            if (!s) break;
            if (copy_user_str(s, kargv[argc], 64) < 0) break;  // 字符串指针也须合法
        }
    }
    if (argc < 1) {                            // 无 argv[0] -> 用 path 基名
        const char* slash = 0;
        for (const char* q = path; *q; q++) if (*q == '/') slash = q;
        strncpy(kargv[0], slash ? slash + 1 : path, 63);
        kargv[0][63] = 0;
        argc = 1;
    }
    const char* argvp[16];
    for (int i = 0; i < argc; i++) argvp[i] = kargv[i];

    // 4. 重载当前任务 (成功即切换地址空间, 不返回)
    uint32_t new_esp = 0;
    int rc = process_exec_reload(cur, buf, (uint32_t)n, &eh, ph, phnum,
                                 argc, argvp, &new_esp);
    if (rc != 0) {
        serial_printf(COM1, "[syscall] execve reload failed: %s\n", path);
        return 0;
    }
    // 更新任务名 (argv[0] 基名)
    const char* slash = 0;
    for (const char* q = kargv[0]; *q; q++) if (*q == '/') slash = q;
    strncpy(cur->name, slash ? slash + 1 : kargv[0], TASK_NAME_MAX - 1);
    cur->name[TASK_NAME_MAX - 1] = 0;
    serial_printf(COM1, "[syscall] execve pid=%u -> '%s' entry=0x%x esp=0x%x\r\n",
                  cur->pid, path, eh.e_entry, new_esp);
    // 改写中断帧: iret 返回新程序 (cs/ss 已是 ring3 选择子, 保持不变)
    r->eip = eh.e_entry;
    r->user_esp = new_esp;
    r->eax = 0;
    return 1;
}

// int 0x80 统一入口: eax=syscall 号, ebx/ecx/edx=参数
// (eax<0 的返回约定: Linux 负错误码, EpochOS 用 -1 表示失败)
void syscall_handler(struct regs* r) {
    uint32_t nr = r->eax;
    int ret = -1;
    switch (nr) {
    case SYS_EXIT:   // exit(int status)
        do_exit((int)r->ebx);
        // 不再返回用户主流程: eip 指向用户代码尾部 jmp $ 循环
        if (task_get_current() && task_get_current()->user_exit_ip) {
            r->eip = task_get_current()->user_exit_ip;
        }
        return;
    case SYS_WRITE:  // write(fd, buf, count)
        ret = do_write((int)r->ebx, (const char*)r->ecx, (int)r->edx);
        break;
    case SYS_READ:   // read(fd, buf, count)
        ret = do_read((int)r->ebx, (char*)r->ecx, (int)r->edx);
        break;
    case SYS_OPEN:   // open(path, flags)
        ret = do_open((const char*)r->ebx, (int)r->ecx);
        break;
    case SYS_CLOSE:  // close(fd)
        ret = do_close((int)r->ebx);
        break;
    case SYS_LSEEK:  // lseek(fd, offset, whence)
        ret = do_lseek((int)r->ebx, (int)r->ecx, (int)r->edx);
        break;
    case SYS_BRK:    // brk(addr)
        ret = do_brk(r->ebx);
        break;
    case SYS_GETPID: // getpid()
        ret = do_getpid();
        break;
    case SYS_WRITEV: // writev(fd, iov, iovcnt)
        ret = do_writev((int)r->ebx, (const void*)r->ecx, (int)r->edx);
        break;
    case SYS_IOCTL:  // ioctl(fd, request, argp)
        ret = do_ioctl((int)r->ebx, (int)r->ecx, (void*)r->edx);
        break;
    case SYS_SYSINFO: // sysinfo(info*)
        ret = do_sysinfo((void*)r->ebx);
        break;
    case SYS_MMAP:    // mmap(addr,len,prot,flags,fd,offset) 6 参
        ret = (int)do_mmap((void*)r->ebx, r->ecx, (int)r->edx,
                           (int)r->esi, (int)r->edi, r->ebp);
        break;
    case SYS_MUNMAP:  // munmap(addr, len)
        ret = do_munmap((void*)r->ebx, r->ecx);
        break;
    case SYS_GETTIMEOFDAY: // gettimeofday(tv, tz)
        ret = do_gettimeofday((void*)r->ebx, (void*)r->ecx);
        break;
    case SYS_NANOSLEEP:    // nanosleep(req, rem)
        ret = do_nanosleep((void*)r->ebx, (void*)r->ecx);
        break;
    case SYS_STAT:         // stat(path, st)
        ret = do_stat((const char*)r->ebx, (void*)r->ecx);
        break;
    case SYS_FSTAT:        // fstat(fd, st)
        ret = do_fstat((int)r->ebx, (void*)r->ecx);
        break;
    case SYS_EXIT_GROUP:   // exit_group(code)
        do_exit_group((int)r->ebx);
        if (task_get_current() && task_get_current()->user_exit_ip)
            r->eip = task_get_current()->user_exit_ip;
        return;
    case SYS_EXECVE: // execve(path, argv, envp)
        if (do_execve(r, (const char*)r->ebx, (char**)r->ecx, (char**)r->edx) == 1) {
            return;                            // 已接管帧跳新程序, 不再回旧程序
        }
        ret = -1;
        break;
    default:
        serial_printf(COM1, "[syscall] pid=%u unknown nr=%u\n",
                      task_get_current_pid(), nr);
        return;
    }
    r->eax = (uint32_t)ret;
}
