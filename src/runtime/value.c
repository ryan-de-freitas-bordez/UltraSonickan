#include "usk/value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *copy_text(const char *text) {
    size_t length = strlen(text);
    if (length == (size_t)-1) return NULL;
    char *copy = (char *)malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, text, length + 1);
    return copy;
}

Value null_value(void) {
    return (Value){.kind = V_NULL};
}

Value int_value(long long number) {
    return (Value){.kind = V_INT, .as.i = number};
}

Value double_value(double number) {
    return (Value){.kind = V_DOUBLE, .as.d = number};
}

Value bool_value(bool boolean) {
    return (Value){.kind = V_BOOL, .as.b = boolean};
}

Value string_value(const char *text) {
    char *copy = copy_text(text ? text : "");
    return copy ? (Value){.kind = V_STRING, .as.s = copy} : null_value();
}

Value character_value(char character) {
    char text[2] = {character, '\0'};
    return string_value(text);
}

Value array_value(Value *items, size_t count) {
    if (count && !items) return null_value();
    Array *array = (Array *)malloc(sizeof(*array));
    if (!array) return null_value();
    array->items = items;
    array->count = count;
    array->capacity = count;
    array->arena = NULL;
    return (Value){.kind = V_ARRAY, .as.a = array};
}

bool truthy(Value value) {
    switch (value.kind) {
        case V_NULL: return false;
        case V_BOOL: return value.as.b;
        case V_INT: return value.as.i != 0;
        case V_DOUBLE: return value.as.d != 0.0;
        case V_STRING: return value.as.s && value.as.s[0] != '\0';
        case V_ARRAY: return value.as.a && value.as.a->count != 0;
    }
    return false;
}

double numeric(Value value) {
    switch (value.kind) {
        case V_INT: return (double)value.as.i;
        case V_DOUBLE: return value.as.d;
        case V_BOOL: return value.as.b ? 1.0 : 0.0;
        default: return 0.0;
    }
}

bool is_numeric(Value value) {
    return value.kind == V_INT || value.kind == V_DOUBLE || value.kind == V_BOOL;
}

bool value_as_boolean(Value value, bool *result) {
    if (!result) return false;
    if (value.kind == V_BOOL) { *result = value.as.b; return true; }
    return false;
}

bool value_as_integer(Value value, long long *result) {
    if (!result || value.kind != V_INT) return false;
    *result = value.as.i;
    return true;
}

bool value_as_double(Value value, double *result) {
    if (!result || !is_numeric(value)) return false;
    *result = numeric(value);
    return true;
}

bool value_as_string(Value value, const char **result) {
    if (!result || value.kind != V_STRING) return false;
    *result = value.as.s;
    return true;
}

bool value_is_null(Value value) {
    return value.kind == V_NULL;
}

size_t value_array_length(Value value) {
    return value.kind == V_ARRAY && value.as.a ? value.as.a->count : 0;
}

size_t value_array_capacity(Value value) {
    return value.kind == V_ARRAY && value.as.a ? value.as.a->capacity : 0;
}

UskValueStatus value_array_get(Value value, size_t index, Value *result) {
    if (value.kind != V_ARRAY || !value.as.a) return USK_VALUE_WRONG_KIND;
    if (value.as.a->count && !value.as.a->items) return USK_VALUE_WRONG_KIND;
    if (index >= value.as.a->count) return USK_VALUE_INDEX_OUT_OF_RANGE;
    if (!result) return USK_VALUE_WRONG_KIND;
    *result = value.as.a->items[index];
    return USK_VALUE_OK;
}

UskValueStatus value_array_set(Value value, size_t index, Value element) {
    if (value.kind != V_ARRAY || !value.as.a) return USK_VALUE_WRONG_KIND;
    if (value.as.a->count && !value.as.a->items) return USK_VALUE_WRONG_KIND;
    if (index >= value.as.a->count) return USK_VALUE_INDEX_OUT_OF_RANGE;
    value.as.a->items[index] = element;
    return USK_VALUE_OK;
}

UskValueStatus value_array_push(Value value, Value element) {
    if (value.kind != V_ARRAY || !value.as.a) return USK_VALUE_WRONG_KIND;
    size_t count = value.as.a->count;
    if (count && !value.as.a->items) return USK_VALUE_WRONG_KIND;
    if (count >= (size_t)-1 / sizeof(Value)) return USK_VALUE_INDEX_OUT_OF_RANGE;
    UskValueStatus status = value_array_reserve(value, count + 1);
    if (status != USK_VALUE_OK) return status;
    value.as.a->items[count] = element;
    value.as.a->count = count + 1;
    return USK_VALUE_OK;
}

