#include "usk/mathlib.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <string.h>

static const UskMathBuiltinInfo builtins[] = {
    {"math::sin", 1, USK_MATH_RESULT_NUMBER, {USK_MATH_ARGUMENT_NUMBER}},
    {"math::cos", 1, USK_MATH_RESULT_NUMBER, {USK_MATH_ARGUMENT_NUMBER}},
    {"math::tan", 1, USK_MATH_RESULT_NUMBER, {USK_MATH_ARGUMENT_NUMBER}},
    {"math::asin", 1, USK_MATH_RESULT_NUMBER, {USK_MATH_ARGUMENT_NUMBER}},
    {"math::acos", 1, USK_MATH_RESULT_NUMBER, {USK_MATH_ARGUMENT_NUMBER}},
    {"math::atan", 1, USK_MATH_RESULT_NUMBER, {USK_MATH_ARGUMENT_NUMBER}},
    {"math::atan2", 2, USK_MATH_RESULT_NUMBER,
        {USK_MATH_ARGUMENT_NUMBER, USK_MATH_ARGUMENT_NUMBER}},
    {"math::exp", 1, USK_MATH_RESULT_NUMBER, {USK_MATH_ARGUMENT_NUMBER}},
    {"math::log", 1, USK_MATH_RESULT_NUMBER, {USK_MATH_ARGUMENT_NUMBER}},
    {"math::log10", 1, USK_MATH_RESULT_NUMBER, {USK_MATH_ARGUMENT_NUMBER}},
    {"math::pow", 2, USK_MATH_RESULT_NUMBER,
        {USK_MATH_ARGUMENT_NUMBER, USK_MATH_ARGUMENT_NUMBER}},
    {"math::sqrt", 1, USK_MATH_RESULT_NUMBER, {USK_MATH_ARGUMENT_NUMBER}},
    {"math::floor", 1, USK_MATH_RESULT_NUMBER, {USK_MATH_ARGUMENT_NUMBER}},
    {"math::ceil", 1, USK_MATH_RESULT_NUMBER, {USK_MATH_ARGUMENT_NUMBER}},
    {"math::round", 1, USK_MATH_RESULT_NUMBER, {USK_MATH_ARGUMENT_NUMBER}},
    {"math::abs", 1, USK_MATH_RESULT_PRESERVE_NUMERIC,
        {USK_MATH_ARGUMENT_NUMBER}},
    {"math::min", 2, USK_MATH_RESULT_PRESERVE_NUMERIC,
        {USK_MATH_ARGUMENT_NUMBER, USK_MATH_ARGUMENT_NUMBER}},
    {"math::max", 2, USK_MATH_RESULT_PRESERVE_NUMERIC,
        {USK_MATH_ARGUMENT_NUMBER, USK_MATH_ARGUMENT_NUMBER}},
    {"math::clamp", 3, USK_MATH_RESULT_PRESERVE_NUMERIC,
        {USK_MATH_ARGUMENT_NUMBER, USK_MATH_ARGUMENT_NUMBER,
         USK_MATH_ARGUMENT_NUMBER}},
    {"math::lerp", 3, USK_MATH_RESULT_NUMBER,
        {USK_MATH_ARGUMENT_NUMBER, USK_MATH_ARGUMENT_NUMBER,
         USK_MATH_ARGUMENT_NUMBER}},
    {"math::is_finite", 1, USK_MATH_RESULT_BOOLEAN,
        {USK_MATH_ARGUMENT_NUMBER}},
    {"math::pi", 0, USK_MATH_RESULT_NUMBER, {0}},
    {"math::e", 0, USK_MATH_RESULT_NUMBER, {0}}
};

size_t usk_math_builtin_count(void) {
    return sizeof(builtins) / sizeof(builtins[0]);
}

const UskMathBuiltinInfo *usk_math_builtin_at(size_t index) {
    if (index >= usk_math_builtin_count()) return NULL;
    return &builtins[index];
}

const UskMathBuiltinInfo *usk_math_builtin_find(const char *name) {
    if (!name) return NULL;
    for (size_t index = 0; index < usk_math_builtin_count(); ++index)
        if (!strcmp(name, builtins[index].name)) return &builtins[index];
    return NULL;
}

