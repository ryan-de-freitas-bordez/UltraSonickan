#include "usk/mathlib.h"

#include <limits.h>

static UskMathIntegerStatus validate_output(long long *result) {
    return result ? USK_MATH_INTEGER_OK : USK_MATH_INTEGER_OVERFLOW;
}

UskMathIntegerStatus usk_math_integer_add(long long left, long long right,
                                          long long *result) {
    if (validate_output(result) != USK_MATH_INTEGER_OK)
        return USK_MATH_INTEGER_OVERFLOW;
    if ((right > 0 && left > LLONG_MAX - right) ||
        (right < 0 && left < LLONG_MIN - right))
        return USK_MATH_INTEGER_OVERFLOW;
    *result = left + right;
    return USK_MATH_INTEGER_OK;
}

UskMathIntegerStatus usk_math_integer_subtract(long long left,
                                                long long right,
                                                long long *result) {
    if (validate_output(result) != USK_MATH_INTEGER_OK)
        return USK_MATH_INTEGER_OVERFLOW;
    if ((right < 0 && left > LLONG_MAX + right) ||
        (right > 0 && left < LLONG_MIN + right))
        return USK_MATH_INTEGER_OVERFLOW;
    *result = left - right;
    return USK_MATH_INTEGER_OK;
}

UskMathIntegerStatus usk_math_integer_multiply(long long left,
                                                long long right,
                                                long long *result) {
    if (validate_output(result) != USK_MATH_INTEGER_OK)
        return USK_MATH_INTEGER_OVERFLOW;
    if (left == 0 || right == 0) {
        *result = 0;
        return USK_MATH_INTEGER_OK;
    }
    if (left == -1 && right == LLONG_MIN) return USK_MATH_INTEGER_OVERFLOW;
    if (right == -1 && left == LLONG_MIN) return USK_MATH_INTEGER_OVERFLOW;
    if (left > 0) {
        if ((right > 0 && left > LLONG_MAX / right) ||
            (right < 0 && right < LLONG_MIN / left))
            return USK_MATH_INTEGER_OVERFLOW;
    } else {
        if ((right > 0 && left < LLONG_MIN / right) ||
            (right < 0 && left < LLONG_MAX / right))
            return USK_MATH_INTEGER_OVERFLOW;
    }
    *result = left * right;
    return USK_MATH_INTEGER_OK;
}

UskMathIntegerStatus usk_math_integer_divide(long long left, long long right,
                                              long long *result) {
    if (validate_output(result) != USK_MATH_INTEGER_OK)
        return USK_MATH_INTEGER_OVERFLOW;
    if (!right) return USK_MATH_INTEGER_DIVISION_BY_ZERO;
    if (left == LLONG_MIN && right == -1)
        return USK_MATH_INTEGER_OVERFLOW;
    *result = left / right;
    return USK_MATH_INTEGER_OK;
}

UskMathIntegerStatus usk_math_integer_remainder(long long left,
                                                 long long right,
                                                 long long *result) {
    if (validate_output(result) != USK_MATH_INTEGER_OK)
        return USK_MATH_INTEGER_OVERFLOW;
    if (!right) return USK_MATH_INTEGER_DIVISION_BY_ZERO;
    if (left == LLONG_MIN && right == -1) {
        *result = 0;
        return USK_MATH_INTEGER_OK;
    }
    *result = left % right;
    return USK_MATH_INTEGER_OK;
}

UskMathIntegerStatus usk_math_integer_negate(long long value,
                                              long long *result) {
    if (validate_output(result) != USK_MATH_INTEGER_OK)
        return USK_MATH_INTEGER_OVERFLOW;
    if (value == LLONG_MIN) return USK_MATH_INTEGER_OVERFLOW;
    *result = -value;
    return USK_MATH_INTEGER_OK;
}

UskMathIntegerStatus usk_math_integer_absolute(long long value,
                                                long long *result) {
    if (value >= 0) {
        if (!result) return USK_MATH_INTEGER_OVERFLOW;
        *result = value;
        return USK_MATH_INTEGER_OK;
    }
    return usk_math_integer_negate(value, result);
}

UskMathIntegerStatus usk_math_integer_power(long long base,
                                             long long exponent,
                                             long long *result) {
    if (!result) return USK_MATH_INTEGER_OVERFLOW;
    if (exponent < 0) return USK_MATH_INTEGER_INVALID_EXPONENT;
    long long accumulated = 1;
    long long factor = base;
    unsigned long long remaining = (unsigned long long)exponent;
    while (remaining) {
        if (remaining & 1u) {
            UskMathIntegerStatus status = usk_math_integer_multiply(
                accumulated, factor, &accumulated);
            if (status != USK_MATH_INTEGER_OK) return status;
        }
        remaining >>= 1;
        if (remaining) {
            UskMathIntegerStatus status = usk_math_integer_multiply(
                factor, factor, &factor);
            if (status != USK_MATH_INTEGER_OK) return status;
        }
    }
    *result = accumulated;
    return USK_MATH_INTEGER_OK;
}

const char *usk_math_integer_status_name(UskMathIntegerStatus status) {
    switch (status) {
        case USK_MATH_INTEGER_OK: return "ok";
        case USK_MATH_INTEGER_DIVISION_BY_ZERO: return "division-by-zero";
        case USK_MATH_INTEGER_OVERFLOW: return "overflow";
        case USK_MATH_INTEGER_INVALID_EXPONENT: return "invalid-exponent";
    }
    return "unknown-integer-math-status";
}
