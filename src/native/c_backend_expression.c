#include "c_backend_internal.h"

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool emit_escaped_string(UskCEmitter *emitter, const char *text) {
    if (!usk_c_emit_write(emitter, "\"")) return false;
    const unsigned char *cursor = (const unsigned char *)(text ? text : "");
    for (; *cursor; ++cursor) {
        switch (*cursor) {
            case '\n': usk_c_emit_write(emitter, "\\n"); break;
            case '\r': usk_c_emit_write(emitter, "\\r"); break;
            case '\t': usk_c_emit_write(emitter, "\\t"); break;
            case '\b': usk_c_emit_write(emitter, "\\b"); break;
            case '\f': usk_c_emit_write(emitter, "\\f"); break;
            case '\\': usk_c_emit_write(emitter, "\\\\"); break;
            case '"': usk_c_emit_write(emitter, "\\\""); break;
            default:
                if (*cursor < 32 || *cursor == 127)
                    usk_c_emit_write(emitter, "\\%03o", (unsigned)*cursor);
                else
                    usk_c_emit_write(emitter, "%c", *cursor);
                break;
        }
        if (emitter->failed) return false;
    }
    return usk_c_emit_write(emitter, "\"");
}

static char *qualified_expression_name(const UskAstExpr *expression) {
    if (!expression) return NULL;
    if (expression->kind == USK_EXPR_NAME) {
        const char *name = expression->as.name ? expression->as.name : "";
        size_t length = strlen(name);
        char *copy = (char *)malloc(length + 1);
        if (copy) memcpy(copy, name, length + 1);
        return copy;
    }
    if (expression->kind != USK_EXPR_MEMBER ||
        expression->as.member.pointer_access) return NULL;
    char *owner = qualified_expression_name(expression->as.member.object);
    char *joined = owner ? usk_c_join_name(owner, expression->as.member.name)
                         : NULL;
    free(owner);
    return joined;
}

static bool is_io_builtin(const char *name, const char *operation) {
    if (!name || !operation) return false;
    if (strcmp(name, operation) == 0) return true;
    if (strcmp(name, operation + 4) == 0) return true;
    char qualified[96];
    int written = snprintf(qualified, sizeof(qualified), "io::%s", operation + 4);
    if (written > 0 && (size_t)written < sizeof(qualified) &&
        strcmp(name, qualified) == 0) return true;
    written = snprintf(qualified, sizeof(qualified), "iostream::io::%s",
                       operation + 4);
    return written > 0 && (size_t)written < sizeof(qualified) &&
           strcmp(name, qualified) == 0;
}

static bool emit_expression_list(UskCEmitter *emitter,
                                 const UskAstExprList *item,
                                 size_t expected_count) {
    size_t count = 0;
    for (; item && !emitter->failed; item = item->next) {
        if (count && !usk_c_emit_write(emitter, ", ")) return false;
        if (!usk_c_emit_expression(emitter, item->expression)) return false;
        count++;
    }
    if (count != expected_count) {
        usk_c_emit_error(emitter, USK_DIAG_INTERNAL_ERROR, (UskSourceSpan){0},
                         "expression list count does not match its AST metadata");
        return false;
    }
    return !emitter->failed;
}

static bool emit_format_values(UskCEmitter *emitter,
                               const UskAstExprList *item,
                               size_t expected_count) {
    if (!expected_count) return usk_c_emit_write(emitter, "NULL, 0");
    if (!usk_c_emit_write(emitter, "(UskFmtArg[]){")) return false;
    size_t count = 0;
    for (; item && !emitter->failed; item = item->next) {
        if (count) usk_c_emit_write(emitter, ", ");
        usk_c_emit_write(emitter, "USK_FMT_ARG(");
        usk_c_emit_expression(emitter, item->expression);
        usk_c_emit_write(emitter, ")");
        count++;
    }
    if (count != expected_count) {
        usk_c_emit_error(emitter, USK_DIAG_INTERNAL_ERROR, (UskSourceSpan){0},
                         "format argument count does not match its AST metadata");
        return false;
    }
    return usk_c_emit_write(emitter, "}, %zu", expected_count);
}

