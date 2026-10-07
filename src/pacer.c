#include "pacer.h"

#include "clock.h"

#include <poll.h>
#include <stdatomic.h>
#include <sys/eventfd.h>

void pacer_set_fps(struct pacer *pacer, double fps) {
    pacer->interval_ns = (uint64_t)(NS_PER_SEC / fps);
}

void pacer_mark(struct pacer *pacer) {
    atomic_store(&pacer->last_ns, monotonic_ns());
}

void pacer_wait(struct pacer *pacer, int wake_fd) {
    uint64_t deadline = atomic_load(&pacer->last_ns) + pacer->interval_ns;
    uint64_t now = monotonic_ns();

    if (now < deadline) {
        uint64_t left = deadline - now;
        struct timespec timeout = {.tv_sec = left / NS_PER_SEC, .tv_nsec = left % NS_PER_SEC};
        struct pollfd wake = {.fd = wake_fd, .events = POLLIN};

        if (ppoll(&wake, 1, &timeout, NULL) > 0) {
            eventfd_t count;
            eventfd_read(wake_fd, &count);
        }
    }

    pacer_mark(pacer);
}
