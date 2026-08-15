#pragma once

#include <kernel/base.h>
#include <kernel/journal.h>
#include <kernel/spinlock.h>

/*
 * LiteFS 是当前内核使用的最小原生文件系统原型：
 * - 固定大小 inode 表，便于在早期启动阶段无递归分配；
 * - 每个文件使用一个连续 extent；
 * - 元数据使用 journal 做 write-ahead logging；
 * - 数据块先持久化，再提交 inode 大小。
 *
 * 这是规范中 nativefs 的可启动最小实现，后续可以在不改变 VFS
 * backend 接口的前提下替换为可扩展的 extent 树和目录树。
 */
#define LITEFS_MAGIC              0x4C465346U /* "LFSF" */
#define LITEFS_VERSION            1U
#define LITEFS_MAX_INODES         8U
#define LITEFS_NAME_LIMIT         16U
#define LITEFS_FILE_EXTENT_BLOCKS 4U
#define LITEFS_MIN_BLOCKS         (LITEFS_DATA_START + LITEFS_MAX_INODES * LITEFS_FILE_EXTENT_BLOCKS)
#define LITEFS_JOURNAL_START      8U
#define LITEFS_JOURNAL_BLOCKS     8U
#define LITEFS_INODE_START        1U
#define LITEFS_INODE_BLOCKS       1U
#define LITEFS_DATA_START         16U

typedef struct __attribute__((packed)) litefs_super_disk {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t journal_start;
    uint32_t journal_blocks;
    uint32_t inode_start;
    uint32_t inode_blocks;
    uint32_t data_start;
    uint32_t inode_count;
    uint64_t generation;
    uint8_t reserved[JOURNAL_BLOCK_SIZE - 52U];
    uint32_t checksum;
} litefs_super_disk_t;

typedef struct __attribute__((packed)) litefs_inode_disk {
    uint32_t flags;
    uint32_t mode;
    uint64_t inode_id;
    uint64_t size;
    uint64_t extent_start;
    uint64_t extent_blocks;
    uint32_t name_length;
    char name[LITEFS_NAME_LIMIT];
    uint32_t checksum;
} litefs_inode_disk_t;

_Static_assert(sizeof(litefs_super_disk_t) == JOURNAL_BLOCK_SIZE,
               "LiteFS superblock must occupy one block");
_Static_assert(sizeof(litefs_inode_disk_t) == 64U,
               "LiteFS inode size is part of the on-disk format");

typedef struct litefs_inode {
    uint32_t flags;
    uint32_t mode;
    uint64_t inode_id;
    uint64_t size;
    uint64_t extent_start;
    uint64_t extent_blocks;
    uint32_t name_length;
    char name[LITEFS_NAME_LIMIT];
} litefs_inode_t;

typedef struct litefs {
    journal_storage_t storage;
    journal_t journal;
    litefs_super_disk_t super;
    litefs_inode_t inodes[LITEFS_MAX_INODES];
    spinlock_t lock;
    uint32_t total_blocks;
    bool mounted;
} litefs_t;

typedef struct litefs_file_context {
    litefs_t *filesystem;
    uint32_t inode_index;
} litefs_file_context_t;

kstatus_t litefs_format(const journal_storage_t *storage, uint32_t total_blocks);
kstatus_t litefs_mount(litefs_t *filesystem, const journal_storage_t *storage,
                       uint32_t total_blocks);
kstatus_t litefs_create_file(litefs_t *filesystem, const char *name,
                             uint32_t mode, uint32_t *inode_index);
kstatus_t litefs_read_file(litefs_t *filesystem, uint32_t inode_index,
                           uint64_t offset, void *buffer, size_t length,
                           uint64_t *bytes);
kstatus_t litefs_write_file(litefs_t *filesystem, uint32_t inode_index,
                            uint64_t offset, const void *buffer, size_t length,
                            uint64_t *bytes);
kstatus_t litefs_fsync(litefs_t *filesystem);

/* 启动阶段执行内存盘掉电、journal 回放、校验和损坏测试，并注册一个 VFS 文件。 */
bool litefs_self_test(void);