static bool emit_io_call(UskCEmitter *emitter, const UskAstExpr *call,
                         const char *operation) {
    const UskAstExprList *argument = call->as.call.arguments;
    size_t count = call->as.call.count;
    if (!strcmp(operation, "writef")) {
        if (!count || !argument) {
            usk_c_emit_error(emitter, USK_DIAG_ARGUMENT_COUNT, call->span,
                             "io::writef requires a format string");
            return false;
        }
        usk_c_emit_write(emitter, "usk_writef(");
        usk_c_emit_expression(emitter, argument->expression);
        usk_c_emit_write(emitter, ", ");
        bool success = emit_format_values(emitter, argument->next, count - 1);
        usk_c_emit_write(emitter, ")");
        return success && !emitter->failed;
    }
    usk_c_emit_write(emitter, !strcmp(operation, "write")
        ? "usk_write(" : "usk_writeln(");
    bool success = emit_format_values(emitter, argument, count);
    usk_c_emit_write(emitter, ")");
    return success && !emitter->failed;
}

static bool emit_call(UskCEmitter *emitter, const UskAstExpr *call) {
    char *qualified = qualified_expression_name(call->as.call.callee);
    if (is_io_builtin(qualified, "usk_writef")) {
        free(qualified);
        return emit_io_call(emitter, call, "writef");
    }
    if (is_io_builtin(qualified, "usk_write")) {
        free(qualified);
        return emit_io_call(emitter, call, "write");
    }
    if (is_io_builtin(qualified, "usk_writeln")) {
        free(qualified);
        return emit_io_call(emitter, call, "writeln");
    }
    if (qualified && (!strcmp(qualified, "io::readline") ||
        !strcmp(qualified, "iostream::io::readline"))) {
        free(qualified);
        usk_c_emit_unsupported(emitter, call->span,
            "io::readline in the allocation-free native C backend");
        return false;
    }
    if (qualified) {
        char *mangled = usk_c_mangle_name(qualified);
        free(qualified);
        if (!mangled) {
            usk_c_emit_error(emitter, USK_DIAG_OUT_OF_MEMORY, call->span,
                             "cannot allocate a C function name");
            return false;
        }
        usk_c_emit_write(emitter, "%s(", mangled);
        free(mangled);
    } else {
        if (!usk_c_emit_expression(emitter, call->as.call.callee)) return false;
        usk_c_emit_write(emitter, "(");
    }
    bool success = emit_expression_list(emitter, call->as.call.arguments,
                                        call->as.call.count);
    usk_c_emit_write(emitter, ")");
    return success && !emitter->failed;
}

static const char *integer_helper(UskCValueKind kind, const char *operator_text) {
    const char *suffix = NULL;
    if (!strcmp(operator_text, "+")) suffix = "add";
    else if (!strcmp(operator_text, "-")) suffix = "subtract";
    else if (!strcmp(operator_text, "*")) suffix = "multiply";
    else if (!strcmp(operator_text, "/")) suffix = "divide";
    else if (!strcmp(operator_text, "%")) suffix = "remainder";
    if (!suffix) return NULL;
    if (kind == USK_C_VALUE_INT32) {
        static char name[48];
        snprintf(name, sizeof(name), "usk_i32_%s", suffix);
        return name;
    }
    if (kind == USK_C_VALUE_INT64) {
        static char name[48];
        snprintf(name, sizeof(name), "usk_i64_%s", suffix);
        return name;
    }
    return NULL;
}

static bool emit_arithmetic(UskCEmitter *emitter,
                            const UskAstExpr *expression) {
    const char *operator_text = expression->as.binary.operator;
    UskCValueKind left_kind = usk_c_infer_expression_kind(emitter,
        expression->as.binary.left);
    UskCValueKind right_kind = usk_c_infer_expression_kind(emitter,
        expression->as.binary.right);
    UskCValueKind kind = usk_c_infer_expression_kind(emitter, expression);
    if (left_kind == USK_C_VALUE_STRING || right_kind == USK_C_VALUE_STRING) {
        usk_c_emit_unsupported(emitter, expression->span,
                               "dynamic string concatenation in native C");
        return false;
    }
    const char *helper = NULL;
    if (kind == USK_C_VALUE_INT32 || kind == USK_C_VALUE_INT64)
        helper = integer_helper(kind, operator_text);
    else if (kind == USK_C_VALUE_FLOAT64 && strcmp(operator_text, "%")) {
        if (!strcmp(operator_text, "+")) helper = "usk_f64_add";
        else if (!strcmp(operator_text, "-")) helper = "usk_f64_subtract";
        else if (!strcmp(operator_text, "*")) helper = "usk_f64_multiply";
        else if (!strcmp(operator_text, "/")) helper = "usk_f64_divide";
    }
    if (!helper) {
        usk_c_emit_unsupported(emitter, expression->span,
            "arithmetic with unresolved, unsigned, or unsupported numeric types");
        return false;
    }
    UskCValueKind previous_expected = emitter->expected_value_kind;
    emitter->expected_value_kind = kind;
    usk_c_emit_write(emitter, "%s(", helper);
    usk_c_emit_expression(emitter, expression->as.binary.left);
    usk_c_emit_write(emitter, ", ");
    usk_c_emit_expression(emitter, expression->as.binary.right);
    usk_c_emit_write(emitter, ")");
    emitter->expected_value_kind = previous_expected;
    return !emitter->failed;
}

