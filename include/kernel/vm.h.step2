#pragma once
#include "../../OS_Implementation_Specification_COMPLETE/include/kernel/vm.h"

/* 规范 VM 核心的构造、引用和 COW 克隆扩展。 */
kstatus_t vm_object_create_anon(size_t size, vm_object_t **out);
kstatus_t vm_object_create_shared(size_t size, vm_object_t **out);
kstatus_t vm_object_create_file(struct vnode *vnode, uint64_t file_offset,
                                size_t size, vm_object_t **out);
kstatus_t vm_object_create_device(paddr_t phys, uint64_t length,
                                  uint32_t cache_mode, void *private_data,
                                  void (*private_release)(void *private_data),
                                  vm_object_t **out);
void vm_object_get(vm_object_t *object);
void vm_object_put(vm_object_t *object);
kstatus_t vm_space_clone_cow(vm_space_t *source, vm_space_t **out);
