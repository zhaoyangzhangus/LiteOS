#ifndef KERNEL_WINDOW_SURFACE_EXCHANGE_H
#define KERNEL_WINDOW_SURFACE_EXCHANGE_H

#include <stdbool.h>
#include <stdint.h>

/* Metadata only: callers hold the window lock for each operation, but never
 * while rendering. Snapshot references pin the backing allocation separately.
 * A back buffer cannot be reused until every snapshot of its old frame ends. */
typedef struct window_surface_exchange {
    uint32_t readers[2];
    uint32_t front;
    uint32_t writer_slot;
    uint64_t serial;
    uint64_t writer_token;
    bool writing;
    bool closed;
} window_surface_exchange_t;

static inline bool window_surface_write_begin(window_surface_exchange_t *state,
                                              uint32_t *slot,
                                              uint64_t *token) {
    uint32_t back = state->front ^ 1U;
    if (state->closed || state->writing || state->readers[back] != 0U ||
        state->serial == UINT64_MAX) return false;
    state->writing = true;
    state->writer_slot = back;
    state->writer_token = ++state->serial;
    *slot = back;
    *token = state->writer_token;
    return true;
}

/* Successful GPU/CPU completion must precede commit. Abort leaves the last
 * complete frame visible, including after a partially overwriting failure. */
static inline bool window_surface_write_end(window_surface_exchange_t *state,
                                            uint64_t token, bool commit) {
    if (!state->writing || token != state->writer_token) return false;
    if (commit && state->closed) return false;
    if (commit) state->front = state->writer_slot;
    state->writing = false;
    return true;
}

static inline bool window_surface_read_begin(window_surface_exchange_t *state,
                                             uint32_t *slot) {
    if (state->closed || state->readers[state->front] == UINT32_MAX) return false;
    *slot = state->front;
    ++state->readers[*slot];
    return true;
}

static inline bool window_surface_read_end(window_surface_exchange_t *state,
                                           uint32_t slot) {
    if (slot > 1U || state->readers[slot] == 0U) return false;
    --state->readers[slot];
    return true;
}

static inline void window_surface_exchange_close(window_surface_exchange_t *state) {
    state->closed = true;
    state->writing = false;
}

#endif
