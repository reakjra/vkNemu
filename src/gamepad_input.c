#include "gamepad_input.h"

#include "activity.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define INPUT_DIR "/dev/input"
#define NODE_LEN 32
#define MAX_EVENTS 16
#define BITS_PER_LONG (sizeof(long) * 8)
#define BIT_LONGS(count) (((count) + BITS_PER_LONG - 1) / BITS_PER_LONG)

struct axis {
    int rest;
    int threshold;
    bool deflected;
};

struct gamepad {
    struct gamepad *next;
    int fd;
    char node[NODE_LEN];
    unsigned long abs_bits[BIT_LONGS(ABS_CNT)];
    unsigned long keys_down[BIT_LONGS(KEY_CNT)];
    struct axis axes[ABS_CNT];
    int held;
};

struct watcher {
    int epoll_fd;
    int inotify_fd;
    int wake_fd;
    double deadzone;
    struct activity activity;
    struct gamepad *gamepads;
};

static struct watcher watcher = {.epoll_fd = -1, .inotify_fd = -1, .wake_fd = -1};
static atomic_int held_inputs;
static atomic_int connected;

static bool test_bit(const unsigned long *bits, unsigned int bit) {
    return bits[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG) & 1;
}

static void assign_bit(unsigned long *bits, unsigned int bit, bool value) {
    unsigned long mask = 1ul << (bit % BITS_PER_LONG);
    if (value)
        bits[bit / BITS_PER_LONG] |= mask;
    else
        bits[bit / BITS_PER_LONG] &= ~mask;
}

static void update_held(struct gamepad *pad, bool was_held, bool now_held) {
    if (was_held == now_held)
        return;

    int delta = now_held ? 1 : -1;
    pad->held += delta;
    atomic_fetch_add(&held_inputs, delta);
}

static bool is_gamepad(struct gamepad *pad) {
    unsigned long key_bits[BIT_LONGS(KEY_CNT)] = {0};
    if (ioctl(pad->fd, EVIOCGBIT(EV_KEY, sizeof key_bits), key_bits) < 0 ||
        ioctl(pad->fd, EVIOCGBIT(EV_ABS, sizeof pad->abs_bits), pad->abs_bits) < 0)
        return false;

    return (test_bit(key_bits, BTN_GAMEPAD) || test_bit(key_bits, BTN_JOYSTICK)) && test_bit(pad->abs_bits, ABS_X);
}

static void read_rest_positions(struct gamepad *pad, double deadzone) {
    for (unsigned int code = 0; code < ABS_CNT; code++) {
        struct input_absinfo info;
        if (test_bit(pad->abs_bits, code) && ioctl(pad->fd, EVIOCGABS(code), &info) == 0)
            pad->axes[code] = (struct axis){
                .rest = info.value,
                .threshold = (int)(deadzone * (info.maximum - info.minimum)),
            };
    }
}

static bool watch_fd(int fd, void *data) {
    struct epoll_event event = {.events = EPOLLIN, .data.ptr = data};
    return epoll_ctl(watcher.epoll_fd, EPOLL_CTL_ADD, fd, &event) == 0;
}

static struct gamepad *find_gamepad(const char *node) {
    for (struct gamepad *pad = watcher.gamepads; pad; pad = pad->next)
        if (!strcmp(pad->node, node))
            return pad;
    return NULL;
}

static void close_gamepad(struct gamepad *pad) {
    for (struct gamepad **link = &watcher.gamepads; *link; link = &(*link)->next)
        if (*link == pad) {
            *link = pad->next;
            atomic_fetch_sub(&connected, 1);
            break;
        }

    atomic_fetch_sub(&held_inputs, pad->held);
    if (pad->fd >= 0)
        close(pad->fd);
    free(pad);
}

static void open_gamepad(const char *node) {
    if (strncmp(node, "event", 5) || strlen(node) >= NODE_LEN || find_gamepad(node))
        return;

    struct gamepad *pad = calloc(1, sizeof *pad);
    if (!pad)
        return;

    char path[sizeof INPUT_DIR + NODE_LEN];
    snprintf(path, sizeof path, "%s/%s", INPUT_DIR, node);
    strcpy(pad->node, node);
    pad->fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);

    if (pad->fd < 0 || !is_gamepad(pad)) {
        close_gamepad(pad);
        return;
    }

    read_rest_positions(pad, watcher.deadzone);
    if (!watch_fd(pad->fd, pad)) {
        close_gamepad(pad);
        return;
    }

    pad->next = watcher.gamepads;
    watcher.gamepads = pad;
    atomic_fetch_add(&connected, 1);
}

