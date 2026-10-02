#include "usk/arraylib.h"

#include <limits.h>
#include <string.h>

static const UskArrayBuiltinInfo builtins[] = {
    {"array::length", 1, USK_ARRAY_RESULT_INTEGER,
        {USK_ARRAY_ARGUMENT_ARRAY}},
    {"array::capacity", 1, USK_ARRAY_RESULT_INTEGER,
        {USK_ARRAY_ARGUMENT_ARRAY}},
    {"array::is_empty", 1, USK_ARRAY_RESULT_BOOLEAN,
        {USK_ARRAY_ARGUMENT_ARRAY}},
    {"array::push", 2, USK_ARRAY_RESULT_INTEGER,
        {USK_ARRAY_ARGUMENT_ARRAY, USK_ARRAY_ARGUMENT_ANY}},
    {"array::pop", 1, USK_ARRAY_RESULT_ANY,
        {USK_ARRAY_ARGUMENT_ARRAY}},
    {"array::insert", 3, USK_ARRAY_RESULT_BOOLEAN,
        {USK_ARRAY_ARGUMENT_ARRAY, USK_ARRAY_ARGUMENT_INTEGER,
         USK_ARRAY_ARGUMENT_ANY}},
    {"array::remove_at", 2, USK_ARRAY_RESULT_ANY,
        {USK_ARRAY_ARGUMENT_ARRAY, USK_ARRAY_ARGUMENT_INTEGER}},
    {"array::clear", 1, USK_ARRAY_RESULT_NULL,
        {USK_ARRAY_ARGUMENT_ARRAY}},
    {"array::contains", 2, USK_ARRAY_RESULT_BOOLEAN,
        {USK_ARRAY_ARGUMENT_ARRAY, USK_ARRAY_ARGUMENT_ANY}},
    {"array::index_of", 2, USK_ARRAY_RESULT_INTEGER,
        {USK_ARRAY_ARGUMENT_ARRAY, USK_ARRAY_ARGUMENT_ANY}},
    {"array::reverse", 1, USK_ARRAY_RESULT_ARRAY,
        {USK_ARRAY_ARGUMENT_ARRAY}},
    {"array::copy", 1, USK_ARRAY_RESULT_ARRAY,
        {USK_ARRAY_ARGUMENT_ARRAY}},
    {"array::fill", 2, USK_ARRAY_RESULT_NULL,
        {USK_ARRAY_ARGUMENT_ARRAY, USK_ARRAY_ARGUMENT_ANY}},
    {"array::reserve", 2, USK_ARRAY_RESULT_BOOLEAN,
        {USK_ARRAY_ARGUMENT_ARRAY, USK_ARRAY_ARGUMENT_INTEGER}},
    {"array::slice", 3, USK_ARRAY_RESULT_ARRAY,
        {USK_ARRAY_ARGUMENT_ARRAY, USK_ARRAY_ARGUMENT_INTEGER,
         USK_ARRAY_ARGUMENT_INTEGER}}
};

bool usk_array_validate(Value value) {
    if (value.kind != V_ARRAY || !value.as.a) return false;
    const Array *array = value.as.a;
    if (array->count > array->capacity ||
        (array->capacity && !array->items)) return false;
    if (!array->arena) return true;
    if (!usk_value_arena_owns(array->arena, array)) return false;
    return !array->items || usk_value_arena_owns(array->arena, array->items);
}

bool usk_array_find(Value value, Value needle, size_t *index) {
    if (!usk_array_validate(value)) return false;
    for (size_t position = 0; position < value.as.a->count; ++position) {
        if (value_equal(value.as.a->items[position], needle)) {
            if (index) *index = position;
            return true;
        }
    }
    return false;
}

