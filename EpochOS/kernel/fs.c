// ============================================================
// EpochOS - 内存文件系统 (ramfs) 实现
// 纯内存文件表, 支持目录/创建/覆写/追加/读取/删除/重命名/列出
// 目录条目: is_dir=1, 不占数据区; 文件名保存完整路径 (如 "home/a.txt")
// ============================================================
#include "fs.h"
#include "string.h"
#include "ata.h"
#include "serial.h"

static struct fs_file files[FS_MAX_FILES];

static void fs_normalize(const char* in, char* out);

void fs_init(void) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        files[i].used = 0;
        files[i].size = 0;
        files[i].is_dir = 0;
        files[i].name[0] = 0;
    }
}

static struct fs_file* fs_find(const char* name) {
    char norm[FS_MAX_NAME];
    if (!name || name[0] == 0) return 0;
    fs_normalize(name, norm);
    if (norm[0] == 0) return 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && strcmp(files[i].name, norm) == 0) {
            return &files[i];
        }
    }
    return 0;
}

// 去掉路径中多余的 '/' 与开头 './'
static void fs_normalize(const char* in, char* out) {
    int o = 0;
    int i = 0;
    while (in[i] == '/') i++;          // 去掉开头 '/'
    while (in[i]) {
        if (in[i] == '/' && (in[i+1] == '/' || in[i+1] == 0)) {
            i++;
            continue;
        }
        if (in[i] == '.' && in[i+1] == '/' && (i == 0 || in[i-1] == '/')) {
            i += 2;
            continue;
        }
        out[o++] = in[i++];
    }
    out[o] = 0;
}

// 获取路径的父目录 ("home/a.txt" -> "home"; "a.txt" -> "")
static void fs_parent(const char* name, char* out) {
    int i = 0;
    int last = -1;
    while (name[i]) {
        if (name[i] == '/') last = i;
        i++;
    }
    if (last <= 0) {
        out[0] = 0;
    } else {
        int j;
        for (j = 0; j < last; j++) out[j] = name[j];
        out[j] = 0;
    }
}

// 获取路径最后一段 ("home/a.txt" -> "a.txt")
static const char* fs_basename(const char* name) {
    const char* p = name;
    const char* last = name;
    while (*p) {
        if (*p == '/') last = p + 1;
        p++;
    }
    return last;
}

int fs_create(const char* name) {
    char norm[FS_MAX_NAME];
    if (!name || name[0] == 0) return -1;
    fs_normalize(name, norm);
    if (norm[0] == 0) return -1;
    if (fs_find(norm)) return 0;               // 已存在
    if (strlen(norm) >= FS_MAX_NAME) return -1;
    // 父目录必须存在
    char parent[FS_MAX_NAME];
    fs_parent(norm, parent);
    if (parent[0] != 0 && !fs_find(parent)) return -1;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) {
            strncpy(files[i].name, norm, FS_MAX_NAME - 1);
            files[i].name[FS_MAX_NAME - 1] = 0;
            files[i].size = 0;
            files[i].is_dir = 0;
            files[i].used = 1;
            fs_mark_dirty();
            return 0;
        }
    }
    return -1;
}

int fs_mkdir(const char* name) {
    char norm[FS_MAX_NAME];
    if (!name || name[0] == 0) return -1;
    fs_normalize(name, norm);
    if (norm[0] == 0) return -1;
    if (fs_find(norm)) return -1;              // 目录已存在
    if (strlen(norm) >= FS_MAX_NAME) return -1;
    char parent[FS_MAX_NAME];
    fs_parent(norm, parent);
    if (parent[0] != 0 && !fs_find(parent)) return -1;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) {
            strncpy(files[i].name, norm, FS_MAX_NAME - 1);
            files[i].name[FS_MAX_NAME - 1] = 0;
            files[i].size = 0;
            files[i].is_dir = 1;
            files[i].used = 1;
            fs_mark_dirty();
            return 0;
        }
    }
    return -1;
}

// 判断 name 是否位于 dir 目录下 (dir="home" 时, "home/a.txt" 为其子条目)
static int fs_under_dir(const char* name, const char* dir) {
    if (dir[0] == 0) return 1;                 // 根目录包含所有
    uint32_t dlen = strlen(dir);
    if (strncmp(name, dir, dlen) != 0) return 0;
    return name[dlen] == '/';
}