UskValueStatus value_array_reserve(Value value, size_t capacity) {
    if (value.kind != V_ARRAY || !value.as.a) return USK_VALUE_WRONG_KIND;
    Array *array = value.as.a;
    if (array->count > array->capacity ||
        (array->count && !array->items)) return USK_VALUE_WRONG_KIND;
    if (capacity <= array->capacity) return USK_VALUE_OK;
    if (capacity > (size_t)-1 / sizeof(Value))
        return USK_VALUE_INDEX_OUT_OF_RANGE;
    size_t grown = array->capacity ? array->capacity : 8;
    while (grown < capacity) {
        if (grown > (size_t)-1 / 2) { grown = capacity; break; }
        grown *= 2;
    }
    if (grown > (size_t)-1 / sizeof(Value)) grown = capacity;
    Value *items = NULL;
    if (array->arena) {
        void *storage = array->items;
        if (!storage) storage = usk_value_arena_allocate(array->arena,
            grown * sizeof(Value), true);
        else if (!usk_value_arena_resize(array->arena, &storage,
            grown * sizeof(Value))) return USK_VALUE_ALLOCATION_FAILED;
        items = (Value *)storage;
    } else {
        items = (Value *)realloc(array->items, grown * sizeof(Value));
    }
    if (!items) return USK_VALUE_ALLOCATION_FAILED;
    array->items = items;
    array->capacity = grown;
    return USK_VALUE_OK;
}

UskValueStatus value_array_insert(Value value, size_t index, Value element) {
    if (value.kind != V_ARRAY || !value.as.a) return USK_VALUE_WRONG_KIND;
    Array *array = value.as.a;
    if (index > array->count) return USK_VALUE_INDEX_OUT_OF_RANGE;
    if (array->count && !array->items) return USK_VALUE_WRONG_KIND;
    if (array->count >= (size_t)-1 / sizeof(Value))
        return USK_VALUE_INDEX_OUT_OF_RANGE;
    UskValueStatus status = value_array_reserve(value, array->count + 1);
    if (status != USK_VALUE_OK) return status;
    memmove(array->items + index + 1, array->items + index,
            (array->count - index) * sizeof(Value));
    array->items[index] = element;
    array->count++;
    return USK_VALUE_OK;
}

UskValueStatus value_array_pop(Value value, Value *result) {
    if (value.kind != V_ARRAY || !value.as.a) return USK_VALUE_WRONG_KIND;
    Array *array = value.as.a;
    if (array->count && !array->items) return USK_VALUE_WRONG_KIND;
    if (!array->count) return USK_VALUE_INDEX_OUT_OF_RANGE;
    Value popped = array->items[array->count - 1];
    array->items[--array->count] = null_value();
    if (result) *result = popped;
    return USK_VALUE_OK;
}

UskValueStatus value_array_remove(Value value, size_t index, Value *result) {
    if (value.kind != V_ARRAY || !value.as.a) return USK_VALUE_WRONG_KIND;
    Array *array = value.as.a;
    if (array->count && !array->items) return USK_VALUE_WRONG_KIND;
    if (index >= array->count) return USK_VALUE_INDEX_OUT_OF_RANGE;
    Value removed = array->items[index];
    if (index + 1 < array->count)
        memmove(array->items + index, array->items + index + 1,
                (array->count - index - 1) * sizeof(Value));
    array->items[--array->count] = null_value();
    if (result) *result = removed;
    return USK_VALUE_OK;
}

UskValueStatus value_array_clear(Value value) {
    if (value.kind != V_ARRAY || !value.as.a) return USK_VALUE_WRONG_KIND;
    if (value.as.a->count && !value.as.a->items) return USK_VALUE_WRONG_KIND;
    for (size_t index = 0; index < value.as.a->count; ++index)
        value.as.a->items[index] = null_value();
    value.as.a->count = 0;
    return USK_VALUE_OK;
}

static bool equal_recursive(Value left, Value right, unsigned depth) {
    if (depth > 128) return false;
    if (left.kind == V_INT && right.kind == V_INT)
        return left.as.i == right.as.i;
    if (is_numeric(left) && is_numeric(right)) return numeric(left) == numeric(right);
    if (left.kind != right.kind) return false;
    switch (left.kind) {
        case V_NULL: return true;
        case V_INT: return left.as.i == right.as.i;
        case V_DOUBLE: return left.as.d == right.as.d;
        case V_BOOL: return left.as.b == right.as.b;
        case V_STRING: return strcmp(left.as.s ? left.as.s : "", right.as.s ? right.as.s : "") == 0;
        case V_ARRAY:
            if (left.as.a == right.as.a) return true;
            if (!left.as.a || !right.as.a || left.as.a->count != right.as.a->count) return false;
            if (left.as.a->count && (!left.as.a->items || !right.as.a->items))
                return false;
            for (size_t index = 0; index < left.as.a->count; ++index)
                if (!equal_recursive(left.as.a->items[index], right.as.a->items[index], depth + 1)) return false;
            return true;
    }
    return false;
}

