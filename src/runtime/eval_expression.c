#include "evaluator_internal.h"
#include "usk/arraylib.h"
#include "usk/mathlib.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static Value evaluate_binary(UskEvaluator *evaluator, const UskAstExpr *expression,
                             Value left, Value right) {
    const char *operator_text = expression->as.binary.operator;
    if (!strcmp(operator_text, "==")) return bool_value(value_equal(left, right));
    if (!strcmp(operator_text, "!=")) return bool_value(!value_equal(left, right));
    if (!strcmp(operator_text, "&&")) return bool_value(truthy(left) && truthy(right));
    if (!strcmp(operator_text, "||")) return bool_value(truthy(left) || truthy(right));

    if ((!strcmp(operator_text, "<") || !strcmp(operator_text, ">") ||
         !strcmp(operator_text, "<=") || !strcmp(operator_text, ">="))) {
        int ordering = 0;
        if (value_compare(left, right, &ordering) != USK_VALUE_OK) {
            usk_eval_error(evaluator, USK_DIAG_TYPE_MISMATCH, expression->span,
                           "operator '%s' requires two numbers or two strings", operator_text);
            return null_value();
        }
        if (!strcmp(operator_text, "<")) return bool_value(ordering < 0);
        if (!strcmp(operator_text, ">")) return bool_value(ordering > 0);
        if (!strcmp(operator_text, "<=")) return bool_value(ordering <= 0);
        return bool_value(ordering >= 0);
    }

    if (!strcmp(operator_text, "+") &&
        (left.kind == V_STRING || right.kind == V_STRING)) {
        char *left_text = value_string(left);
        char *right_text = value_string(right);
        size_t length = strlen(left_text) + strlen(right_text);
        char *joined = (char *)malloc(length + 1);
        if (!joined) {
            free(left_text);
            free(right_text);
            usk_eval_error(evaluator, USK_DIAG_OUT_OF_MEMORY, expression->span,
                           "cannot allocate concatenated string");
            return null_value();
        }
        strcpy(joined, left_text);
        strcat(joined, right_text);
        Value result = string_value_in(evaluator->value_storage, joined);
        free(joined);
        free(left_text);
        free(right_text);
        return result;
    }

    if (!is_numeric(left) || !is_numeric(right)) {
        usk_eval_error(evaluator, USK_DIAG_TYPE_MISMATCH, expression->span,
                       "operator '%s' requires numeric operands", operator_text);
        return null_value();
    }
    if (!strcmp(operator_text, "%")) {
        if (left.kind != V_INT || right.kind != V_INT) {
            usk_eval_error(evaluator, USK_DIAG_TYPE_MISMATCH, expression->span,
                           "operator '%%' requires integer operands");
            return null_value();
        }
        long long remainder = 0;
        UskMathIntegerStatus status = usk_math_integer_remainder(
            left.as.i, right.as.i, &remainder);
        if (status == USK_MATH_INTEGER_DIVISION_BY_ZERO) {
            usk_eval_error(evaluator, USK_DIAG_INVALID_LITERAL, expression->span,
                           "remainder divisor is zero");
            return null_value();
        }
        return int_value(remainder);
    }
    if (left.kind == V_INT && right.kind == V_INT) {
        long long result = 0;
        UskMathIntegerStatus status;
        if (!strcmp(operator_text, "+"))
            status = usk_math_integer_add(left.as.i, right.as.i, &result);
        else if (!strcmp(operator_text, "-"))
            status = usk_math_integer_subtract(left.as.i, right.as.i, &result);
        else if (!strcmp(operator_text, "*"))
            status = usk_math_integer_multiply(left.as.i, right.as.i, &result);
        else if (!strcmp(operator_text, "/"))
            status = usk_math_integer_divide(left.as.i, right.as.i, &result);
        else {
            usk_eval_error(evaluator, USK_DIAG_UNSUPPORTED_FEATURE,
                           expression->span,
                           "binary operator '%s' is not implemented",
                           operator_text);
            return null_value();
        }
        if (status == USK_MATH_INTEGER_DIVISION_BY_ZERO)
            usk_eval_error(evaluator, USK_DIAG_INVALID_LITERAL,
                           expression->span, "division by zero");
        else if (status == USK_MATH_INTEGER_OVERFLOW)
            usk_eval_error(evaluator, USK_DIAG_NUMERIC_OVERFLOW,
                expression->span, "integer result is outside the USKInt range");
        if (status != USK_MATH_INTEGER_OK) return null_value();
        return int_value(result);
    }
    double left_number = numeric(left), right_number = numeric(right);
    double floating_result = 0.0;
    if (!strcmp(operator_text, "+")) floating_result = left_number + right_number;
    else if (!strcmp(operator_text, "-")) floating_result = left_number - right_number;
    else if (!strcmp(operator_text, "*")) floating_result = left_number * right_number;
    if (!strcmp(operator_text, "/")) {
        if (right_number == 0.0) {
            usk_eval_error(evaluator, USK_DIAG_INVALID_LITERAL, expression->span,
                           "division by zero");
            return null_value();
        }
        floating_result = left_number / right_number;
    } else if (strcmp(operator_text, "+") && strcmp(operator_text, "-") &&
               strcmp(operator_text, "*")) {
        usk_eval_error(evaluator, USK_DIAG_UNSUPPORTED_FEATURE,
                       expression->span,
                       "binary operator '%s' is not implemented", operator_text);
        return null_value();
    }
    if (!isfinite(floating_result)) {
        usk_eval_error(evaluator, USK_DIAG_NUMERIC_OVERFLOW, expression->span,
                       "floating-point result is outside the USKDouble range");
        return null_value();
    }
    return double_value(floating_result);
}

