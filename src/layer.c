#define VK_USE_PLATFORM_WAYLAND_KHR
#define VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_XLIB_KHR

#include "clock.h"
#include "dispatch_map.h"
#include "gamepad_input.h"
#include "idle_notify.h"
#include "pacer.h"
#include "wayland_focus.h"
#include "x11_watch.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#define VKNEMU_EXPORT __attribute__((visibility("default")))
#define HOOK(fn) {"vk" #fn, (PFN_vkVoidFunction)vknemu_##fn}
#define ARRAY_LEN(array) (sizeof(array) / sizeof((array)[0]))

struct hook {
    const char *name;
    PFN_vkVoidFunction function;
};

struct layer_link_header {
    VkStructureType sType;
    const void *pNext;
    VkLayerFunction function;
};

struct instance_data {
    PFN_vkGetInstanceProcAddr next_gipa;
    PFN_vkDestroyInstance destroy_instance;
    PFN_vkCreateXlibSurfaceKHR create_xlib_surface;
    PFN_vkCreateXcbSurfaceKHR create_xcb_surface;
    PFN_vkCreateWaylandSurfaceKHR create_wayland_surface;
};

struct device_data {
    PFN_vkGetDeviceProcAddr next_gdpa;
    PFN_vkDestroyDevice destroy_device;
    PFN_vkQueuePresentKHR queue_present;
};

enum input_source {
    INPUT_NONE = 0,
    INPUT_KEYBOARD = 1 << 0,
    INPUT_GAMEPAD = 1 << 1,
    INPUT_ALL = INPUT_KEYBOARD | INPUT_GAMEPAD,
};

struct input_mode {
    const char *name;
    enum input_source sources;
};

static const struct input_mode input_modes[] = {
    {"all", INPUT_ALL},
    {"keyboard", INPUT_KEYBOARD},
    {"gamepad", INPUT_GAMEPAD},
    {"none", INPUT_NONE},
};

static struct dispatch_map instances = DISPATCH_MAP_INIT;
static struct dispatch_map devices = DISPATCH_MAP_INIT;
static struct pacer pacer;
static pthread_once_t tracking_once = PTHREAD_ONCE_INIT;
static pthread_once_t x11_input_once = PTHREAD_ONCE_INIT;
static enum input_source sources;
static uint64_t timeout_ns;
static bool track_focus;
static bool track_windows;
static uint64_t delay_end_ns;
static int wake_fd = -1;

static double env_double(const char *name, double fallback, double min) {
    const char *value = getenv(name);
    if (!value)
        return fallback;

    char *end;
    double parsed = strtod(value, &end);
    return end != value && parsed >= min ? parsed : fallback;
}

static bool env_flag(const char *name, bool fallback) {
    const char *value = getenv(name);
    return value ? strcmp(value, "0") != 0 : fallback;
}

static enum input_source env_input_sources(void) {
    const char *value = getenv("VKNEMU_INPUT");
    for (size_t i = 0; value && i < ARRAY_LEN(input_modes); i++)
        if (!strcmp(value, input_modes[i].name))
            return input_modes[i].sources;
    return INPUT_ALL;
}

static void start_tracking(void) {
    timeout_ns = (uint64_t)(env_double("VKNEMU_TIMEOUT", 2, 0) * NS_PER_SEC);
    delay_end_ns = monotonic_ns() + (uint64_t)(env_double("VKNEMU_DELAY", 0, 0) * NS_PER_SEC);
    sources = env_input_sources();
    track_focus = env_flag("VKNEMU_UNFOCUSED", true);
    track_windows = track_focus || (sources & INPUT_KEYBOARD);
    wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

    pacer_set_fps(&pacer, env_double("VKNEMU_IDLE_FPS", 30, 1));
    if (sources & INPUT_KEYBOARD)
        idle_notify_start((uint32_t)(timeout_ns / NS_PER_MS), wake_fd);
    if (sources & INPUT_GAMEPAD)
        gamepad_input_start(env_double("VKNEMU_DEADZONE", 0.02, 0), timeout_ns, wake_fd);
    if (track_windows)
        x11_watch_start(wake_fd);
}

static void ensure_tracking(void) {
    pthread_once(&tracking_once, start_tracking);
}

