#include "mixer.h"

#include <uapi/syscall.h>

/* freestanding 用户程序自行提供编译器可能生成的基础清零例程。 */
__attribute__((noinline)) void *memset(void *destination, int value, size_t size) {
    uint8_t *bytes = (uint8_t *)destination;
    while (size-- != 0U) *bytes++ = (uint8_t)value;
    return destination;
}

/* audiod 只使用稳定的原始 syscall ABI，不依赖 libc 或运行时初始化。 */
static int64_t audiod_syscall_one(uint64_t number, uint64_t arg0) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t audiod_syscall_two(uint64_t number, uint64_t arg0,
                                  uint64_t arg1) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static void audiod_debug_write(const char *text) {
    size_t length = 0U;
    while (text[length] != '\0') ++length;
    (void)audiod_syscall_two(OS_SYS_DEBUG_WRITE, (uint64_t)text, length);
}

__attribute__((noreturn)) static void audiod_exit(uint64_t status) {
    (void)audiod_syscall_one(OS_SYS_THREAD_EXIT, status);
    for (;;) __asm__ volatile ("pause");
}

/* Audio hardware is optional during boot. Keep the reason in the real-test
 * log, but never let a missing or partially supported endpoint block desktop. */
__attribute__((noreturn)) static void audiod_unavailable(const char *stage) {
    audiod_debug_write("LITEOS_AUDIOD_UNAVAILABLE ");
    audiod_debug_write(stage);
    audiod_debug_write("\r\n");
    audiod_exit(0U);
}

static os_audio_stream_config_t audiod_config(void) {
    os_audio_stream_config_t config = {0};
    config.hdr.size = sizeof(config);
    config.hdr.version = OS_SYSCALL_ABI_VERSION;
    config.direction = OS_AUDIO_PLAYBACK;
    config.sample_rate = 48000U;
    config.channels = 2U;
    config.sample_format = OS_AUDIO_SAMPLE_S16_LE;
    config.period_frames = 128U;
    config.period_count = 2U;
    return config;
}

static os_audio_control_t audiod_control(uint32_t code) {
    os_audio_control_t control = {0};
    control.hdr.size = sizeof(control);
    control.hdr.version = OS_SYSCALL_ABI_VERSION;
    control.code = code;
    return control;
}

__attribute__((noreturn)) void audiod_entry(void) {
    os_audio_stream_config_t config = audiod_config();
    os_audio_open_t open = {0};
    audiod_server_t server = {0};
    uint8_t input[512] = {0};
    uint8_t output[512] = {0};
    os_audio_stream_stats_t stats = {0};
    uint32_t client = AUDIOD_INVALID_CLIENT;
    os_handle_t handle;
    os_audio_control_t control;

    open.hdr.size = sizeof(open);
    open.hdr.version = OS_SYSCALL_ABI_VERSION;
    open.config = config;
    int64_t status = audiod_syscall_one(OS_SYS_AUDIO_OPEN, (uint64_t)&open);
    if (status < 0) audiod_unavailable("OPEN");
    handle = open.handle;
    if (handle == OS_INVALID_HANDLE || audiod_server_init(&server) != AUDIOD_OK ||
        audiod_client_open(&server, &config, &client) != AUDIOD_OK ||
        audiod_client_submit(&server, client, input, 128U) != AUDIOD_OK ||
        audiod_mix(&server, output, sizeof(output), 128U) != AUDIOD_OK) {
        (void)audiod_syscall_one(OS_SYS_HANDLE_CLOSE, handle);
        audiod_exit(1U);
    }

    control = audiod_control(OS_AUDIO_CONTROL_QUEUE);
    control.period = 0U;
    control.frames = 128U;
    control.buffer = (uint64_t)output;
    control.buffer_size = sizeof(output);
    status = audiod_syscall_two(OS_SYS_AUDIO_CONTROL, handle,
                                (uint64_t)&control);
    if (status < 0) {
        (void)audiod_syscall_one(OS_SYS_HANDLE_CLOSE, handle);
        audiod_unavailable("QUEUE");
    }

    control = audiod_control(OS_AUDIO_CONTROL_START);
    status = audiod_syscall_two(OS_SYS_AUDIO_CONTROL, handle,
                                (uint64_t)&control);
    if (status < 0) {
        (void)audiod_syscall_one(OS_SYS_HANDLE_CLOSE, handle);
        audiod_unavailable("START");
    }

    control = audiod_control(OS_AUDIO_CONTROL_COMPLETE);
    control.period = 0U;
    control.frames = 128U;
    status = audiod_syscall_two(OS_SYS_AUDIO_CONTROL, handle,
                                (uint64_t)&control);
    if (status < 0) {
        (void)audiod_syscall_one(OS_SYS_HANDLE_CLOSE, handle);
        audiod_unavailable("COMPLETE");
    }

    stats.hdr.size = sizeof(stats);
    stats.hdr.version = OS_SYSCALL_ABI_VERSION;
    control = audiod_control(OS_AUDIO_CONTROL_GET_STATS);
    control.buffer = (uint64_t)&stats;
    control.buffer_size = sizeof(stats);
    status = audiod_syscall_two(OS_SYS_AUDIO_CONTROL, handle,
                                (uint64_t)&control);
    (void)audiod_client_close(&server, client);
    (void)audiod_syscall_one(OS_SYS_HANDLE_CLOSE, handle);
    if (status < 0) audiod_unavailable("GET_STATS");
    audiod_debug_write("LITEOS_AUDIOD_READY\r\n");
    audiod_exit(0U);
}
