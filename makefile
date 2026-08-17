TOOLPREFIX ?= x86_64-w64-mingw32-
CC = $(TOOLPREFIX)gcc
LD = $(TOOLPREFIX)gcc
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump
# These programs run in the build environment rather than in LiteOS.  Keep
# them on the WSL/Linux host so `make test` and build-id do not need Wine.
HOSTCC ?= gcc

DEBUG ?= 0
ifeq ($(DEBUG),1)
DEBUG_CFLAGS = -O0 -g3 -fno-omit-frame-pointer
else ifeq ($(DEBUG),2)
# Keep source-level symbols for the interactive debugger while retaining the
# release optimisation level. The graphical shell redraws the full framebuffer
# after pointer/key events, so -O1 makes input look like a stalled guest.
DEBUG_CFLAGS = -O2 -g3 -fno-omit-frame-pointer
else
DEBUG_CFLAGS = -O2
endif

COMMON_CFLAGS = $(DEBUG_CFLAGS) -Wall -Wextra -Werror -ffreestanding -fno-builtin \
                -fno-stack-protector -fno-stack-check -fno-pic -mno-red-zone \
                -mno-sse -mno-mmx -mno-stack-arg-probe -fshort-wchar -Iinclude -MMD -MP
KERNEL_CFLAGS = $(COMMON_CFLAGS) -mabi=sysv -DLITEOS_KERNEL_BUILD
USER_CFLAGS = $(COMMON_CFLAGS) -mabi=sysv -Iuser/audiod -I$(BUILD)/generated
LOADER_LDFLAGS = -nostdlib -Wl,--entry,efi_main -Wl,--subsystem,10 \
                 -Wl,--image-base,0x400000 -Wl,--section-alignment,0x1000 \
                 -Wl,--file-alignment,0x1000 -Wl,--disable-auto-import
KERNEL_LDFLAGS = -nostdlib -Wl,--entry,kernel_entry -Wl,--subsystem,0 \
                 -Wl,--image-base,0xffffffff80000000 -Wl,--section-alignment,0x1000 \
                 -Wl,--file-alignment,0x1000 -Wl,--dynamicbase \
                 -Wl,-Map,$(BUILD)/kernel/kernel.map

BUILD ?= build

FONT_TTF ?= /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
FONT_RASTER_HEIGHT ?= 19
FONT_GENERATOR = $(BUILD)/font12x24-gen
FONT_HEADER = $(BUILD)/generated/font12x24_data.h

# GNU objcopy's binary backend derives symbols from its input pathname and
# replaces every non-alphanumeric separator with an underscore.  Keep the
# spelling in one place so embedded user-program blobs work with an alternate
# BUILD directory as well as the default build/ directory.
binary_input_symbol = $(subst -,_,$(subst .,_,$(subst /,_,$(1))))
LOADER_OBJECTS = $(BUILD)/loader/main.o $(BUILD)/loader/elf.o $(BUILD)/loader/sha256.o \
                 $(BUILD)/loader/rsa.o \
                 $(BUILD)/loader/memory_map.o

KERNEL_PE = $(BUILD)/kernel/kernel.pe
KERNEL_ELF = $(BUILD)/esp/EFI/LITEOS/kernel.elf
KERNEL_BUILD_ID = $(BUILD)/esp/EFI/LITEOS/kernel.build-id
KERNEL_SYMBOLS = $(BUILD)/kernel/kernel.sym
USER_AUDIOD_PE = $(BUILD)/user/audiod.pe
USER_AUDIOD_ELF = $(BUILD)/user/audiod.elf
USER_AUDIOD_OBJECTS = $(BUILD)/user/audiod-service.o $(BUILD)/user/audiod-mixer.o
USER_AUDIOD_LDFLAGS = -nostdlib -Wl,--entry,audiod_entry -Wl,--subsystem,0 \
                     -Wl,--image-base,0x400000 -Wl,--section-alignment,0x1000 \
                     -Wl,--file-alignment,0x1000 -Wl,--disable-auto-import
USER_NETMGR_PE = $(BUILD)/user/netmgr.pe
USER_NETMGR_ELF = $(BUILD)/user/netmgr.elf
USER_NETMGR_LDFLAGS = -nostdlib -Wl,--entry,netmgr_entry -Wl,--subsystem,0 \
                      -Wl,--image-base,0x400000 -Wl,--section-alignment,0x1000 \
                      -Wl,--file-alignment,0x1000 -Wl,--disable-auto-import
