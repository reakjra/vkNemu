#include "wayland_focus.h"

#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

struct focus_watcher {
    struct wl_display *display;
    struct wl_event_queue *queue;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
};

static struct focus_watcher watcher;
static atomic_bool tracking;
static atomic_bool ready;
static atomic_bool focused = true;
static atomic_int keys_down;

// wayland at its best as usual
static void on_keymap(void *, struct wl_keyboard *, uint32_t, int32_t fd, uint32_t) {
    close(fd);
}

static void on_enter(void *, struct wl_keyboard *, uint32_t, struct wl_surface *, struct wl_array *keys) {
    atomic_store(&focused, true);
    atomic_store(&keys_down, (int)(keys->size / sizeof(uint32_t)));
}

static void on_leave(void *, struct wl_keyboard *, uint32_t, struct wl_surface *) {
    atomic_store(&focused, false);
    atomic_store(&keys_down, 0);
}

static void on_key(void *, struct wl_keyboard *, uint32_t, uint32_t, uint32_t, uint32_t state) {
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
        atomic_fetch_add(&keys_down, 1);
    else if (atomic_load(&keys_down) > 0)
        atomic_fetch_sub(&keys_down, 1);
}

static void on_modifiers(void *, struct wl_keyboard *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = on_keymap,
    .enter = on_enter,
    .leave = on_leave,
    .key = on_key,
    .modifiers = on_modifiers,
};

static void on_capabilities(void *, struct wl_seat *seat, uint32_t capabilities) {
    if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) || watcher.keyboard)
        return;

    watcher.keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(watcher.keyboard, &keyboard_listener, NULL);
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = on_capabilities,
};

static void on_global(void *, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t) {
    if (strcmp(interface, wl_seat_interface.name) || watcher.seat)
        return;

    watcher.seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    wl_seat_add_listener(watcher.seat, &seat_listener, NULL);
}

static void on_global_remove(void *, struct wl_registry *, uint32_t) {}

static const struct wl_registry_listener registry_listener = {
    .global = on_global,
    .global_remove = on_global_remove,
};

void wayland_focus_track(struct wl_display *display) {
    if (atomic_exchange(&tracking, true))
        return;

    watcher.display = display;
    watcher.queue = wl_display_create_queue(display);

    struct wl_display *wrapper = wl_proxy_create_wrapper(display);
    wl_proxy_set_queue((struct wl_proxy *)wrapper, watcher.queue);
    struct wl_registry *registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_flush(display);

    atomic_store_explicit(&ready, true, memory_order_release);
}

// thread deadlocked lol
void wayland_focus_dispatch(void) {
    if (!atomic_load_explicit(&ready, memory_order_acquire))
        return;

    wl_display_dispatch_queue_pending(watcher.display, watcher.queue);
    wl_display_flush(watcher.display);
}

bool wayland_focus_is_focused(void) {
    return atomic_load(&focused);
}

bool wayland_focus_keys_held(void) {
    return atomic_load(&keys_down) > 0;
}
