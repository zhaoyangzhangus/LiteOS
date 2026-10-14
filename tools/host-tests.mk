# Host-side test compilation, validation entrypoints and regression suites.
# Support direct invocation as well as inclusion from the main makefile.
BUILD ?= build
HOSTCC ?= cc
ifeq ($(strip $(BUILD)),)
$(error BUILD must not be empty)
endif
ifeq ($(strip $(HOSTCC)),)
$(error HOSTCC must not be empty)
endif
# With multiple translation units, one -MF file would only describe the last
# unit. Emit all dependency rules to stdout and publish after a successful link.
# Argument 2 overrides translation units for fixtures that include a .c file.
define HOST_TEST_LINK
@set -eu; \
temporary=''; dependencies=''; \
trap 'trap "" HUP INT TERM; if [ -n "$$temporary" ]; then rm -f -- "$$temporary"; fi; if [ -n "$$dependencies" ]; then rm -f -- "$$dependencies"; fi' 0; \
trap 'exit 129' HUP; trap 'exit 130' INT; trap 'exit 143' TERM; \
temporary="$$(mktemp "$@.tmp.XXXXXX")"; \
dependencies="$$(mktemp "$@.d.tmp.XXXXXX")"; \
printf 'HOST_TEST_LINK %s\n' '$@'; \
$(HOSTCC) $(1) -MM -MP -MT $@ $(if $(2),$(2),$(filter %.c,$^)) > "$$dependencies"; \
$(HOSTCC) $(1) $(if $(2),$(2),$(filter %.c,$^)) -o "$$temporary"; \
mv -f -- "$$temporary" '$@'; \
mv -f -- "$$dependencies" '$@.d'
endef

$(BUILD)/sha256-test.exe: tests/kernel/sha256_test.c boot/uefi/sha256.c | $(BUILD)
	$(call HOST_TEST_LINK,-O2 -Wall -Wextra -Werror -Iinclude)

$(BUILD)/mm-api-test.exe: tests/kernel/mm_api_test.c | $(BUILD)
	$(call HOST_TEST_LINK,-std=c11 -O2 -Wall -Wextra -Werror -Iinclude)

$(BUILD)/memory-map-test.exe: tests/kernel/memory_map_test.c boot/uefi/memory_map.c | $(BUILD)
	$(call HOST_TEST_LINK,-O2 -Wall -Wextra -Werror -Iinclude)

$(BUILD)/fat32-test.exe: tests/kernel/fat32_test.c kernel/block/bio.c kernel/fs/pagecache/cache.c \
                          kernel/fs/nativefs/fat32.c kernel/fs/nativefs/directory_codec.c \
                          kernel/fs/nativefs/fat_table.c kernel/fs/nativefs/transaction.c \
                          kernel/fs/nativefs/directory_lifecycle.c \
                          kernel/fs/nativefs/file_lifecycle.c | $(BUILD)
	$(call HOST_TEST_LINK,-O2 -Wall -Wextra -Werror -Iinclude)

$(BUILD)/cache-test.exe: tests/kernel/cache_test.c kernel/block/bio.c kernel/fs/pagecache/cache.c | $(BUILD)
	$(call HOST_TEST_LINK,-O2 -Wall -Wextra -Werror -Iinclude)

$(BUILD)/primitives-test.exe: tests/kernel/primitives_test.c | $(BUILD)
	$(call HOST_TEST_LINK,-std=c11 -O2 -Wall -Wextra -Werror -Iinclude)

primitives-test: $(BUILD)/primitives-test.exe
	./$(BUILD)/primitives-test.exe

.PHONY: vulkan-probe-test
vulkan-probe-test:
	python3 tests/user/vulkan_render_pixels_test.py
	python3 tests/user/vulkan_render_api_test.py
	python3 tests/user/vulkan_shared_api_test.py
	python3 tests/user/vulkan_import_lifetime_test.py
	python3 tests/user/vulkan_probe_test.py
	python3 tests/user/vulkan_runtime_abi_test.py
	python3 tests/user/vulkan_stage_test.py

.PHONY: vulkan-video-test
.PHONY: chrome-vulkan-deployment-test
# This integration gate intentionally requires the canonical guest runtime;
# keep it separate from source-only host tests which need no deployed ESP.
chrome-vulkan-deployment-test: chrome-start-test $(DYNAMIC_CHROME_START_ELF)
	python3 tests/user/chrome_staging_build_gate_test.py
	python3 tests/user/chrome_stage_dependencies_test.py
	python3 tests/user/chrome_vulkan_runtime_gate_test.py