static void scan_gamepads(void) {
    DIR *dir = opendir(INPUT_DIR);
    if (!dir)
        return;

    for (struct dirent *entry; (entry = readdir(dir));)
        open_gamepad(entry->d_name);
    closedir(dir);
}

static void read_inotify(void) {
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t len;

    while ((len = read(watcher.inotify_fd, buf, sizeof buf)) > 0)
        for (char *cursor = buf; cursor < buf + len;) {
            const struct inotify_event *event = (const struct inotify_event *)cursor;
            if (event->len)
                open_gamepad(event->name);
            cursor += sizeof *event + event->len;
        }
}

static bool apply_key(struct gamepad *pad, const struct input_event *event) {
    bool pressed = event->value != 0;
    update_held(pad, test_bit(pad->keys_down, event->code), pressed);
    assign_bit(pad->keys_down, event->code, pressed);
    return true;
}

static bool apply_axis(struct gamepad *pad, const struct input_event *event) {
    struct axis *axis = &pad->axes[event->code];
    bool was_deflected = axis->deflected;
    bool deflected = abs(event->value - axis->rest) > axis->threshold;
    update_held(pad, was_deflected, deflected);
    axis->deflected = deflected;
    return was_deflected || deflected;
}

static bool apply_event(struct gamepad *pad, const struct input_event *event) {
    if (event->type == EV_KEY && event->code < KEY_CNT)
        return apply_key(pad, event);
    if (event->type == EV_ABS && event->code < ABS_CNT)
        return apply_axis(pad, event);
    return false;
}

static bool read_gamepad(struct gamepad *pad, bool *active) {
    struct input_event events[64];
    ssize_t len;

    while ((len = read(pad->fd, events, sizeof events)) > 0)
        for (size_t i = 0; i < (size_t)len / sizeof *events; i++)
            *active |= apply_event(pad, &events[i]);

    return len < 0 && errno == EAGAIN;
}

static bool open_watcher(void) {
    watcher.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    watcher.inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

    return watcher.epoll_fd >= 0 && watcher.inotify_fd >= 0 &&
           inotify_add_watch(watcher.inotify_fd, INPUT_DIR, IN_CREATE | IN_ATTRIB) >= 0 && watch_fd(watcher.inotify_fd, NULL);
}

static void close_watcher(void) {
    while (watcher.gamepads)
        close_gamepad(watcher.gamepads);
    if (watcher.inotify_fd >= 0)
        close(watcher.inotify_fd);
    if (watcher.epoll_fd >= 0)
        close(watcher.epoll_fd);
}

static void *watch_gamepads(void *) {
    if (!open_watcher()) {
        close_watcher();
        return NULL;
    }
    scan_gamepads();

    struct epoll_event events[MAX_EVENTS];
    for (;;) {
        int count = epoll_wait(watcher.epoll_fd, events, MAX_EVENTS, -1);
        if (count < 0 && errno != EINTR)
            break;

        bool active = false;
        for (int i = 0; i < count; i++) {
            struct gamepad *pad = events[i].data.ptr;
            if (!pad)
                read_inotify();
            else if (!read_gamepad(pad, &active))
                close_gamepad(pad);
        }

        if (active)
            activity_mark(&watcher.activity, watcher.wake_fd);
    }

    close_watcher();
    return NULL;
}

void gamepad_input_start(double deadzone, uint64_t timeout_ns, int wake_fd) {
    watcher.deadzone = deadzone;
    activity_start(&watcher.activity, timeout_ns);
    watcher.wake_fd = wake_fd;

    pthread_t thread;
    if (pthread_create(&thread, NULL, watch_gamepads, NULL) == 0)
        pthread_detach(thread);
}

bool gamepad_input_is_connected(void) {
    return atomic_load(&connected) > 0;
}

bool gamepad_input_is_idle(void) {
    return atomic_load(&held_inputs) == 0 && activity_is_idle(&watcher.activity);
}