UskValueStatus usk_array_swap(Value value, size_t left, size_t right) {
    if (!usk_array_validate(value)) return USK_VALUE_WRONG_KIND;
    Array *array = value.as.a;
    if (left >= array->count || right >= array->count)
        return USK_VALUE_INDEX_OUT_OF_RANGE;
    Value temporary = array->items[left];
    array->items[left] = array->items[right];
    array->items[right] = temporary;
    return USK_VALUE_OK;
}

bool usk_array_iterator_begin(Value value, size_t start, size_t count,
                              UskArrayIterator *iterator) {
    if (!iterator || !usk_array_validate(value) ||
        start > value.as.a->count || count > value.as.a->count - start)
        return false;
    iterator->owner = value;
    iterator->next_index = start;
    iterator->end_index = start + count;
    return true;
}

UskArrayIteratorStatus usk_array_iterator_next(UskArrayIterator *iterator,
                                                Value *item) {
    if (!iterator || !item || !usk_array_validate(iterator->owner))
        return USK_ARRAY_ITERATOR_INVALID;
    if (iterator->next_index >= iterator->end_index)
        return USK_ARRAY_ITERATOR_END;
    Array *array = iterator->owner.as.a;
    if (iterator->next_index >= array->count)
        return USK_ARRAY_ITERATOR_INVALID;
    *item = array->items[iterator->next_index++];
    return USK_ARRAY_ITERATOR_ITEM;
}

size_t usk_array_builtin_count(void) {
    return sizeof(builtins) / sizeof(builtins[0]);
}

const UskArrayBuiltinInfo *usk_array_builtin_at(size_t index) {
    return index < usk_array_builtin_count() ? &builtins[index] : NULL;
}

const UskArrayBuiltinInfo *usk_array_builtin_find(const char *name) {
    if (!name) return NULL;
    for (size_t index = 0; index < usk_array_builtin_count(); ++index)
        if (!strcmp(name, builtins[index].name)) return &builtins[index];
    return NULL;
}

const UskArrayBuiltinInfo *usk_array_method_find(const char *method) {
    if (!method) return NULL;
    for (size_t index = 0; index < usk_array_builtin_count(); ++index) {
        const char *separator = strstr(builtins[index].name, "::");
        if (separator && !strcmp(method, separator + 2)) return &builtins[index];
    }
    return NULL;
}

bool usk_array_builtin_is_name(const char *name) {
    return usk_array_builtin_find(name) != NULL;
}

static bool require_integer(Value value, long long *integer) {
    if (value.kind != V_INT) return false;
    if (integer) *integer = value.as.i;
    return true;
}

static bool require_array(Value value, Array **array) {
    if (value.kind != V_ARRAY || !value.as.a) return false;
    if (value.as.a->count && !value.as.a->items) return false;
    if (value.as.a->count > value.as.a->capacity) return false;
    *array = value.as.a;
    return true;
}

static bool to_size(long long value, size_t *result) {
    if (value < 0 || (unsigned long long)value > (unsigned long long)(size_t)-1)
        return false;
    *result = (size_t)value;
    return true;
}

static UskArrayBuiltinStatus allocate_items(UskValueArena *arena, size_t count,
                                            Value **items) {
    *items = NULL;
    if (!count) return USK_ARRAY_BUILTIN_OK;
    if (!arena || count > (size_t)-1 / sizeof(**items))
        return arena ? USK_ARRAY_BUILTIN_RANGE_ERROR
                     : USK_ARRAY_BUILTIN_ALLOCATION_FAILURE;
    *items = (Value *)usk_value_arena_allocate(arena,
        count * sizeof(**items), false);
    return *items ? USK_ARRAY_BUILTIN_OK
                  : USK_ARRAY_BUILTIN_ALLOCATION_FAILURE;
}