bool usk_math_builtin_is_name(const char *name) {
    return usk_math_builtin_find(name) != NULL;
}

bool usk_math_value_is_finite(Value value) {
    return is_numeric(value) && isfinite(numeric(value));
}

bool usk_math_value_as_number(Value value, double *result) {
    if (!result || !is_numeric(value)) return false;
    *result = numeric(value);
    return true;
}

bool usk_math_value_as_integer(Value value, long long *result) {
    if (!result || value.kind != V_INT) return false;
    *result = value.as.i;
    return true;
}

int usk_math_compare(Value left, Value right) {
    if (left.kind == V_INT && right.kind == V_INT)
        return left.as.i < right.as.i ? -1 : left.as.i > right.as.i ? 1 : 0;
    double a = numeric(left), b = numeric(right);
    return a < b ? -1 : a > b ? 1 : 0;
}

Value usk_math_minimum(Value left, Value right) {
    return usk_math_compare(left, right) <= 0 ? left : right;
}

Value usk_math_maximum(Value left, Value right) {
    return usk_math_compare(left, right) >= 0 ? left : right;
}

static UskMathBuiltinStatus number_at(const Value *arguments, size_t count,
                                      size_t index, double *result) {
    if (!arguments || index >= count || !result)
        return USK_MATH_BUILTIN_TYPE_MISMATCH;
    if (!is_numeric(arguments[index])) return USK_MATH_BUILTIN_TYPE_MISMATCH;
    *result = numeric(arguments[index]);
    return USK_MATH_BUILTIN_OK;
}

static UskMathBuiltinStatus number_result(double number, Value *result) {
    if (!isfinite(number)) return USK_MATH_BUILTIN_RANGE_ERROR;
    *result = double_value(number);
    return USK_MATH_BUILTIN_OK;
}

static UskMathBuiltinStatus preserve_minmax(const Value *arguments,
    const char *name, Value *result) {
    if (arguments[0].kind == V_INT && arguments[1].kind == V_INT) {
        *result = !strcmp(name, "math::min")
            ? int_value(arguments[0].as.i < arguments[1].as.i
                ? arguments[0].as.i : arguments[1].as.i)
            : int_value(arguments[0].as.i > arguments[1].as.i
                ? arguments[0].as.i : arguments[1].as.i);
        return USK_MATH_BUILTIN_OK;
    }
    double left = numeric(arguments[0]), right = numeric(arguments[1]);
    return number_result(!strcmp(name, "math::min")
        ? fmin(left, right) : fmax(left, right), result);
}

