#include "dispatch_map.h"

#include <stdlib.h>
#include <string.h>

struct dispatch_entry {
    const void *key;
    void *value;
};

static const void *dispatch_key(const void *handle) {
    return *(const void *const *)handle;
}

static struct dispatch_entry *find_entry(struct dispatch_map *map, const void *key) {
    for (size_t i = 0; i < map->len; i++)
        if (map->entries[i].key == key)
            return &map->entries[i];
    return NULL;
}

static bool reserve_entry(struct dispatch_map *map) {
    if (map->len < map->cap)
        return true;

    size_t cap = map->cap ? map->cap * 2 : 4;
    struct dispatch_entry *entries = realloc(map->entries, cap * sizeof *entries);
    if (!entries)
        return false;

    map->entries = entries;
    map->cap = cap;
    return true;
}

bool dispatch_map_put(struct dispatch_map *map, const void *handle, const void *value, size_t size) {
    void *copy = malloc(size);
    if (!copy)
        return false;
    memcpy(copy, value, size);

    pthread_mutex_lock(&map->lock);
    bool stored = reserve_entry(map);
    if (stored)
        map->entries[map->len++] = (struct dispatch_entry){dispatch_key(handle), copy};
    pthread_mutex_unlock(&map->lock);

    if (!stored)
        free(copy);
    return stored;
}

void *dispatch_map_get(struct dispatch_map *map, const void *handle) {
    pthread_mutex_lock(&map->lock);
    struct dispatch_entry *entry = find_entry(map, dispatch_key(handle));
    void *value = entry ? entry->value : NULL;
    pthread_mutex_unlock(&map->lock);
    return value;
}

void dispatch_map_remove(struct dispatch_map *map, const void *handle) {
    pthread_mutex_lock(&map->lock);
    struct dispatch_entry *entry = find_entry(map, dispatch_key(handle));
    void *value = entry ? entry->value : NULL;
    if (entry)
        *entry = map->entries[--map->len];
    pthread_mutex_unlock(&map->lock);
    free(value);
}