USER_NETD_PE = $(BUILD)/user/netd.pe
USER_NETD_ELF = $(BUILD)/user/netd.elf
USER_NETD_LDFLAGS = -nostdlib -Wl,--entry,netd_entry -Wl,--subsystem,0 \
                    -Wl,--image-base,0x400000 -Wl,--section-alignment,0x1000 \
                    -Wl,--file-alignment,0x1000 -Wl,--disable-auto-import

.PHONY: all clean esp loader kernel test header-sanity abi-sanity rsa-test bluetooth-test firmware-test audiod-test release-metadata \
        $(BUILD)/build-id.exe $(BUILD)/sha256-test.exe $(BUILD)/buddy-test.exe \
        $(BUILD)/memory-map-test.exe $(BUILD)/fat32-test.exe $(BUILD)/cache-test.exe \
        $(BUILD)/rsa-test.exe $(BUILD)/bluetooth-test.exe $(BUILD)/firmware-test.exe \
        $(BUILD)/audiod-test.exe $(BUILD)/header-sanity.exe $(BUILD)/abi-sanity.exe

all: esp

$(BUILD)/loader $(BUILD)/kernel $(BUILD)/user $(BUILD)/esp $(BUILD)/esp/EFI/BOOT $(BUILD)/esp/EFI/LITEOS:
	mkdir -p $@

$(BUILD)/generated:
	mkdir -p $@

$(FONT_GENERATOR): tools/font12x24_gen.c | $(BUILD)
	pkg-config --exists freetype2
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror \
		$$(pkg-config --cflags freetype2) $< -o $@ \
		$$(pkg-config --libs freetype2)

$(FONT_HEADER): $(FONT_GENERATOR) $(FONT_TTF) | $(BUILD)/generated
	$(FONT_GENERATOR) "$(FONT_TTF)" "$@.tmp" "$(FONT_RASTER_HEIGHT)"
	mv "$@.tmp" "$@"

$(BUILD)/esp/boot $(BUILD)/esp/lib $(BUILD)/esp/sbin:
	mkdir -p $@

$(BUILD)/loader/%.o: src/%.c | $(BUILD)/loader
	$(CC) $(COMMON_CFLAGS) -c $< -o $@

$(BUILD)/loader/%.o: src/%.S | $(BUILD)/loader
	$(CC) $(COMMON_CFLAGS) -c $< -o $@

$(BUILD)/user/audiod-service.o: user/audiod/service.c | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/audiod-mixer.o: user/audiod/mixer.c | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_AUDIOD_PE): $(USER_AUDIOD_OBJECTS) | $(BUILD)/user
	$(LD) $(USER_AUDIOD_LDFLAGS) $^ -o $@

$(USER_AUDIOD_ELF): $(USER_AUDIOD_PE) | $(BUILD)/user
	$(OBJCOPY) -O elf64-x86-64 $< $@

$(BUILD)/kernel/audiod-blob.o: $(USER_AUDIOD_ELF) | $(BUILD)/kernel
	$(OBJCOPY) -I binary -O pe-x86-64 -B i386:x86-64 \
		--redefine-sym _binary_$(call binary_input_symbol,$(USER_AUDIOD_ELF))_start=liteos_audiod_blob_start \
		--redefine-sym _binary_$(call binary_input_symbol,$(USER_AUDIOD_ELF))_end=liteos_audiod_blob_end $< $@

USER_GSHELL_PE = $(BUILD)/user/gshell.pe
USER_GSHELL_ELF = $(BUILD)/user/gshell.elf
USER_GSHELL_LDFLAGS = -nostdlib -Wl,--entry,gshell_entry -Wl,--subsystem,0 \
                      -Wl,--image-base,0x400000 -Wl,--section-alignment,0x1000 \
                      -Wl,--file-alignment,0x1000 -Wl,--disable-auto-import

$(BUILD)/user/gshell.o: user/gshell/main.c $(FONT_HEADER) | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_GSHELL_PE): $(BUILD)/user/gshell.o | $(BUILD)/user
	$(LD) $(USER_GSHELL_LDFLAGS) $^ -o $@

