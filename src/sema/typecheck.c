#include "usk/typecheck.h"

#include <string.h>

typedef struct {
    const char *name;
    UskTypeKind kind;
    size_t size_bytes;
    size_t alignment_bytes;
    bool is_signed;
    bool is_numeric;
} PrimitiveType;

static const PrimitiveType primitive_types[] = {
    {"USKNull",   USK_TYPE_NULL,      0, 1, false, false},
    {"USKInt",    USK_TYPE_INTEGER,   4, 4, true,  true},
    {"USKDouble", USK_TYPE_FLOATING,  8, 8, true,  true},
    {"USKString", USK_TYPE_STRING,    8, 8, false, false},
    {"USKChar",   USK_TYPE_CHARACTER, 1, 1, false, false},
    {"USKFloat",  USK_TYPE_FLOATING,  4, 4, true,  true},
    {"USKBool",   USK_TYPE_BOOLEAN,   1, 1, false, false},
    {"USKAuto",   USK_TYPE_UNKNOWN,   8, 8, false, false},
    {"USKLong",   USK_TYPE_INTEGER,  8, 8, true,  true},
    {"USKShort",  USK_TYPE_INTEGER,   1, 1, true,  true}
};

static UskTypeInfo make_type(const char *name, UskTypeKind kind, size_t size,
                             size_t alignment, bool is_signed, bool numeric) {
    UskTypeInfo type = {name, kind, size, alignment, is_signed, numeric};
    return type;
}

bool usk_type_resolve(const char *name, UskTypeInfo *result) {
    if (!name || !result) return false;
    const size_t count = sizeof(primitive_types) / sizeof(primitive_types[0]);
    for (size_t index = 0; index < count; ++index) {
        const PrimitiveType *type = &primitive_types[index];
        if (strcmp(name, type->name) == 0) {
            *result = make_type(type->name, type->kind, type->size_bytes,
                                type->alignment_bytes, type->is_signed,
                                type->is_numeric);
            return true;
        }
    }
    *result = make_type(name, USK_TYPE_UNKNOWN, 0, 0, false, false);
    return false;
}

bool usk_type_resolve_modified(const char *name, bool is_long, bool is_short,
                               bool is_unsigned, UskTypeInfo *result) {
    if (!name || !result || (is_long && is_short)) return false;
    if (!usk_type_resolve(name, result)) return false;
    if (is_long) {
        if (result->kind == USK_TYPE_INTEGER) {
            result->size_bytes = 8;
            result->alignment_bytes = 8;
        } else if (result->kind == USK_TYPE_FLOATING) {
            result->size_bytes = 16;
            result->alignment_bytes = 16;
        } else return false;
    }
    if (is_short) {
        if (result->kind == USK_TYPE_INTEGER) {
            result->size_bytes = 1;
            result->alignment_bytes = 1;
        } else if (result->kind == USK_TYPE_FLOATING) {
            result->size_bytes = 2;
            result->alignment_bytes = 2;
        } else return false;
    }
    if (is_unsigned) {
        if (result->kind != USK_TYPE_INTEGER) return false;
        result->is_signed = false;
    }
    return true;
}

bool usk_type_can_convert(UskTypeInfo from, UskTypeInfo to) {
    if (to.kind == USK_TYPE_UNKNOWN || from.kind == USK_TYPE_UNKNOWN) return true;
    if (from.kind == to.kind) return true;
    if (from.kind == USK_TYPE_INTEGER && to.kind == USK_TYPE_FLOATING) return true;
    if (from.kind == USK_TYPE_FLOATING && to.kind == USK_TYPE_INTEGER) return false;
    return false;
}

UskTypeInfo usk_type_promote(UskTypeInfo left, UskTypeInfo right) {
    if (left.kind == USK_TYPE_UNKNOWN) return right;
    if (right.kind == USK_TYPE_UNKNOWN) return left;
    if (left.kind == USK_TYPE_FLOATING || right.kind == USK_TYPE_FLOATING) {
        UskTypeInfo result;
        usk_type_resolve("USKDouble", &result);
        return result;
    }
    if (left.kind == USK_TYPE_INTEGER && right.kind == USK_TYPE_INTEGER)
        return left.size_bytes >= right.size_bytes ? left : right;
    return make_type("USKNull", USK_TYPE_NULL, 0, 1, false, false);
}

const char *usk_type_kind_name(UskTypeKind kind) {
    switch (kind) {
        case USK_TYPE_NULL: return "null";
        case USK_TYPE_INTEGER: return "integer";
        case USK_TYPE_FLOATING: return "floating-point";
        case USK_TYPE_STRING: return "string";
        case USK_TYPE_CHARACTER: return "character";
        case USK_TYPE_BOOLEAN: return "boolean";
        case USK_TYPE_ARRAY: return "array";
        case USK_TYPE_UNKNOWN: return "unknown or user-defined";
    }
    return "unknown";
}

bool value_matches_type(const char *type, Value value) {
    UskTypeInfo descriptor;
    if (!type || !type[0] || strcmp(type, "USKAuto") == 0) return true;
    if (!usk_type_resolve(type, &descriptor)) return true;
    switch (descriptor.kind) {
        case USK_TYPE_NULL: return value.kind == V_NULL;
        case USK_TYPE_INTEGER: return value.kind == V_INT;
        case USK_TYPE_FLOATING: return value.kind == V_DOUBLE || value.kind == V_INT;
        case USK_TYPE_STRING: return value.kind == V_STRING;
        case USK_TYPE_CHARACTER:
            return value.kind == V_STRING && value.as.s && strlen(value.as.s) == 1;
        case USK_TYPE_BOOLEAN: return value.kind == V_BOOL;
        case USK_TYPE_ARRAY: return value.kind == V_ARRAY;
        case USK_TYPE_UNKNOWN: return true;
    }
    return false;
}
