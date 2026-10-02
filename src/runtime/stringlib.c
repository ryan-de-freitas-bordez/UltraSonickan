#include "usk/stringlib.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>

static const UskStringBuiltinInfo builtins[] = {
    {"string::length", 1, USK_STRING_RESULT_INTEGER,
        {USK_STRING_ARGUMENT_TEXT}},
    {"string::contains", 2, USK_STRING_RESULT_BOOLEAN,
        {USK_STRING_ARGUMENT_TEXT, USK_STRING_ARGUMENT_TEXT}},
    {"string::starts_with", 2, USK_STRING_RESULT_BOOLEAN,
        {USK_STRING_ARGUMENT_TEXT, USK_STRING_ARGUMENT_TEXT}},
    {"string::ends_with", 2, USK_STRING_RESULT_BOOLEAN,
        {USK_STRING_ARGUMENT_TEXT, USK_STRING_ARGUMENT_TEXT}},
    {"string::index_of", 2, USK_STRING_RESULT_INTEGER,
        {USK_STRING_ARGUMENT_TEXT, USK_STRING_ARGUMENT_TEXT}},
    {"string::substring", 3, USK_STRING_RESULT_TEXT,
        {USK_STRING_ARGUMENT_TEXT, USK_STRING_ARGUMENT_INTEGER,
         USK_STRING_ARGUMENT_INTEGER}},
    {"string::concat", 2, USK_STRING_RESULT_TEXT,
        {USK_STRING_ARGUMENT_TEXT, USK_STRING_ARGUMENT_TEXT}},
    {"string::repeat", 2, USK_STRING_RESULT_TEXT,
        {USK_STRING_ARGUMENT_TEXT, USK_STRING_ARGUMENT_INTEGER}},
    {"string::replace", 3, USK_STRING_RESULT_TEXT,
        {USK_STRING_ARGUMENT_TEXT, USK_STRING_ARGUMENT_TEXT,
         USK_STRING_ARGUMENT_TEXT}},
    {"string::trim", 1, USK_STRING_RESULT_TEXT,
        {USK_STRING_ARGUMENT_TEXT}},
    {"string::to_upper", 1, USK_STRING_RESULT_TEXT,
        {USK_STRING_ARGUMENT_TEXT}},
    {"string::to_lower", 1, USK_STRING_RESULT_TEXT,
        {USK_STRING_ARGUMENT_TEXT}},
    {"string::is_empty", 1, USK_STRING_RESULT_BOOLEAN,
        {USK_STRING_ARGUMENT_TEXT}},
    {"string::split", 2, USK_STRING_RESULT_ARRAY,
        {USK_STRING_ARGUMENT_TEXT, USK_STRING_ARGUMENT_TEXT}}
};

size_t usk_string_builtin_count(void) {
    return sizeof(builtins) / sizeof(builtins[0]);
}

const UskStringBuiltinInfo *usk_string_builtin_at(size_t index) {
    if (index >= usk_string_builtin_count()) return NULL;
    return &builtins[index];
}

const UskStringBuiltinInfo *usk_string_builtin_find(const char *name) {
    if (!name) return NULL;
    for (size_t index = 0; index < usk_string_builtin_count(); ++index)
        if (!strcmp(builtins[index].name, name)) return &builtins[index];
    return NULL;
}

bool usk_string_builtin_is_name(const char *name) {
    return usk_string_builtin_find(name) != NULL;
}

bool usk_string_equal_bytes(const char *left, const char *right) {
    if (!left) left = "";
    if (!right) right = "";
    return strcmp(left, right) == 0;
}

bool usk_string_contains_bytes(const char *text, const char *needle) {
    if (!text) text = "";
    if (!needle) needle = "";
    return strstr(text, needle) != NULL;
}

bool usk_string_starts_with_bytes(const char *text, const char *prefix) {
    if (!text) text = "";
    if (!prefix) prefix = "";
    size_t prefix_length = strlen(prefix);
    return strlen(text) >= prefix_length &&
        memcmp(text, prefix, prefix_length) == 0;
}

bool usk_string_ends_with_bytes(const char *text, const char *suffix) {
    if (!text) text = "";
    if (!suffix) suffix = "";
    size_t text_length = strlen(text), suffix_length = strlen(suffix);
    return text_length >= suffix_length &&
        memcmp(text + text_length - suffix_length, suffix, suffix_length) == 0;
}

