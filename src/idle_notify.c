#include "idle_notify.h"

#include "ext-idle-notify-v1-client-protocol.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <wayland-client.h>

struct idle_globals {
    struct ext_idle_notifier_v1 *notifier;
    struct wl_seat *seat;
};

static atomic_bool idle;
static atomic_bool available;
static int wake_fd = -1;

static void set_idle(bool value) {
    atomic_store(&idle, value);
    if (!value)
        eventfd_write(wake_fd, 1);
}

static void on_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    struct idle_globals *globals = data;
    if (!strcmp(interface, ext_idle_notifier_v1_interface.name) && version >= 2)
        globals->notifier = wl_registry_bind(registry, name, &ext_idle_notifier_v1_interface, 2);
    else if (!strcmp(interface, wl_seat_interface.name) && !globals->seat)
        globals->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
}

static void on_global_remove(void *, struct wl_registry *, uint32_t) {}

static const struct wl_registry_listener registry_listener = {
    .global = on_global,
    .global_remove = on_global_remove,
};

static void on_idled(void *, struct ext_idle_notification_v1 *) {
    set_idle(true);
}

static void on_resumed(void *, struct ext_idle_notification_v1 *) {
    set_idle(false);
}

static const struct ext_idle_notification_v1_listener notification_listener = {
    .idled = on_idled,
    .resumed = on_resumed,
};

static void *watch_idle(void *arg) {
    uint32_t timeout_ms = (uint32_t)(uintptr_t)arg;

    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "vknemu: no wayland display, staying inactive\n");
        return NULL;
    }

    struct idle_globals globals = {0};
    wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, &globals);
    wl_display_roundtrip(display);

    if (!globals.notifier || !globals.seat) {
        fprintf(stderr, "vknemu: compositor lacks ext_idle_notifier_v1 v2, staying inactive\n");
        wl_display_disconnect(display);
        return NULL;
    }

    struct ext_idle_notification_v1 *notification =
        ext_idle_notifier_v1_get_input_idle_notification(globals.notifier, timeout_ms, globals.seat);
    ext_idle_notification_v1_add_listener(notification, &notification_listener, NULL);
    atomic_store(&available, true);

    while (wl_display_dispatch(display) != -1)
        ;

    atomic_store(&available, false);
    set_idle(false);
    wl_display_disconnect(display);
    return NULL;
}

void idle_notify_start(uint32_t timeout_ms, int fd) {
    wake_fd = fd;

    pthread_t thread;
    if (pthread_create(&thread, NULL, watch_idle, (void *)(uintptr_t)timeout_ms) == 0)
        pthread_detach(thread);
}

bool idle_notify_is_available(void) {
    return atomic_load(&available);
}

bool idle_notify_is_idle(void) {
    return atomic_load(&idle);
}