.PHONY: chrome-vulkan-integration-test
# CHROMIUM_SRC selects the same canonical checkout; these checks do not
# regenerate GN or replace anything in the live QEMU image.
chrome-vulkan-integration-test: chrome-vulkan-deployment-test
	python3 tests/user/chrome_guest_library_policy_test.py
	python3 tests/user/chrome_glibc_toolchain_test.py
	python3 tests/user/chrome_vulkan_backend_test.py
	python3 tests/user/chrome_vulkan_canvas_test.py
	python3 tests/user/chrome_surface_lifetime_test.py
	python3 tests/user/chrome_mutex_priority_retry_test.py
	python3 tests/user/chrome_raw_clock_test.py
	python3 tests/user/chrome_pinned_headers_test.py
	python3 tests/user/chrome_surface_abi_test.py
	python3 tests/user/chrome_vulkan_only_context_test.py
	python3 tests/user/chrome_vulkan_compositor_policy_test.py
	python3 tests/user/chrome_vulkan_raster_policy_test.py
	python3 tests/user/chrome_vulkan_renderer_profile_test.py
	python3 tests/user/chrome_vulkan_shared_image_stub_test.py
	python3 tests/user/chrome_vulkan_display_policy_test.py
	python3 tests/user/chrome_vulkan_inprocess_test.py
	python3 tests/user/chrome_vulkan_compile_test.py --runtime glibc --vulkan-only-context

vulkan-video-test: vulkan-probe-test
	python3 tests/user/vulkan_video_cache_test.py
	python3 tests/user/vulkan_video_download_test.py
	python3 tests/user/vulkan_video_bitstream_padding_test.py
	python3 tests/user/vulkan_video_hardware_format_test.py
	python3 tests/user/vulkan_video_decoder_lifecycle_test.py
	python3 tests/user/vulkan_video_frame_layout_test.py
	python3 tests/user/vulkan_video_queue_selection_test.py
	python3 tests/user/vulkan_video_query_test.py
	python3 tests/user/vulkan_video_query_status_test.py
	python3 tests/user/vulkan_video_query_patch_test.py

$(BUILD)/bluetooth-test.exe: tests/kernel/bluetooth_test.c kernel/drivers/bluetooth/core.c | $(BUILD)
	$(call HOST_TEST_LINK,-O2 -Wall -Wextra -Werror -Iinclude)

bluetooth-test: $(BUILD)/bluetooth-test.exe
	./$(BUILD)/bluetooth-test.exe

$(BUILD)/firmware-test.exe: tests/kernel/firmware_test.c kernel/drivers/core/firmware.c | $(BUILD)
	$(call HOST_TEST_LINK,-O2 -Wall -Wextra -Werror -Iinclude)

firmware-test: $(BUILD)/firmware-test.exe
	./$(BUILD)/firmware-test.exe

$(BUILD)/audiod-test.exe: tests/kernel/audiod_test.c user/services/audiod/mixer.c | $(BUILD)
	$(call HOST_TEST_LINK,-O2 -Wall -Wextra -Werror -Iinclude -Iuser/services/audiod)

audiod-test: $(BUILD)/audiod-test.exe
	./$(BUILD)/audiod-test.exe

$(BUILD)/wait-subscription-test.exe: tests/kernel/wait_subscription_test.c \
		tests/kernel/wait_subscription_host.c kernel/sync/wait.c \
		kernel/object/object.c | $(BUILD)
	$(call HOST_TEST_LINK,-std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DOS_WAIT_HOST_TEST -Iinclude)

$(BUILD)/vfs-user-api-test.exe: tests/kernel/vfs_user_api_test.c \
		kernel/fs/vfs/user_api.c kernel/fs/vfs/internal.h | $(BUILD)
	$(call HOST_TEST_LINK,-std=gnu11 -O2 -Wall -Wextra -Werror -Iinclude)

wait-subscription-test: $(BUILD)/wait-subscription-test.exe
	./$(BUILD)/wait-subscription-test.exe

$(BUILD)/frame-trace-disabled-test.exe: tests/kernel/frame_trace_gate_test.c | $(BUILD)
	$(call HOST_TEST_LINK,-std=c11 -O2 -Wall -Wextra -Werror -Iinclude)

$(BUILD)/frame-trace-enabled-test.exe: tests/kernel/frame_trace_gate_test.c | $(BUILD)
	$(call HOST_TEST_LINK,-std=c11 -O2 -Wall -Wextra -Werror -DOS_ENABLE_FRAME_TRACE=1 -Iinclude)