int fs_write(const char* name, const uint8_t* data, uint32_t size) {
    struct fs_file* f = fs_find(name);
    if (!f) {
        if (fs_create(name) != 0) return -1;
        f = fs_find(name);
    }
    if (f->is_dir) return -1;
    if (size > FS_MAX_SIZE) size = FS_MAX_SIZE;
    if (size > 0 && data) {
        memcpy(f->data, data, size);
    }
    f->size = size;
    fs_mark_dirty();
    return 0;
}

int fs_append(const char* name, const uint8_t* data, uint32_t size) {
    struct fs_file* f = fs_find(name);
    if (!f) {
        if (fs_create(name) != 0) return -1;
        f = fs_find(name);
    }
    if (f->is_dir) return -1;
    if (f->size + size > FS_MAX_SIZE) {
        size = FS_MAX_SIZE - f->size;
    }
    if (size > 0 && data) {
        memcpy(f->data + f->size, data, size);
    }
    f->size += size;
    fs_mark_dirty();
    return 0;
}

int fs_read(const char* name, uint8_t* out, uint32_t max) {
    struct fs_file* f = fs_find(name);
    if (!f) return -1;
    if (f->is_dir) return -1;
    uint32_t n = (f->size < max) ? f->size : max;
    if (n > 0) {
        memcpy(out, f->data, n);
    }
    return (int)n;
}

int fs_delete(const char* name) {
    struct fs_file* f = fs_find(name);
    if (!f) return -1;
    if (f->is_dir) return -1;                  // 目录用 fs_rmdir
    f->used = 0;
    f->size = 0;
    f->is_dir = 0;
    f->name[0] = 0;
    fs_mark_dirty();
    return 0;
}

int fs_rmdir(const char* name) {
    struct fs_file* f = fs_find(name);
    if (!f) return -1;
    if (!f->is_dir) return -1;
    // 检查目录是否为空: 是否存在其下子条目
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && fs_under_dir(files[i].name, name)) {
            return -1;                         // 非空
        }
    }
    f->used = 0;
    f->size = 0;
    f->is_dir = 0;
    f->name[0] = 0;
    fs_mark_dirty();
    return 0;
}

void fs_format(void) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        files[i].used = 0;
        files[i].is_dir = 0;
        files[i].size = 0;
        files[i].name[0] = 0;
    }
    fs_mark_dirty();
}

int fs_rename(const char* oldname, const char* newname) {
    struct fs_file* f = fs_find(oldname);
    char norm[FS_MAX_NAME];
    if (!f) return -1;
    fs_normalize(newname, norm);
    if (norm[0] == 0) return -1;
    if (strlen(norm) >= FS_MAX_NAME) return -1;
    if (fs_find(norm)) return -1;              // 目标已存在
    char parent[FS_MAX_NAME];
    fs_parent(norm, parent);
    if (parent[0] != 0 && !fs_find(parent)) return -1;
    // 目录重命名时, 其下子条目路径也要更新
    strncpy(f->name, norm, FS_MAX_NAME - 1);
    f->name[FS_MAX_NAME - 1] = 0;
    if (f->is_dir) {
        uint32_t olen = strlen(oldname);
        for (int i = 0; i < FS_MAX_FILES; i++) {
            if (files[i].used && fs_under_dir(files[i].name, oldname)) {
                char newpath[FS_MAX_NAME];
                int j = 0;
                while (norm[j]) { newpath[j] = norm[j]; j++; }
                const char* rest = files[i].name + olen;   // "/xxx"
                while (*rest && j < FS_MAX_NAME - 1) { newpath[j++] = *rest++; }
                newpath[j] = 0;
                strncpy(files[i].name, newpath, FS_MAX_NAME - 1);
                files[i].name[FS_MAX_NAME - 1] = 0;
            }
        }
    }
    fs_mark_dirty();
    return 0;
}

int fs_exists(const char* name) {
    return fs_find(name) ? 1 : 0;
}

