#include "tls.h"

#include <bearssl.h>

#define WGET_TLS_RETRY_SLEEP_NS 10000000ULL
#define WGET_TLS_IO_CHUNK       2048U
#define WGET_TLS_EAGAIN          (-11LL)
#define WGET_TLS_EBUSY           (-16LL)

extern const br_x509_trust_anchor *wget_trust_anchor_list(size_t *count);

typedef struct wget_tls_noanchor {
    const br_x509_class *vtable;
    const br_x509_class **inner;
} wget_tls_noanchor_t;

typedef struct wget_tls_state {
    br_ssl_client_context client;
    br_x509_minimal_context x509;
    wget_tls_noanchor_t noanchor;
    br_sslio_context io;
    uint8_t io_buffer[BR_SSL_BUFSIZE_MONO];
} wget_tls_state_t;

static wget_tls_state_t g_tls_state;
static uint64_t g_tls_entropy_counter = 0xC6BC279692B5CC83ULL;

static uint32_t tls_date_digit(char value) {
    return value >= '0' && value <= '9' ? (uint32_t)(value - '0') : 0U;
}

static uint32_t tls_build_month(const char *date) {
    if (date[0] == 'J' && date[1] == 'a') return 1U;
    if (date[0] == 'F') return 2U;
    if (date[0] == 'M' && date[2] == 'r') return 3U;
    if (date[0] == 'A' && date[1] == 'p') return 4U;
    if (date[0] == 'M' && date[2] == 'y') return 5U;
    if (date[0] == 'J' && date[1] == 'u' && date[2] == 'n') return 6U;
    if (date[0] == 'J' && date[1] == 'u' && date[2] == 'l') return 7U;
    if (date[0] == 'A' && date[1] == 'u') return 8U;
    if (date[0] == 'S') return 9U;
    if (date[0] == 'O') return 10U;
    if (date[0] == 'N') return 11U;
    return 12U;
}

static uint32_t tls_days_before_month(uint32_t month, uint32_t leap) {
    static const uint16_t days[] = {
        0U, 31U, 59U, 90U, 120U, 151U, 181U,
        212U, 243U, 273U, 304U, 334U,
    };
    uint32_t value = days[month - 1U];
    if (leap != 0U && month > 2U) ++value;
    return value;
}

static void tls_set_build_time(br_x509_minimal_context *x509) {
    static const char date[] = __DATE__;
    static const char time[] = __TIME__;
    uint32_t month = tls_build_month(date);
    uint32_t day = date[4] == ' ' ?
                   (date[5] == ' ' ? tls_date_digit(date[6]) : tls_date_digit(date[5])) :
                   tls_date_digit(date[4]) * 10U + tls_date_digit(date[5]);
    uint32_t year = tls_date_digit(date[7]) * 1000U + tls_date_digit(date[8]) * 100U +
                    tls_date_digit(date[9]) * 10U + tls_date_digit(date[10]);
    uint32_t leap = (year % 4U == 0U &&
                     (year % 100U != 0U || year % 400U == 0U)) ? 1U : 0U;
    uint32_t days = 1U + 365U * year + year / 4U - year / 100U + year / 400U +
                    tls_days_before_month(month, leap) + day - 1U;
    uint32_t seconds = tls_date_digit(time[0]) * 36000U +
                       tls_date_digit(time[1]) * 3600U +
                       tls_date_digit(time[3]) * 600U +
                       tls_date_digit(time[4]) * 60U +
                       tls_date_digit(time[6]) * 10U + tls_date_digit(time[7]);
    br_x509_minimal_set_time(x509, days, seconds);
}

static int64_t tls_syscall_one(uint64_t number, uint64_t arg0) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t tls_syscall_three(uint64_t number, uint64_t arg0,
                                 uint64_t arg1, uint64_t arg2) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    register uint64_t rdx __asm__("rdx") = arg2;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi),
                      "+d"(rdx) : : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static int64_t tls_syscall_six(uint64_t number, uint64_t arg0,
                               uint64_t arg1, uint64_t arg2,
                               uint64_t arg3, uint64_t arg4,
                               uint64_t arg5) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg0;
    register uint64_t rsi __asm__("rsi") = arg1;
    register uint64_t rdx __asm__("rdx") = arg2;
    register uint64_t r10 __asm__("r10") = arg3;
    register uint64_t r8 __asm__("r8") = arg4;
    register uint64_t r9 __asm__("r9") = arg5;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi), "+S"(rsi),
                      "+d"(rdx), "+r"(r10), "+r"(r8), "+r"(r9) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static bool tls_sleep_ns(uint64_t delay_ns) {
    os_handle_t timer = OS_INVALID_HANDLE;
    os_wait_result_t result = {0};
    int64_t status = tls_syscall_three(OS_SYS_TIMER_CREATE, delay_ns, 0U,
                                       (uint64_t)(uintptr_t)&timer);
    if (status < 0 || timer == OS_INVALID_HANDLE) return false;
    status = tls_syscall_three(OS_SYS_WAIT_ONE, timer, OS_WAIT_INFINITE,
                               (uint64_t)(uintptr_t)&result);
    (void)tls_syscall_one(OS_SYS_HANDLE_CLOSE, timer);
    return status >= 0;
}