long long usk_string_index_of_bytes(const char *text, const char *needle) {
    if (!text) text = "";
    if (!needle) needle = "";
    const char *match = strstr(text, needle);
    if (!match) return -1;
    size_t offset = (size_t)(match - text);
    return offset > (size_t)LLONG_MAX ? -1 : (long long)offset;
}

bool usk_string_substring_bytes(const char *text, size_t start, size_t count,
                                char *destination, size_t capacity) {
    if (!text || !destination) return false;
    size_t length = strlen(text);
    if (start > length || count > length - start || count >= capacity)
        return false;
    if (count) memcpy(destination, text + start, count);
    destination[count] = '\0';
    return true;
}

size_t usk_string_trim_bounds(const char *text, size_t *start, size_t *end) {
    if (!text || !start || !end) return 0;
    size_t length = strlen(text), first = 0, last = length;
    while (first < last && isspace((unsigned char)text[first])) first++;
    while (last > first && isspace((unsigned char)text[last - 1])) last--;
    *start = first;
    *end = last;
    return last - first;
}

char usk_string_ascii_upper(char character) {
    unsigned char byte = (unsigned char)character;
    if (byte >= 'a' && byte <= 'z') byte = (unsigned char)(byte - 'a' + 'A');
    return (char)byte;
}

char usk_string_ascii_lower(char character) {
    unsigned char byte = (unsigned char)character;
    if (byte >= 'A' && byte <= 'Z') byte = (unsigned char)(byte - 'A' + 'a');
    return (char)byte;
}

static const char *text_argument(const Value *arguments, size_t index,
                                 size_t count) {
    if (index >= count || arguments[index].kind != V_STRING) return NULL;
    return arguments[index].as.s ? arguments[index].as.s : "";
}

static bool integer_argument(const Value *arguments, size_t index,
                             size_t count, long long *result) {
    if (index >= count || arguments[index].kind != V_INT || !result)
        return false;
    *result = arguments[index].as.i;
    return true;
}

static Value arena_text(UskValueArena *arena, const char *text, size_t length) {
    if (!arena || length == (size_t)-1) return null_value();
    char *copy = (char *)usk_value_arena_allocate(arena, length + 1, false);
    if (!copy) return null_value();
    if (length) memcpy(copy, text, length);
    copy[length] = '\0';
    return (Value){.kind = V_STRING, .as.s = copy};
}

static UskStringBuiltinStatus make_repeat(const char *text, long long count,
                                          UskValueArena *arena, Value *result) {
    if (count < 0) return USK_STRING_BUILTIN_RANGE_ERROR;
    if ((unsigned long long)count > (unsigned long long)((size_t)-1))
        return USK_STRING_BUILTIN_RANGE_ERROR;
    size_t length = strlen(text), repetitions = (size_t)count;
    if (repetitions && length > ((size_t)-1 - 1) / repetitions)
        return USK_STRING_BUILTIN_RANGE_ERROR;
    size_t output_length = length * repetitions;
    if (output_length > (size_t)LLONG_MAX || output_length == (size_t)-1)
        return USK_STRING_BUILTIN_RANGE_ERROR;
    char *output = (char *)usk_value_arena_allocate(arena,
        output_length + 1, false);
    if (!output) return USK_STRING_BUILTIN_ALLOCATION_FAILURE;
    size_t offset = 0;
    for (size_t index = 0; index < repetitions; ++index) {
        if (length) memcpy(output + offset, text, length);
        offset += length;
    }
    output[offset] = '\0';
    *result = (Value){.kind = V_STRING, .as.s = output};
    return USK_STRING_BUILTIN_OK;
}