int fs_is_dir(const char* name) {
    struct fs_file* f = fs_find(name);
    return f ? (f->is_dir ? 1 : 0) : 0;
}

// 检查 dir 下是否存在同名的子项
static int fs_dir_has_entry(const char* dir, const char* base) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) continue;
        if (!fs_under_dir(files[i].name, dir)) continue;
        if (strcmp(fs_basename(files[i].name), base) == 0) return 1;
    }
    return 0;
}

int fs_list(char* out, uint32_t max) {
    uint32_t used = 0;
    int count = 0;
    out[0] = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used) {
            count++;
            char line[64];
            int j = 0;
            while (files[i].name[j] && j < FS_MAX_NAME) {
                line[j] = files[i].name[j];
                j++;
            }
            if (files[i].is_dir) { line[j] = '/'; j++; }
            line[j] = ' ';
            j++;
            char num[16];
            uitoa(files[i].size, num, 10);
            int k = 0;
            while (num[k]) {
                line[j++] = num[k++];
            }
            line[j++] = '\n';
            line[j] = 0;
            uint32_t len = (uint32_t)j;
            if (used + len < max) {
                for (uint32_t m = 0; m < len; m++) {
                    out[used + m] = line[m];
                }
                used += len;
            }
        }
    }
    out[used] = 0;
    return count;
}

int fs_list_dir(const char* dir, char* out, uint32_t max) {
    uint32_t used = 0;
    int count = 0;
    out[0] = 0;
    char d[FS_MAX_NAME];
    fs_normalize(dir, d);
    uint32_t dlen = strlen(d);
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) continue;
        // 条目必须位于 dir 下
        const char* rest = files[i].name;
        if (dlen > 0) {
            if (strncmp(files[i].name, d, dlen) != 0) continue;
            rest = files[i].name + dlen;
            if (rest[0] == '/') rest++;
        }
        if (rest[0] == 0) continue;            // 就是 dir 自身, 跳过
        if (strchr(rest, '/')) continue;       // 更深层级, 跳过
        // 检查兄弟重复: 同名 basename 只列一次
        int dup = 0;
        for (int j = 0; j < i; j++) {
            if (!files[j].used) continue;
            const char* r2 = files[j].name;
            if (dlen > 0) {
                if (strncmp(files[j].name, d, dlen) != 0) continue;
                r2 = files[j].name + dlen;
                if (r2[0] == '/') r2++;
            }
            if (r2[0] != 0 && !strchr(r2, '/') && strcmp(r2, rest) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup) continue;
        count++;
        char line[64];
        int j = 0;
        while (rest[j] && j < FS_MAX_NAME) {
            line[j] = rest[j];
            j++;
        }
        if (files[i].is_dir) { line[j] = '/'; j++; }
        if (!files[i].is_dir) {
            line[j] = ' ';
            j++;
            char num[16];
            uitoa(files[i].size, num, 10);
            int k = 0;
            while (num[k]) line[j++] = num[k++];
        }
        line[j++] = '\n';
        line[j] = 0;
        uint32_t len = (uint32_t)j;
        if (used + len < max) {
            for (uint32_t m = 0; m < len; m++) out[used + m] = line[m];
            used += len;
        }
    }
    out[used] = 0;
    return count;
}

uint32_t fs_get_size(const char* name) {
    struct fs_file* f = fs_find(name);
    return f ? f->size : 0;
}

// ============================================================
// ATA 硬盘持久化
// 磁盘镜像区固定槽位文件表:
//   LBA 1020: super (512B, magic="EPFS", version)
//   LBA 1024 + i*9: 槽位 i { header 1 扇区 + data 8 扇区 (4096B) }
// ============================================================

#define FS_DISK_SUPER_LBA  1020
#define FS_DISK_BASE_LBA   1024
#define FS_DISK_SLOT_LBAS  9      // 每文件 9 扇区
#define FS_DISK_DATA_SECT  8      // 数据 8 扇区 = 4096B

static int fs_dirty = 0;

struct fs_disk_super {
    char     magic[4];      // "EPFS"
    uint32_t version;
    uint32_t pad[125];
};

struct fs_disk_header {
    char     name[FS_MAX_NAME];
    uint32_t size;
    uint32_t flags;         // bit0 = is_dir
    uint32_t pad[120];
};

