#ifndef LITEOS_WGET_TLS_H
#define LITEOS_WGET_TLS_H

#include <stdint.h>
#include <stdbool.h>

#include <uapi/all.h>

typedef struct wget_tls_transport {
    os_handle_t socket;
    uint64_t timeout_ns;
    bool active;
} wget_tls_transport_t;

/* Start a TLS 1.0--1.2 client session on an already connected socket. */
bool wget_tls_start(wget_tls_transport_t *transport, os_handle_t socket,
                    const char *server_name, uint64_t timeout_ns,
                    bool no_check_certificate);

/* Send one complete plaintext request through the TLS session. */
bool wget_tls_send_all(wget_tls_transport_t *transport,
                       const void *buffer, size_t length);

/* Read decrypted application bytes. Returns 0 on EOF and a negative value on error. */
int64_t wget_tls_recv(wget_tls_transport_t *transport, void *buffer,
                      size_t capacity);

/* Drop the TLS state. The caller still owns and closes the socket. */
void wget_tls_stop(wget_tls_transport_t *transport);

#endif
