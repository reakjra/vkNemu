#pragma once

#include <stdint.h>

void gamepad_input_start(double deadzone, uint64_t timeout_ns, int wake_fd);
bool gamepad_input_is_connected(void);
bool gamepad_input_is_idle(void);