$(USER_GSHELL_ELF): $(USER_GSHELL_PE) | $(BUILD)/user
	$(OBJCOPY) -O elf64-x86-64 $< $@

$(BUILD)/kernel/gshell-blob.o: $(USER_GSHELL_ELF) | $(BUILD)/kernel
	$(OBJCOPY) -I binary -O pe-x86-64 -B i386:x86-64 \
		--redefine-sym _binary_$(call binary_input_symbol,$(USER_GSHELL_ELF))_start=liteos_gshell_blob_start \
		--redefine-sym _binary_$(call binary_input_symbol,$(USER_GSHELL_ELF))_end=liteos_gshell_blob_end $< $@

USER_NOTEPAD_PE = $(BUILD)/user/notepad.pe
USER_NOTEPAD_ELF = $(BUILD)/user/notepad.elf
USER_NOTEPAD_LDFLAGS = -nostdlib -Wl,--entry,notepad_entry -Wl,--subsystem,0 \
                       -Wl,--image-base,0x400000 -Wl,--section-alignment,0x1000 \
                       -Wl,--file-alignment,0x1000 -Wl,--disable-auto-import

$(BUILD)/user/notepad.o: user/notepad/main.c $(FONT_HEADER) | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_NOTEPAD_PE): $(BUILD)/user/notepad.o | $(BUILD)/user
	$(LD) $(USER_NOTEPAD_LDFLAGS) $^ -o $@

$(USER_NOTEPAD_ELF): $(USER_NOTEPAD_PE) | $(BUILD)/user
	$(OBJCOPY) -O elf64-x86-64 $< $@

$(BUILD)/kernel/notepad-blob.o: $(USER_NOTEPAD_ELF) | $(BUILD)/kernel
	$(OBJCOPY) -I binary -O pe-x86-64 -B i386:x86-64 \
		--redefine-sym _binary_$(call binary_input_symbol,$(USER_NOTEPAD_ELF))_start=liteos_notepad_blob_start \
		--redefine-sym _binary_$(call binary_input_symbol,$(USER_NOTEPAD_ELF))_end=liteos_notepad_blob_end $< $@

USER_FILEMAN_PE = $(BUILD)/user/fileman.pe
USER_FILEMAN_ELF = $(BUILD)/user/fileman.elf
USER_FILEMAN_LDFLAGS = -nostdlib -Wl,--entry,fileman_entry -Wl,--subsystem,0 \
                       -Wl,--image-base,0x400000 -Wl,--section-alignment,0x1000 \
                       -Wl,--file-alignment,0x1000 -Wl,--disable-auto-import

$(BUILD)/user/fileman.o: user/fileman/main.c $(FONT_HEADER) | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_FILEMAN_PE): $(BUILD)/user/fileman.o | $(BUILD)/user
	$(LD) $(USER_FILEMAN_LDFLAGS) $^ -o $@

$(USER_FILEMAN_ELF): $(USER_FILEMAN_PE) | $(BUILD)/user
	$(OBJCOPY) -O elf64-x86-64 $< $@

$(BUILD)/kernel/fileman-blob.o: $(USER_FILEMAN_ELF) | $(BUILD)/kernel
	$(OBJCOPY) -I binary -O pe-x86-64 -B i386:x86-64 \
		--redefine-sym _binary_$(call binary_input_symbol,$(USER_FILEMAN_ELF))_start=liteos_fileman_blob_start \
		--redefine-sym _binary_$(call binary_input_symbol,$(USER_FILEMAN_ELF))_end=liteos_fileman_blob_end $< $@

$(BUILD)/user/netmgr.o: user/netmgr/main.c $(FONT_HEADER) | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_NETMGR_PE): $(BUILD)/user/netmgr.o | $(BUILD)/user
	$(LD) $(USER_NETMGR_LDFLAGS) $^ -o $@

$(USER_NETMGR_ELF): $(USER_NETMGR_PE) | $(BUILD)/user
	$(OBJCOPY) -O elf64-x86-64 $< $@