bool value_equal(Value left, Value right) {
    return equal_recursive(left, right, 0);
}

UskValueStatus value_compare(Value left, Value right, int *ordering) {
    if (!ordering) return USK_VALUE_UNSUPPORTED_COMPARISON;
    if (left.kind == V_INT && right.kind == V_INT) {
        *ordering = left.as.i < right.as.i ? -1 :
                    left.as.i > right.as.i ? 1 : 0;
        return USK_VALUE_OK;
    }
    if (is_numeric(left) && is_numeric(right)) {
        double a = numeric(left), b = numeric(right);
        *ordering = a < b ? -1 : a > b ? 1 : 0;
        return USK_VALUE_OK;
    }
    if (left.kind == V_STRING && right.kind == V_STRING) {
        int comparison = strcmp(left.as.s ? left.as.s : "", right.as.s ? right.as.s : "");
        *ordering = comparison < 0 ? -1 : comparison > 0 ? 1 : 0;
        return USK_VALUE_OK;
    }
    return USK_VALUE_UNSUPPORTED_COMPARISON;
}

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} ValueStringBuilder;

static bool string_builder_reserve(ValueStringBuilder *builder, size_t extra) {
    if (extra > (size_t)-1 - builder->length - 1) return false;
    size_t required = builder->length + extra + 1;
    if (required <= builder->capacity) return true;
    size_t capacity = builder->capacity ? builder->capacity : 64;
    while (capacity < required) {
        if (capacity > (size_t)-1 / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    char *data = (char *)realloc(builder->data, capacity);
    if (!data) return false;
    builder->data = data;
    builder->capacity = capacity;
    if (!builder->length) builder->data[0] = '\0';
    return true;
}

static bool string_builder_append(ValueStringBuilder *builder,
                                  const char *text, size_t length) {
    if (!string_builder_reserve(builder, length)) return false;
    if (length) memcpy(builder->data + builder->length, text, length);
    builder->length += length;
    builder->data[builder->length] = '\0';
    return true;
}

static bool string_builder_append_text(ValueStringBuilder *builder,
                                       const char *text) {
    if (!text) text = "";
    return string_builder_append(builder, text, strlen(text));
}

static bool value_string_append(ValueStringBuilder *builder, Value value,
                                unsigned depth) {
    char buffer[128];
    switch (value.kind) {
        case V_NULL:
            return string_builder_append_text(builder, "null");
        case V_BOOL:
            return string_builder_append_text(builder,
                value.as.b ? "true" : "false");
        case V_INT: {
            int length = snprintf(buffer, sizeof(buffer), "%lld", value.as.i);
            return length >= 0 && (size_t)length < sizeof(buffer) &&
                string_builder_append(builder, buffer, (size_t)length);
        }
        case V_DOUBLE: {
            int length = snprintf(buffer, sizeof(buffer), "%.15g", value.as.d);
            return length >= 0 && (size_t)length < sizeof(buffer) &&
                string_builder_append(builder, buffer, (size_t)length);
        }
        case V_STRING:
            return string_builder_append_text(builder, value.as.s);
        case V_ARRAY:
            if (depth >= 64)
                return string_builder_append_text(builder, "[...]");
            if (!string_builder_append_text(builder, "[")) return false;
            if (value.as.a) {
                if (value.as.a->count && !value.as.a->items) return false;
                for (size_t index = 0; index < value.as.a->count; ++index) {
                    if (index && !string_builder_append_text(builder, ", "))
                        return false;
                    if (!value_string_append(builder, value.as.a->items[index],
                                             depth + 1)) return false;
                }
            }
            return string_builder_append_text(builder, "]");
    }
    return string_builder_append_text(builder, "null");
}

char *value_string(Value value) {
    ValueStringBuilder builder = {0};
    if (!value_string_append(&builder, value, 0)) {
        free(builder.data);
        return NULL;
    }
    if (!builder.data) {
        builder.data = copy_text("");
        if (!builder.data) return NULL;
    }
    return builder.data;
}

const char *type_name(Value value) {
    switch (value.kind) {
        case V_NULL: return "USKNull";
        case V_INT: return "USKInt";
        case V_DOUBLE: return "USKDouble";
        case V_STRING: return "USKString";
        case V_BOOL: return "USKBool";
        case V_ARRAY: return "USKAuto";
    }
    return "USKNull";
}
