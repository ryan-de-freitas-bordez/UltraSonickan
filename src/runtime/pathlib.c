#include "usk/pathlib.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const UskPathBuiltinInfo builtins[] = {
    {"path::normalize", 1, USK_PATH_RESULT_TEXT, {USK_PATH_ARGUMENT_TEXT}},
    {"path::basename", 1, USK_PATH_RESULT_TEXT, {USK_PATH_ARGUMENT_TEXT}},
    {"path::dirname", 1, USK_PATH_RESULT_TEXT, {USK_PATH_ARGUMENT_TEXT}},
    {"path::extension", 1, USK_PATH_RESULT_TEXT, {USK_PATH_ARGUMENT_TEXT}},
    {"path::stem", 1, USK_PATH_RESULT_TEXT, {USK_PATH_ARGUMENT_TEXT}},
    {"path::join", 2, USK_PATH_RESULT_TEXT,
        {USK_PATH_ARGUMENT_TEXT, USK_PATH_ARGUMENT_TEXT}},
    {"path::is_absolute", 1, USK_PATH_RESULT_BOOLEAN,
        {USK_PATH_ARGUMENT_TEXT}},
    {"path::component_count", 1, USK_PATH_RESULT_INTEGER,
        {USK_PATH_ARGUMENT_TEXT}},
    {"path::change_extension", 2, USK_PATH_RESULT_TEXT,
        {USK_PATH_ARGUMENT_TEXT, USK_PATH_ARGUMENT_TEXT}},
    {"path::has_extension", 2, USK_PATH_RESULT_BOOLEAN,
        {USK_PATH_ARGUMENT_TEXT, USK_PATH_ARGUMENT_TEXT}}
};

size_t usk_path_builtin_count(void) {
    return sizeof(builtins) / sizeof(builtins[0]);
}

const UskPathBuiltinInfo *usk_path_builtin_at(size_t index) {
    return index < usk_path_builtin_count() ? &builtins[index] : NULL;
}

const UskPathBuiltinInfo *usk_path_builtin_find(const char *name) {
    if (!name) return NULL;
    for (size_t index = 0; index < usk_path_builtin_count(); ++index)
        if (!strcmp(name, builtins[index].name)) return &builtins[index];
    return NULL;
}

bool usk_path_builtin_is_name(const char *name) {
    return usk_path_builtin_find(name) != NULL;
}

typedef struct {
    size_t *starts;
    size_t *lengths;
    size_t count;
    size_t capacity;
} PathComponents;

static bool reserve_components(PathComponents *components, size_t wanted) {
    if (wanted <= components->capacity) return true;
    size_t capacity = components->capacity ? components->capacity : 8;
    while (capacity < wanted) {
        if (capacity > (size_t)-1 / 2) { capacity = wanted; break; }
        capacity *= 2;
    }
    if (capacity > (size_t)-1 / sizeof(size_t)) return false;
    size_t *starts = (size_t *)realloc(components->starts,
                                      capacity * sizeof(size_t));
    if (!starts) return false;
    components->starts = starts;
    size_t *lengths = (size_t *)realloc(components->lengths,
                                       capacity * sizeof(size_t));
    if (!lengths) return false;
    components->lengths = lengths;
    components->capacity = capacity;
    return true;
}

static void free_components(PathComponents *components) {
    free(components->starts);
    free(components->lengths);
    memset(components, 0, sizeof(*components));
}

static bool component_equals(const char *path, size_t start, size_t length,
                             const char *text) {
    return strlen(text) == length && memcmp(path + start, text, length) == 0;
}

static bool add_component(PathComponents *components, size_t start,
                          size_t length) {
    if (!reserve_components(components, components->count + 1)) return false;
    components->starts[components->count] = start;
    components->lengths[components->count] = length;
    components->count++;
    return true;
}