$(BUILD)/kernel/netmgr-blob.o: $(USER_NETMGR_ELF) | $(BUILD)/kernel
	$(OBJCOPY) -I binary -O pe-x86-64 -B i386:x86-64 \
		--redefine-sym _binary_$(call binary_input_symbol,$(USER_NETMGR_ELF))_start=liteos_netmgr_blob_start \
		--redefine-sym _binary_$(call binary_input_symbol,$(USER_NETMGR_ELF))_end=liteos_netmgr_blob_end $< $@

$(BUILD)/user/netd.o: user/netd/main.c | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_NETD_PE): $(BUILD)/user/netd.o | $(BUILD)/user
	$(LD) $(USER_NETD_LDFLAGS) $^ -o $@

$(USER_NETD_ELF): $(USER_NETD_PE) | $(BUILD)/user
	$(OBJCOPY) -O elf64-x86-64 $< $@

$(BUILD)/kernel/entry.o: kernel/kernel_entry.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/ascii_font.o: kernel/graphics/ascii_font.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/buddy.o: kernel/mm/boot_buddy.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -DLITEOS_CANONICAL_MM_BRIDGE -c $< -o $@

$(BUILD)/kernel/paging.o: kernel/arch/x86_64/boot_paging.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/direct.o: kernel/mm/direct.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/page-db.o: kernel/mm/page_db.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/page-table.o: kernel/mm/page_table.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/kmalloc.o: kernel/mm/kmalloc.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/vmalloc.o: kernel/mm/vmalloc.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/vm-space.o: kernel/mm/vm_space.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/shared-section.o: kernel/core/shared_section.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/cpu.o: kernel/arch/x86_64/cpu.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/interrupt.o: kernel/arch/x86_64/interrupt.S | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/interrupt-core.o: kernel/arch/x86_64/interrupt.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/irq-core.o: kernel/arch/x86_64/irq.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/uaccess-entry.o: kernel/arch/x86_64/uaccess.S | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/uaccess.o: kernel/arch/x86_64/uaccess.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/syscall.o: kernel/arch/x86_64/syscall.S | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/context.o: kernel/arch/x86_64/context.S | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/user-entry.o: kernel/arch/x86_64/user.S | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/apic.o: kernel/arch/x86_64/apic.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/acpi.o: kernel/arch/x86_64/acpi.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/smp.o: kernel/arch/x86_64/smp.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/trampoline.o: kernel/arch/x86_64/trampoline.S | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/tlb.o: kernel/arch/x86_64/tlb.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/timer.o: kernel/arch/x86_64/timer.S | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/stack-check.o: kernel/arch/x86_64/stack-check.S | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/page-fault-entry.o: kernel/arch/x86_64/page_fault.S | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/syscall-core.o: kernel/core/syscall.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/ipc.o: kernel/core/ipc.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/address-space.o: kernel/mm/address_space.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/page-fault-core.o: kernel/mm/page_fault.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/page.o: kernel/mm/page.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/slab.o: kernel/mm/slab.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/object.o: kernel/core/object.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/scheduler.o: kernel/process/scheduler.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/sched-core.o: kernel/sched/core.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/process-core.o: kernel/process/process.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/resource.o: kernel/core/resource.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/audit.o: kernel/core/audit.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/user-elf.o: kernel/process/elf.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/user-init.o: kernel/process/user_init.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/user-test-blob.o: kernel/process/user_test_blob.S | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/user-init-blob.o: kernel/process/init_blob.S | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/user-init-blob.bin: $(BUILD)/kernel/user-init-blob.o | $(BUILD)/kernel
	$(OBJCOPY) -O binary --only-section=.rdata $< $@

$(BUILD)/make-init-image.exe: tools/make_init_image.c | $(BUILD)
	$(HOSTCC) -O2 -Wall -Wextra -Werror $< -o $@

$(BUILD)/make-test-elf.exe: tools/make_test_elf.c | $(BUILD)
	$(HOSTCC) -O2 -Wall -Wextra -Werror $< -o $@