UskMathBuiltinStatus usk_math_builtin_call(
    const char *name, const Value *arguments, size_t argument_count,
    Value *result) {
    const UskMathBuiltinInfo *info = usk_math_builtin_find(name);
    if (!info) return USK_MATH_BUILTIN_UNKNOWN;
    if (!result || (argument_count && !arguments))
        return USK_MATH_BUILTIN_TYPE_MISMATCH;
    if (argument_count != info->argument_count)
        return USK_MATH_BUILTIN_ARGUMENT_COUNT;
    for (size_t index = 0; index < argument_count; ++index)
        if (!is_numeric(arguments[index]))
            return USK_MATH_BUILTIN_TYPE_MISMATCH;

    double first = 0.0, second = 0.0, third = 0.0;
    if (argument_count && number_at(arguments, argument_count, 0, &first) !=
            USK_MATH_BUILTIN_OK)
        return USK_MATH_BUILTIN_TYPE_MISMATCH;
    if (argument_count > 1 && number_at(arguments, argument_count, 1, &second) !=
            USK_MATH_BUILTIN_OK)
        return USK_MATH_BUILTIN_TYPE_MISMATCH;
    if (argument_count > 2 && number_at(arguments, argument_count, 2, &third) !=
            USK_MATH_BUILTIN_OK)
        return USK_MATH_BUILTIN_TYPE_MISMATCH;

    *result = null_value();
    if (!strcmp(name, "math::pi"))
        return number_result(3.14159265358979323846264338327950288, result);
    if (!strcmp(name, "math::e"))
        return number_result(2.71828182845904523536028747135266250, result);
    if (!strcmp(name, "math::is_finite")) {
        *result = bool_value(isfinite(first));
        return USK_MATH_BUILTIN_OK;
    }
    if (!strcmp(name, "math::min") || !strcmp(name, "math::max"))
        return preserve_minmax(arguments, name, result);
    if (!strcmp(name, "math::clamp")) {
        if (usk_math_compare(arguments[1], arguments[2]) > 0)
            return USK_MATH_BUILTIN_DOMAIN_ERROR;
        if (arguments[0].kind == V_INT && arguments[1].kind == V_INT &&
            arguments[2].kind == V_INT) {
            long long value = arguments[0].as.i;
            if (value < arguments[1].as.i) value = arguments[1].as.i;
            if (value > arguments[2].as.i) value = arguments[2].as.i;
            *result = int_value(value);
            return USK_MATH_BUILTIN_OK;
        }
        double value = first < second ? second : first;
        if (value > third) value = third;
        return number_result(value, result);
    }
    if (!strcmp(name, "math::lerp"))
        return number_result(first + (second - first) * third, result);
    if (!strcmp(name, "math::abs")) {
        if (arguments[0].kind == V_INT) {
            if (arguments[0].as.i == LLONG_MIN)
                return USK_MATH_BUILTIN_RANGE_ERROR;
            *result = int_value(arguments[0].as.i < 0
                ? -arguments[0].as.i : arguments[0].as.i);
            return USK_MATH_BUILTIN_OK;
        }
        return number_result(fabs(first), result);
    }

    errno = 0;
    double value = 0.0;
    if (!strcmp(name, "math::sin")) value = sin(first);
    else if (!strcmp(name, "math::cos")) value = cos(first);
    else if (!strcmp(name, "math::tan")) value = tan(first);
    else if (!strcmp(name, "math::asin")) {
        if (first < -1.0 || first > 1.0) return USK_MATH_BUILTIN_DOMAIN_ERROR;
        value = asin(first);
    } else if (!strcmp(name, "math::acos")) {
        if (first < -1.0 || first > 1.0) return USK_MATH_BUILTIN_DOMAIN_ERROR;
        value = acos(first);
    } else if (!strcmp(name, "math::atan")) value = atan(first);
    else if (!strcmp(name, "math::atan2")) value = atan2(first, second);
    else if (!strcmp(name, "math::exp")) value = exp(first);
    else if (!strcmp(name, "math::log")) {
        if (first <= 0.0) return USK_MATH_BUILTIN_DOMAIN_ERROR;
        value = log(first);
    } else if (!strcmp(name, "math::log10")) {
        if (first <= 0.0) return USK_MATH_BUILTIN_DOMAIN_ERROR;
        value = log10(first);
    } else if (!strcmp(name, "math::pow")) value = pow(first, second);
    else if (!strcmp(name, "math::sqrt")) {
        if (first < 0.0) return USK_MATH_BUILTIN_DOMAIN_ERROR;
        value = sqrt(first);
    } else if (!strcmp(name, "math::floor")) value = floor(first);
    else if (!strcmp(name, "math::ceil")) value = ceil(first);
    else if (!strcmp(name, "math::round")) value = round(first);
    else return USK_MATH_BUILTIN_UNKNOWN;
    if (errno == EDOM) return USK_MATH_BUILTIN_DOMAIN_ERROR;
    if (errno == ERANGE) return USK_MATH_BUILTIN_RANGE_ERROR;
    return number_result(value, result);
}

const char *usk_math_builtin_status_name(UskMathBuiltinStatus status) {
    switch (status) {
        case USK_MATH_BUILTIN_OK: return "ok";
        case USK_MATH_BUILTIN_UNKNOWN: return "not a math built-in";
        case USK_MATH_BUILTIN_ARGUMENT_COUNT: return "wrong argument count";
        case USK_MATH_BUILTIN_TYPE_MISMATCH: return "numeric argument required";
        case USK_MATH_BUILTIN_DOMAIN_ERROR: return "value is outside the function domain";
        case USK_MATH_BUILTIN_RANGE_ERROR: return "numeric result is out of range";
    }
    return "unknown math built-in status";
}
