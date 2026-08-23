#pragma once

#include <kernel/base.h>

/*
 * QEMU Standard VGA / Bochs VBE hidden-VRAM page flip.
 *
 * This backend is optional. On non-QEMU hardware the compositor keeps the
 * existing GOP publication path unchanged.
 */
bool qemu_stdvga_flip_init(uint64_t framebuffer_physical,
                           uint64_t framebuffer_virtual,
                           uint64_t framebuffer_size);

bool qemu_stdvga_hardware_present(void);
bool qemu_stdvga_flip_available(void);

volatile uint32_t *qemu_stdvga_front_buffer(void);
volatile uint32_t *qemu_stdvga_back_buffer(void);

/*
 * All WC writes into qemu_stdvga_back_buffer() must be complete before this
 * call. The function executes a final local SFENCE before changing Y_OFFSET.
 */
bool qemu_stdvga_flip(void);

uint32_t qemu_stdvga_last_error(void);