static UskArrayBuiltinStatus make_copy(Value source, UskValueArena *arena,
                                       bool reverse, size_t start, size_t count,
                                       Value *result) {
    Array *array = NULL;
    if (!require_array(source, &array)) return USK_ARRAY_BUILTIN_TYPE_MISMATCH;
    if (start > array->count || count > array->count - start)
        return USK_ARRAY_BUILTIN_RANGE_ERROR;
    Value *items = NULL;
    UskArrayBuiltinStatus status = allocate_items(arena, count, &items);
    if (status != USK_ARRAY_BUILTIN_OK) return status;
    for (size_t index = 0; index < count; ++index) {
        size_t source_index = reverse ? start + count - index - 1 : start + index;
        items[index] = array->items[source_index];
    }
    *result = array_value_in(arena, items, count);
    return arena->failed ? USK_ARRAY_BUILTIN_ALLOCATION_FAILURE
                         : USK_ARRAY_BUILTIN_OK;
}

static UskArrayBuiltinStatus validate_arguments(
    const UskArrayBuiltinInfo *info, const Value *arguments, size_t count) {
    if (count != info->argument_count) return USK_ARRAY_BUILTIN_ARGUMENT_COUNT;
    if (!arguments) return USK_ARRAY_BUILTIN_TYPE_MISMATCH;
    for (size_t index = 0; index < count; ++index) {
        UskArrayArgumentKind kind = info->arguments[index];
        if (kind == USK_ARRAY_ARGUMENT_ARRAY) {
            Array *array = NULL;
            if (!require_array(arguments[index], &array))
                return USK_ARRAY_BUILTIN_TYPE_MISMATCH;
        } else if (kind == USK_ARRAY_ARGUMENT_INTEGER &&
                   arguments[index].kind != V_INT) {
            return USK_ARRAY_BUILTIN_TYPE_MISMATCH;
        }
    }
    return USK_ARRAY_BUILTIN_OK;
}