static UskStringBuiltinStatus make_replacement(const char *text,
    const char *needle, const char *replacement, UskValueArena *arena,
    Value *result) {
    size_t text_length = strlen(text), needle_length = strlen(needle);
    size_t replacement_length = strlen(replacement);
    if (!needle_length) return USK_STRING_BUILTIN_RANGE_ERROR;
    size_t matches = 0;
    for (const char *cursor = text; (cursor = strstr(cursor, needle)) != NULL;
         cursor += needle_length) matches++;
    size_t output_length = text_length;
    if (replacement_length >= needle_length) {
        size_t growth = replacement_length - needle_length;
        if (growth && matches > ((size_t)-1 - output_length) / growth)
            return USK_STRING_BUILTIN_RANGE_ERROR;
        output_length += growth * matches;
    } else {
        output_length -= (needle_length - replacement_length) * matches;
    }
    if (output_length > (size_t)LLONG_MAX)
        return USK_STRING_BUILTIN_RANGE_ERROR;
    char *output = (char *)usk_value_arena_allocate(arena,
        output_length + 1, false);
    if (!output) return USK_STRING_BUILTIN_ALLOCATION_FAILURE;
    const char *cursor = text;
    size_t written = 0;
    const char *match;
    while ((match = strstr(cursor, needle)) != NULL) {
        size_t prefix_length = (size_t)(match - cursor);
        memcpy(output + written, cursor, prefix_length);
        written += prefix_length;
        memcpy(output + written, replacement, replacement_length);
        written += replacement_length;
        cursor = match + needle_length;
    }
    size_t tail_length = strlen(cursor);
    memcpy(output + written, cursor, tail_length);
    written += tail_length;
    output[written] = '\0';
    *result = (Value){.kind = V_STRING, .as.s = output};
    return USK_STRING_BUILTIN_OK;
}

static UskStringBuiltinStatus make_case(const char *text, bool uppercase,
                                        UskValueArena *arena, Value *result) {
    size_t length = strlen(text);
    char *output = (char *)usk_value_arena_allocate(arena, length + 1, false);
    if (!output) return USK_STRING_BUILTIN_ALLOCATION_FAILURE;
    for (size_t index = 0; index < length; ++index)
        output[index] = uppercase ? usk_string_ascii_upper(text[index])
                                  : usk_string_ascii_lower(text[index]);
    output[length] = '\0';
    *result = (Value){.kind = V_STRING, .as.s = output};
    return USK_STRING_BUILTIN_OK;
}

static UskStringBuiltinStatus make_split(const char *text, const char *separator,
                                         UskValueArena *arena, Value *result) {
    size_t separator_length = strlen(separator);
    if (!separator_length) return USK_STRING_BUILTIN_RANGE_ERROR;
    size_t count = 1;
    for (const char *cursor = text; (cursor = strstr(cursor, separator)) != NULL;
         cursor += separator_length) {
        if (count == (size_t)-1) return USK_STRING_BUILTIN_RANGE_ERROR;
        count++;
    }
    if (count > (size_t)-1 / sizeof(Value))
        return USK_STRING_BUILTIN_RANGE_ERROR;
    Value *items = (Value *)usk_value_arena_allocate(arena,
        count * sizeof(*items), false);
    if (!items) return USK_STRING_BUILTIN_ALLOCATION_FAILURE;
    const char *cursor = text;
    for (size_t index = 0; index < count; ++index) {
        const char *match = strstr(cursor, separator);
        size_t segment_length = match ? (size_t)(match - cursor) : strlen(cursor);
        items[index] = arena_text(arena, cursor, segment_length);
        if (arena->failed) return USK_STRING_BUILTIN_ALLOCATION_FAILURE;
        if (!match) break;
        cursor = match + separator_length;
    }
    *result = array_value_in(arena, items, count);
    return arena->failed ? USK_STRING_BUILTIN_ALLOCATION_FAILURE
                         : USK_STRING_BUILTIN_OK;
}

