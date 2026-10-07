#pragma once

#include <stdint.h>

struct pacer {
    uint64_t interval_ns;
    _Atomic uint64_t last_ns;
};

void pacer_set_fps(struct pacer *pacer, double fps);
void pacer_mark(struct pacer *pacer);
void pacer_wait(struct pacer *pacer, int wake_fd);