bool usk_c_emit_condition(UskCEmitter *emitter,
                          const UskAstExpr *expression) {
    if (!emitter || !expression) return false;
    UskCValueKind kind = usk_c_infer_expression_kind(emitter, expression);
    if (kind == USK_C_VALUE_STRING) {
        usk_c_emit_write(emitter, "usk_string_truthy(");
        usk_c_emit_expression(emitter, expression);
        return usk_c_emit_write(emitter, ")");
    }
    if (kind == USK_C_VALUE_NULL || kind == USK_C_VALUE_POINTER) {
        usk_c_emit_write(emitter, "(");
        usk_c_emit_expression(emitter, expression);
        return usk_c_emit_write(emitter, " != NULL)");
    }
    return usk_c_emit_expression(emitter, expression);
}

static bool emit_binary(UskCEmitter *emitter, const UskAstExpr *expression) {
    const char *operator_text = expression->as.binary.operator;
    if (!strcmp(operator_text, "&&") || !strcmp(operator_text, "||")) {
        usk_c_emit_write(emitter, "(");
        usk_c_emit_condition(emitter, expression->as.binary.left);
        usk_c_emit_write(emitter, " %s ", operator_text);
        usk_c_emit_condition(emitter, expression->as.binary.right);
        return usk_c_emit_write(emitter, ")");
    }
    UskCValueKind left_kind = usk_c_infer_expression_kind(emitter,
        expression->as.binary.left);
    UskCValueKind right_kind = usk_c_infer_expression_kind(emitter,
        expression->as.binary.right);
    if (left_kind == USK_C_VALUE_STRING && right_kind == USK_C_VALUE_STRING &&
        (!strcmp(operator_text, "==") || !strcmp(operator_text, "!=") ||
         !strcmp(operator_text, "<") || !strcmp(operator_text, ">") ||
         !strcmp(operator_text, "<=") || !strcmp(operator_text, ">="))) {
        usk_c_emit_write(emitter, "(usk_string_compare(");
        usk_c_emit_expression(emitter, expression->as.binary.left);
        usk_c_emit_write(emitter, ", ");
        usk_c_emit_expression(emitter, expression->as.binary.right);
        const char *comparison = !strcmp(operator_text, "==") ? " == 0)" :
            !strcmp(operator_text, "!=") ? " != 0)" :
            !strcmp(operator_text, "<") ? " < 0)" :
            !strcmp(operator_text, ">") ? " > 0)" :
            !strcmp(operator_text, "<=") ? " <= 0)" : " >= 0)";
        return usk_c_emit_write(emitter, ")%s", comparison);
    }
    if (!strcmp(operator_text, "+") || !strcmp(operator_text, "-") ||
        !strcmp(operator_text, "*") || !strcmp(operator_text, "/") ||
        !strcmp(operator_text, "%"))
        return emit_arithmetic(emitter, expression);
    usk_c_emit_write(emitter, "(");
    usk_c_emit_expression(emitter, expression->as.binary.left);
    usk_c_emit_write(emitter, " %s ", operator_text);
    usk_c_emit_expression(emitter, expression->as.binary.right);
    return usk_c_emit_write(emitter, ")");
}

