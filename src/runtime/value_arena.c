#include "usk/value.h"

#include <stdlib.h>
#include <string.h>

struct UskValueArenaAllocation {
    void *storage;
    size_t size;
    UskValueArenaAllocation *next;
};

void usk_value_arena_init(UskValueArena *arena) {
    usk_value_arena_init_limited(arena, 0, 0);
}

void usk_value_arena_init_limited(UskValueArena *arena,
                                  size_t maximum_allocations,
                                  size_t maximum_bytes) {
    if (!arena) return;
    memset(arena, 0, sizeof(*arena));
    arena->maximum_allocations = maximum_allocations;
    arena->maximum_bytes = maximum_bytes;
}

void usk_value_arena_destroy(UskValueArena *arena) {
    if (!arena) return;
    UskValueArenaAllocation *allocation = arena->allocations;
    while (allocation) {
        UskValueArenaAllocation *next = allocation->next;
        free(allocation->storage);
        free(allocation);
        allocation = next;
    }
    memset(arena, 0, sizeof(*arena));
}

void *usk_value_arena_allocate(UskValueArena *arena, size_t size,
                               bool zero_fill) {
    if (!arena || !size || arena->failed) return NULL;
    if ((arena->maximum_allocations &&
         arena->allocation_count >= arena->maximum_allocations) ||
        (arena->maximum_bytes &&
         (arena->allocation_bytes > arena->maximum_bytes ||
          size > arena->maximum_bytes - arena->allocation_bytes))) {
        arena->failed = true;
        return NULL;
    }
    if (arena->allocation_count == (size_t)-1) {
        arena->failed = true;
        return NULL;
    }
    void *storage = zero_fill ? calloc(1, size) : malloc(size);
    if (!storage) {
        arena->failed = true;
        return NULL;
    }
    UskValueArenaAllocation *allocation =
        (UskValueArenaAllocation *)malloc(sizeof(*allocation));
    if (!allocation) {
        free(storage);
        arena->failed = true;
        return NULL;
    }
    if (size > (size_t)-1 - arena->allocation_bytes) {
        free(storage);
        free(allocation);
        arena->failed = true;
        return NULL;
    }
    allocation->storage = storage;
    allocation->size = size;
    allocation->next = arena->allocations;
    arena->allocations = allocation;
    arena->allocation_count++;
    arena->allocation_bytes += size;
    return storage;
}

bool usk_value_arena_resize(UskValueArena *arena, void **storage, size_t size) {
    if (!arena || !storage || !*storage || !size || arena->failed) return false;
    UskValueArenaAllocation *allocation = arena->allocations;
    while (allocation && allocation->storage != *storage)
        allocation = allocation->next;
    if (!allocation) return false;
    size_t prior_bytes = arena->allocation_bytes - allocation->size;
    if (size > (size_t)-1 - prior_bytes) {
        arena->failed = true;
        return false;
    }
    if (arena->maximum_bytes &&
        (prior_bytes > arena->maximum_bytes ||
         size > arena->maximum_bytes - prior_bytes)) {
        arena->failed = true;
        return false;
    }
    void *grown = realloc(*storage, size);
    if (!grown) {
        arena->failed = true;
        return false;
    }
    allocation->storage = grown;
    allocation->size = size;
    arena->allocation_bytes = prior_bytes + size;
    *storage = grown;
    return true;
}

bool usk_value_arena_owns(const UskValueArena *arena, const void *storage) {
    if (!arena || !storage) return false;
    for (const UskValueArenaAllocation *allocation = arena->allocations;
         allocation; allocation = allocation->next)
        if (allocation->storage == storage) return true;
    return false;
}

Value string_value_in(UskValueArena *arena, const char *text) {
    if (!text) text = "";
    size_t length = strlen(text);
    if (length == (size_t)-1) {
        if (arena) arena->failed = true;
        return null_value();
    }
    char *copy = (char *)usk_value_arena_allocate(arena, length + 1, false);
    if (!copy) return null_value();
    memcpy(copy, text, length + 1);
    return (Value){.kind = V_STRING, .as.s = copy};
}

Value character_value_in(UskValueArena *arena, char character) {
    char text[2] = {character, '\0'};
    return string_value_in(arena, text);
}

Value array_value_in(UskValueArena *arena, Value *items, size_t count) {
    if (!arena || (count && (!items || !usk_value_arena_owns(arena, items)))) {
        if (arena) arena->failed = true;
        return null_value();
    }
    Array *array = (Array *)usk_value_arena_allocate(arena, sizeof(*array), true);
    if (!array) return null_value();
    array->items = items;
    array->count = count;
    array->capacity = count;
    array->arena = arena;
    return (Value){.kind = V_ARRAY, .as.a = array};
}

size_t usk_value_arena_allocation_count(const UskValueArena *arena) {
    return arena ? arena->allocation_count : 0;
}

size_t usk_value_arena_allocation_bytes(const UskValueArena *arena) {
    return arena ? arena->allocation_bytes : 0;
}

size_t usk_value_arena_remaining_allocations(const UskValueArena *arena) {
    if (!arena || !arena->maximum_allocations) return (size_t)-1;
    return arena->allocation_count >= arena->maximum_allocations
        ? 0 : arena->maximum_allocations - arena->allocation_count;
}

size_t usk_value_arena_remaining_bytes(const UskValueArena *arena) {
    if (!arena || !arena->maximum_bytes) return (size_t)-1;
    return arena->allocation_bytes >= arena->maximum_bytes
        ? 0 : arena->maximum_bytes - arena->allocation_bytes;
}

bool usk_value_arena_exhausted(const UskValueArena *arena) {
    return !arena || arena->failed;
}