$(BUILD)/kernel/object-core.o: kernel/object/core.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/handle.o: kernel/object/handle.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/wait.o: kernel/sync/wait.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/futex.o: kernel/sync/futex.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/mutex.o: kernel/sync/mutex.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/io.o: kernel/core/io.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/canonical-io.o: kernel/core/canonical_io.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/completion-port.o: kernel/core/completion_port.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/message-port.o: kernel/core/message_port.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/timer-core.o: kernel/core/timer.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/deferred.o: kernel/core/deferred.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/service.o: kernel/core/service.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/watchdog.o: kernel/core/watchdog.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/power.o: kernel/core/power.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/crash-dump.o: kernel/core/crash_dump.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/input-core.o: kernel/core/input.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/gpu-core.o: kernel/core/gpu_core.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/display-core.o: kernel/core/display.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/dma-core.o: kernel/core/dma.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/audio-core.o: kernel/core/audio.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/hda.o: kernel/drivers/hda.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/bluetooth-core.o: kernel/core/bluetooth.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/firmware-core.o: kernel/core/firmware.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/credential-core.o: kernel/core/credential.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/sha256-core.o: kernel/core/sha256.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/rsa.o: src/rsa.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/update-core.o: kernel/core/update.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/update-boot.o: kernel/core/update_boot.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/package.o: kernel/core/package.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/rcu.o: kernel/core/rcu.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/telemetry.o: kernel/core/telemetry.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/perf.o: kernel/core/perf.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/iommu-core.o: kernel/drivers/iommu_core.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/device-core.o: kernel/core/device_core.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/driver.o: kernel/core/driver.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/pci.o: kernel/drivers/pci.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/pci-core.o: kernel/drivers/pci_core.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/nvme.o: kernel/drivers/nvme.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/nvme-core.o: kernel/drivers/nvme_core.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/e1000.o: kernel/drivers/e1000.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/security.o: kernel/core/security.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/gpu.o: kernel/drivers/gpu.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/usb.o: kernel/drivers/usb.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci.o: kernel/drivers/xhci.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/vfs.o: kernel/fs/vfs.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/block.o: kernel/fs/block.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/block-core.o: kernel/fs/block_core.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/fat32.o: kernel/fs/fat32.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/cache.o: kernel/fs/cache.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/journal.o: kernel/fs/journal.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/litefs.o: kernel/fs/litefs.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/window.o: kernel/graphics/window.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/window-server.o: kernel/graphics/window_server.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/net-core.o: kernel/net/core.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/net-manager.o: kernel/net/manager.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/socket.o: kernel/net/socket.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $@

loader: $(BUILD)/esp/EFI/BOOT/BOOTX64.EFI

$(BUILD)/esp/EFI/BOOT/BOOTX64.EFI: $(LOADER_OBJECTS) | $(BUILD)/esp/EFI/BOOT
	$(LD) $(LOADER_LDFLAGS) $(LOADER_OBJECTS) -o $@

kernel: $(KERNEL_ELF)

