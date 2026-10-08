#include "x11_watch.h"

#include "activity.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <xcb/xcb.h>
#include <xcb/xinput.h>

#define MAX_ANCESTORS 16

struct x11_tracker {
    xcb_connection_t *connection;
    xcb_window_t root;
    xcb_atom_t active_atom;
    bool watches_focus;
    uint8_t xinput_opcode;
    struct activity activity;
    xcb_window_t ancestors[MAX_ANCESTORS];
    size_t ancestor_count;
    int wake_fd;
};

static struct x11_tracker tracker = {.wake_fd = -1};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool focused = true;
static atomic_bool raw_input;

static xcb_atom_t intern_atom(const char *name) {
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(tracker.connection, 1, strlen(name), name);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(tracker.connection, cookie, NULL);
    xcb_atom_t atom = reply ? reply->atom : XCB_ATOM_NONE;
    free(reply);
    return atom;
}

static xcb_window_t screen_root(int screen) {
    xcb_screen_iterator_t roots = xcb_setup_roots_iterator(xcb_get_setup(tracker.connection));
    for (; roots.rem && screen > 0; screen--)
        xcb_screen_next(&roots);
    return roots.rem ? roots.data->root : XCB_WINDOW_NONE;
}

static xcb_window_t active_window(void) {
    xcb_get_property_cookie_t cookie =
        xcb_get_property(tracker.connection, 0, tracker.root, tracker.active_atom, XCB_ATOM_WINDOW, 0, 1);
    xcb_get_property_reply_t *reply = xcb_get_property_reply(tracker.connection, cookie, NULL);

    xcb_window_t window = XCB_WINDOW_NONE;
    if (reply && xcb_get_property_value_length(reply) == sizeof window)
        memcpy(&window, xcb_get_property_value(reply), sizeof window);
    free(reply);
    return window;
}

static xcb_window_t parent_window(xcb_window_t window) {
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(tracker.connection, xcb_query_tree(tracker.connection, window), NULL);
    xcb_window_t parent = tree ? tree->parent : XCB_WINDOW_NONE;
    free(tree);
    return parent;
}

static void refresh_focus(void) {
    xcb_window_t active = active_window();

    pthread_mutex_lock(&lock);
    bool now_focused = tracker.ancestor_count == 0;
    for (size_t i = 0; i < tracker.ancestor_count && !now_focused; i++)
        now_focused = tracker.ancestors[i] == active;
    pthread_mutex_unlock(&lock);

    if (now_focused && !atomic_exchange(&focused, true))
        eventfd_write(tracker.wake_fd, 1);
    else if (!now_focused)
        atomic_store(&focused, false);
}

static bool watch_root_properties(void) {
    uint32_t event_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_void_cookie_t cookie =
        xcb_change_window_attributes_checked(tracker.connection, tracker.root, XCB_CW_EVENT_MASK, &event_mask);
    xcb_generic_error_t *error = xcb_request_check(tracker.connection, cookie);
    bool watching = !error;
    free(error);
    return watching;
}

static bool is_active_window_change(const xcb_generic_event_t *event) {
    return (event->response_type & ~0x80) == XCB_PROPERTY_NOTIFY &&
           ((const xcb_property_notify_event_t *)event)->atom == tracker.active_atom;
}

static bool is_raw_input(const xcb_generic_event_t *event) {
    return (event->response_type & ~0x80) == XCB_GE_GENERIC &&
           ((const xcb_ge_generic_event_t *)event)->extension == tracker.xinput_opcode;
}

static void *watch_events(void *) {
    xcb_generic_event_t *event;
    while ((event = xcb_wait_for_event(tracker.connection))) {
        bool changed = is_active_window_change(event);
        bool input = is_raw_input(event);
        free(event);
        if (changed)
            refresh_focus();
        if (input)
            activity_mark(&tracker.activity, tracker.wake_fd);
    }

    atomic_store(&raw_input, false);
    atomic_store(&focused, true);
    eventfd_write(tracker.wake_fd, 1);
    return NULL;
}

