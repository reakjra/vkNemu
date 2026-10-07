#pragma once

#include <stdbool.h>
#include <stdint.h>

void idle_notify_start(uint32_t timeout_ms, int wake_fd);
bool idle_notify_is_idle(void);
