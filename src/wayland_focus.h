#pragma once

#include <stdbool.h>

struct wl_display;

void wayland_focus_track(struct wl_display *display);
void wayland_focus_dispatch(void);
bool wayland_focus_is_focused(void);
bool wayland_focus_keys_held(void);
