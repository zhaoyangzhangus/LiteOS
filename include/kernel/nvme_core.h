#pragma once

#include <kernel/device.h>
#include <kernel/dma.h>
#include <kernel/mm.h>
#include <kernel/pci.h>

#define NVME_ADMIN_QUEUE_DEPTH 64U
#define NVME_IO_QUEUE_DEPTH    64U
#define NVME_MAX_IO_QUEUES     8U
#define NVME_MAX_CONTROLLERS   8U

typedef struct __attribute__((packed)) nvme_command {
    uint32_t opcode_flags;
    uint32_t namespace_id;
    uint64_t reserved;
    uint64_t metadata;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} nvme_command_t;

typedef struct __attribute__((packed)) nvme_completion {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t command_id;
    uint16_t status;
} nvme_completion_t;

struct nvme_controller;
struct nvme_pending_io;

typedef struct nvme_queue {
    struct nvme_controller *controller;
    page_t *submission_page;
    page_t *completion_page;
    dma_mapping_t submission_dma;
    dma_mapping_t completion_dma;
    nvme_command_t *submission;
    nvme_completion_t *completion;
    uint16_t queue_id;
    uint16_t depth;
    uint16_t submission_tail;
    uint16_t completion_head;
    uint16_t phase;
    atomic_uint pending_count;
    uint8_t irq_vector;
    uint8_t reserved[1];
    bool active;
    spinlock_t lock;
    list_head_t pending_ios;
    atomic_bool completion_queued;
    atomic_uint completion_work_refs;
} nvme_queue_t;

typedef struct nvme_controller {
    device_t *device;
    pci_device_t *pci;
    volatile uint8_t *registers;
    vaddr_t registers_va;
    uint64_t registers_size;
    uint64_t capabilities;
    uint32_t doorbell_stride;
    page_t *admin_submission_page;
    page_t *admin_completion_page;
    dma_mapping_t admin_submission_dma;
    dma_mapping_t admin_completion_dma;
    nvme_command_t *admin_submission;
    nvme_completion_t *admin_completion;
    uint16_t admin_depth;
    uint16_t admin_submission_tail;
    uint16_t admin_completion_head;
    uint16_t admin_phase;
    atomic_uint next_command_id;
    spinlock_t admin_lock;
    nvme_queue_t io_queues[NVME_MAX_IO_QUEUES];
    uint16_t io_queue_count;
    atomic_uint next_io_queue;
    uint32_t namespace_count;
    uint64_t namespace_block_count;
    uint32_t namespace_block_size;
    bool started;
    bool identified;
} nvme_controller_t;

kstatus_t nvme_driver_register(void);
bool nvme_driver_self_test(void);
/* 在页分配器和 BIO 层就绪后，读取 LBA0 验证完整数据通路。 */
bool nvme_hardware_io_self_test(void);
/* 验证无挂起 I/O 时的控制器复位、队列重建和复位后的读盘路径。 */
bool nvme_hardware_reset_self_test(void);
const nvme_controller_t *nvme_active_controller(void);
nvme_controller_t *nvme_controller_at(uint32_t index);
bool nvme_hardware_present(void);
kstatus_t nvme_last_error(void);
uint32_t nvme_last_stage(void);
uint16_t nvme_last_completion_status(void);
/* 定时器只投递轮询任务，真正的 CQ 消费在可抢占的 deferred 上下文执行。 */
bool nvme_schedule_deferred_poll(void);

/*
 * 同步关键 I/O 可直接消费本设备的 CQ，不依赖 deferred worker 获得 CPU。
 * queue->lock 与普通 deferred completion 共用，因此两条消费者路径不会同时
 * 修改 completion_head/pending_ios。
 */
uint32_t nvme_poll_device_completions(device_t *device, uint32_t budget);
kstatus_t nvme_recover_after_timeout(device_t *device);
