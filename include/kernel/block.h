#pragma once
#pragma once
#include "base.h"
#include "list.h"
#include "mm.h"
#include "io.h"

typedef struct bio_vec {
    page_t *page;
    uint32_t offset;
    uint32_t length;
} bio_vec_t;

typedef struct bio {
    uint64_t lba;
    uint32_t op;
    uint32_t flags;
    bio_vec_t *vecs;
    uint16_t vec_count;
    uint16_t reserved;
    io_request_t *io;
    list_head_t node;
} bio_t;

/* block 层选择的逻辑队列通过请求 flags 传给具体驱动。 */
#define IOREQ_BLOCK_QUEUE_VALID (1U << 31)
#define IOREQ_BLOCK_QUEUE_MASK  0x000000FFU
#define IOREQ_BLOCK_QUEUE_SHIFT 24U
#define BLOCK_BIO_BATCH_MAX     32U

bool block_multiqueue_self_test(void);

kstatus_t block_submit_bio(bio_t *bio);
kstatus_t block_submit_bio_batch(bio_t *bios, uint32_t count,
                                  uint32_t *submitted);

#define BIO_OP_READ  1U
#define BIO_OP_WRITE 2U
#define BIO_OP_FLUSH 3U