void x11_watch_start(int wake_fd) {
    int screen;
    xcb_connection_t *connection = xcb_connect(NULL, &screen);
    if (xcb_connection_has_error(connection)) {
        xcb_disconnect(connection);
        return;
    }

    tracker.connection = connection;
    tracker.wake_fd = wake_fd;
    tracker.root = screen_root(screen);
    tracker.active_atom = intern_atom("_NET_ACTIVE_WINDOW");

    pthread_t thread;
    if (tracker.root == XCB_WINDOW_NONE || pthread_create(&thread, NULL, watch_events, NULL) != 0) {
        tracker.connection = NULL;
        xcb_disconnect(connection);
        return;
    }
    pthread_detach(thread);
    tracker.watches_focus = tracker.active_atom != XCB_ATOM_NONE && watch_root_properties();
}

static bool select_raw_input(void) {
    struct {
        xcb_input_event_mask_t head;
        uint32_t bits;
    } mask = {
        .head = {.deviceid = XCB_INPUT_DEVICE_ALL_MASTER, .mask_len = 1},
        .bits = XCB_INPUT_XI_EVENT_MASK_RAW_KEY_PRESS | XCB_INPUT_XI_EVENT_MASK_RAW_KEY_RELEASE |
                XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_PRESS | XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_RELEASE |
                XCB_INPUT_XI_EVENT_MASK_RAW_MOTION,
    };
    xcb_generic_error_t *error = xcb_request_check(
        tracker.connection, xcb_input_xi_select_events_checked(tracker.connection, tracker.root, 1, &mask.head));
    bool selected = !error;
    free(error);
    return selected;
}

// this is needed for gamescope since as usual it makes my life more joyful everytime i have to touch it.
void x11_watch_start_input(uint64_t timeout_ns) {
    if (!tracker.connection)
        return;

    const xcb_query_extension_reply_t *xinput = xcb_get_extension_data(tracker.connection, &xcb_input_id);
    if (!xinput || !xinput->present)
        return;

    xcb_input_xi_query_version_reply_t *version =
        xcb_input_xi_query_version_reply(tracker.connection, xcb_input_xi_query_version(tracker.connection, 2, 1), NULL);
    bool raw_always_delivered = version && version->major_version == 2 && version->minor_version >= 1;
    free(version);

    tracker.xinput_opcode = xinput->major_opcode;
    activity_start(&tracker.activity, timeout_ns);
    atomic_store(&raw_input, raw_always_delivered && select_raw_input());
}

void x11_watch_track(uint32_t window) {
    if (!tracker.watches_focus)
        return;

    xcb_window_t ancestors[MAX_ANCESTORS];
    size_t count = 0;
    for (xcb_window_t current = window; current != XCB_WINDOW_NONE && current != tracker.root && count < MAX_ANCESTORS;
         current = parent_window(current))
        ancestors[count++] = current;

    pthread_mutex_lock(&lock);
    memcpy(tracker.ancestors, ancestors, count * sizeof *ancestors);
    tracker.ancestor_count = count;
    pthread_mutex_unlock(&lock);

    refresh_focus();
}

bool x11_watch_is_focused(void) {
    return atomic_load(&focused);
}

bool x11_watch_input_is_available(void) {
    return atomic_load(&raw_input);
}

bool x11_watch_input_is_idle(void) {
    return !atomic_load(&raw_input) || activity_is_idle(&tracker.activity);
}

static bool any_key_down(const xcb_query_keymap_reply_t *keymap) {
    for (size_t i = 0; i < sizeof keymap->keys; i++)
        if (keymap->keys[i])
            return true;
    return false;
}

bool x11_watch_input_held(void) {
    if (!tracker.connection)
        return false;

    xcb_query_keymap_cookie_t keymap_cookie = xcb_query_keymap(tracker.connection);
    xcb_query_pointer_cookie_t pointer_cookie = xcb_query_pointer(tracker.connection, tracker.root);
    xcb_query_keymap_reply_t *keymap = xcb_query_keymap_reply(tracker.connection, keymap_cookie, NULL);
    xcb_query_pointer_reply_t *pointer = xcb_query_pointer_reply(tracker.connection, pointer_cookie, NULL);

    uint16_t buttons = XCB_KEY_BUT_MASK_BUTTON_1 | XCB_KEY_BUT_MASK_BUTTON_2 | XCB_KEY_BUT_MASK_BUTTON_3;
    bool held = (keymap && any_key_down(keymap)) || (pointer && pointer->mask & buttons);
    free(keymap);
    free(pointer);
    return held;
}
