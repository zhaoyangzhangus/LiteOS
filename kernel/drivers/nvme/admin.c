#include "internal.h"

#define NVME_REG_DBS 0x1000U

static void nvme_admin_ring_submission(nvme_controller_t *controller) {
    nvme_write32(controller, NVME_REG_DBS, controller->admin_submission_tail);
}

static void nvme_admin_ring_completion(nvme_controller_t *controller) {
    nvme_write32(controller, NVME_REG_DBS + controller->doorbell_stride,
                 controller->admin_completion_head);
}

kstatus_t nvme_admin_submit(nvme_controller_t *controller,
                            const nvme_command_t *command,
                            nvme_completion_t *result) {
    uint16_t slot;
    uint16_t command_id;
    nvme_command_t submission;
    uint64_t deadline;

    if (controller == 0 || command == 0 || result == 0 || !controller->started) {
        return K_EINVAL;
    }
    nvme_lock(&controller->admin_lock);
    slot = controller->admin_submission_tail;
    command_id = nvme_next_command_id(controller);
    submission = *command;
    submission.opcode_flags = (submission.opcode_flags & 0x0000FFFFU) |
                              ((uint32_t)command_id << 16);
    controller->admin_submission[slot] = submission;
    dma_sync_for_device(&controller->admin_submission_dma);
    dma_wmb();
    controller->admin_submission_tail =
        (uint16_t)((slot + 1U) % controller->admin_depth);
    nvme_admin_ring_submission(controller);

    deadline = nvme_timeout_deadline(controller);
    for (;;) {
        nvme_completion_t completion;
        uint16_t status;
        dma_sync_for_cpu(&controller->admin_completion_dma);
        completion = controller->admin_completion[
            controller->admin_completion_head];
        status = completion.status;
        if ((status & 1U) == controller->admin_phase) {
            kstatus_t result_status;
            *result = completion;
            nvme_record_completion_status(status);
            controller->admin_completion_head = (uint16_t)(
                (controller->admin_completion_head + 1U) % controller->admin_depth);
            if (controller->admin_completion_head == 0U) {
                controller->admin_phase ^= 1U;
            }
            nvme_admin_ring_completion(controller);
            result_status = ((status >> 1) & 0x7FFFU) == 0U ? K_OK : K_EIO;
            nvme_unlock(&controller->admin_lock);
            return result_status;
        }
        if (nvme_deadline_reached(deadline)) {
            nvme_unlock(&controller->admin_lock);
            return K_ETIMEDOUT;
        }
        __asm__ volatile ("pause");
    }
}
