#pragma once

#include <kernel/base.h>
#include <kernel/device.h>
#include <kernel/spinlock.h>

#define JOURNAL_BLOCK_SIZE 512U

typedef kstatus_t (*journal_read_block_t)(void *context, uint64_t block,
                                          void *buffer, size_t length);
typedef kstatus_t (*journal_write_block_t)(void *context, uint64_t block,
                                           const void *buffer, size_t length);
typedef kstatus_t (*journal_flush_t)(void *context);
typedef kstatus_t (*journal_apply_record_t)(void *context, uint64_t target,
                                            const void *data, size_t length);

typedef struct journal_storage {
    journal_read_block_t read;
    journal_write_block_t write;
    journal_flush_t flush;
    void *context;
} journal_storage_t;

typedef struct journal {
    journal_storage_t storage;
    spinlock_t lock;
    uint64_t start_block;
    uint32_t block_count;
    uint32_t next_slot;
    uint64_t next_sequence;
    uint64_t active_sequence;
    uint32_t active_records;
    bool active;
} journal_t;

typedef struct journal_transaction {
    journal_t *journal;
    uint64_t sequence;
    bool active;
} journal_transaction_t;

/* 将 512 字节日志块绑定到规范 BIO/块设备；start_lba 以 512 字节扇区计。 */
typedef struct journal_block_backend {
    device_t *device;
    uint64_t start_lba;
    uint64_t block_count;
} journal_block_backend_t;

kstatus_t journal_init(journal_t *journal, const journal_storage_t *storage,
                       uint64_t start_block, uint32_t block_count);
kstatus_t journal_begin(journal_t *journal, journal_transaction_t *transaction);
kstatus_t journal_record(journal_transaction_t *transaction, uint64_t target,
                         const void *data, size_t length);
kstatus_t journal_commit(journal_transaction_t *transaction);
kstatus_t journal_replay(journal_t *journal, journal_apply_record_t apply,
                         void *context);
bool journal_block_storage_init(journal_storage_t *storage,
                                journal_block_backend_t *backend);
bool journal_block_storage_self_test(device_t *device, uint64_t start_lba,
                                     uint32_t block_count);
bool journal_self_test(void);
