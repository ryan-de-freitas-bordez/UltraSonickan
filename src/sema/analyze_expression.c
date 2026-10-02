#include "analyzer_internal.h"
#include "usk/arraylib.h"
#include "usk/filesystem.h"
#include "usk/mathlib.h"
#include "usk/pathlib.h"
#include "usk/stringlib.h"

#include <stdlib.h>
#include <string.h>

static UskSemanticType type_of(UskTypeKind kind, const char *name,
                               size_t dimensions) {
    return (UskSemanticType){kind, name, dimensions, false, true, false, false};
}

static bool is_unknown(UskSemanticType type) {
    return type.kind == USK_TYPE_UNKNOWN;
}

static char *call_name(const UskAstExpr *expression) {
    if (!expression) return NULL;
    if (expression->kind == USK_EXPR_NAME) {
        const char *name = expression->as.name ? expression->as.name : "";
        size_t length = strlen(name);
        char *copy = (char *)malloc(length + 1);
        if (copy) memcpy(copy, name, length + 1);
        return copy;
    }
    if (expression->kind == USK_EXPR_MEMBER) {
        char *base = call_name(expression->as.member.object);
        if (!base) return NULL;
        const char *member = expression->as.member.name ? expression->as.member.name : "";
        const char *separator = expression->as.member.pointer_access ? "->" : "::";
        size_t length = strlen(base) + strlen(separator) + strlen(member) + 1;
        char *joined = (char *)malloc(length);
        if (joined) snprintf(joined, length, "%s%s%s", base, separator, member);
        free(base);
        return joined;
    }
    return NULL;
}

static bool builtin_call(UskSemanticAnalyzer *analyzer, const char *name,
                         const UskAstExpr *call, UskSemanticType *result) {
    if (!strcmp(name, "io::writef") || !strcmp(name, "iostream::io::writef") ||
        !strcmp(name, "writef")) {
        if (call->as.call.count < 1)
            usk_sema_error(analyzer, USK_DIAG_ARGUMENT_COUNT, call->span,
                           "writef requires a format argument");
        if (call->as.call.arguments && call->as.call.arguments->expression) {
            UskSemanticType format_type = usk_sema_expression(analyzer,
                call->as.call.arguments->expression);
            if (format_type.kind != USK_TYPE_STRING && !is_unknown(format_type))
                usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
                    call->as.call.arguments->expression->span,
                    "writef format argument must be USKString");
        }
        *result = type_of(USK_TYPE_NULL, "USKNull", 0);
        return true;
    }
    if (!strcmp(name, "io::write") || !strcmp(name, "iostream::io::write") ||
        !strcmp(name, "io::writeln") || !strcmp(name, "iostream::io::writeln") ||
        !strcmp(name, "print") || !strcmp(name, "println")) {
        *result = type_of(USK_TYPE_NULL, "USKNull", 0);
        return true;
    }
    if (!strcmp(name, "io::readline") || !strcmp(name, "iostream::io::readline") ||
        !strcmp(name, "readline")) {
        if (call->as.call.count)
            usk_sema_error(analyzer, USK_DIAG_ARGUMENT_COUNT, call->span,
                           "readline takes no arguments");
        *result = type_of(USK_TYPE_STRING, "USKString", 0);
        return true;
    }
    if (!strcmp(name, "typeof")) {
        if (call->as.call.count != 1)
            usk_sema_error(analyzer, USK_DIAG_ARGUMENT_COUNT, call->span,
                           "typeof takes exactly one argument");
        *result = type_of(USK_TYPE_STRING, "USKString", 0);
        return true;
    }
    return false;
}

