// ============================================================
// EpochOS - 内存文件系统 (ramfs) 头文件
// 支持目录: 条目名保存完整路径, 目录为独立条目 (is_dir=1)
// ============================================================
#ifndef EPOCHOS_FS_H
#define EPOCHOS_FS_H

#include "types.h"

#define FS_MAX_FILES   64
#define FS_MAX_NAME    24
#define FS_MAX_SIZE    4096

struct fs_file {
    char     name[FS_MAX_NAME];
    uint8_t  data[FS_MAX_SIZE];
    uint32_t size;
    uint8_t  used;
    uint8_t  is_dir;           // 1=目录, 0=普通文件
};

void fs_init(void);
int  fs_create(const char* name);              // 创建空文件, 成功返回 0
int  fs_write(const char* name, const uint8_t* data, uint32_t size);
int  fs_append(const char* name, const uint8_t* data, uint32_t size);
int  fs_read(const char* name, uint8_t* out, uint32_t max);
int  fs_delete(const char* name);              // 删除文件(或空目录)
int  fs_exists(const char* name);
int  fs_is_dir(const char* name);
int  fs_mkdir(const char* name);               // 创建目录, 成功返回 0
int  fs_rmdir(const char* name);               // 删除空目录, 非空返回 -1
int  fs_rename(const char* oldname, const char* newname);
int  fs_list(char* out, uint32_t max);         // 列出全部条目, 返回个数
// 列出 dir 下的一级条目 (dir="/" 或 "" 表示根目录), 返回个数
int  fs_list_dir(const char* dir, char* out, uint32_t max);
uint32_t fs_get_size(const char* name);

// ---- ATA 硬盘持久化 (磁盘镜像区 LBA 1024 起) ----
void fs_disk_mount(void);        // 挂载: 从磁盘加载文件表 (无镜像则初始化空)
void fs_disk_save(void);         // 保存: 将整个文件表写入磁盘
int  fs_disk_dirty(void);        // 是否有未保存修改
void fs_mark_dirty(void);        // 标记文件表有未保存修改
void fs_format(void);            // 格式化: 清空全部文件 (ramfs 与下次保存的磁盘)

#endif
