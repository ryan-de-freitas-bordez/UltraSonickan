#include "c_backend_internal.h"

#include <stdlib.h>
#include <string.h>

UskCBinding *usk_c_scope_begin(UskCEmitter *emitter) {
    return emitter ? emitter->locals : NULL;
}

void usk_c_scope_end(UskCEmitter *emitter, UskCBinding *marker) {
    if (!emitter) return;
    while (emitter->locals != marker) {
        UskCBinding *binding = emitter->locals;
        if (!binding) break;
        emitter->locals = binding->next;
        free(binding);
    }
}

bool usk_c_bind_local(UskCEmitter *emitter, const char *name,
                      UskCValueKind kind) {
    if (!emitter || !name || !name[0] || emitter->failed) return false;
    UskCBinding *binding = (UskCBinding *)malloc(sizeof(*binding));
    if (!binding) {
        usk_c_emit_error(emitter, USK_DIAG_OUT_OF_MEMORY,
            (UskSourceSpan){0}, "cannot allocate local type metadata");
        return false;
    }
    binding->name = name;
    binding->kind = kind;
    binding->next = emitter->locals;
    emitter->locals = binding;
    return true;
}

UskCValueKind usk_c_binding_kind(const UskCEmitter *emitter,
                                 const char *name) {
    for (const UskCBinding *binding = emitter ? emitter->locals : NULL;
         binding; binding = binding->next)
        if (name && binding->name && strcmp(binding->name, name) == 0)
            return binding->kind;
    return USK_C_VALUE_UNKNOWN;
}

UskCValueKind usk_c_value_kind_from_type(const UskAstType *type) {
    if (!type || !type->name) return USK_C_VALUE_UNKNOWN;
    if (!strcmp(type->name, "USKNull")) return USK_C_VALUE_NULL;
    if (!strcmp(type->name, "USKBool")) return USK_C_VALUE_BOOL;
    if (!strcmp(type->name, "USKInt")) {
        if (type->is_unsigned)
            return type->is_long ? USK_C_VALUE_UINT64 :
                   type->is_short ? USK_C_VALUE_UINT8 : USK_C_VALUE_UINT32;
        return type->is_long ? USK_C_VALUE_INT64 :
               type->is_short ? USK_C_VALUE_INT8 : USK_C_VALUE_INT32;
    }
    if (!strcmp(type->name, "USKLong"))
        return type->is_unsigned ? USK_C_VALUE_UINT64 : USK_C_VALUE_INT64;
    if (!strcmp(type->name, "USKShort"))
        return type->is_unsigned ? USK_C_VALUE_UINT8 : USK_C_VALUE_INT8;
    if (!strcmp(type->name, "USKDouble")) return USK_C_VALUE_FLOAT64;
    if (!strcmp(type->name, "USKFloat")) return USK_C_VALUE_FLOAT32;
    if (!strcmp(type->name, "USKString")) return USK_C_VALUE_STRING;
    if (!strcmp(type->name, "USKChar"))
        return type->is_unsigned ? USK_C_VALUE_UINT8 : USK_C_VALUE_INT8;
    if (!strcmp(type->name, "USKAuto")) return USK_C_VALUE_POINTER;
    return USK_C_VALUE_UNKNOWN;
}

static bool is_integer_kind(UskCValueKind kind) {
    return kind == USK_C_VALUE_INT8 || kind == USK_C_VALUE_INT32 ||
           kind == USK_C_VALUE_INT64 || kind == USK_C_VALUE_UINT8 ||
           kind == USK_C_VALUE_UINT32 || kind == USK_C_VALUE_UINT64;
}

static UskCValueKind promote_numeric_kinds(UskCValueKind left,
                                           UskCValueKind right) {
    if (left == USK_C_VALUE_UNKNOWN || right == USK_C_VALUE_UNKNOWN)
        return USK_C_VALUE_UNKNOWN;
    if (left == USK_C_VALUE_FLOAT64 || right == USK_C_VALUE_FLOAT64)
        return USK_C_VALUE_FLOAT64;
    if (left == USK_C_VALUE_FLOAT32 || right == USK_C_VALUE_FLOAT32)
        return USK_C_VALUE_FLOAT64;
    if (is_integer_kind(left) && is_integer_kind(right)) {
        if (left == USK_C_VALUE_UINT64 || right == USK_C_VALUE_UINT64)
            return USK_C_VALUE_UINT64;
        if (left == USK_C_VALUE_INT64 || right == USK_C_VALUE_INT64)
            return USK_C_VALUE_INT64;
        if (left == USK_C_VALUE_UINT32 || right == USK_C_VALUE_UINT32)
            return USK_C_VALUE_UINT32;
        return USK_C_VALUE_INT32;
    }
    return USK_C_VALUE_UNKNOWN;
}

