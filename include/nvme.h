#ifndef LITEOS_NVME_H
#define LITEOS_NVME_H

#include "buddy.h"
#include "pci.h"

#define LITEOS_NVME_QUEUE_DEPTH 64U

typedef struct __attribute__((packed)) {
    UINT32 OpcodeAndFlags;
    UINT32 NamespaceId;
    UINT64 Reserved;
    UINT64 Metadata;
    UINT64 Prp1;
    UINT64 Prp2;
    UINT32 CommandDword10;
    UINT32 CommandDword11;
    UINT32 CommandDword12;
    UINT32 CommandDword13;
    UINT32 CommandDword14;
    UINT32 CommandDword15;
} LITEOS_NVME_COMMAND;

typedef struct __attribute__((packed)) {
    UINT32 CommandSpecific;
    UINT32 Reserved;
    UINT16 SubmissionHead;
    UINT16 SubmissionIdentifier;
    UINT16 Status;
    UINT16 Reserved2;
} LITEOS_NVME_COMPLETION;

typedef struct {
    LITEOS_PCI_DEVICE PciDevice;
    LITEOS_PHYSICAL_BLOCK SubmissionQueueBlock;
    LITEOS_PHYSICAL_BLOCK CompletionQueueBlock;
    LITEOS_NVME_COMMAND *SubmissionQueue;
    LITEOS_NVME_COMPLETION *CompletionQueue;
    UINT16 SubmissionTail;
    UINT16 CompletionHead;
    UINT16 QueueIdentifier;
    UINT16 CompletionPhase;
    BOOLEAN Initialized;
} LITEOS_NVME_CONTROLLER;

BOOLEAN liteos_nvme_is_device(const LITEOS_PCI_DEVICE *device);
BOOLEAN liteos_nvme_controller_init(LITEOS_NVME_CONTROLLER *controller,
                                    const LITEOS_PCI_DEVICE *device);
BOOLEAN liteos_nvme_controller_destroy(LITEOS_NVME_CONTROLLER *controller);
BOOLEAN liteos_nvme_build_read(LITEOS_NVME_COMMAND *command, UINT32 namespace_id,
                               UINT64 physical_buffer, UINT64 starting_lba,
                               UINT32 block_count);
BOOLEAN liteos_nvme_submit(LITEOS_NVME_CONTROLLER *controller,
                           const LITEOS_NVME_COMMAND *command);

#endif
