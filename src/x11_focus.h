#pragma once

#include <stdbool.h>
#include <stdint.h>

void x11_focus_start(int wake_fd);
void x11_focus_track(uint32_t window);
bool x11_focus_is_focused(void);
bool x11_focus_input_held(void);