static int64_t tls_socket_send_raw(os_handle_t socket, const void *buffer,
                                   size_t length) {
    return tls_syscall_six(OS_SYS_SOCKET_SEND, socket,
                           (uint64_t)(uintptr_t)buffer, length,
                           0U, 0U, 0U);
}

static int64_t tls_socket_recv_raw(os_handle_t socket, void *buffer,
                                   size_t capacity, uint64_t timeout_ns) {
    uint32_t source_address = 0U;
    uint16_t source_port = 0U;
    return tls_syscall_six(OS_SYS_SOCKET_RECV, socket,
                           (uint64_t)(uintptr_t)buffer, capacity,
                           (uint64_t)(uintptr_t)&source_address,
                           (uint64_t)(uintptr_t)&source_port,
                           timeout_ns);
}

static int64_t tls_normalize_status(int64_t status) {
    /* Some syscall return paths preserve a 32-bit negative errno as a
       zero-extended value; make both ABI forms comparable. */
    if (status > INT32_MAX && (uint64_t)status <= UINT32_MAX) {
        return (int64_t)(int32_t)(uint32_t)status;
    }
    return status;
}

static int tls_low_read(void *context, unsigned char *data, size_t length) {
    wget_tls_transport_t *transport = (wget_tls_transport_t *)context;
    size_t chunk;
    int64_t received;
    if (transport == 0 || !transport->active || data == 0 || length == 0U) return -1;
    chunk = length > WGET_TLS_IO_CHUNK ? WGET_TLS_IO_CHUNK : length;
    received = tls_normalize_status(
        tls_socket_recv_raw(transport->socket, data, chunk,
                            transport->timeout_ns));
    if (received <= 0 || (uint64_t)received > chunk) return -1;
    return (int)received;
}

static int tls_low_write(void *context, const unsigned char *data, size_t length) {
    wget_tls_transport_t *transport = (wget_tls_transport_t *)context;
    const uint8_t *cursor = data;
    uint64_t waited = 0U;
    if (transport == 0 || !transport->active || data == 0 || length == 0U) return -1;
    while (length != 0U && waited <= transport->timeout_ns) {
        size_t chunk = length > WGET_TLS_IO_CHUNK ? WGET_TLS_IO_CHUNK : length;
        int64_t sent = tls_normalize_status(
            tls_socket_send_raw(transport->socket, cursor, chunk));
        if (sent > 0 && (uint64_t)sent <= chunk) {
            cursor += (size_t)sent;
            length -= (size_t)sent;
            continue;
        }
        if (sent != WGET_TLS_EAGAIN && sent != WGET_TLS_EBUSY && sent != 0) {
            return -1;
        }
        if (!tls_sleep_ns(WGET_TLS_RETRY_SLEEP_NS)) return -1;
        waited += WGET_TLS_RETRY_SLEEP_NS;
    }
    return length == 0U ? (int)(cursor - (const uint8_t *)data) : -1;
}

static uint64_t tls_rdtsc(void) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32U) | low;
}