static UskEnvironmentStatus assign_target(UskEvaluator *evaluator,
                                          Environment *environment,
                                          const UskAstExpr *target,
                                          Value value) {
    if (target->kind == USK_EXPR_NAME)
        return assign_var(environment, target->as.name, value);
    if (target->kind == USK_EXPR_INDEX) {
        Value object = usk_eval_expression(evaluator, environment, target->as.index.object);
        Value index = usk_eval_expression(evaluator, environment, target->as.index.index);
        if (object.kind != V_ARRAY || index.kind != V_INT || index.as.i < 0)
            return USK_ENV_ASSIGN_UNDEFINED;
        UskValueStatus status = value_array_set(object, (size_t)index.as.i, value);
        return status == USK_VALUE_OK ? USK_ENV_ASSIGN_OK : USK_ENV_ASSIGN_UNDEFINED;
    }
    return USK_ENV_ASSIGN_UNDEFINED;
}

static Value evaluate_assignment(UskEvaluator *evaluator,
                                Environment *environment,
                                const UskAstExpr *expression) {
    const char *operator_text = expression->as.assignment.operator;
    const UskAstExpr *target = expression->as.assignment.target;
    Value right = usk_eval_expression(evaluator, environment,
                                      expression->as.assignment.value);
    if (strcmp(operator_text, "=")) {
        Value left = usk_eval_expression(evaluator, environment, target);
        const char *binary_operator = operator_text[0] == '+' ? "+" :
            operator_text[0] == '-' ? "-" : operator_text[0] == '*' ? "*" :
            operator_text[0] == '/' ? "/" : "%";
        UskAstExpr binary = {0};
        binary.kind = USK_EXPR_BINARY;
        binary.span = expression->span;
        binary.as.binary.operator = binary_operator;
        right = evaluate_binary(evaluator, &binary, left, right);
    }
    UskEnvironmentStatus status = assign_target(evaluator, environment, target, right);
    if (status == USK_ENV_ASSIGN_UNDEFINED)
        usk_eval_error(evaluator, USK_DIAG_UNKNOWN_NAME, expression->span,
                       "assignment target is not a mutable variable or array element");
    else if (status == USK_ENV_ASSIGN_IMMUTABLE)
        usk_eval_error(evaluator, USK_DIAG_INVALID_DECLARATION, expression->span,
                       "cannot assign to a const variable");
    else if (status == USK_ENV_ASSIGN_TYPE_MISMATCH)
        usk_eval_error(evaluator, USK_DIAG_TYPE_MISMATCH, expression->span,
                       "assigned value does not match the variable's declared type");
    return right;
}

static Value evaluate_member(UskEvaluator *evaluator, Environment *environment,
                             const UskAstExpr *expression) {
    Value object = usk_eval_expression(evaluator, environment,
                                      expression->as.member.object);
    const char *member = expression->as.member.name;
    if (object.kind == V_ARRAY && (!strcmp(member, "length") || !strcmp(member, "size")))
        return int_value((long long)value_array_length(object));
    if (object.kind == V_STRING && (!strcmp(member, "length") || !strcmp(member, "size")))
        return int_value((long long)strlen(object.as.s ? object.as.s : ""));
    usk_eval_error(evaluator, USK_DIAG_UNSUPPORTED_FEATURE, expression->span,
                   "member '%s' is unavailable for this value", member ? member : "<member>");
    return null_value();
}