static UskSemanticType check_call(UskSemanticAnalyzer *analyzer,
                                  const UskAstExpr *expression) {
    const UskAstExpr *callee = expression->as.call.callee;
    const UskArrayBuiltinInfo *array_method = callee &&
        callee->kind == USK_EXPR_MEMBER
        ? usk_array_method_find(callee->as.member.name) : NULL;
    if (array_method) {
        UskSemanticType receiver = usk_sema_expression(analyzer,
            callee->as.member.object);
        if (!receiver.array_dimensions && !is_unknown(receiver))
            usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
                callee->as.member.object->span,
                "array method '%s' requires an array receiver",
                callee->as.member.name);
        if (expression->as.call.count + 1 != array_method->argument_count)
            usk_sema_error(analyzer, USK_DIAG_ARGUMENT_COUNT, expression->span,
                "array method '%s' expects %zu arguments; received %zu",
                callee->as.member.name, array_method->argument_count - 1,
                expression->as.call.count);
        const UskAstExprList *argument = expression->as.call.arguments;
        size_t index = 1;
        for (; argument; argument = argument->next, ++index) {
            UskSemanticType actual = usk_sema_expression(analyzer,
                argument->expression);
            if (index >= array_method->argument_count) continue;
            UskArrayArgumentKind expected = array_method->arguments[index];
            if (expected == USK_ARRAY_ARGUMENT_INTEGER &&
                actual.kind != USK_TYPE_INTEGER && !is_unknown(actual))
                usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
                    argument->expression->span,
                    "array method '%s' expects an integer argument",
                    callee->as.member.name);
            if (expected == USK_ARRAY_ARGUMENT_ARRAY &&
                !actual.array_dimensions && !is_unknown(actual))
                usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
                    argument->expression->span,
                    "array method '%s' expects an array argument",
                    callee->as.member.name);
        }
        switch (array_method->result_kind) {
            case USK_ARRAY_RESULT_ARRAY: return receiver;
            case USK_ARRAY_RESULT_BOOLEAN:
                return type_of(USK_TYPE_BOOLEAN, "USKBool", 0);
            case USK_ARRAY_RESULT_INTEGER:
                return type_of(USK_TYPE_INTEGER, "USKInt", 0);
            case USK_ARRAY_RESULT_NULL:
                return type_of(USK_TYPE_NULL, "USKNull", 0);
            case USK_ARRAY_RESULT_ANY:
            default:
                if (receiver.array_dimensions) {
                    receiver.array_dimensions--;
                    return receiver;
                }
                return type_of(USK_TYPE_UNKNOWN, "USKAuto", 0);
        }
    }
    char *name = call_name(expression->as.call.callee);
    if (!name) {
        usk_sema_error(analyzer, USK_DIAG_UNSUPPORTED_FEATURE, expression->span,
                       "call target is not a named function");
        return type_of(USK_TYPE_UNKNOWN, "USKAuto", 0);
    }
    UskSemanticType result;
    const UskMathBuiltinInfo *math_builtin = usk_math_builtin_find(name);
    if (math_builtin) {
        if (expression->as.call.count != math_builtin->argument_count)
            usk_sema_error(analyzer, USK_DIAG_ARGUMENT_COUNT, expression->span,
                "math operation '%s' expects %zu arguments; received %zu",
                name, math_builtin->argument_count,
                expression->as.call.count);
        const UskAstExprList *argument = expression->as.call.arguments;
        bool all_integer = true;
        bool unknown_argument = false;
        for (size_t index = 0; argument;
             ++index, argument = argument->next) {
            UskSemanticType actual = usk_sema_expression(analyzer,
                argument->expression);
            if (index >= math_builtin->argument_count) continue;
            if (is_unknown(actual)) {
                unknown_argument = true;
                all_integer = false;
            } else if (actual.kind != USK_TYPE_INTEGER &&
                       actual.kind != USK_TYPE_FLOATING) {
                usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
                    argument->expression->span,
                    "argument %zu to '%s' must be numeric", index + 1, name);
                all_integer = false;
            } else if (actual.kind != USK_TYPE_INTEGER) {
                all_integer = false;
            }
        }
        switch (math_builtin->result_kind) {
            case USK_MATH_RESULT_BOOLEAN:
                result = type_of(USK_TYPE_BOOLEAN, "USKBool", 0);
                break;
            case USK_MATH_RESULT_INTEGER:
                result = type_of(USK_TYPE_INTEGER, "USKInt", 0);
                break;
            case USK_MATH_RESULT_PRESERVE_NUMERIC:
                result = unknown_argument
                    ? type_of(USK_TYPE_UNKNOWN, "USKAuto", 0)
                    : all_integer
                        ? type_of(USK_TYPE_INTEGER, "USKInt", 0)
                        : type_of(USK_TYPE_FLOATING, "USKDouble", 0);
                break;
            case USK_MATH_RESULT_NUMBER:
            default:
                result = type_of(USK_TYPE_FLOATING, "USKDouble", 0);
                break;
        }
        free(name);
        return result;
    }
    const UskStringBuiltinInfo *string_builtin =
        usk_string_builtin_find(name);
    if (string_builtin) {
        if (expression->as.call.count != string_builtin->argument_count)
            usk_sema_error(analyzer, USK_DIAG_ARGUMENT_COUNT, expression->span,
                "string operation '%s' expects %zu arguments; received %zu",
                name, string_builtin->argument_count,
                expression->as.call.count);
        const UskAstExprList *argument = expression->as.call.arguments;
        for (size_t index = 0; argument;
             ++index, argument = argument->next) {
            UskSemanticType actual = usk_sema_expression(analyzer,
                argument->expression);
            if (index >= string_builtin->argument_count) continue;
            UskTypeKind expected = string_builtin->arguments[index] ==
                USK_STRING_ARGUMENT_TEXT ? USK_TYPE_STRING :
                string_builtin->arguments[index] == USK_STRING_ARGUMENT_INTEGER
                    ? USK_TYPE_INTEGER : USK_TYPE_UNKNOWN;
            if (expected != USK_TYPE_UNKNOWN && actual.kind != expected &&
                !is_unknown(actual))
                usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
                    argument->expression->span,
                    "argument %zu to '%s' must be %s", index + 1, name,
                    expected == USK_TYPE_STRING ? "USKString" : "USKInt");
        }
        switch (string_builtin->result_kind) {
            case USK_STRING_RESULT_TEXT:
                result = type_of(USK_TYPE_STRING, "USKString", 0);
                break;
            case USK_STRING_RESULT_BOOLEAN:
                result = type_of(USK_TYPE_BOOLEAN, "USKBool", 0);
                break;
            case USK_STRING_RESULT_INTEGER:
                result = type_of(USK_TYPE_INTEGER, "USKInt", 0);
                break;
            case USK_STRING_RESULT_ARRAY:
                result = type_of(USK_TYPE_STRING, "USKString", 1);
                break;
            default:
                result = type_of(USK_TYPE_UNKNOWN, "USKAuto", 0);
                break;
        }
        free(name);
        return result;
    }
    const UskPathBuiltinInfo *path_builtin = usk_path_builtin_find(name);
    if (path_builtin) {
        if (expression->as.call.count != path_builtin->argument_count)
            usk_sema_error(analyzer, USK_DIAG_ARGUMENT_COUNT, expression->span,
                "path operation '%s' expects %zu arguments; received %zu",
                name, path_builtin->argument_count,
                expression->as.call.count);
        const UskAstExprList *argument = expression->as.call.arguments;
        for (size_t index = 0; argument;
             ++index, argument = argument->next) {
            UskSemanticType actual = usk_sema_expression(analyzer,
                argument->expression);
            if (index < path_builtin->argument_count &&
                actual.kind != USK_TYPE_STRING && !is_unknown(actual))
                usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
                    argument->expression->span,
                    "argument %zu to '%s' must be USKString", index + 1, name);
        }
        result = path_builtin->result_kind == USK_PATH_RESULT_TEXT
            ? type_of(USK_TYPE_STRING, "USKString", 0)
            : path_builtin->result_kind == USK_PATH_RESULT_BOOLEAN
                ? type_of(USK_TYPE_BOOLEAN, "USKBool", 0)
                : type_of(USK_TYPE_INTEGER, "USKInt", 0);
        free(name);
        return result;
    }
    const UskFsBuiltinInfo *fs_builtin = usk_fs_builtin_find(name);
    if (fs_builtin) {
        if (expression->as.call.count != fs_builtin->argument_count)
            usk_sema_error(analyzer, USK_DIAG_ARGUMENT_COUNT, expression->span,
                "filesystem operation '%s' expects %zu arguments; received %zu",
                name, fs_builtin->argument_count,
                expression->as.call.count);
        const UskAstExprList *argument = expression->as.call.arguments;
        for (size_t index = 0; argument;
             ++index, argument = argument->next) {
            UskSemanticType actual = usk_sema_expression(analyzer,
                argument->expression);
            if (index >= fs_builtin->argument_count) continue;
            UskFsArgumentKind expected = fs_builtin->arguments[index];
            if (expected == USK_FS_ARGUMENT_TEXT &&
                actual.kind != USK_TYPE_STRING && !is_unknown(actual))
                usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
                    argument->expression->span,
                    "argument %zu to '%s' must be USKString", index + 1, name);
            if (expected == USK_FS_ARGUMENT_ARRAY &&
                !actual.array_dimensions && !is_unknown(actual))
                usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
                    argument->expression->span,
                    "argument %zu to '%s' must be an array", index + 1, name);
        }
        switch (fs_builtin->result_kind) {
            case USK_FS_RESULT_TEXT:
                result = type_of(USK_TYPE_STRING, "USKString", 0); break;
            case USK_FS_RESULT_BOOLEAN:
                result = type_of(USK_TYPE_BOOLEAN, "USKBool", 0); break;
            case USK_FS_RESULT_INTEGER:
                result = type_of(USK_TYPE_INTEGER, "USKInt", 0); break;
            case USK_FS_RESULT_ARRAY:
                result = type_of(USK_TYPE_STRING, "USKString", 1); break;
            case USK_FS_RESULT_NULL:
            default:
                result = type_of(USK_TYPE_NULL, "USKNull", 0); break;
        }
        free(name);
        return result;
    }
    const UskArrayBuiltinInfo *array_builtin = usk_array_builtin_find(name);
    if (array_builtin) {
        if (expression->as.call.count != array_builtin->argument_count)
            usk_sema_error(analyzer, USK_DIAG_ARGUMENT_COUNT, expression->span,
                "array operation '%s' expects %zu arguments; received %zu",
                name, array_builtin->argument_count,
                expression->as.call.count);
        const UskAstExprList *argument = expression->as.call.arguments;
        for (size_t index = 0; argument;
             ++index, argument = argument->next) {
            UskSemanticType actual = usk_sema_expression(analyzer,
                argument->expression);
            if (index >= array_builtin->argument_count) continue;
            UskArrayArgumentKind expected = array_builtin->arguments[index];
            if (expected == USK_ARRAY_ARGUMENT_ARRAY &&
                !actual.array_dimensions && !is_unknown(actual))
                usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
                    argument->expression->span,
                    "argument %zu to '%s' must be an array", index + 1, name);
            if (expected == USK_ARRAY_ARGUMENT_INTEGER &&
                actual.kind != USK_TYPE_INTEGER && !is_unknown(actual))
                usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
                    argument->expression->span,
                    "argument %zu to '%s' must be an integer", index + 1, name);
        }
        switch (array_builtin->result_kind) {
            case USK_ARRAY_RESULT_ARRAY:
                result = type_of(USK_TYPE_UNKNOWN, "USKAuto", 1); break;
            case USK_ARRAY_RESULT_BOOLEAN:
                result = type_of(USK_TYPE_BOOLEAN, "USKBool", 0); break;
            case USK_ARRAY_RESULT_INTEGER:
                result = type_of(USK_TYPE_INTEGER, "USKInt", 0); break;
            case USK_ARRAY_RESULT_NULL:
                result = type_of(USK_TYPE_NULL, "USKNull", 0); break;
            case USK_ARRAY_RESULT_ANY:
            default:
                result = type_of(USK_TYPE_UNKNOWN, "USKAuto", 0); break;
        }
        free(name);
        return result;
    }
    if (builtin_call(analyzer, name, expression, &result)) {
        free(name);
        for (const UskAstExprList *argument = expression->as.call.arguments;
             argument; argument = argument->next) {
            if (argument != expression->as.call.arguments)
                usk_sema_expression(analyzer, argument->expression);
        }
        return result;
    }

    UskSemanticSymbol *function = usk_sema_find_function(analyzer, name);
    if (!function) {
        usk_sema_error(analyzer, USK_DIAG_UNKNOWN_NAME, expression->span,
                       "unknown function '%s'", name);
        free(name);
        return type_of(USK_TYPE_UNKNOWN, "USKAuto", 0);
    }
    const UskAstDecl *declaration = function->declaration;
    if (declaration && declaration->visibility == USK_VISIBILITY_PRIVATE &&
        analyzer->current_owner) {
        const char *owner_end = strstr(function->name, "::");
        size_t owner_length = owner_end ? (size_t)(owner_end - function->name) : 0;
        if (owner_length && strncmp(function->name, analyzer->current_owner, owner_length) != 0)
            usk_sema_error(analyzer, USK_DIAG_INVALID_DECLARATION, expression->span,
                           "function '%s' is private", name);
    }
    size_t required = 0, parameter_count = 0;
    for (const UskAstParameter *parameter = declaration->as.function.parameters;
         parameter; parameter = parameter->next) {
        parameter_count++;
        if (!parameter->default_value) required++;
    }
    if (expression->as.call.count < required || expression->as.call.count > parameter_count)
        usk_sema_error(analyzer, USK_DIAG_ARGUMENT_COUNT, expression->span,
                       "function '%s' expects %zu to %zu arguments; received %zu",
                       name, required, parameter_count, expression->as.call.count);
    const UskAstParameter *parameter = declaration->as.function.parameters;
    const UskAstExprList *argument = expression->as.call.arguments;
    while (parameter && argument) {
        UskSemanticType argument_type = usk_sema_expression(analyzer,
            argument->expression);
        if (!usk_sema_types_compatible(usk_sema_resolve_ast_type(analyzer,
                                       parameter->type, declaration->span),
                                       argument_type))
            usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
                argument->expression->span,
                "argument for '%s' does not match parameter '%s' of type %s",
                name, parameter->name, parameter->type->name);
        parameter = parameter->next;
        argument = argument->next;
    }
    for (; argument; argument = argument->next)
        usk_sema_expression(analyzer, argument->expression);
    result = usk_sema_resolve_ast_type(analyzer,
        declaration->as.function.return_type, declaration->span);
    free(name);
    return result;
}