UskArrayBuiltinStatus usk_array_builtin_call(
    const char *name, const Value *arguments, size_t argument_count,
    UskValueArena *arena, Value *result) {
    const UskArrayBuiltinInfo *info = usk_array_builtin_find(name);
    if (!info) return USK_ARRAY_BUILTIN_UNKNOWN;
    if (!result) return USK_ARRAY_BUILTIN_TYPE_MISMATCH;
    UskArrayBuiltinStatus status = validate_arguments(info, arguments,
                                                       argument_count);
    if (status != USK_ARRAY_BUILTIN_OK) return status;
    Array *array = arguments[0].as.a;
    long long integer = 0;
    *result = null_value();

    if (!strcmp(name, "array::length")) {
        if (array->count > (size_t)LLONG_MAX) return USK_ARRAY_BUILTIN_RANGE_ERROR;
        *result = int_value((long long)array->count);
    } else if (!strcmp(name, "array::capacity")) {
        if (array->capacity > (size_t)LLONG_MAX) return USK_ARRAY_BUILTIN_RANGE_ERROR;
        *result = int_value((long long)array->capacity);
    } else if (!strcmp(name, "array::is_empty")) {
        *result = bool_value(array->count == 0);
    } else if (!strcmp(name, "array::push")) {
        status = value_array_push(arguments[0], arguments[1]) == USK_VALUE_OK
            ? USK_ARRAY_BUILTIN_OK : USK_ARRAY_BUILTIN_ALLOCATION_FAILURE;
        if (status == USK_ARRAY_BUILTIN_OK) {
            if (array->count > (size_t)LLONG_MAX) return USK_ARRAY_BUILTIN_RANGE_ERROR;
            *result = int_value((long long)array->count);
        }
    } else if (!strcmp(name, "array::pop")) {
        UskValueStatus value_status = value_array_pop(arguments[0], result);
        if (value_status == USK_VALUE_INDEX_OUT_OF_RANGE) *result = null_value();
        else if (value_status != USK_VALUE_OK) status = USK_ARRAY_BUILTIN_TYPE_MISMATCH;
    } else if (!strcmp(name, "array::insert")) {
        require_integer(arguments[1], &integer);
        size_t index = 0;
        if (!to_size(integer, &index) || index > array->count)
            return USK_ARRAY_BUILTIN_RANGE_ERROR;
        UskValueStatus value_status = value_array_insert(arguments[0], index,
                                                          arguments[2]);
        if (value_status == USK_VALUE_ALLOCATION_FAILED)
            status = USK_ARRAY_BUILTIN_ALLOCATION_FAILURE;
        else if (value_status != USK_VALUE_OK)
            return USK_ARRAY_BUILTIN_RANGE_ERROR;
        else *result = bool_value(true);
    } else if (!strcmp(name, "array::remove_at")) {
        require_integer(arguments[1], &integer);
        size_t index = 0;
        if (!to_size(integer, &index)) return USK_ARRAY_BUILTIN_RANGE_ERROR;
        UskValueStatus value_status = value_array_remove(arguments[0], index,
                                                          result);
        if (value_status == USK_VALUE_INDEX_OUT_OF_RANGE)
            return USK_ARRAY_BUILTIN_RANGE_ERROR;
        if (value_status != USK_VALUE_OK) return USK_ARRAY_BUILTIN_TYPE_MISMATCH;
    } else if (!strcmp(name, "array::clear")) {
        if (value_array_clear(arguments[0]) != USK_VALUE_OK)
            return USK_ARRAY_BUILTIN_TYPE_MISMATCH;
    } else if (!strcmp(name, "array::contains") ||
               !strcmp(name, "array::index_of")) {
        size_t index = 0;
        while (index < array->count &&
               !value_equal(array->items[index], arguments[1])) index++;
        if (!strcmp(name, "array::contains"))
            *result = bool_value(index < array->count);
        else *result = int_value(index < array->count && index <= (size_t)LLONG_MAX
            ? (long long)index : -1);
    } else if (!strcmp(name, "array::reverse")) {
        status = make_copy(arguments[0], arena, true, 0, array->count, result);
    } else if (!strcmp(name, "array::copy")) {
        status = make_copy(arguments[0], arena, false, 0, array->count, result);
    } else if (!strcmp(name, "array::fill")) {
        for (size_t index = 0; index < array->count; ++index)
            array->items[index] = arguments[1];
    } else if (!strcmp(name, "array::reserve")) {
        require_integer(arguments[1], &integer);
        size_t requested = 0;
        if (!to_size(integer, &requested)) return USK_ARRAY_BUILTIN_RANGE_ERROR;
        UskValueStatus value_status = value_array_reserve(arguments[0], requested);
        if (value_status == USK_VALUE_ALLOCATION_FAILED)
            status = USK_ARRAY_BUILTIN_ALLOCATION_FAILURE;
        else if (value_status != USK_VALUE_OK)
            return USK_ARRAY_BUILTIN_RANGE_ERROR;
        else *result = bool_value(true);
    } else if (!strcmp(name, "array::slice")) {
        long long start_number = 0, count_number = 0;
        require_integer(arguments[1], &start_number);
        require_integer(arguments[2], &count_number);
        size_t start = 0, count = 0;
        if (!to_size(start_number, &start) || !to_size(count_number, &count))
            return USK_ARRAY_BUILTIN_RANGE_ERROR;
        status = make_copy(arguments[0], arena, false, start, count, result);
    }
    return status;
}

const char *usk_array_builtin_status_name(UskArrayBuiltinStatus status) {
    switch (status) {
        case USK_ARRAY_BUILTIN_OK: return "ok";
        case USK_ARRAY_BUILTIN_UNKNOWN: return "not an array built-in";
        case USK_ARRAY_BUILTIN_ARGUMENT_COUNT: return "wrong argument count";
        case USK_ARRAY_BUILTIN_TYPE_MISMATCH: return "array argument type mismatch";
        case USK_ARRAY_BUILTIN_RANGE_ERROR: return "array index or size out of range";
        case USK_ARRAY_BUILTIN_ALLOCATION_FAILURE: return "allocation failure";
    }
    return "unknown array built-in status";
}