static bool emit_compound_assignment(UskCEmitter *emitter,
                                     const UskAstExpr *expression) {
    const char *operator_text = expression->as.assignment.operator;
    const UskAstExpr *target = expression->as.assignment.target;
    if (!operator_text || strlen(operator_text) != 2 || operator_text[1] != '=' ||
        !target || target->kind != USK_EXPR_NAME) {
        usk_c_emit_unsupported(emitter, expression->span,
            "compound assignment except to a local variable");
        return false;
    }
    UskCValueKind kind = usk_c_infer_expression_kind(emitter, target);
    char operation[2] = {operator_text[0], '\0'};
    const char *helper = NULL;
    if (kind == USK_C_VALUE_INT32 || kind == USK_C_VALUE_INT64)
        helper = integer_helper(kind, operation);
    else if (kind == USK_C_VALUE_FLOAT64) {
        helper = operator_text[0] == '+' ? "usk_f64_add" :
                 operator_text[0] == '-' ? "usk_f64_subtract" :
                 operator_text[0] == '*' ? "usk_f64_multiply" :
                 operator_text[0] == '/' ? "usk_f64_divide" : NULL;
    }
    if (!helper) {
        usk_c_emit_unsupported(emitter, expression->span,
                               "compound assignment for this value type");
        return false;
    }
    UskCValueKind previous_expected = emitter->expected_value_kind;
    emitter->expected_value_kind = kind;
    usk_c_emit_write(emitter, "(%s = %s(%s, ", target->as.name, helper,
                     target->as.name);
    usk_c_emit_expression(emitter, expression->as.assignment.value);
    usk_c_emit_write(emitter, "))");
    emitter->expected_value_kind = previous_expected;
    return !emitter->failed;
}

static bool emit_assignment(UskCEmitter *emitter,
                            const UskAstExpr *expression) {
    const char *operator_text = expression->as.assignment.operator;
    if (operator_text && !strcmp(operator_text, "=")) {
        usk_c_emit_write(emitter, "(");
        usk_c_emit_expression(emitter, expression->as.assignment.target);
        usk_c_emit_write(emitter, " = ");
        UskCValueKind previous_expected = emitter->expected_value_kind;
        emitter->expected_value_kind = usk_c_infer_expression_kind(emitter,
            expression->as.assignment.target);
        usk_c_emit_expression(emitter, expression->as.assignment.value);
        emitter->expected_value_kind = previous_expected;
        return usk_c_emit_write(emitter, ")");
    }
    return emit_compound_assignment(emitter, expression);
}

static bool emit_unary(UskCEmitter *emitter, const UskAstExpr *expression) {
    const char *operator_text = expression->as.unary.operator;
    const UskAstExpr *operand = expression->as.unary.operand;
    bool postfix = operator_text && !strncmp(operator_text, "post", 4);
    const char *operator_name = postfix ? operator_text + 4 : operator_text;
    if (!strcmp(operator_name, "!")) {
        usk_c_emit_write(emitter, "(!");
        usk_c_emit_condition(emitter, operand);
        return usk_c_emit_write(emitter, ")");
    }
    UskCValueKind kind = usk_c_infer_expression_kind(emitter, operand);
    if (!strcmp(operator_name, "++") || !strcmp(operator_name, "--")) {
        if (!operand || operand->kind != USK_EXPR_NAME) {
            usk_c_emit_unsupported(emitter, expression->span,
                "increment or decrement of non-local lvalues");
            return false;
        }
        const char *prefix = kind == USK_C_VALUE_INT32 ? "usk_i32" :
            kind == USK_C_VALUE_INT64 ? "usk_i64" :
            kind == USK_C_VALUE_FLOAT64 ? "usk_f64" : NULL;
        if (!prefix) {
            usk_c_emit_unsupported(emitter, expression->span,
                                   "increment for this value type");
            return false;
        }
        usk_c_emit_write(emitter, "%s_%s_%s(&( %s ))", prefix,
            postfix ? "post" : "pre",
            operator_name[0] == '+' ? "increment" : "decrement",
            operand->as.name);
        return !emitter->failed;
    }
    if (!strcmp(operator_name, "-")) {
        const char *helper = kind == USK_C_VALUE_INT32 ? "usk_i32_negate" :
            kind == USK_C_VALUE_INT64 ? "usk_i64_negate" : NULL;
        if (helper) {
            usk_c_emit_write(emitter, "%s(", helper);
            usk_c_emit_expression(emitter, operand);
            return usk_c_emit_write(emitter, ")");
        }
        if (kind == USK_C_VALUE_FLOAT64) {
            usk_c_emit_write(emitter, "usk_f64_check(-(");
            usk_c_emit_expression(emitter, operand);
            return usk_c_emit_write(emitter, "))");
        }
        usk_c_emit_unsupported(emitter, expression->span,
                               "unary minus for this value type");
        return false;
    }
    usk_c_emit_write(emitter, "(%s", operator_name ? operator_name : "?");
    usk_c_emit_expression(emitter, operand);
    return usk_c_emit_write(emitter, ")");
}