frame-trace-test: $(BUILD)/frame-trace-disabled-test.exe \
		$(BUILD)/frame-trace-enabled-test.exe
	./$(BUILD)/frame-trace-disabled-test.exe
	./$(BUILD)/frame-trace-enabled-test.exe

.PHONY: chromium-source-test
chromium-source-test:
	python3 tests/run_chromium_source_tests.py

$(BUILD)/chrome-start-test.exe: tests/user/chrome_start_test.c \
		user/runtime/chrome_start.c | $(BUILD)
	$(call HOST_TEST_LINK,-std=c11 -O2 -Wall -Wextra -Werror -Iuser/libc/include -Iinclude,$<)

chrome-start-test: $(BUILD)/chrome-start-test.exe
	./$(BUILD)/chrome-start-test.exe

$(BUILD)/libc-personality-test.exe: tests/user/libc_personality_test.c \
		user/libc/personality.c | $(BUILD)
	$(call HOST_TEST_LINK,-std=c11 -O2 -Wall -Wextra -Werror -Iuser/libc/include -Iinclude)

$(BUILD)/header-sanity.exe: tools/header_sanity.c | $(BUILD)
	$(call HOST_TEST_LINK,-std=c11 -ffreestanding -Wall -Wextra -Werror -Iinclude)

header-sanity: $(BUILD)/header-sanity.exe
	./$(BUILD)/header-sanity.exe

STAGE_VERIFY_MODE ?= strict
stage-sanity: roadmap-stage-layout
	@case "$(STAGE_VERIFY_MODE)" in \
		strict) ./tools/verify-qemu-stages.sh "$(STAGE_LOG)" ;; \
		interactive) ./tools/verify-qemu-stages.sh --interactive "$(STAGE_LOG)" ;; \
		*) echo 'stage-sanity: STAGE_VERIFY_MODE must be strict or interactive' >&2; exit 2 ;; \
	esac

$(BUILD)/abi-sanity.exe: tools/abi_sanity.c | $(BUILD)
	$(call HOST_TEST_LINK,-std=c11 -Wall -Wextra -Werror -Iinclude)

abi-sanity: $(BUILD)/abi-sanity.exe
	./$(BUILD)/abi-sanity.exe

.PHONY: host-regression-test graphics-regression-test nvidia-deployment-test runtime-indirect-relocation-test

$(BUILD)/runtime-indirect-relocation-test.exe: tests/user/runtime_indirect_relocation_test.c \
    user/runtime/ld.c user/runtime/elf64.h | $(BUILD)
	$(call HOST_TEST_LINK,-std=c11 -O2 -fno-builtin -Wall -Wextra -Werror -Iinclude -Iuser/runtime,tests/user/runtime_indirect_relocation_test.c)

runtime-indirect-relocation-test: $(BUILD)/runtime-indirect-relocation-test.exe
	./$(BUILD)/runtime-indirect-relocation-test.exe
	python3 tests/user/runtime_static_tls_test.py
HOST_INCREMENTAL_TESTS = $(addprefix $(BUILD)/,mm-api-test.exe primitives-test.exe \
    header-sanity.exe abi-sanity.exe frame-trace-disabled-test.exe frame-trace-enabled-test.exe \
    sha256-test.exe memory-map-test.exe fat32-test.exe cache-test.exe bluetooth-test.exe \
    firmware-test.exe audiod-test.exe wait-subscription-test.exe vfs-user-api-test.exe \
    chrome-start-test.exe libc-personality-test.exe nvidia-port-test.exe runtime-indirect-relocation-test.exe)
$(HOST_INCREMENTAL_TESTS): $(BUILD)/.host-toolchain makefile tools/host-tests.mk
$(NVIDIA_PORT_HOST_OBJECTS) $(BUILD)/nvidia/host-test/nv_port_test.o: $(BUILD)/.host-toolchain makefile tools/host-tests.mk

# Keep graphics regressions in one ordered suite. Each script compiles actual
# production code in a disposable fixture; none starts another QEMU or build.
HOST_GRAPHICS_REGRESSIONS = input_batch input_motion_coalesce input_motion_eviction \
    input_pending_publication compositor_alpha compositor_present_wake window_surface_exchange window_surface_backing window_surface_snapshot window_surface_api linux_window_surface \
    compositor_publication_preempt compositor_tile_index tile_geometry \
    window_drag_move window_fullscreen standard_caption window_event_queue window_event_read window_event_wake_batch \
    window_popup_resize window_resize_cursor window_resize_geometry window_corner_alpha \
    native_window_cleanup desktop_icon_clip desktop_icon_filter