void fs_mark_dirty(void) { fs_dirty = 1; }
int fs_disk_dirty(void) { return fs_dirty; }

void fs_disk_mount(void) {
    if (!g_ata_master.present) {
        serial_write_str("[fs] no ATA disk, ramfs only\n");
        return;
    }
    struct fs_disk_super super;
    int r = ata_read_sectors(0, FS_DISK_SUPER_LBA, 1, &super);
    serial_printf(COM1, "[fs] read super r=%d b0=%d,%d,%d,%d b4=%d,%d,%d,%d\n",
                  r, super.magic[0], super.magic[1], super.magic[2], super.magic[3],
                  super.magic[4], super.magic[5], super.magic[6], super.magic[7]);
    if (r != 0) return;
    if (memcmp(super.magic, "EPFS", 4) != 0) {
        serial_write_str("[fs] no EPFS image, fresh ramfs\n");
        fs_dirty = 1;      // 待首次保存
        return;
    }
    // 第一遍: 恢复目录 (保证父目录先于文件)
    for (int i = 0; i < FS_MAX_FILES; i++) {
        struct fs_disk_header hdr;
        uint32_t lba = FS_DISK_BASE_LBA + i * FS_DISK_SLOT_LBAS;
        if (ata_read_sectors(0, lba, 1, &hdr) != 0) continue;
        if (hdr.name[0] == 0) continue;
        hdr.name[FS_MAX_NAME - 1] = 0;
        if (hdr.flags & 1) fs_mkdir(hdr.name);
    }
    // 第二遍: 恢复文件
    int loaded = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        struct fs_disk_header hdr;
        uint32_t lba = FS_DISK_BASE_LBA + i * FS_DISK_SLOT_LBAS;
        if (ata_read_sectors(0, lba, 1, &hdr) != 0) continue;
        if (hdr.name[0] == 0) continue;
        hdr.name[FS_MAX_NAME - 1] = 0;
        if (hdr.flags & 1) continue;      // 目录已处理
        uint8_t tmp[4096];
        if (ata_read_sectors(0, lba + 1, FS_DISK_DATA_SECT, tmp) != 0) continue;
        uint32_t sz = hdr.size > FS_MAX_SIZE ? FS_MAX_SIZE : hdr.size;
        if (fs_write(hdr.name, tmp, sz) == 0) loaded++;
    }
    fs_dirty = 0;
    serial_printf(COM1, "[fs] mounted %d files from disk\n", loaded);
}

void fs_disk_save(void) {
    if (!g_ata_master.present) return;
    // super
    struct fs_disk_super super;
    memcpy(super.magic, "EPFS", 4);
    super.version = 1;
    for (int i = 0; i < 125; i++) super.pad[i] = 0;
    if (ata_write_sectors(0, FS_DISK_SUPER_LBA, 1, &super) != 0) {
        serial_write_str("[fs] save failed (super)\n");
        return;
    }
    // 槽位
    for (int i = 0; i < FS_MAX_FILES; i++) {
        struct fs_disk_header hdr;
        uint8_t data[4096];
        memset(&hdr, 0, sizeof(hdr));
        memset(data, 0, sizeof(data));
        if (files[i].used) {
            strncpy(hdr.name, files[i].name, FS_MAX_NAME - 1);
            hdr.size = files[i].size;
            hdr.flags = files[i].is_dir ? 1 : 0;
            if (!files[i].is_dir && files[i].size > 0) {
                memcpy(data, files[i].data, files[i].size);
            }
        }
        uint32_t lba = FS_DISK_BASE_LBA + i * FS_DISK_SLOT_LBAS;
        if (ata_write_sectors(0, lba, 1, &hdr) != 0) {
            serial_write_str("[fs] save failed (slot header)\n");
            return;
        }
        if (!files[i].is_dir) {
            if (ata_write_sectors(0, lba + 1, FS_DISK_DATA_SECT, data) != 0) {
                serial_write_str("[fs] save failed (slot data)\n");
                return;
            }
        }
    }
    fs_dirty = 0;
    serial_write_str("[fs] saved to disk\n");
}