bool usk_path_normalize(const char *path, char *destination,
                       size_t capacity, size_t *written) {
    if (!path || !destination || capacity < 2) return false;
    size_t length = strlen(path), root = usk_path_root_length(path);
    PathComponents components = {0};
    size_t offset = root;
    bool absolute = usk_path_is_absolute(path);
    while (offset < length) {
        while (offset < length && usk_path_is_separator(path[offset])) offset++;
        size_t start = offset;
        while (offset < length && !usk_path_is_separator(path[offset])) offset++;
        size_t part_length = offset - start;
        if (!part_length || component_equals(path, start, part_length, "."))
            continue;
        if (component_equals(path, start, part_length, "..")) {
            if (components.count && !component_equals(path,
                    components.starts[components.count - 1],
                    components.lengths[components.count - 1], "..")) {
                components.count--;
            } else if (!absolute &&
                       !add_component(&components, start, part_length)) {
                free_components(&components);
                return false;
            }
        } else if (!add_component(&components, start, part_length)) {
            free_components(&components);
            return false;
        }
    }

    size_t output = 0;
    if (root) {
        for (size_t index = 0; index < root; ++index) {
            char character = path[index];
            if (usk_path_is_separator(character)) character = '/';
            if (output + 1 >= capacity) { free_components(&components); return false; }
            destination[output++] = character;
        }
    }
    bool drive_relative = root == 2 && path[1] == ':' &&
                          !usk_path_is_separator(path[2]);
    for (size_t index = 0; index < components.count; ++index) {
        bool need_separator = output && destination[output - 1] != '/' &&
                              !(drive_relative && output == 2 && index == 0);
        if (need_separator) {
            if (output + 1 >= capacity) { free_components(&components); return false; }
            destination[output++] = '/';
        }
        size_t part_length = components.lengths[index];
        if (part_length > capacity - output - 1) {
            free_components(&components);
            return false;
        }
        memcpy(destination + output, path + components.starts[index], part_length);
        output += part_length;
    }
    if (!output) destination[output++] = '.';
    destination[output] = '\0';
    free_components(&components);
    if (written) *written = output;
    return true;
}

static const char *path_argument(const Value *arguments, size_t index) {
    return arguments[index].as.s ? arguments[index].as.s : "";
}

static Value arena_text(UskValueArena *arena, const char *text,
                        size_t length) {
    char *copy = (char *)usk_value_arena_allocate(arena, length + 1, false);
    if (!copy) return null_value();
    if (length) memcpy(copy, text, length);
    copy[length] = '\0';
    return (Value){.kind = V_STRING, .as.s = copy};
}

static UskPathBuiltinStatus normalize_value(const char *path,
    UskValueArena *arena, Value *result) {
    size_t length = strlen(path);
    if (length > (size_t)-4) return USK_PATH_BUILTIN_RANGE_ERROR;
    char *buffer = (char *)malloc(length + 4);
    if (!buffer) return USK_PATH_BUILTIN_ALLOCATION_FAILURE;
    size_t written = 0;
    bool success = usk_path_normalize(path, buffer, length + 4, &written);
    if (!success) { free(buffer); return USK_PATH_BUILTIN_ALLOCATION_FAILURE; }
    *result = arena_text(arena, buffer, written);
    free(buffer);
    return arena->failed ? USK_PATH_BUILTIN_ALLOCATION_FAILURE
                         : USK_PATH_BUILTIN_OK;
}

static UskPathBuiltinStatus join_values(const char *base, const char *child,
    UskValueArena *arena, Value *result) {
    if (usk_path_is_absolute(child))
        return normalize_value(child, arena, result);
    size_t base_length = strlen(base), child_length = strlen(child);
    if (base_length > (size_t)-1 - child_length - 2)
        return USK_PATH_BUILTIN_RANGE_ERROR;
    char *joined = (char *)malloc(base_length + child_length + 2);
    if (!joined) return USK_PATH_BUILTIN_ALLOCATION_FAILURE;
    memcpy(joined, base, base_length);
    size_t offset = base_length;
    if (offset && !usk_path_is_separator(base[offset - 1])) joined[offset++] = '/';
    memcpy(joined + offset, child, child_length + 1);
    UskPathBuiltinStatus status = normalize_value(joined, arena, result);
    free(joined);
    return status;
}

static UskPathBuiltinStatus text_slice(const char *text, size_t first,
    size_t last, UskValueArena *arena, Value *result) {
    if (last < first) return USK_PATH_BUILTIN_RANGE_ERROR;
    *result = arena_text(arena, text + first, last - first);
    return arena->failed ? USK_PATH_BUILTIN_ALLOCATION_FAILURE
                         : USK_PATH_BUILTIN_OK;
}