graphics-regression-test:
	@set -eu; for name in $(HOST_GRAPHICS_REGRESSIONS); do \
	    printf 'GRAPHICS_REGRESSION %s\n' "$$name"; \
	    python3 "tests/kernel/$${name}_test.py"; \
	done

.PHONY: fileman-regression-test
fileman-regression-test:
	python3 tests/user/fileman_damage_regions_test.py
	python3 tests/user/fileman_damage_culling_test.py
	python3 tests/user/fileman_text_damage_test.py

host-regression-test: graphics-regression-test runtime-indirect-relocation-test fileman-regression-test
	python3 tests/user/rtl8126_firmware_stage_test.py
	python3 tests/kernel/cube_result_log_test.py
	python3 tests/kernel/nvidia_event_alloc_test.py
	python3 tests/kernel/rtl8126_autoneg_test.py
	python3 tests/kernel/gop_cursor_publication_test.py
	python3 tests/user/netd_cold_link_retry_test.py
	python3 tests/user/netd_persistent_log_test.py
	python3 tests/kernel/nv_port_rwlock_wait_test.py
	python3 tests/user/qemu_device_names_test.py
	python3 tests/user/tool_configuration_namespace_test.py
	python3 tests/user/guest_failure_gate_test.py
	python3 tests/user/build_report_namespace_test.py
	python3 tests/user/nvidia_stage_namespace_test.py
	python3 tests/user/object_list_paths_test.py
	python3 tests/user/chrome_widget_stage_gate_test.py
	python3 tests/kernel/arch_entry_namespace_test.py
	python3 tests/kernel/runtime_blob_namespace_test.py
	python3 tests/kernel/window_device_namespace_test.py
	python3 tests/user/runtime_elf_namespace_test.py
	python3 tests/user/elf_runtime_verifier_test.py
	python3 tests/user/libc_namespace_test.py
	python3 tests/user/libc_type_namespace_test.py
	python3 tests/user/libc_statfs_layout_test.py
	python3 tests/user/libc_sendfile_test.py
	python3 tests/user/native_diagnostic_names_test.py
	python3 tests/user/freestanding_platform_test.py
	python3 tests/user/adapter_paths_test.py
	python3 tests/user/native_entry_namespace_test.py
	python3 tests/user/qemu_cpu_model_test.py
	python3 tests/user/qemu_prepare_only_test.py
	python3 tests/user/qemu_vfio_preflight_test.py
	python3 tests/user/guest_memory_reader_test.py
	python3 tests/user/video_readback_comparison_test.py
	python3 tests/user/qemu_proxy_path_test.py
	python3 tests/kernel/build_id_cache_test.py
	python3 tests/kernel/host_test_cache_test.py
	python3 tests/kernel/host_multi_source_cache_test.py
	python3 tests/kernel/chrome_staging_test.py
	python3 tests/user/chrome_stage_fixed_mode_test.py
	python3 tests/user/chrome_staging_build_gate_test.py
	python3 tests/user/chrome_entrypoints_test.py
	python3 tests/user/chrome_stage_dependencies_test.py
	python3 tests/user/chrome_incremental_builder_test.py
	python3 tests/user/chrome_builder_sdk_guard_test.py
	python3 tests/user/chromium_source_config_test.py
	python3 tests/user/chrome_trace_build_guard_test.py
	python3 tests/user/debug_socket_namespace_test.py
	python3 tests/user/chrome_debugger_cleanup_test.py
	python3 tests/user/chrome_trace_breakpoint_test.py
	python3 tests/kernel/cube_present_test.py
	python3 tests/kernel/cube_diagnostic_test.py
	python3 tests/kernel/triangle_diagnostic_test.py
	python3 tests/kernel/nvidia_probe_namespace_test.py
	python3 tests/kernel/fat_chain_tail_test.py
	python3 tests/kernel/fat_snapshot_cycle_test.py
	python3 tests/user/service_diagnostic_test.py
	python3 tests/kernel/qmp_client_test.py
	python3 tests/kernel/qemu_frame_test.py
	python3 tests/kernel/qemu_stream_logging_test.py
	python3 tests/kernel/qemu_repair_regions_test.py
	python3 tests/kernel/qemu_page_transaction_test.py
	python3 tests/kernel/rect_extent_clip_test.py
	python3 tests/kernel/vfs_unlink_snapshot_test.py
	python3 tests/kernel/fat_typed_remove_test.py
	python3 tests/kernel/vfs_remove_scan_test.py
	python3 tests/kernel/vfs_fat_lookup_scan_test.py
	python3 tests/kernel/vfs_fsync_identity_test.py
	python3 tests/kernel/vfs_cached_open_test.py
	python3 tests/kernel/vfs_open_metadata_test.py
	python3 tests/kernel/vfs_idle_handle_test.py
	python3 tests/kernel/vfs_vnode_preempt_test.py
	python3 tests/kernel/vfs_rename_replace_test.py
	python3 tests/kernel/vfs_symlink_test.py
	python3 tests/kernel/vfs_symlink_resolve_test.py
	python3 tests/kernel/vfs_subtree_match_test.py
	python3 tests/user/gshell_foreground_test.py
	python3 tests/user/wget_request_test.py
	python3 tests/user/system_identity_test.py
	python3 tests/user/graphics_namespace_test.py
	python3 tests/kernel/boot_layout_test.py
	python3 tests/kernel/storage_layout_test.py
	python3 tests/kernel/kernel_protocol_test.py
	python3 tests/kernel/stage_log_protocol_test.py
	python3 tests/kernel/console_printf_test.py
	python3 tests/kernel/nvidia_workqueue_test.py
	python3 tests/kernel/arch_diagnostic_names_test.py
	python3 tests/user/exec_fd_map_test.py
	python3 tests/user/loader_diagnostic_test.py
	python3 tests/kernel/nvidia_diagnostic_protocol_test.py
	python3 tests/kernel/nvidia_diagnostic_budget_test.py
	python3 tests/kernel/nvidia_string_compare_test.py
	python3 tests/kernel/init_core_diagnostic_test.py
	python3 tests/kernel/stage_verifier_modes_test.py
	python3 tests/kernel/network_diagnostic_test.py
	python3 tests/kernel/socket_diagnostic_test.py
	python3 tests/kernel/vm_diagnostic_test.py
	python3 tests/kernel/perf_diagnostic_test.py
	python3 tests/kernel/process_diagnostic_test.py
	python3 tests/kernel/graphics_diagnostic_test.py
	python3 tests/kernel/linux_diagnostic_test.py
	python3 tests/kernel/linux_gettimeofday_test.py
	python3 tests/kernel/linux_clear_tid_test.py
	python3 tests/kernel/linux_process_clone_test.py
	python3 tests/kernel/linux_mkdir_test.py
	python3 tests/kernel/linux_readlink_test.py
	python3 tests/kernel/linux_symlink_test.py
	python3 tests/kernel/linux_unlink_test.py
	python3 tests/kernel/linux_rename_test.py
	$(HOSTCC) -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-pie -no-pie -Iinclude tests/kernel/file_record_lock_test.c kernel/fs/vfs/record_lock.c -o $(BUILD)/file-record-lock-test.exe
	./$(BUILD)/file-record-lock-test.exe
	python3 tests/kernel/linux_fcntl_lock_test.py
	python3 tests/kernel/linux_nofile_limit_test.py
	python3 tests/kernel/linux_fionread_test.py
	python3 tests/kernel/vfs_directory_fd_path_test.py
	python3 tests/kernel/linux_symlink_stat_test.py
	python3 tests/kernel/linux_mmap_fd_abi_test.py
	python3 tests/kernel/linux_sysinfo_test.py
	python3 tests/kernel/linux_vulkan_mmap_flags_test.py
	python3 tests/kernel/nvidia_alloc_mmap_context_test.py
	python3 tests/kernel/linux_vulkan_clock_affinity_test.py
	python3 tests/kernel/linux_sched_query_test.py
	python3 tests/kernel/vm_advice_tlb_batch_test.py
	python3 tests/kernel/nvidia_event_dispatch_test.py
	python3 tests/kernel/nvidia_event_queue_test.py
	python3 tests/kernel/kmalloc_try_cached_test.py
	python3 tests/kernel/nvidia_event_source_test.py
	python3 tests/kernel/nvidia_event_bridge_test.py
	python3 tests/kernel/nvidia_event_poll_test.py
	python3 tests/kernel/linux_wait_source_capture_test.py
	python3 tests/user/vulkan_video_frame_reuse_test.py
	python3 tests/user/vulkan_video_pacing_test.py
	python3 tests/user/vulkan_video_wait_test.py
	python3 tests/kernel/nvidia_host_import_descriptor_test.py
	python3 tests/kernel/vm_user_page_pin_test.py
	python3 tests/kernel/nvidia_user_pages_test.py
	python3 tests/kernel/boot_realtest_diagnostic_test.py
	python3 tests/kernel/chrome_validation_tools_test.py
	python3 tests/user/libc_diagnostic_test.py
	python3 tests/user/libc_trace_budget_test.py
	python3 tests/user/libc_port_trace_gate_test.py
	python3 tests/user/allocator_namespace_test.py
	python3 tests/user/runtime_alias_cleanup_test.py
	python3 tests/user/syscall_namespace_test.py
	python3 tests/user/libc_scan_exports_test.py
	python3 tests/user/linux_events_probe_test.py
	python3 tests/user/imageview_load_test.py
	python3 tests/kernel/usb_input_diagnostic_test.py
	python3 tests/kernel/filesystem_diagnostic_test.py
	python3 tests/kernel/build_config_test.py
	python3 tests/kernel/host_test_entrypoint_test.py
	python3 tests/user/gshell_prompt_test.py
	python3 tests/user/gshell_output_ring_test.py
	python3 tests/user/gshell_output_chunk_test.py
	python3 tests/user/gshell_launch_progress_test.py
	python3 tests/user/text_mouse_test.py
	python3 tests/user/server_caption_apps_test.py
	python3 tests/kernel/window_title_metadata_test.py
	python3 tests/kernel/linux_window_title_test.py
	python3 tests/kernel/window_decoration_switch_test.py
	python3 tests/kernel/window_interactive_test.py
	python3 tests/user/chrome_partial_frame_oracle_test.py
	python3 tests/kernel/window_geometry_query_test.py
	python3 tests/kernel/window_maximize_owned_test.py
	python3 tests/user/notepad_draw_test.py
	python3 tests/user/notepad_history_test.py
	python3 tests/user/notepad_layout_test.py
	$(HOSTCC) -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-pie -no-pie -Iinclude tests/user/message_box_test.c -o $(BUILD)/message-box-test.exe
	./$(BUILD)/message-box-test.exe
	python3 tests/user/notepad_message_test.py
	python3 tests/user/notepad_io_test.py
	python3 tests/user/text_font_lifecycle_test.py
	python3 tests/user/text_measure_buffer_test.py
	python3 tests/kernel/nvidia_deployment_test.py --host-only