static UskSemanticType check_binary(UskSemanticAnalyzer *analyzer,
                                    const UskAstExpr *expression) {
    const char *operator_text = expression->as.binary.operator;
    UskSemanticType left = usk_sema_expression(analyzer, expression->as.binary.left);
    UskSemanticType right = usk_sema_expression(analyzer, expression->as.binary.right);
    if (!strcmp(operator_text, "&&") || !strcmp(operator_text, "||") ||
        !strcmp(operator_text, "==") || !strcmp(operator_text, "!="))
        return type_of(USK_TYPE_BOOLEAN, "USKBool", 0);
    if (!strcmp(operator_text, "<") || !strcmp(operator_text, ">") ||
        !strcmp(operator_text, "<=") || !strcmp(operator_text, ">=")) {
        bool numeric = (left.kind == USK_TYPE_INTEGER || left.kind == USK_TYPE_FLOATING || is_unknown(left)) &&
                       (right.kind == USK_TYPE_INTEGER || right.kind == USK_TYPE_FLOATING || is_unknown(right));
        bool strings = left.kind == USK_TYPE_STRING && right.kind == USK_TYPE_STRING;
        if (!numeric && !strings)
            usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH, expression->span,
                           "operator '%s' compares incompatible types", operator_text);
        return type_of(USK_TYPE_BOOLEAN, "USKBool", 0);
    }
    if (!strcmp(operator_text, "+") &&
        (left.kind == USK_TYPE_STRING || right.kind == USK_TYPE_STRING))
        return type_of(USK_TYPE_STRING, "USKString", 0);
    if ((left.kind != USK_TYPE_INTEGER && left.kind != USK_TYPE_FLOATING && !is_unknown(left)) ||
        (right.kind != USK_TYPE_INTEGER && right.kind != USK_TYPE_FLOATING && !is_unknown(right)))
        usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH, expression->span,
                       "operator '%s' requires numeric operands", operator_text);
    if (left.kind == USK_TYPE_FLOATING || right.kind == USK_TYPE_FLOATING)
        return type_of(USK_TYPE_FLOATING, "USKDouble", 0);
    return type_of(USK_TYPE_INTEGER, "USKInt", 0);
}

