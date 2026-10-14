#include "internal.h"

/* REFACTOR_FS_FAT32_TRANSACTION_OWNER: cluster-chain rollback and release. */

#ifdef OS_KERNEL_BUILD
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
#ifdef OS_KERNEL_BUILD
    return kmalloc(size, 0);
#else
    return malloc(size);
#endif
}

static VOID fat_transaction_free(VOID *memory) {
#ifdef OS_KERNEL_BUILD
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

/* Brent guard shared by non-mutating validation and rollback snapshots. */
static BOOLEAN fat_chain_cycle_step(UINT32 next, UINT32 *checkpoint,
                                    UINT32 *distance, UINT32 *window) {
    if (next == *checkpoint) return 0;
    if (++*distance == *window) {
        *checkpoint = next;
        *distance = 0U;
        if (*window <= UINT32_MAX / 2U) *window *= 2U;
        else *window = UINT32_MAX;
    }
    return 1;
}

static BOOLEAN fat_snapshot_chain(const OS_FAT32 *filesystem, UINT32 first,
                                  FAT_CHAIN_SNAPSHOT *snapshot) {
    UINT32 cluster = first;
    UINT32 checkpoint = first, distance = 0U, window = 1U;
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
        if (!fat_chain_cycle_step(next, &checkpoint, &distance, &window)) return 0;
        cluster = next;
    }
    return 0;
}

/* Source links are retained, not mutated: validate their tail without a
 * rollback snapshot or copying every FAT replica into heap allocations. */
static BOOLEAN fat_chain_tail(const OS_FAT32 *filesystem, UINT32 first,
                              UINT32 *tail) {
    UINT32 cluster = first;
    UINT32 checkpoint = first, distance = 0U, window = 1U;
    *tail = 0U;
    if (first == 0U) return 1;
    for (UINT32 hops = 0U; hops < filesystem->ClusterCount; ++hops) {
        UINT32 next;
        if (!fat32_cluster_valid(filesystem, cluster) ||
            !fat32_read_next_cluster(filesystem, cluster, &next)) return 0;
        if (next == 0U) { *tail = cluster; return 1; }
        /* Brent cycle detection: one FAT read per hop, no visited-set heap.
         * Keep the volume bound as a final guard and saturate window growth. */
        if (!fat_chain_cycle_step(next, &checkpoint, &distance, &window)) return 0;
        cluster = next;
    }
    return 0;
}

static VOID fat_snapshot_restore(OS_FAT32 *filesystem,
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

BOOLEAN fat32_free_cluster_chain(OS_FAT32 *filesystem, UINT32 first) {
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

/* Caller holds the mutation lock. Keep both names and FAT copies available
 * for recovery until writeback commits; target LFN/name slots stay intact. */
BOOLEAN fat32_replace_file_entry(OS_FAT32 *filesystem,
    UINT64 source_lba, UINT32 source_offset,
    const UINT64 lfn_lba[20], const UINT32 lfn_offset[20], UINT32 lfn_count,
    UINT64 target_lba, UINT32 target_offset) {
    UINT8 source[32], target[32], replacement[32], lfn[20][32];
    FAT_CHAIN_SNAPSHOT retired = {0};
    UINT32 retained_tail = 0U, retired_tail = 0U;
    BOOLEAN success = 0;
    if (lfn_count > 20U ||
        !fat32_read_directory_slot(filesystem, source_lba, source_offset, source) ||
        !fat32_read_directory_slot(filesystem, target_lba, target_offset, target) ||
        ((source[11] | target[11]) & 0x10U) != 0U ||
        fat32_file_is_open_at(filesystem, target_lba, target_offset)) return 0;
    for (UINT32 i = 0U; i < lfn_count; ++i)
        if (!fat32_read_directory_slot(filesystem, lfn_lba[i], lfn_offset[i], lfn[i])) return 0;
    if (!fat_chain_tail(filesystem, fat32_entry_cluster(source), &retained_tail) ||
        !fat_chain_tail(filesystem, fat32_entry_cluster(target), &retired_tail)) goto done;
    /* Both chains reached EOC under the mutation lock. In a singly linked
     * FAT, intersecting chains share their entire suffix and final cluster.
     * Comparing tails avoids O(source_count * target_count) work. Cycles and
     * invalid links were already rejected by the bounded traversals. */
    if (retired_tail != 0U && retired_tail == retained_tail) goto done;
    if (!fat_snapshot_chain(filesystem, fat32_entry_cluster(target), &retired)) goto done;
    for (UINT32 i = 0U; i < 32U; ++i) replacement[i] = source[i];
    for (UINT32 i = 0U; i < 11U; ++i) replacement[i] = target[i];
    replacement[12] = target[12]; /* Preserve short-name case flags. */
    if (!fat32_write_directory_slot(filesystem, target_lba, target_offset, replacement) ||
        !fat32_delete_directory_slot(filesystem, source_lba, source_offset)) goto rollback;
    for (UINT32 i = 0U; i < lfn_count; ++i)
        if (!fat32_delete_directory_slot(filesystem, lfn_lba[i], lfn_offset[i])) goto rollback;
    for (UINT32 i = 0U; i < retired.Count; ++i)
        if (!fat32_write_file_fat_value(filesystem, retired.Clusters[i], 0U, 0U)) goto rollback;
    if (!fat32_writeback(filesystem)) goto rollback;
    fat32_rebind_open_file(filesystem, source_lba, source_offset, target_lba, target_offset);
    success = 1;
    goto done;
rollback:
    fat_snapshot_restore(filesystem, &retired, retired.Count);
    (void)fat32_write_directory_slot(filesystem, target_lba, target_offset, target);
    (void)fat32_write_directory_slot(filesystem, source_lba, source_offset, source);
    for (UINT32 i = 0U; i < lfn_count; ++i)
        (void)fat32_write_directory_slot(filesystem, lfn_lba[i], lfn_offset[i], lfn[i]);
    (void)fat32_writeback(filesystem);
done:
    fat_snapshot_destroy(&retired);
    return success;
}

void fat32_rollback_directory_extensions(OS_FAT32 *filesystem,
                                          UINT32 anchor,
                                          const UINT32 clusters[21],
                                          UINT32 count) {
    if (filesystem == 0 || count == 0U) return;
    (void)fat32_write_file_fat_value(filesystem, anchor,
                                     FAT32_TRANSACTION_EOC_MIN, 1U);
    (void)fat32_free_cluster_chain(filesystem, clusters[0]);
}