UskCValueKind usk_c_infer_expression_kind(const UskCEmitter *emitter,
                                          const UskAstExpr *expression) {
    if (!expression) return USK_C_VALUE_UNKNOWN;
    switch (expression->kind) {
        case USK_EXPR_NULL: return USK_C_VALUE_NULL;
        case USK_EXPR_BOOLEAN: return USK_C_VALUE_BOOL;
        case USK_EXPR_INTEGER: return USK_C_VALUE_INT32;
        case USK_EXPR_DOUBLE: return USK_C_VALUE_FLOAT64;
        case USK_EXPR_STRING: return USK_C_VALUE_STRING;
        case USK_EXPR_NAME:
            return usk_c_binding_kind(emitter, expression->as.name);
        case USK_EXPR_ARRAY: return USK_C_VALUE_UNKNOWN;
        case USK_EXPR_UNARY:
            if (!strcmp(expression->as.unary.operator, "!"))
                return USK_C_VALUE_BOOL;
            if (!strncmp(expression->as.unary.operator, "++", 2) ||
                !strncmp(expression->as.unary.operator, "--", 2) ||
                !strncmp(expression->as.unary.operator, "post", 4))
                return usk_c_infer_expression_kind(emitter,
                    expression->as.unary.operand);
            return usk_c_infer_expression_kind(emitter,
                expression->as.unary.operand);
        case USK_EXPR_BINARY: {
            const char *operator_text = expression->as.binary.operator;
            if (!strcmp(operator_text, "==") || !strcmp(operator_text, "!=") ||
                !strcmp(operator_text, "<") || !strcmp(operator_text, ">") ||
                !strcmp(operator_text, "<=") || !strcmp(operator_text, ">=") ||
                !strcmp(operator_text, "&&") || !strcmp(operator_text, "||"))
                return USK_C_VALUE_BOOL;
            return promote_numeric_kinds(
                usk_c_infer_expression_kind(emitter, expression->as.binary.left),
                usk_c_infer_expression_kind(emitter, expression->as.binary.right));
        }
        case USK_EXPR_ASSIGNMENT:
            return usk_c_infer_expression_kind(emitter,
                expression->as.assignment.target);
        case USK_EXPR_CALL:
        case USK_EXPR_MEMBER:
        case USK_EXPR_INDEX:
            return USK_C_VALUE_UNKNOWN;
        case USK_EXPR_CONDITIONAL: {
            UskCValueKind left = usk_c_infer_expression_kind(emitter,
                expression->as.conditional.when_true);
            UskCValueKind right = usk_c_infer_expression_kind(emitter,
                expression->as.conditional.when_false);
            return left == right ? left : promote_numeric_kinds(left, right);
        }
    }
    return USK_C_VALUE_UNKNOWN;
}

const char *usk_c_value_kind_name(UskCValueKind kind) {
    switch (kind) {
        case USK_C_VALUE_UNKNOWN: return "unknown";
        case USK_C_VALUE_NULL: return "USKNull";
        case USK_C_VALUE_BOOL: return "USKBool";
        case USK_C_VALUE_INT8: return "USKShort";
        case USK_C_VALUE_INT32: return "USKInt";
        case USK_C_VALUE_INT64: return "USKLong";
        case USK_C_VALUE_UINT8: return "unsigned USKShort";
        case USK_C_VALUE_UINT32: return "unsigned USKInt";
        case USK_C_VALUE_UINT64: return "unsigned USKLong";
        case USK_C_VALUE_FLOAT32: return "USKFloat";
        case USK_C_VALUE_FLOAT64: return "USKDouble";
        case USK_C_VALUE_STRING: return "USKString";
        case USK_C_VALUE_POINTER: return "USKAuto";
    }
    return "unknown";
}
