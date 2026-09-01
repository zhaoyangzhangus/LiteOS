#include "internal.h"

/* REFACTOR_FS_FAT32_TRANSACTION_OWNER: cluster-chain rollback and release. */

#ifdef LITEOS_KERNEL_BUILD
#include <kernel/kmem.h>
#else
#include <stdlib.h>
#endif

#define FAT32_TRANSACTION_EOC_MIN 0x0FFFFFF8U

typedef struct fat_chain_snapshot {
    UINT32 *Clusters;
    UINT32 *Values;
    UINT32 Count;
    UINT32 Capacity;
    UINT32 FatCount;
} FAT_CHAIN_SNAPSHOT;

static VOID *fat_transaction_alloc(size_t size) {
#ifdef LITEOS_KERNEL_BUILD
    return kmalloc(size, 0);
#else
    return malloc(size);
#endif
}

static VOID fat_transaction_free(VOID *memory) {
#ifdef LITEOS_KERNEL_BUILD
    kfree(memory);
#else
    free(memory);
#endif
}

static VOID fat_snapshot_destroy(FAT_CHAIN_SNAPSHOT *snapshot) {
    if (snapshot == 0) return;
    fat_transaction_free(snapshot->Clusters);
    fat_transaction_free(snapshot->Values);
    snapshot->Clusters = 0;
    snapshot->Values = 0;
    snapshot->Count = 0U;
    snapshot->Capacity = 0U;
    snapshot->FatCount = 0U;
}

static BOOLEAN fat_snapshot_reserve(FAT_CHAIN_SNAPSHOT *snapshot) {
    UINT32 capacity;
    UINT64 value_count;
    UINT32 *clusters;
    UINT32 *values;
    if (snapshot == 0 || snapshot->FatCount == 0U) return 0;
    if (snapshot->Count < snapshot->Capacity) return 1;
    capacity = snapshot->Capacity == 0U ? 16U : snapshot->Capacity * 2U;
    value_count = (UINT64)capacity * snapshot->FatCount;
    if (capacity < snapshot->Capacity ||
        value_count > (UINT64)((size_t)-1 / sizeof(UINT32))) return 0;
    clusters = (UINT32 *)fat_transaction_alloc((size_t)capacity * sizeof(UINT32));
    values = (UINT32 *)fat_transaction_alloc((size_t)value_count * sizeof(UINT32));
    if (clusters == 0 || values == 0) {
        fat_transaction_free(clusters);
        fat_transaction_free(values);
        return 0;
    }
    for (UINT32 i = 0U; i < snapshot->Count; ++i) {
        clusters[i] = snapshot->Clusters[i];
        for (UINT32 fat = 0U; fat < snapshot->FatCount; ++fat) {
            values[(UINT64)i * snapshot->FatCount + fat] =
                snapshot->Values[(UINT64)i * snapshot->FatCount + fat];
        }
    }
    fat_transaction_free(snapshot->Clusters);
    fat_transaction_free(snapshot->Values);
    snapshot->Clusters = clusters;
    snapshot->Values = values;
    snapshot->Capacity = capacity;
    return 1;
}

static BOOLEAN fat_snapshot_chain(const LITEOS_FAT32 *filesystem, UINT32 first,
                                  FAT_CHAIN_SNAPSHOT *snapshot) {
    UINT32 cluster = first;
    if (filesystem == 0 || snapshot == 0 || first == 0U ||
        !fat32_cluster_valid(filesystem, first)) return first == 0U;
    snapshot->FatCount = filesystem->FatCount;
    for (UINT32 hops = 0U; hops < filesystem->ClusterCount; ++hops) {
        UINT32 next;
        if (!fat32_cluster_valid(filesystem, cluster) ||
            !fat_snapshot_reserve(snapshot)) return 0;
        snapshot->Clusters[snapshot->Count] = cluster;
        for (UINT32 fat = 1U; fat <= filesystem->FatCount; ++fat) {
            if (!fat32_read_fat_entry(
                    filesystem, fat, cluster,
                    &snapshot->Values[(UINT64)snapshot->Count *
                                      snapshot->FatCount + fat - 1U])) return 0;
        }
        ++snapshot->Count;
        if (!fat32_read_next_cluster(filesystem, cluster, &next)) return 0;
        if (next == 0U) return 1;
        cluster = next;
    }
    return 0;
}

static VOID fat_snapshot_restore(LITEOS_FAT32 *filesystem,
                                  const FAT_CHAIN_SNAPSHOT *snapshot,
                                  UINT32 count) {
    if (filesystem == 0 || snapshot == 0 || snapshot->Values == 0) return;
    if (count > snapshot->Count) count = snapshot->Count;
    while (count != 0U) {
        UINT32 index = --count;
        for (UINT32 fat = 1U; fat <= snapshot->FatCount; ++fat) {
            (void)fat32_write_fat_entry(
                filesystem, fat, snapshot->Clusters[index],
                snapshot->Values[(UINT64)index * snapshot->FatCount + fat - 1U]);
        }
    }
}

BOOLEAN fat32_free_cluster_chain(LITEOS_FAT32 *filesystem, UINT32 first) {
    FAT_CHAIN_SNAPSHOT snapshot = {0};
    if (first == 0U) return 1;
    if (!fat_snapshot_chain(filesystem, first, &snapshot)) {
        fat_snapshot_destroy(&snapshot);
        return 0;
    }
    for (UINT32 index = 0U; index < snapshot.Count; ++index) {
        if (!fat32_write_file_fat_value(filesystem, snapshot.Clusters[index],
                                        0U, 0U)) {
            fat_snapshot_restore(filesystem, &snapshot, index + 1U);
            fat_snapshot_destroy(&snapshot);
            return 0;
        }
    }
    fat_snapshot_destroy(&snapshot);
    return 1;
}

void fat32_rollback_directory_extensions(LITEOS_FAT32 *filesystem,
                                          UINT32 anchor,
                                          const UINT32 clusters[21],
                                          UINT32 count) {
    if (filesystem == 0 || count == 0U) return;
    (void)fat32_write_file_fat_value(filesystem, anchor,
                                     FAT32_TRANSACTION_EOC_MIN, 1U);
    (void)fat32_free_cluster_chain(filesystem, clusters[0]);
}
