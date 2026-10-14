#pragma once
#include <stdint.h>
#include <limits.h>

typedef struct video_pacing {
    uint64_t deadline, step, remainder, carry, divisor;
} video_pacing_t;

static inline void video_pacing_init(video_pacing_t *p, int numerator,
                                      int denominator) {
    /* Bound malformed rates, preserving ordinary fractional frame rates. */
    if (numerator < 1 || denominator < 1 ||
        (int64_t)numerator < denominator ||
        (int64_t)numerator > (int64_t)denominator * 240) {
        numerator = 30;
        denominator = 1;
    }
    uint64_t period = UINT64_C(1000000000) * (uint32_t)denominator;
    *p = (video_pacing_t){.step = period / (uint32_t)numerator,
                         .remainder = period % (uint32_t)numerator,
                         .divisor = (uint32_t)numerator};
}

static inline uint64_t video_pacing_advance(video_pacing_t *p, uint64_t now) {
    uint64_t step = p->step;
    p->carry += p->remainder;
    if (p->carry >= p->divisor) {
        p->carry -= p->divisor;
        ++step;
    }
    /* Recover from a pause/stall without a burst of catch-up frames. */
    if (p->deadline == 0 ||
        (now > p->deadline && now - p->deadline > p->step * 4))
        p->deadline = now;
    p->deadline = p->deadline > UINT64_MAX - step ? UINT64_MAX : p->deadline + step;
    return p->deadline;
}

static inline int video_pacing_timeout(uint64_t deadline, uint64_t now) {
    if (now >= deadline) return 0;
    uint64_t remaining = deadline - now;
    uint64_t ms = remaining / UINT64_C(1000000) +
                  (remaining % UINT64_C(1000000) != 0);
    return ms > INT_MAX ? INT_MAX : (int)ms;
}