static bool emit_expression_body(UskCEmitter *emitter,
                                 const UskAstExpr *expression) {
    switch (expression->kind) {
        case USK_EXPR_NULL: return usk_c_emit_write(emitter, "((USKNull)0)");
        case USK_EXPR_BOOLEAN:
            return usk_c_emit_write(emitter, "%s",
                expression->as.boolean ? "true" : "false");
        case USK_EXPR_INTEGER:
            if ((emitter->expected_value_kind == USK_C_VALUE_INT32 ||
                 emitter->expected_value_kind == USK_C_VALUE_UNKNOWN) &&
                (expression->as.integer < INT32_MIN ||
                 expression->as.integer > INT32_MAX)) {
                usk_c_emit_error(emitter, USK_DIAG_NUMERIC_OVERFLOW,
                    expression->span,
                    "integer literal does not fit the native USKInt width");
                return false;
            }
            return usk_c_emit_write(emitter, "(%lldLL)", expression->as.integer);
        case USK_EXPR_DOUBLE:
            return usk_c_emit_write(emitter, "(%.17g)", expression->as.floating);
        case USK_EXPR_STRING:
            return emit_escaped_string(emitter, expression->as.string);
        case USK_EXPR_NAME:
            return usk_c_emit_write(emitter, "%s",
                expression->as.name ? expression->as.name : "<invalid-name>");
        case USK_EXPR_ARRAY:
            usk_c_emit_unsupported(emitter, expression->span,
                                   "array literal expressions");
            return false;
        case USK_EXPR_UNARY:
            return emit_unary(emitter, expression);
        case USK_EXPR_BINARY:
            return emit_binary(emitter, expression);
        case USK_EXPR_ASSIGNMENT:
            return emit_assignment(emitter, expression);
        case USK_EXPR_CALL:
            return emit_call(emitter, expression);
        case USK_EXPR_MEMBER:
            if (expression->as.member.pointer_access) {
                usk_c_emit_write(emitter, "(");
                usk_c_emit_expression(emitter, expression->as.member.object);
                return usk_c_emit_write(emitter, "->%s)",
                    expression->as.member.name ? expression->as.member.name : "<member>");
            }
            usk_c_emit_write(emitter, "(");
            usk_c_emit_expression(emitter, expression->as.member.object);
            return usk_c_emit_write(emitter, ".%s)",
                expression->as.member.name ? expression->as.member.name : "<member>");
        case USK_EXPR_INDEX:
            usk_c_emit_write(emitter, "(");
            usk_c_emit_expression(emitter, expression->as.index.object);
            usk_c_emit_write(emitter, "[");
            usk_c_emit_expression(emitter, expression->as.index.index);
            return usk_c_emit_write(emitter, "])");
        case USK_EXPR_CONDITIONAL:
            usk_c_emit_write(emitter, "(");
            usk_c_emit_condition(emitter, expression->as.conditional.condition);
            usk_c_emit_write(emitter, " ? ");
            usk_c_emit_expression(emitter, expression->as.conditional.when_true);
            usk_c_emit_write(emitter, " : ");
            usk_c_emit_expression(emitter, expression->as.conditional.when_false);
            return usk_c_emit_write(emitter, ")");
    }
    usk_c_emit_error(emitter, USK_DIAG_INTERNAL_ERROR, expression->span,
                     "AST contains an unknown expression node");
    return false;
}

bool usk_c_emit_expression(UskCEmitter *emitter,
                           const UskAstExpr *expression) {
    if (!emitter || !expression || emitter->failed) return false;
    if (emitter->options.maximum_expression_depth &&
        emitter->expression_depth >= emitter->options.maximum_expression_depth) {
        usk_c_emit_error(emitter, USK_DIAG_RESOURCE_LIMIT, expression->span,
                         "C backend expression depth limit reached");
        return false;
    }
    emitter->expression_depth++;
    emitter->result->expressions_emitted++;
    bool success = emit_expression_body(emitter, expression);
    emitter->expression_depth--;
    return success && !emitter->failed;
}