static void start_x11_input(void) {
    x11_watch_start_input(timeout_ns);
}

static void track_x11_window(uint32_t window) {
    ensure_tracking();
    if (sources & INPUT_KEYBOARD)
        pthread_once(&x11_input_once, start_x11_input);
    if (track_windows)
        x11_watch_track(window);
}

static void track_wayland_display(struct wl_display *display) {
    ensure_tracking();
    if (track_windows)
        wayland_focus_track(display);
}

static bool delay_elapsed(void) {
    return monotonic_ns() >= delay_end_ns;
}

static bool focus_lost(void) {
    return track_focus && !(x11_watch_is_focused() && wayland_focus_is_focused());
}

static bool keyboard_watched(void) {
    return (sources & INPUT_KEYBOARD) && (idle_notify_is_available() || x11_watch_input_is_available());
}

static bool gamepad_watched(void) {
    return (sources & INPUT_GAMEPAD) && gamepad_input_is_connected();
}

static bool keyboard_idle(void) {
    return !keyboard_watched() ||
           (idle_notify_is_idle() && x11_watch_input_is_idle() && !x11_watch_input_held() && !wayland_focus_keys_held());
}

static bool gamepad_idle(void) {
    return !gamepad_watched() || gamepad_input_is_idle();
}

static bool should_limit(void) {
    bool input_idle = (keyboard_watched() || gamepad_watched()) && keyboard_idle() && gamepad_idle();
    return delay_elapsed() && (focus_lost() || input_idle);
}

static void *find_layer_link(const void *chain, VkStructureType type) {
    for (const struct layer_link_header *link = chain; link; link = link->pNext)
        if (link->sType == type && link->function == VK_LAYER_LINK_INFO)
            return (void *)link;
    return NULL;
}

static PFN_vkVoidFunction find_hook(const struct hook *hooks, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++)
        if (!strcmp(hooks[i].name, name))
            return hooks[i].function;
    return NULL;
}

static PFN_vkVoidFunction hook_if_next(const struct hook *hooks, size_t count, const char *name, PFN_vkVoidFunction next) {
    PFN_vkVoidFunction hook = find_hook(hooks, count, name);
    return hook && next ? hook : next;
}

