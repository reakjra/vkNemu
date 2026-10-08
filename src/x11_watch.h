#pragma once

#include <stdbool.h>
#include <stdint.h>

void x11_watch_start(int wake_fd);
void x11_watch_start_input(uint64_t timeout_ns);
void x11_watch_track(uint32_t window);
bool x11_watch_is_focused(void);
bool x11_watch_input_is_available(void);
bool x11_watch_input_is_idle(void);
bool x11_watch_input_held(void);
