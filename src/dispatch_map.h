#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

struct dispatch_entry;

struct dispatch_map {
    pthread_mutex_t lock;
    struct dispatch_entry *entries;
    size_t len;
    size_t cap;
};

#define DISPATCH_MAP_INIT {.lock = PTHREAD_MUTEX_INITIALIZER}

bool dispatch_map_put(struct dispatch_map *map, const void *handle, const void *value, size_t size);
void *dispatch_map_get(struct dispatch_map *map, const void *handle);
void dispatch_map_remove(struct dispatch_map *map, const void *handle);
