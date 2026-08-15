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

kstatus_t block_submit_bio(bio_t *bio);
