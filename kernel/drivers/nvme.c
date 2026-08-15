#include "nvme.h"

#define NVME_ADMIN_CLASS       0x01U
#define NVME_NVM_SUBCLASS      0x08U
#define NVME_NVM_PROGRAMMING   0x02U
#define NVME_READ_OPCODE       0x02U

static VOID memory_zero(UINT8 *memory, UINT64 size) {
    while (size-- != 0) *memory++ = 0;
}

static BOOLEAN ring_full(const LITEOS_NVME_CONTROLLER *controller) {
    UINT16 next = (UINT16)((controller->SubmissionTail + 1U) % LITEOS_NVME_QUEUE_DEPTH);
    return next == controller->CompletionHead;
}

BOOLEAN liteos_nvme_is_device(const LITEOS_PCI_DEVICE *device) {
    return device != 0 && device->ClassCode == NVME_ADMIN_CLASS &&
           device->Subclass == NVME_NVM_SUBCLASS &&
           device->ProgIf == NVME_NVM_PROGRAMMING;
}

BOOLEAN liteos_nvme_controller_init(LITEOS_NVME_CONTROLLER *controller,
                                    const LITEOS_PCI_DEVICE *device) {
    if (controller == 0 || controller->Initialized || !liteos_nvme_is_device(device) ||
        device->Bars[0] == 0) return 0;
    controller->PciDevice = *device;
    if (!liteos_buddy_alloc(LITEOS_BUDDY_MIN_ORDER, &controller->SubmissionQueueBlock) ||
        !liteos_buddy_alloc(LITEOS_BUDDY_MIN_ORDER, &controller->CompletionQueueBlock)) {
        liteos_buddy_free(&controller->SubmissionQueueBlock);
        return 0;
    }
    controller->SubmissionQueue = (LITEOS_NVME_COMMAND *)
        (uintptr_t)controller->SubmissionQueueBlock.PhysicalAddress;
    controller->CompletionQueue = (LITEOS_NVME_COMPLETION *)
        (uintptr_t)controller->CompletionQueueBlock.PhysicalAddress;
    memory_zero((UINT8 *)controller->SubmissionQueue, LITEOS_BUDDY_MIN_BLOCK_SIZE);
    memory_zero((UINT8 *)controller->CompletionQueue, LITEOS_BUDDY_MIN_BLOCK_SIZE);
    controller->SubmissionTail = 0;
    controller->CompletionHead = 0;
    controller->QueueIdentifier = 1;
    controller->CompletionPhase = 1;
    controller->Initialized = 1;
    return 1;
}

BOOLEAN liteos_nvme_controller_destroy(LITEOS_NVME_CONTROLLER *controller) {
    if (controller == 0 || !controller->Initialized) return 0;
    if (!liteos_buddy_free(&controller->CompletionQueueBlock) ||
        !liteos_buddy_free(&controller->SubmissionQueueBlock)) return 0;
    controller->SubmissionQueue = 0;
    controller->CompletionQueue = 0;
    controller->Initialized = 0;
    return 1;
}

BOOLEAN liteos_nvme_build_read(LITEOS_NVME_COMMAND *command, UINT32 namespace_id,
                               UINT64 physical_buffer, UINT64 starting_lba,
                               UINT32 block_count) {
    if (command == 0 || namespace_id == 0 || physical_buffer == 0 ||
        (physical_buffer & 0xFFFULL) != 0 || block_count == 0 || block_count > 65536U) return 0;
    UINT8 *bytes = (UINT8 *)command;
    for (UINTN i = 0; i < sizeof(*command); ++i) bytes[i] = 0;
    command->OpcodeAndFlags = NVME_READ_OPCODE;
    command->NamespaceId = namespace_id;
    command->Prp1 = physical_buffer;
    command->CommandDword10 = (UINT32)starting_lba;
    command->CommandDword11 = (UINT32)(starting_lba >> 32);
    command->CommandDword12 = (block_count - 1U) & 0xFFFFU;
    return 1;
}

BOOLEAN liteos_nvme_submit(LITEOS_NVME_CONTROLLER *controller,
                           const LITEOS_NVME_COMMAND *command) {
    if (controller == 0 || command == 0 || !controller->Initialized || ring_full(controller)) return 0;
    controller->SubmissionQueue[controller->SubmissionTail] = *command;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    controller->SubmissionTail = (UINT16)((controller->SubmissionTail + 1U) %
                                           LITEOS_NVME_QUEUE_DEPTH);
    return 1;
}