static uint64_t tls_mix(uint64_t value) {
    value ^= value >> 30U;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27U;
    value *= 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

static void tls_inject_entropy(br_ssl_engine_context *engine,
                               const char *server_name, os_handle_t socket) {
    uint64_t seed = tls_rdtsc() ^ (uint64_t)socket ^
                    (uintptr_t)server_name ^ ++g_tls_entropy_counter;
    uint8_t entropy[32];
    for (size_t index = 0U; index < sizeof(entropy); index += 8U) {
        seed = tls_mix(seed + index + 0x9E3779B97F4A7C15ULL);
        for (uint32_t byte = 0U; byte < 8U; ++byte) {
            entropy[index + byte] = (uint8_t)(seed >> (byte * 8U));
        }
    }
    br_ssl_engine_inject_entropy(engine, entropy, sizeof(entropy));
}

static void noanchor_start_chain(const br_x509_class **context,
                                 const char *server_name) {
    wget_tls_noanchor_t *wrapper = (wget_tls_noanchor_t *)context;
    (void)server_name;
    /* --no-check-certificate also disables hostname policy. */
    (*wrapper->inner)->start_chain(wrapper->inner, 0);
}

static void noanchor_start_cert(const br_x509_class **context, uint32_t length) {
    wget_tls_noanchor_t *wrapper = (wget_tls_noanchor_t *)context;
    (*wrapper->inner)->start_cert(wrapper->inner, length);
}

static void noanchor_append(const br_x509_class **context,
                            const unsigned char *data, size_t length) {
    wget_tls_noanchor_t *wrapper = (wget_tls_noanchor_t *)context;
    (*wrapper->inner)->append(wrapper->inner, data, length);
}

static void noanchor_end_cert(const br_x509_class **context) {
    wget_tls_noanchor_t *wrapper = (wget_tls_noanchor_t *)context;
    (*wrapper->inner)->end_cert(wrapper->inner);
}

static unsigned noanchor_end_chain(const br_x509_class **context) {
    wget_tls_noanchor_t *wrapper = (wget_tls_noanchor_t *)context;
    unsigned result = (*wrapper->inner)->end_chain(wrapper->inner);
    /* This vtable is installed only for --no-check-certificate.  Keep the
       parsed public key even when path, time, or hostname policy fails.  Keep
       structural DER/parser errors fatal. */
    if (result >= BR_ERR_X509_WRONG_KEY_TYPE &&
        result <= BR_ERR_X509_NOT_TRUSTED) return 0U;
    return result;
}

static const br_x509_pkey *noanchor_get_pkey(const br_x509_class *const *context,
                                             unsigned *usages) {
    const wget_tls_noanchor_t *wrapper = (const wget_tls_noanchor_t *)context;
    return (*wrapper->inner)->get_pkey(wrapper->inner, usages);
}

static const br_x509_class g_noanchor_vtable = {
    sizeof(wget_tls_noanchor_t),
    noanchor_start_chain,
    noanchor_start_cert,
    noanchor_append,
    noanchor_end_cert,
    noanchor_end_chain,
    noanchor_get_pkey,
};

bool wget_tls_start(wget_tls_transport_t *transport, os_handle_t socket,
                    const char *server_name, uint64_t timeout_ns,
                    bool no_check_certificate) {
    if (transport == 0 || server_name == 0 || server_name[0] == '\0' ||
        socket == OS_INVALID_HANDLE) return false;
    transport->socket = socket;
    transport->timeout_ns = timeout_ns;
    transport->active = false;
    size_t trust_anchor_count = 0U;
    const br_x509_trust_anchor *trust_anchors =
        wget_trust_anchor_list(&trust_anchor_count);
    br_ssl_client_init_full(&g_tls_state.client, &g_tls_state.x509,
                            trust_anchors, trust_anchor_count);
    tls_set_build_time(&g_tls_state.x509);
    if (no_check_certificate) {
        g_tls_state.noanchor.vtable = &g_noanchor_vtable;
        g_tls_state.noanchor.inner = &g_tls_state.x509.vtable;
        br_ssl_engine_set_x509(&g_tls_state.client.eng,
                               &g_tls_state.noanchor.vtable);
    }
    /* The first client reset preserves the configured I/O buffer; install it
       before reset so BearSSL does not treat an uninitialised engine as an
       invalid parameter. */
    br_ssl_engine_set_buffer(&g_tls_state.client.eng, g_tls_state.io_buffer,
                             sizeof(g_tls_state.io_buffer), 0);
    tls_inject_entropy(&g_tls_state.client.eng, server_name, socket);
    if (!br_ssl_client_reset(&g_tls_state.client, server_name, 0)) {
        return false;
    }
    br_sslio_init(&g_tls_state.io, &g_tls_state.client.eng,
                  tls_low_read, transport, tls_low_write, transport);
    transport->active = true;
    return true;
}

bool wget_tls_send_all(wget_tls_transport_t *transport,
                       const void *buffer, size_t length) {
    int result;
    if (transport == 0 || !transport->active ||
        (length != 0U && buffer == 0)) return false;
    result = br_sslio_write_all(&g_tls_state.io, buffer, length);
    return result == 0 && br_sslio_flush(&g_tls_state.io) == 0;
}

int64_t wget_tls_recv(wget_tls_transport_t *transport, void *buffer,
                      size_t capacity) {
    int result;
    if (transport == 0 || !transport->active || buffer == 0 || capacity == 0U) {
        return -22;
    }
    result = br_sslio_read(&g_tls_state.io, buffer, capacity);
    if (result < 0) return -1;
    return result;
}

void wget_tls_stop(wget_tls_transport_t *transport) {
    if (transport == 0) return;
    transport->active = false;
}