UskPathBuiltinStatus usk_path_builtin_call(
    const char *name, const Value *arguments, size_t argument_count,
    UskValueArena *arena, Value *result) {
    const UskPathBuiltinInfo *info = usk_path_builtin_find(name);
    if (!info) return USK_PATH_BUILTIN_UNKNOWN;
    if (!result || !arena) return USK_PATH_BUILTIN_TYPE_MISMATCH;
    if (argument_count != info->argument_count)
        return USK_PATH_BUILTIN_ARGUMENT_COUNT;
    if (!arguments) return USK_PATH_BUILTIN_TYPE_MISMATCH;
    for (size_t index = 0; index < argument_count; ++index)
        if (arguments[index].kind != V_STRING)
            return USK_PATH_BUILTIN_TYPE_MISMATCH;
    const char *first = path_argument(arguments, 0);
    const char *second = argument_count > 1 ? path_argument(arguments, 1) : "";
    *result = null_value();
    if (!strcmp(name, "path::normalize"))
        return normalize_value(first, arena, result);
    if (!strcmp(name, "path::is_absolute")) {
        *result = bool_value(usk_path_is_absolute(first));
        return USK_PATH_BUILTIN_OK;
    }
    if (!strcmp(name, "path::component_count")) {
        size_t count = usk_path_component_count(first);
        if (count > (size_t)LLONG_MAX) return USK_PATH_BUILTIN_RANGE_ERROR;
        *result = int_value((long long)count);
        return USK_PATH_BUILTIN_OK;
    }
    if (!strcmp(name, "path::join"))
        return join_values(first, second, arena, result);
    if (!strcmp(name, "path::basename") || !strcmp(name, "path::dirname")) {
        size_t start = 0, end = 0;
        size_t length = strlen(first);
        if (!usk_path_component_bounds(first, &start, &end)) {
            if (!strcmp(name, "path::basename"))
                *result = arena_text(arena, "", 0);
            else *result = arena_text(arena, ".", 1);
        } else if (!strcmp(name, "path::basename")) {
            *result = arena_text(arena, first + start, end - start);
        } else {
            size_t parent_end = start;
            while (parent_end && usk_path_is_separator(first[parent_end - 1]))
                parent_end--;
            if (!parent_end && length && usk_path_is_separator(first[0])) {
                *result = arena_text(arena, "/", 1);
            } else if (!parent_end) {
                *result = arena_text(arena, ".", 1);
            } else {
                *result = arena_text(arena, first, parent_end);
            }
        }
        return arena->failed ? USK_PATH_BUILTIN_ALLOCATION_FAILURE
                             : USK_PATH_BUILTIN_OK;
    }
    if (!strcmp(name, "path::extension") || !strcmp(name, "path::stem") ||
        !strcmp(name, "path::has_extension")) {
        size_t start = 0, end = 0;
        bool has_component = usk_path_component_bounds(first, &start, &end);
        size_t extension_start = 0, extension_end = 0;
        bool has_extension = usk_path_extension_bounds(first,
            &extension_start, &extension_end);
        if (!strcmp(name, "path::has_extension")) {
            *result = bool_value(usk_path_has_extension(first, second));
            return USK_PATH_BUILTIN_OK;
        }
        if (!has_component) return text_slice(first, 0, 0, arena, result);
        size_t basename_end = end;
        size_t basename_start = start;
        if (!strcmp(name, "path::extension")) {
            if (!has_extension) return text_slice(first, end, end, arena, result);
            return text_slice(first, extension_start, extension_end, arena, result);
        }
        if (has_extension) basename_end = extension_start - 1;
        return text_slice(first, basename_start, basename_end, arena, result);
    }
    if (!strcmp(name, "path::change_extension")) {
        size_t start = 0, end = 0, extension_start = 0, extension_end = 0;
        bool has_component = usk_path_component_bounds(first, &start, &end);
        bool has_extension = usk_path_extension_bounds(first,
            &extension_start, &extension_end);
        size_t replace_from = has_extension ? extension_start - 1 : end;
        if (!has_component) replace_from = strlen(first);
        size_t second_length = strlen(second);
        bool add_dot = second_length && second[0] != '.';
        size_t prefix_length = replace_from;
        if (prefix_length > (size_t)-1 - second_length - (size_t)add_dot - 1)
            return USK_PATH_BUILTIN_RANGE_ERROR;
        size_t output_length = prefix_length + second_length + (size_t)add_dot;
        char *buffer = (char *)usk_value_arena_allocate(arena,
            output_length + 1, false);
        if (!buffer) return USK_PATH_BUILTIN_ALLOCATION_FAILURE;
        memcpy(buffer, first, prefix_length);
        size_t offset = prefix_length;
        if (add_dot) buffer[offset++] = '.';
        memcpy(buffer + offset, second, second_length);
        buffer[output_length] = '\0';
        *result = (Value){.kind = V_STRING, .as.s = buffer};
        return USK_PATH_BUILTIN_OK;
    }
    return USK_PATH_BUILTIN_UNKNOWN;
}

const char *usk_path_builtin_status_name(UskPathBuiltinStatus status) {
    switch (status) {
        case USK_PATH_BUILTIN_OK: return "ok";
        case USK_PATH_BUILTIN_UNKNOWN: return "not a path built-in";
        case USK_PATH_BUILTIN_ARGUMENT_COUNT: return "wrong argument count";
        case USK_PATH_BUILTIN_TYPE_MISMATCH: return "path argument type mismatch";
        case USK_PATH_BUILTIN_RANGE_ERROR: return "path length is out of range";
        case USK_PATH_BUILTIN_ALLOCATION_FAILURE: return "allocation failure";
    }
    return "unknown path built-in status";
}
