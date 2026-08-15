#pragma once
#include "base.h"
#include "mm.h"

struct device;

enum dma_direction {
    DMA_TO_DEVICE = 0,
    DMA_FROM_DEVICE,
    DMA_BIDIRECTIONAL,
};

typedef struct dma_segment {
    iova_t addr;
    uint64_t length;
} dma_segment_t;

typedef struct dma_mapping {
    struct device *device;
    page_t **pages;
    uint32_t page_count;
    uint32_t direction;

    dma_segment_t *segments;
    uint32_t segment_count;

    uint64_t mapped_length;
    uint32_t flags;
} dma_mapping_t;

kstatus_t dma_map_pages(struct device *dev, page_t **pages, uint32_t page_count,
                        enum dma_direction dir, dma_mapping_t *out);
void dma_unmap(dma_mapping_t *mapping);
void dma_sync_for_device(dma_mapping_t *mapping);
void dma_sync_for_cpu(dma_mapping_t *mapping);
