#pragma once
#include "../../OS_Implementation_Specification_COMPLETE/include/kernel/dma.h"

/* 返回当前映射是否仍由 DMA 核心持有，供驱动卸载路径做幂等检查。 */
bool dma_mapping_active(const dma_mapping_t *mapping);