static VKAPI_ATTR VkResult VKAPI_CALL vknemu_CreateInstance(const VkInstanceCreateInfo *info,
                                                            const VkAllocationCallbacks *allocator,
                                                            VkInstance *instance) {
    VkLayerInstanceCreateInfo *link = find_layer_link(info->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);
    if (!link)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)next_gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkResult result = create_instance(info, allocator, instance);
    if (result != VK_SUCCESS)
        return result;

    struct instance_data data = {
        .next_gipa = next_gipa,
        .destroy_instance = (PFN_vkDestroyInstance)next_gipa(*instance, "vkDestroyInstance"),
        .create_xlib_surface = (PFN_vkCreateXlibSurfaceKHR)next_gipa(*instance, "vkCreateXlibSurfaceKHR"),
        .create_xcb_surface = (PFN_vkCreateXcbSurfaceKHR)next_gipa(*instance, "vkCreateXcbSurfaceKHR"),
        .create_wayland_surface = (PFN_vkCreateWaylandSurfaceKHR)next_gipa(*instance, "vkCreateWaylandSurfaceKHR"),
    };
    if (!dispatch_map_put(&instances, *instance, &data, sizeof data)) {
        data.destroy_instance(*instance, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vknemu_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator) {
    struct instance_data *data = dispatch_map_get(&instances, instance);
    data->destroy_instance(instance, allocator);
    dispatch_map_remove(&instances, instance);
}

static VKAPI_ATTR VkResult VKAPI_CALL vknemu_CreateXlibSurfaceKHR(VkInstance instance,
                                                                  const VkXlibSurfaceCreateInfoKHR *info,
                                                                  const VkAllocationCallbacks *allocator,
                                                                  VkSurfaceKHR *surface) {
    struct instance_data *data = dispatch_map_get(&instances, instance);
    VkResult result = data->create_xlib_surface(instance, info, allocator, surface);
    if (result == VK_SUCCESS)
        track_x11_window((uint32_t)info->window);
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL vknemu_CreateXcbSurfaceKHR(VkInstance instance,
                                                                 const VkXcbSurfaceCreateInfoKHR *info,
                                                                 const VkAllocationCallbacks *allocator,
                                                                 VkSurfaceKHR *surface) {
    struct instance_data *data = dispatch_map_get(&instances, instance);
    VkResult result = data->create_xcb_surface(instance, info, allocator, surface);
    if (result == VK_SUCCESS)
        track_x11_window(info->window);
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL vknemu_CreateWaylandSurfaceKHR(VkInstance instance,
                                                                     const VkWaylandSurfaceCreateInfoKHR *info,
                                                                     const VkAllocationCallbacks *allocator,
                                                                     VkSurfaceKHR *surface) {
    struct instance_data *data = dispatch_map_get(&instances, instance);
    VkResult result = data->create_wayland_surface(instance, info, allocator, surface);
    if (result == VK_SUCCESS)
        track_wayland_display(info->display);
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL vknemu_CreateDevice(VkPhysicalDevice physical_device,
                                                          const VkDeviceCreateInfo *info,
                                                          const VkAllocationCallbacks *allocator,
                                                          VkDevice *device) {
    VkLayerDeviceCreateInfo *link = find_layer_link(info->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
    if (!link)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)next_gipa(VK_NULL_HANDLE, "vkCreateDevice");
    VkResult result = create_device(physical_device, info, allocator, device);
    if (result != VK_SUCCESS)
        return result;

    struct device_data data = {
        .next_gdpa = next_gdpa,
        .destroy_device = (PFN_vkDestroyDevice)next_gdpa(*device, "vkDestroyDevice"),
        .queue_present = (PFN_vkQueuePresentKHR)next_gdpa(*device, "vkQueuePresentKHR"),
    };
    if (!dispatch_map_put(&devices, *device, &data, sizeof data)) {
        data.destroy_device(*device, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    ensure_tracking();
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL vknemu_DestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator) {
    struct device_data *data = dispatch_map_get(&devices, device);
    data->destroy_device(device, allocator);
    dispatch_map_remove(&devices, device);
}

static VKAPI_ATTR VkResult VKAPI_CALL vknemu_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info) {
    struct device_data *data = dispatch_map_get(&devices, queue);

    wayland_focus_dispatch();
    if (should_limit())
        pacer_wait(&pacer, wake_fd);
    else
        pacer_mark(&pacer);

    return data->queue_present(queue, info);
}

VKNEMU_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vknemu_GetDeviceProcAddr(VkDevice device, const char *name);

static const struct hook device_hooks[] = {
    HOOK(GetDeviceProcAddr),
    HOOK(DestroyDevice),
    HOOK(QueuePresentKHR),
};

VKNEMU_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vknemu_GetDeviceProcAddr(VkDevice device, const char *name) {
    struct device_data *data = dispatch_map_get(&devices, device);
    if (!data)
        return NULL;

    return hook_if_next(device_hooks, ARRAY_LEN(device_hooks), name, data->next_gdpa(device, name));
}

VKNEMU_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vknemu_GetInstanceProcAddr(VkInstance instance, const char *name);

static const struct hook instance_hooks[] = {
    HOOK(GetInstanceProcAddr),
    HOOK(CreateInstance),
    HOOK(DestroyInstance),
    HOOK(CreateDevice),
    HOOK(GetDeviceProcAddr),
};

static const struct hook surface_hooks[] = {
    HOOK(CreateXlibSurfaceKHR),
    HOOK(CreateXcbSurfaceKHR),
    HOOK(CreateWaylandSurfaceKHR),
};

VKNEMU_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vknemu_GetInstanceProcAddr(VkInstance instance, const char *name) {
    PFN_vkVoidFunction hook = find_hook(instance_hooks, ARRAY_LEN(instance_hooks), name);
    if (hook)
        return hook;

    struct instance_data *data = instance ? dispatch_map_get(&instances, instance) : NULL;
    if (!data)
        return NULL;

    return hook_if_next(surface_hooks, ARRAY_LEN(surface_hooks), name, data->next_gipa(instance, name));
}