$(KERNEL_PE): $(BUILD)/kernel/entry.o $(BUILD)/kernel/ascii_font.o \
                                    $(BUILD)/kernel/buddy.o $(BUILD)/kernel/paging.o \
                                    $(BUILD)/kernel/direct.o $(BUILD)/kernel/page-db.o \
                                    $(BUILD)/kernel/page-table.o \
                                    $(BUILD)/kernel/kmalloc.o $(BUILD)/kernel/vmalloc.o \
                                    $(BUILD)/kernel/vm-space.o $(BUILD)/kernel/shared-section.o \
                                    $(BUILD)/kernel/cpu.o $(BUILD)/kernel/interrupt.o \
                                    $(BUILD)/kernel/interrupt-core.o \
                                    $(BUILD)/kernel/irq-core.o \
                                    $(BUILD)/kernel/uaccess-entry.o $(BUILD)/kernel/uaccess.o \
                                    $(BUILD)/kernel/syscall.o $(BUILD)/kernel/syscall-core.o \
                                    $(BUILD)/kernel/context.o \
                                    $(BUILD)/kernel/user-entry.o \
                                    $(BUILD)/kernel/apic.o $(BUILD)/kernel/acpi.o \
                                    $(BUILD)/kernel/smp.o $(BUILD)/kernel/trampoline.o \
                                    $(BUILD)/kernel/tlb.o \
                                    $(BUILD)/kernel/timer.o \
                                    $(BUILD)/kernel/stack-check.o \
                                    $(BUILD)/kernel/page-fault-entry.o \
                                    $(BUILD)/kernel/ipc.o \
                                    $(BUILD)/kernel/address-space.o \
                                    $(BUILD)/kernel/page-fault-core.o \
                                    $(BUILD)/kernel/page.o $(BUILD)/kernel/slab.o \
                                    $(BUILD)/kernel/object.o $(BUILD)/kernel/scheduler.o \
                                    $(BUILD)/kernel/sched-core.o \
                                    $(BUILD)/kernel/process-core.o \
                                    $(BUILD)/kernel/resource.o \
                                    $(BUILD)/kernel/audit.o \
                                    $(BUILD)/kernel/user-elf.o \
                                    $(BUILD)/kernel/user-init.o \
                                    $(BUILD)/kernel/user-test-blob.o \
                                    $(BUILD)/kernel/object-core.o $(BUILD)/kernel/handle.o \
                                    $(BUILD)/kernel/wait.o $(BUILD)/kernel/futex.o \
                                    $(BUILD)/kernel/mutex.o \
                                    $(BUILD)/kernel/io.o $(BUILD)/kernel/canonical-io.o \
                                    $(BUILD)/kernel/completion-port.o \
                                    $(BUILD)/kernel/message-port.o \
                                    $(BUILD)/kernel/timer-core.o \
                                    $(BUILD)/kernel/deferred.o $(BUILD)/kernel/service.o \
                                    $(BUILD)/kernel/watchdog.o \
                                    $(BUILD)/kernel/power.o \
                                    $(BUILD)/kernel/crash-dump.o \
                                    $(BUILD)/kernel/input-core.o \
                                    $(BUILD)/kernel/gpu-core.o $(BUILD)/kernel/display-core.o \
                                    $(BUILD)/kernel/dma-core.o $(BUILD)/kernel/audio-core.o \
                                    $(BUILD)/kernel/hda.o \
                                    $(BUILD)/kernel/bluetooth-core.o \
                                    $(BUILD)/kernel/credential-core.o \
                                    $(BUILD)/kernel/sha256-core.o \
                                    $(BUILD)/kernel/rsa.o \
                                    $(BUILD)/kernel/update-core.o \
                                    $(BUILD)/kernel/update-boot.o \
                                    $(BUILD)/kernel/package.o \
                                    $(BUILD)/kernel/firmware-core.o \
                                    $(BUILD)/kernel/rcu.o \
                                    $(BUILD)/kernel/telemetry.o \
                                    $(BUILD)/kernel/perf.o \
                                    $(BUILD)/kernel/iommu-core.o \
                                    $(BUILD)/kernel/device-core.o \
                                    $(BUILD)/kernel/driver.o \
                                    $(BUILD)/kernel/pci.o $(BUILD)/kernel/pci-core.o \
                                    $(BUILD)/kernel/nvme.o $(BUILD)/kernel/nvme-core.o \
                                    $(BUILD)/kernel/e1000.o \
                                    $(BUILD)/kernel/security.o $(BUILD)/kernel/gpu.o \
                                    $(BUILD)/kernel/usb.o $(BUILD)/kernel/xhci.o \
                                    $(BUILD)/kernel/vfs.o $(BUILD)/kernel/block.o \
                                    $(BUILD)/kernel/block-core.o \
                                    $(BUILD)/kernel/fat32.o $(BUILD)/kernel/cache.o \
                                    $(BUILD)/kernel/journal.o \
                                    $(BUILD)/kernel/litefs.o \
                                    $(BUILD)/kernel/window.o \
                                    $(BUILD)/kernel/window-server.o \
                                    $(BUILD)/kernel/net-core.o \
                                    $(BUILD)/kernel/net-manager.o \
                                    $(BUILD)/kernel/socket.o | \
                                    $(BUILD)/kernel
	$(LD) $(KERNEL_LDFLAGS) $^ -o $@

$(KERNEL_ELF): $(KERNEL_PE) | $(BUILD)/esp/EFI/LITEOS
	$(OBJCOPY) -O elf64-x86-64 $< $@

$(BUILD)/build-id.exe: tools/build_id.c src/sha256.c | $(BUILD)
	$(HOSTCC) -O2 -Wall -Wextra -Werror -Iinclude $^ -o $@

$(KERNEL_BUILD_ID): $(KERNEL_ELF) $(BUILD)/build-id.exe | $(BUILD)/esp/EFI/LITEOS
	$(BUILD)/build-id.exe $(KERNEL_ELF) $@