static UskSemanticType check_assignment(UskSemanticAnalyzer *analyzer,
                                        const UskAstExpr *expression) {
    const UskAstExpr *target = expression->as.assignment.target;
    UskSemanticType target_type = type_of(USK_TYPE_UNKNOWN, "USKAuto", 0);
    if (target->kind == USK_EXPR_NAME) {
        UskSemanticSymbol *symbol = usk_sema_lookup(analyzer, target->as.name);
        if (!symbol) {
            usk_sema_error(analyzer, USK_DIAG_UNKNOWN_NAME, target->span,
                           "assignment to unknown name '%s'", target->as.name);
        } else if (symbol->kind != USK_SYMBOL_VARIABLE) {
            usk_sema_error(analyzer, USK_DIAG_INVALID_DECLARATION, target->span,
                           "'%s' is not a variable", target->as.name);
        } else {
            target_type = symbol->type;
            if (target_type.is_const)
                usk_sema_error(analyzer, USK_DIAG_INVALID_DECLARATION, target->span,
                               "cannot assign to const '%s'", target->as.name);
        }
    } else if (target->kind == USK_EXPR_INDEX) {
        target_type = usk_sema_expression(analyzer, target);
    } else {
        usk_sema_error(analyzer, USK_DIAG_INVALID_DECLARATION, target->span,
                       "assignment target is not mutable storage");
    }
    UskSemanticType value_type = usk_sema_expression(analyzer,
        expression->as.assignment.value);
    if (strcmp(expression->as.assignment.operator, "=") &&
        (target_type.kind != USK_TYPE_INTEGER && target_type.kind != USK_TYPE_FLOATING &&
         target_type.kind != USK_TYPE_STRING && !is_unknown(target_type)))
        usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH, expression->span,
                       "compound assignment requires a numeric or string target");
    if (!usk_sema_types_compatible(target_type, value_type) &&
        strcmp(expression->as.assignment.operator, "+="))
        usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH, expression->span,
                       "assigned value is incompatible with target type %s",
                       target_type.name ? target_type.name : "unknown");
    return target_type;
}