Value usk_eval_expression(UskEvaluator *evaluator, Environment *environment,
                          const UskAstExpr *expression) {
    if (!expression || evaluator->failed) return null_value();
    switch (expression->kind) {
        case USK_EXPR_NULL: return null_value();
        case USK_EXPR_BOOLEAN: return bool_value(expression->as.boolean);
        case USK_EXPR_INTEGER: return int_value(expression->as.integer);
        case USK_EXPR_DOUBLE: return double_value(expression->as.floating);
        case USK_EXPR_STRING:
            return string_value_in(evaluator->value_storage,
                                   expression->as.string);
        case USK_EXPR_NAME: {
            Variable *variable = lookup_var(environment, expression->as.name);
            if (variable) return variable->value;
            usk_eval_error(evaluator, USK_DIAG_UNKNOWN_NAME, expression->span,
                           "unknown name '%s'", expression->as.name);
            return null_value();
        }
        case USK_EXPR_ARRAY: {
            size_t count = expression->as.array.count;
            if (count > (size_t)-1 / sizeof(Value)) {
                usk_eval_error(evaluator, USK_DIAG_OUT_OF_MEMORY,
                    expression->span, "array literal exceeds runtime limits");
                return null_value();
            }
            Value *items = count ? (Value *)usk_value_arena_allocate(
                evaluator->value_storage, count * sizeof(*items), true) : NULL;
            if (count && !items) {
                usk_eval_error(evaluator, USK_DIAG_OUT_OF_MEMORY, expression->span,
                               "cannot allocate array literal");
                return null_value();
            }
            size_t index = 0;
            for (const UskAstExprList *item = expression->as.array.items;
                 item && index < count; item = item->next)
                items[index++] = usk_eval_expression(evaluator, environment, item->expression);
            return array_value_in(evaluator->value_storage, items, index);
        }
        case USK_EXPR_UNARY: {
            Value operand = usk_eval_expression(evaluator, environment,
                                                expression->as.unary.operand);
            const char *operator_text = expression->as.unary.operator;
            if (!strcmp(operator_text, "!")) return bool_value(!truthy(operand));
            if (!strcmp(operator_text, "+")) return operand;
            if (!strcmp(operator_text, "-")) {
                if (operand.kind == V_DOUBLE) return double_value(-operand.as.d);
                if (operand.kind == V_INT) {
                    long long negated = 0;
                    if (usk_math_integer_negate(operand.as.i, &negated) !=
                        USK_MATH_INTEGER_OK) {
                        usk_eval_error(evaluator, USK_DIAG_NUMERIC_OVERFLOW,
                            expression->span,
                            "negated integer is outside the USKInt range");
                        return null_value();
                    }
                    return int_value(negated);
                }
                usk_eval_error(evaluator, USK_DIAG_TYPE_MISMATCH, expression->span,
                               "unary minus requires a number");
                return null_value();
            }
            if (!strcmp(operator_text, "++") || !strcmp(operator_text, "--") ||
                !strcmp(operator_text, "post++") || !strcmp(operator_text, "post--")) {
                if (expression->as.unary.operand->kind != USK_EXPR_NAME || !is_numeric(operand)) {
                    usk_eval_error(evaluator, USK_DIAG_TYPE_MISMATCH, expression->span,
                                   "increment and decrement require a numeric variable");
                    return null_value();
                }
                bool decrement = strstr(operator_text, "--") != NULL;
                Value updated;
                if (operand.kind == V_DOUBLE) {
                    double next = operand.as.d + (decrement ? -1.0 : 1.0);
                    if (!isfinite(next)) {
                        usk_eval_error(evaluator, USK_DIAG_NUMERIC_OVERFLOW,
                            expression->span,
                            "incremented value is outside the USKDouble range");
                        return null_value();
                    }
                    updated = double_value(next);
                } else {
                    long long next = 0;
                    UskMathIntegerStatus status = decrement
                        ? usk_math_integer_subtract(operand.as.i, 1, &next)
                        : usk_math_integer_add(operand.as.i, 1, &next);
                    if (status != USK_MATH_INTEGER_OK) {
                        usk_eval_error(evaluator, USK_DIAG_NUMERIC_OVERFLOW,
                            expression->span,
                            "incremented integer is outside the USKInt range");
                        return null_value();
                    }
                    updated = int_value(next);
                }
                UskEnvironmentStatus status = assign_var(environment,
                    expression->as.unary.operand->as.name, updated);
                if (status != USK_ENV_ASSIGN_OK) {
                    usk_eval_error(evaluator, USK_DIAG_INVALID_DECLARATION, expression->span,
                                   "increment target is missing, const, or has an incompatible type");
                    return null_value();
                }
                return strncmp(operator_text, "post", 4) == 0 ? operand : updated;
            }
            usk_eval_error(evaluator, USK_DIAG_UNSUPPORTED_FEATURE, expression->span,
                           "unary operator '%s' is not implemented", operator_text);
            return null_value();
        }
        case USK_EXPR_BINARY: {
            Value left = usk_eval_expression(evaluator, environment,
                                             expression->as.binary.left);
            if (!strcmp(expression->as.binary.operator, "&&") && !truthy(left))
                return bool_value(false);
            if (!strcmp(expression->as.binary.operator, "||") && truthy(left))
                return bool_value(true);
            Value right = usk_eval_expression(evaluator, environment,
                                              expression->as.binary.right);
            return evaluate_binary(evaluator, expression, left, right);
        }
        case USK_EXPR_ASSIGNMENT:
            return evaluate_assignment(evaluator, environment, expression);
        case USK_EXPR_CALL: {
            size_t count = expression->as.call.count;
            const UskArrayBuiltinInfo *method =
                expression->as.call.callee &&
                expression->as.call.callee->kind == USK_EXPR_MEMBER
                ? usk_array_method_find(
                    expression->as.call.callee->as.member.name) : NULL;
            size_t receiver_count = method ? 1 : 0;
            if (count > (size_t)-1 - receiver_count) {
                usk_eval_error(evaluator, USK_DIAG_OUT_OF_MEMORY, expression->span,
                               "call argument count overflows runtime limits");
                return null_value();
            }
            Value *arguments = count + receiver_count
                ? (Value *)calloc(count + receiver_count, sizeof(*arguments)) : NULL;
            if (count + receiver_count && !arguments) {
                usk_eval_error(evaluator, USK_DIAG_OUT_OF_MEMORY, expression->span,
                               "cannot allocate call arguments");
                return null_value();
            }
            size_t index = 0;
            if (method) {
                arguments[index++] = usk_eval_expression(evaluator, environment,
                    expression->as.call.callee->as.member.object);
            }
            for (const UskAstExprList *argument = expression->as.call.arguments;
                 argument && index < count + receiver_count;
                 argument = argument->next)
                arguments[index++] = usk_eval_expression(evaluator, environment,
                                                         argument->expression);
            Value result = usk_eval_call(evaluator, environment,
                expression->as.call.callee, arguments, index, expression->span);
            free(arguments);
            return result;
        }
        case USK_EXPR_MEMBER:
            return evaluate_member(evaluator, environment, expression);
        case USK_EXPR_INDEX: {
            Value object = usk_eval_expression(evaluator, environment,
                                               expression->as.index.object);
            Value index = usk_eval_expression(evaluator, environment,
                                              expression->as.index.index);
            if (index.kind != V_INT || index.as.i < 0) {
                usk_eval_error(evaluator, USK_DIAG_TYPE_MISMATCH, expression->span,
                               "array index must be a nonnegative USKInt");
                return null_value();
            }
            if (object.kind == V_ARRAY) {
                Value result;
                if (value_array_get(object, (size_t)index.as.i, &result) == USK_VALUE_OK)
                    return result;
                usk_eval_error(evaluator, USK_DIAG_INVALID_LITERAL, expression->span,
                               "array index is outside the array bounds");
                return null_value();
            }
            if (object.kind == V_STRING) {
                size_t length = strlen(object.as.s ? object.as.s : "");
                if ((size_t)index.as.i < length)
                    return character_value_in(evaluator->value_storage,
                                               object.as.s[index.as.i]);
            }
            usk_eval_error(evaluator, USK_DIAG_TYPE_MISMATCH, expression->span,
                           "indexing requires an array or string and an in-range index");
            return null_value();
        }
        case USK_EXPR_CONDITIONAL:
            return truthy(usk_eval_expression(evaluator, environment,
                    expression->as.conditional.condition))
                ? usk_eval_expression(evaluator, environment,
                    expression->as.conditional.when_true)
                : usk_eval_expression(evaluator, environment,
                    expression->as.conditional.when_false);
    }
    usk_eval_error(evaluator, USK_DIAG_INTERNAL_ERROR, expression->span,
                   "unknown AST expression kind");
    return null_value();
}
