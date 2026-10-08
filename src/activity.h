#pragma once

#include "clock.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/eventfd.h>

struct activity {
    _Atomic uint64_t last_ns;
    uint64_t timeout_ns;
};

static inline void activity_start(struct activity *activity, uint64_t timeout_ns) {
    activity->timeout_ns = timeout_ns;
    atomic_store(&activity->last_ns, monotonic_ns());
}

static inline void activity_mark(struct activity *activity, int wake_fd) {
    uint64_t now = monotonic_ns();
    if (now - atomic_exchange(&activity->last_ns, now) > activity->timeout_ns)
        eventfd_write(wake_fd, 1);
}

static inline bool activity_is_idle(struct activity *activity) {
    return monotonic_ns() - atomic_load(&activity->last_ns) > activity->timeout_ns;
}
