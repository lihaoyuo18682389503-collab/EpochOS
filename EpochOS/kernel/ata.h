// ============================================================
// EpochOS - ATA PIO 硬盘驱动 (主通道 0x1F0, IRQ14)
// 支持: 磁盘识别 / 扇区读写 (LBA28)
// ============================================================
#ifndef EPOCHOS_ATA_H
#define EPOCHOS_ATA_H

#include "types.h"

#define ATA_PRIMARY_IO   0x1F0
#define ATA_PRIMARY_CTRL 0x3F6
#define ATA_IRQ          14

#define SECTOR_SIZE 512

struct ata_device {
    int present;
    int is_master;
    uint32_t sectors;        // 总扇区数
    uint16_t cylinders;
    uint8_t  heads;
    uint8_t  sectors_per_track;
    char model[41];          // 型号字符串
};

int ata_init(void);
int ata_identify(struct ata_device* dev, int master);
int ata_read_sectors(uint8_t drive, uint32_t lba, uint8_t count, void* buf);
int ata_write_sectors(uint8_t drive, uint32_t lba, uint8_t count, const void* buf);

extern struct ata_device g_ata_master;
extern struct ata_device g_ata_slave;

#endif