UskSemanticType usk_sema_expression(UskSemanticAnalyzer *analyzer,
                                   const UskAstExpr *expression) {
    if (!expression) return type_of(USK_TYPE_UNKNOWN, "USKAuto", 0);
    analyzer->result.expressions_checked++;
    switch (expression->kind) {
        case USK_EXPR_NULL: return type_of(USK_TYPE_NULL, "USKNull", 0);
        case USK_EXPR_BOOLEAN: return type_of(USK_TYPE_BOOLEAN, "USKBool", 0);
        case USK_EXPR_INTEGER: return type_of(USK_TYPE_INTEGER, "USKInt", 0);
        case USK_EXPR_DOUBLE: return type_of(USK_TYPE_FLOATING, "USKDouble", 0);
        case USK_EXPR_STRING: return type_of(USK_TYPE_STRING, "USKString", 0);
        case USK_EXPR_NAME: {
            UskSemanticSymbol *symbol = usk_sema_lookup(analyzer, expression->as.name);
            if (!symbol) {
                usk_sema_error(analyzer, USK_DIAG_UNKNOWN_NAME, expression->span,
                               "unknown name '%s'", expression->as.name);
                return type_of(USK_TYPE_UNKNOWN, "USKAuto", 0);
            }
            if (symbol->kind == USK_SYMBOL_FUNCTION)
                usk_sema_error(analyzer, USK_DIAG_INVALID_DECLARATION, expression->span,
                               "function '%s' must be called", expression->as.name);
            return symbol->type;
        }
        case USK_EXPR_ARRAY: {
            UskSemanticType element = type_of(USK_TYPE_UNKNOWN, "USKAuto", 0);
            bool first = true;
            for (const UskAstExprList *item = expression->as.array.items;
                 item; item = item->next) {
                UskSemanticType item_type = usk_sema_expression(analyzer, item->expression);
                if (first) { element = item_type; first = false; }
                else if (!usk_sema_types_compatible(element, item_type))
                    usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
                        item->expression->span,
                        "array literal elements must have compatible types");
            }
            element.array_dimensions++;
            return element;
        }
        case USK_EXPR_UNARY: {
            UskSemanticType operand = usk_sema_expression(analyzer,
                expression->as.unary.operand);
            if (!strcmp(expression->as.unary.operator, "!"))
                return type_of(USK_TYPE_BOOLEAN, "USKBool", 0);
            if (!strcmp(expression->as.unary.operator, "++") ||
                !strcmp(expression->as.unary.operator, "--") ||
                !strcmp(expression->as.unary.operator, "post++") ||
                !strcmp(expression->as.unary.operator, "post--")) {
                if (operand.kind != USK_TYPE_INTEGER && operand.kind != USK_TYPE_FLOATING)
                    usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH, expression->span,
                                   "increment/decrement requires a numeric target");
            } else if (operand.kind != USK_TYPE_INTEGER && operand.kind != USK_TYPE_FLOATING && !is_unknown(operand))
                usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH, expression->span,
                               "unary operator requires a number");
            return operand;
        }
        case USK_EXPR_BINARY:
            return check_binary(analyzer, expression);
        case USK_EXPR_ASSIGNMENT:
            return check_assignment(analyzer, expression);
        case USK_EXPR_CALL:
            return check_call(analyzer, expression);
        case USK_EXPR_MEMBER: {
            char *name = call_name(expression);
            UskSemanticSymbol *symbol = name ? usk_sema_lookup(analyzer, name) : NULL;
            if (symbol && symbol->kind == USK_SYMBOL_ENUM_VALUE) {
                free(name);
                return symbol->type;
            }
            UskSemanticType object = usk_sema_expression(analyzer,
                expression->as.member.object);
            const char *member = expression->as.member.name;
            if ((object.kind == USK_TYPE_STRING || object.array_dimensions) &&
                (!strcmp(member, "length") || !strcmp(member, "size"))) {
                free(name);
                return type_of(USK_TYPE_INTEGER, "USKInt", 0);
            }
            free(name);
            usk_sema_error(analyzer, USK_DIAG_UNSUPPORTED_FEATURE, expression->span,
                           "member access '%s' is not statically resolved", member);
            return type_of(USK_TYPE_UNKNOWN, "USKAuto", 0);
        }
        case USK_EXPR_INDEX: {
            UskSemanticType object = usk_sema_expression(analyzer,
                expression->as.index.object);
            UskSemanticType index = usk_sema_expression(analyzer,
                expression->as.index.index);
            if (index.kind != USK_TYPE_INTEGER && !is_unknown(index))
                usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
                    expression->as.index.index->span,
                    "array index must be an integer");
            if (object.array_dimensions) {
                object.array_dimensions--;
                return object;
            }
            if (object.kind == USK_TYPE_STRING)
                return type_of(USK_TYPE_CHARACTER, "USKChar", 0);
            usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH, expression->span,
                           "indexing requires an array or string");
            return type_of(USK_TYPE_UNKNOWN, "USKAuto", 0);
        }
        case USK_EXPR_CONDITIONAL: {
            usk_sema_expression(analyzer, expression->as.conditional.condition);
            UskSemanticType yes = usk_sema_expression(analyzer,
                expression->as.conditional.when_true);
            UskSemanticType no = usk_sema_expression(analyzer,
                expression->as.conditional.when_false);
            if (!usk_sema_types_compatible(yes, no))
                usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH, expression->span,
                               "conditional expression branches have incompatible types");
            return yes.kind == USK_TYPE_UNKNOWN ? no : yes;
        }
    }
    return type_of(USK_TYPE_UNKNOWN, "USKAuto", 0);
}