$(KERNEL_SYMBOLS): $(KERNEL_ELF) | $(BUILD)/kernel
	$(OBJDUMP) -t $< > $@

$(BUILD)/esp/EFI/LITEOS/loader.conf: loader.conf.example | $(BUILD)/esp/EFI/LITEOS
	cp $< $@

INIT_SERVICE_OFFSET = $(shell $(OBJDUMP) -t $(BUILD)/kernel/user-init-blob.o 2>/dev/null | \
	awk '$$NF == "liteos_init_service" { print $$9; exit }')

$(BUILD)/esp/init: $(BUILD)/kernel/user-init-blob.bin $(BUILD)/make-init-image.exe | $(BUILD)/esp
	$(BUILD)/make-init-image.exe $< $@ 0 $(INIT_SERVICE_OFFSET)

$(BUILD)/esp/init-runtime: $(BUILD)/kernel/user-init-blob.bin $(BUILD)/make-init-image.exe | $(BUILD)/esp
	$(BUILD)/make-init-image.exe $< $@ 1 $(INIT_SERVICE_OFFSET)

$(BUILD)/esp/sbin/deviced: $(BUILD)/esp/init | $(BUILD)/esp/sbin
	cp $< $@

$(BUILD)/esp/sbin/logd: $(BUILD)/esp/init | $(BUILD)/esp/sbin
	cp $< $@

$(BUILD)/esp/sbin/crashd: $(BUILD)/esp/init | $(BUILD)/esp/sbin
	cp $< $@

$(BUILD)/esp/boot/exec-exit.elf: $(BUILD)/make-test-elf.exe | $(BUILD)/esp/boot
	$(BUILD)/make-test-elf.exe $@ exec

$(BUILD)/esp/lib/ld-liteos.so.1: $(BUILD)/make-test-elf.exe | $(BUILD)/esp/lib
	$(BUILD)/make-test-elf.exe $@ interp

$(BUILD)/esp/sbin/gshell: $(USER_GSHELL_ELF) | $(BUILD)/esp/sbin
	cp $< $@

$(BUILD)/esp/sbin/notepad: $(USER_NOTEPAD_ELF) | $(BUILD)/esp/sbin
	cp $< $@

$(BUILD)/esp/sbin/fileman: $(USER_FILEMAN_ELF) | $(BUILD)/esp/sbin
	cp $< $@

$(BUILD)/esp/sbin/fm: $(USER_FILEMAN_ELF) | $(BUILD)/esp/sbin
	cp $< $@

$(BUILD)/esp/sbin/audiod: $(USER_AUDIOD_ELF) | $(BUILD)/esp/sbin
	cp $< $@

$(BUILD)/esp/sbin/netmgr: $(USER_NETMGR_ELF) | $(BUILD)/esp/sbin
	cp $< $@

$(BUILD)/esp/sbin/netd: $(USER_NETD_ELF) | $(BUILD)/esp/sbin
	cp $< $@

esp: $(BUILD)/esp/EFI/BOOT/BOOTX64.EFI $(KERNEL_ELF) \
     $(BUILD)/esp/EFI/LITEOS/loader.conf $(KERNEL_BUILD_ID) $(KERNEL_SYMBOLS) \
     $(BUILD)/esp/init $(BUILD)/esp/init-runtime \
     $(BUILD)/esp/boot/exec-exit.elf $(BUILD)/esp/lib/ld-liteos.so.1 \
     $(BUILD)/esp/sbin/deviced $(BUILD)/esp/sbin/logd \
     $(BUILD)/esp/sbin/crashd $(BUILD)/esp/sbin/gshell \
     $(BUILD)/esp/sbin/notepad $(BUILD)/esp/sbin/fileman \
     $(BUILD)/esp/sbin/fm $(BUILD)/esp/sbin/audiod \
     $(BUILD)/esp/sbin/netmgr $(BUILD)/esp/sbin/netd
	@echo ESP image prepared at $(BUILD)/esp

release-metadata: $(KERNEL_BUILD_ID) $(KERNEL_SYMBOLS)
	@echo Release metadata prepared: $(KERNEL_BUILD_ID) and $(KERNEL_SYMBOLS)

$(BUILD)/sha256-test.exe: tests/sha256_test.c src/sha256.c | $(BUILD)
	$(HOSTCC) -O2 -Wall -Wextra -Werror -Iinclude $^ -o $@

