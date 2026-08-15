#pragma once
#include "../../OS_Implementation_Specification_COMPLETE/include/kernel/io.h"

/* 设备移除与用户取消共享同一 CAS 状态机，但保留各自的完成错误码。 */

void io_request_init(io_request_t *req, uint32_t opcode, struct device *device,
                     struct process *process, io_vec_t *vecs, uint32_t vec_count);

/* 设备移除与用户取消共享同一 CAS 状态机，但保留各自的完成错误码。 */
kstatus_t io_cancel_with_status(io_request_t *req, kstatus_t status);
