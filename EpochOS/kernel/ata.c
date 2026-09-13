// ============================================================
// EpochOS - ATA PIO 硬盘驱动 (主通道)
// 通过轮询状态寄存器实现, 不依赖 IRQ, 简单可靠
// ============================================================
#include "ata.h"
#include "types.h"
#include "string.h"
#include "serial.h"

struct ata_device g_ata_master;
struct ata_device g_ata_slave;

static inline void outb_p(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb_p(uint16_t port) {
    uint8_t ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint16_t inw_p(uint16_t port) {
    uint16_t ret;
    __asm__ __volatile__("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void insl_p(uint16_t port, void* addr, uint32_t count) {
    __asm__ __volatile__("rep insl" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}

static inline void outsl_p(uint16_t port, const void* addr, uint32_t count) {
    __asm__ __volatile__("rep outsl" : "+S"(addr), "+c"(count) : "d"(port));
}

// 等待 400ns (4 次读 alt status)
static void ata_wait400ns(void) {
    for (int i = 0; i < 4; i++) {
        inb_p(ATA_PRIMARY_CTRL);
    }
}

// 等待 BSY 清空, 超时 30s
static int ata_wait_bsy(void) {
    uint8_t st;
    for (uint32_t i = 0; i < 30000000; i++) {
        st = inb_p(ATA_PRIMARY_IO + 7);
        if (!(st & 0x80)) return 0;
    }
    return -1;
}

// 等待 DRQ 或 ERR
static int ata_wait_drq(void) {
    uint8_t st;
    for (uint32_t i = 0; i < 30000000; i++) {
        st = inb_p(ATA_PRIMARY_IO + 7);
        if (st & 0x01) return -2;          // ERR
        if (st & 0x08) return 0;           // DRQ
    }
    return -1;
}

// 识别磁盘 (dev_id: 0=master, 1=slave)
int ata_identify(struct ata_device* dev, int master) {
    memset(dev, 0, sizeof(struct ata_device));
    dev->is_master = master ? 1 : 0;
    dev->present = 0;

    uint16_t io = ATA_PRIMARY_IO;

    // 选择设备
    outb_p(io + 6, master ? 0xA0 : 0xB0);
    ata_wait400ns();

    // 清除错误寄存器
    outb_p(io + 1, 0);
    outb_p(io + 2, 0);
    outb_p(io + 3, 0);
    outb_p(io + 4, 0);
    outb_p(io + 5, 0);
    outb_p(io + 7, 0xEC);   // IDENTIFY

    uint8_t st = inb_p(io + 7);
    if (st == 0x00) return 0;  // 无设备
    // 等待 BSY 清空
    for (int i = 0; i < 100; i++) {
        st = inb_p(io + 7);
        if (!(st & 0x80)) break;
    }
    // 检查 LBA/ERR
    if (st & 0x01) {
        // 读取错误寄存器: 若 0x04 表示设备不存在
        uint8_t err = inb_p(io + 1);
        if (err & 0x04) return 0;
    }

    uint16_t data[256];
    for (int i = 0; i < 256; i++) {
        data[i] = inw_p(io);
    }

    // 解析
    dev->present = 1;
    dev->cylinders = data[1];
    dev->heads = data[3];
    dev->sectors_per_track = data[6];
    if (data[83] & (1 << 10)) {
        // 48-bit LBA
        dev->sectors = ((uint32_t)data[100] << 0) |
                       ((uint32_t)data[101] << 16);
    } else if (data[49] & (1 << 9)) {
        // 28-bit LBA
        dev->sectors = ((uint32_t)data[60] << 0) |
                       ((uint32_t)data[61] << 16);
    } else {
        // CHS
        dev->sectors = dev->cylinders * dev->heads * dev->sectors_per_track;
    }

    // 型号字符串 (20 words -> 40 chars, 两字节交换)
    for (int i = 0; i < 20; i++) {
        uint16_t w = data[27 + i];
        dev->model[i * 2] = (char)(w >> 8);
        dev->model[i * 2 + 1] = (char)(w & 0xFF);
    }
    dev->model[40] = 0;
    // 去除尾部空格
    int len = strlen(dev->model);
    while (len > 0 && dev->model[len - 1] == ' ') {
        dev->model[--len] = 0;
    }
    return 1;
}

// 读扇区 (LBA28)
int ata_read_sectors(uint8_t drive, uint32_t lba, uint8_t count, void* buf) {
    uint16_t io = ATA_PRIMARY_IO;
    uint8_t dev = drive ? 0xF0 : 0xE0;   // LBA 模式: bit6=1

    if (count == 0 || count > 128) return -1;

    outb_p(io + 6, dev | ((lba >> 24) & 0x0F));
    ata_wait400ns();
    outb_p(io + 1, 0);
    outb_p(io + 2, count);
    outb_p(io + 3, (uint8_t)lba);
    outb_p(io + 4, (uint8_t)(lba >> 8));
    outb_p(io + 5, (uint8_t)(lba >> 16));
    outb_p(io + 7, 0x20);   // READ SECTORS

    uint8_t* dst = (uint8_t*)buf;
    for (uint8_t s = 0; s < count; s++) {
        if (ata_wait_bsy() != 0) return -2;
        uint8_t st = inb_p(io + 7);
        if (st & 0x01) return -3;   // ERR
        if (!(st & 0x08)) return -4;
        insl_p(io, dst, 128);       // 512 bytes = 128 dwords
        dst += SECTOR_SIZE;
    }
    return 0;
}

// 写扇区 (LBA28)
int ata_write_sectors(uint8_t drive, uint32_t lba, uint8_t count, const void* buf) {
    uint16_t io = ATA_PRIMARY_IO;
    uint8_t dev = drive ? 0xF0 : 0xE0;   // LBA 模式: bit6=1

    if (count == 0 || count > 128) return -1;

    outb_p(io + 6, dev | ((lba >> 24) & 0x0F));
    ata_wait400ns();
    outb_p(io + 1, 0);
    outb_p(io + 2, count);
    outb_p(io + 3, (uint8_t)lba);
    outb_p(io + 4, (uint8_t)(lba >> 8));
    outb_p(io + 5, (uint8_t)(lba >> 16));
    outb_p(io + 7, 0x30);   // WRITE SECTORS

    const uint8_t* src = (const uint8_t*)buf;
    for (uint8_t s = 0; s < count; s++) {
        if (ata_wait_bsy() != 0) return -2;
        uint8_t st = inb_p(io + 7);
        if (st & 0x01) return -3;
        if (!(st & 0x08)) return -4;
        outsl_p(io, src, 128);
        src += SECTOR_SIZE;
        // 等待完成
        if (ata_wait_bsy() != 0) return -5;
        // 刷新缓存
        outb_p(io + 7, 0xE7);
        if (ata_wait_bsy() != 0) return -6;
    }
    return 0;
}

int ata_init(void) {
    memset(&g_ata_master, 0, sizeof(g_ata_master));
    memset(&g_ata_slave, 0, sizeof(g_ata_slave));
    ata_identify(&g_ata_master, 1);
    ata_identify(&g_ata_slave, 0);
    return g_ata_master.present || g_ata_slave.present;
}