UskStringBuiltinStatus usk_string_builtin_call(
    const char *name, const Value *arguments, size_t argument_count,
    UskValueArena *arena, Value *result) {
    const UskStringBuiltinInfo *info = usk_string_builtin_find(name);
    if (!info) return USK_STRING_BUILTIN_UNKNOWN;
    if (!result || !arena || (argument_count && !arguments))
        return USK_STRING_BUILTIN_TYPE_MISMATCH;
    if (argument_count != info->argument_count)
        return USK_STRING_BUILTIN_ARGUMENT_COUNT;
    for (size_t index = 0; index < argument_count; ++index) {
        if (info->arguments[index] == USK_STRING_ARGUMENT_TEXT &&
            arguments[index].kind != V_STRING)
            return USK_STRING_BUILTIN_TYPE_MISMATCH;
        if (info->arguments[index] == USK_STRING_ARGUMENT_INTEGER &&
            arguments[index].kind != V_INT)
            return USK_STRING_BUILTIN_TYPE_MISMATCH;
    }
    *result = null_value();
    const char *first = text_argument(arguments, 0, argument_count);
    const char *second = text_argument(arguments, 1, argument_count);
    const char *third = text_argument(arguments, 2, argument_count);
    size_t length = strlen(first);
    if (!strcmp(name, "string::length")) {
        if (length > (size_t)LLONG_MAX) return USK_STRING_BUILTIN_RANGE_ERROR;
        *result = int_value((long long)length);
    } else if (!strcmp(name, "string::contains")) {
        *result = bool_value(usk_string_contains_bytes(first, second));
    } else if (!strcmp(name, "string::starts_with")) {
        *result = bool_value(usk_string_starts_with_bytes(first, second));
    } else if (!strcmp(name, "string::ends_with")) {
        *result = bool_value(usk_string_ends_with_bytes(first, second));
    } else if (!strcmp(name, "string::index_of")) {
        *result = int_value(usk_string_index_of_bytes(first, second));
    } else if (!strcmp(name, "string::substring")) {
        long long start = 0, count = 0;
        integer_argument(arguments, 1, argument_count, &start);
        integer_argument(arguments, 2, argument_count, &count);
        if (start < 0 || count < 0 ||
            (unsigned long long)start > (unsigned long long)((size_t)-1) ||
            (unsigned long long)count > (unsigned long long)((size_t)-1) ||
            (size_t)start > length ||
            (size_t)count > length - (size_t)start)
            return USK_STRING_BUILTIN_RANGE_ERROR;
        *result = arena_text(arena, first + (size_t)start, (size_t)count);
    } else if (!strcmp(name, "string::concat")) {
        size_t second_length = strlen(second);
        if (second_length == (size_t)-1 ||
            length > (size_t)-1 - second_length - 1)
            return USK_STRING_BUILTIN_RANGE_ERROR;
        char *output = (char *)usk_value_arena_allocate(arena,
            length + second_length + 1, false);
        if (!output) return USK_STRING_BUILTIN_ALLOCATION_FAILURE;
        memcpy(output, first, length);
        memcpy(output + length, second, second_length + 1);
        *result = (Value){.kind = V_STRING, .as.s = output};
    } else if (!strcmp(name, "string::repeat")) {
        long long count = 0;
        integer_argument(arguments, 1, argument_count, &count);
        return make_repeat(first, count, arena, result);
    } else if (!strcmp(name, "string::replace")) {
        return make_replacement(first, second, third, arena, result);
    } else if (!strcmp(name, "string::trim")) {
        size_t start = 0, end = 0;
        usk_string_trim_bounds(first, &start, &end);
        *result = arena_text(arena, first + start, end - start);
    } else if (!strcmp(name, "string::to_upper")) {
        return make_case(first, true, arena, result);
    } else if (!strcmp(name, "string::to_lower")) {
        return make_case(first, false, arena, result);
    } else if (!strcmp(name, "string::is_empty")) {
        *result = bool_value(length == 0);
    } else if (!strcmp(name, "string::split")) {
        return make_split(first, second, arena, result);
    }
    return arena->failed ? USK_STRING_BUILTIN_ALLOCATION_FAILURE
                         : USK_STRING_BUILTIN_OK;
}

const char *usk_string_builtin_status_name(UskStringBuiltinStatus status) {
    switch (status) {
        case USK_STRING_BUILTIN_OK: return "ok";
        case USK_STRING_BUILTIN_UNKNOWN: return "not a string built-in";
        case USK_STRING_BUILTIN_ARGUMENT_COUNT: return "wrong argument count";
        case USK_STRING_BUILTIN_TYPE_MISMATCH: return "string argument type mismatch";
        case USK_STRING_BUILTIN_RANGE_ERROR: return "string index or size out of range";
        case USK_STRING_BUILTIN_ALLOCATION_FAILURE: return "allocation failure";
    }
    return "unknown string built-in status";
}