$(BUILD)/buddy-test.exe: tests/buddy_test.c kernel/mm/boot_buddy.c | $(BUILD)
	$(HOSTCC) -O2 -Wall -Wextra -Werror -Iinclude $^ -o $@

$(BUILD)/memory-map-test.exe: tests/memory_map_test.c src/memory_map.c | $(BUILD)
	$(HOSTCC) -O2 -Wall -Wextra -Werror -Iinclude $^ -o $@

$(BUILD)/fat32-test.exe: tests/fat32_test.c kernel/fs/block.c kernel/fs/cache.c \
                          kernel/fs/fat32.c kernel/fs/vfs.c kernel/core/security.c | $(BUILD)
	$(HOSTCC) -O2 -Wall -Wextra -Werror -Iinclude $^ -o $@

$(BUILD)/cache-test.exe: tests/cache_test.c kernel/fs/block.c kernel/fs/cache.c | $(BUILD)
	$(HOSTCC) -O2 -Wall -Wextra -Werror -Iinclude $^ -o $@

$(BUILD)/rsa-test.exe: tests/rsa_test.c src/rsa.c | $(BUILD)
	$(HOSTCC) -O2 -Wall -Wextra -Werror -mno-stack-arg-probe -Iinclude $^ -o $@

rsa-test: $(BUILD)/rsa-test.exe
	$(BUILD)/rsa-test.exe

$(BUILD)/bluetooth-test.exe: tests/bluetooth_test.c kernel/core/bluetooth.c | $(BUILD)
	$(HOSTCC) -O2 -Wall -Wextra -Werror -Iinclude $^ -o $@

bluetooth-test: $(BUILD)/bluetooth-test.exe
	$(BUILD)/bluetooth-test.exe

$(BUILD)/firmware-test.exe: tests/firmware_test.c kernel/core/firmware.c \
                            kernel/core/sha256.c | $(BUILD)
	$(HOSTCC) -O2 -Wall -Wextra -Werror -Iinclude $^ -o $@

firmware-test: $(BUILD)/firmware-test.exe
	$(BUILD)/firmware-test.exe

$(BUILD)/audiod-test.exe: tests/audiod_test.c user/audiod/mixer.c | $(BUILD)
	$(HOSTCC) -O2 -Wall -Wextra -Werror -Iinclude -Iuser/audiod $^ -o $@

audiod-test: $(BUILD)/audiod-test.exe
	$(BUILD)/audiod-test.exe

$(BUILD)/header-sanity.exe: tools/header_sanity.c | $(BUILD)
	$(HOSTCC) -std=c11 -ffreestanding -Wall -Wextra -Werror -Iinclude $< -o $@

header-sanity: $(BUILD)/header-sanity.exe
	$(BUILD)/header-sanity.exe

$(BUILD)/abi-sanity.exe: tools/abi_sanity.c | $(BUILD)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -Iinclude $< -o $@

abi-sanity: $(BUILD)/abi-sanity.exe
	$(BUILD)/abi-sanity.exe

test: $(BUILD)/sha256-test.exe $(BUILD)/buddy-test.exe $(BUILD)/memory-map-test.exe \
      $(BUILD)/fat32-test.exe $(BUILD)/cache-test.exe $(BUILD)/rsa-test.exe \
      $(BUILD)/bluetooth-test.exe $(BUILD)/firmware-test.exe $(BUILD)/audiod-test.exe \
      $(BUILD)/abi-sanity.exe
	$(BUILD)/sha256-test.exe
	$(BUILD)/buddy-test.exe
	$(BUILD)/memory-map-test.exe
	$(BUILD)/fat32-test.exe
	$(BUILD)/cache-test.exe
	$(BUILD)/rsa-test.exe
	$(BUILD)/bluetooth-test.exe
	$(BUILD)/firmware-test.exe
	$(BUILD)/audiod-test.exe
	$(BUILD)/abi-sanity.exe

clean:
	rm -rf $(BUILD)

# 让头文件修改自动使相关 Loader/Kernel 对象失效，避免 ABI 结构体错位。 
-include $(wildcard $(BUILD)/loader/*.d) $(wildcard $(BUILD)/kernel/*.d) \
         $(wildcard $(BUILD)/user/*.d)