# Deployment assertions deliberately require the actual canonical payload;
# unlike the host suite they must not pass by silently skipping missing ELFs.
nvidia-deployment-test:
	python3 tests/kernel/nvidia_deployment_test.py

ifneq ($(OS),Windows_NT)
test: host-regression-test
endif

test: $(BUILD)/sha256-test.exe $(BUILD)/mm-api-test.exe $(BUILD)/memory-map-test.exe \
      $(BUILD)/fat32-test.exe $(BUILD)/cache-test.exe $(BUILD)/primitives-test.exe \
      $(BUILD)/bluetooth-test.exe $(BUILD)/firmware-test.exe $(BUILD)/audiod-test.exe \
      $(BUILD)/abi-sanity.exe $(BUILD)/nvidia-port-test.exe \
      $(BUILD)/wait-subscription-test.exe \
      $(BUILD)/vfs-user-api-test.exe \
      $(BUILD)/chrome-start-test.exe \
      $(BUILD)/libc-personality-test.exe \
      $(BUILD)/frame-trace-disabled-test.exe $(BUILD)/frame-trace-enabled-test.exe
	./$(BUILD)/sha256-test.exe
	./$(BUILD)/mm-api-test.exe
	./$(BUILD)/memory-map-test.exe
	./$(BUILD)/fat32-test.exe
	./$(BUILD)/cache-test.exe
	./$(BUILD)/primitives-test.exe
	./$(BUILD)/bluetooth-test.exe
	./$(BUILD)/firmware-test.exe
	./$(BUILD)/audiod-test.exe
	./$(BUILD)/abi-sanity.exe
	./$(BUILD)/nvidia-port-test.exe
	./$(BUILD)/wait-subscription-test.exe
	./$(BUILD)/vfs-user-api-test.exe
	./$(BUILD)/chrome-start-test.exe
	./$(BUILD)/libc-personality-test.exe
	./$(BUILD)/libc-personality-test.exe linux
	./$(BUILD)/frame-trace-disabled-test.exe
	./$(BUILD)/frame-trace-enabled-test.exe
