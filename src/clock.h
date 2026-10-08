#pragma once

#include <stdint.h>
#include <time.h>

#define NS_PER_SEC 1000000000ull
#define NS_PER_MS 1000000ull

static inline uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NS_PER_SEC + (uint64_t)ts.tv_nsec;
}
