#include "usk/bytecode.h"
#include "usk/arraylib.h"
#include "usk/filesystem.h"
#include "usk/mathlib.h"
#include "usk/pathlib.h"
#include "usk/stringlib.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t maximum_steps;
    size_t maximum_call_depth;
    size_t instructions_executed;
} VmBudget;

typedef struct {
    const UskBytecodeChunk *chunk;
    UskValueArena *value_storage;
    const UskBytecodeHost *host;
    UskBytecodeVmOptions options;
    Value *stack;
    size_t stack_count;
    size_t stack_capacity;
    Value *locals;
    size_t instruction;
    size_t call_depth;
    VmBudget *budget;
    UskBytecodeStatus status;
} VmState;

static UskBytecodeVmResult execute_chunk_internal(
    const UskBytecodeChunk *chunk, const Value *arguments,
    size_t argument_count, UskValueArena *value_storage,
    const UskBytecodeHost *host, const UskBytecodeVmOptions *options,
    VmBudget *budget, size_t call_depth);

static bool stack_push(VmState *vm, Value value) {
    if (vm->stack_count >= vm->stack_capacity) {
        vm->status = USK_BYTECODE_LIMIT_EXCEEDED;
        return false;
    }
    vm->stack[vm->stack_count++] = value;
    return true;
}

static bool stack_pop(VmState *vm, Value *value) {
    if (!vm->stack_count) {
        vm->status = USK_BYTECODE_STACK_UNDERFLOW;
        return false;
    }
    *value = vm->stack[--vm->stack_count];
    return true;
}

static bool stack_peek(VmState *vm, size_t distance, Value *value) {
    if (distance >= vm->stack_count) {
        vm->status = USK_BYTECODE_STACK_UNDERFLOW;
        return false;
    }
    *value = vm->stack[vm->stack_count - distance - 1];
    return true;
}

static bool string_concatenate(VmState *vm, Value left, Value right,
                               Value *result) {
    char *left_text = value_string(left);
    char *right_text = value_string(right);
    if (!left_text || !right_text) {
        free(left_text);
        free(right_text);
        vm->status = USK_BYTECODE_ALLOCATION_FAILURE;
        return false;
    }
    size_t left_length = strlen(left_text), right_length = strlen(right_text);
    if (left_length > (size_t)-1 - right_length - 1) {
        free(left_text);
        free(right_text);
        vm->status = USK_BYTECODE_LIMIT_EXCEEDED;
        return false;
    }
    char *joined = (char *)malloc(left_length + right_length + 1);
    if (!joined) {
        free(left_text);
        free(right_text);
        vm->status = USK_BYTECODE_ALLOCATION_FAILURE;
        return false;
    }
    memcpy(joined, left_text, left_length);
    memcpy(joined + left_length, right_text, right_length + 1);
    *result = string_value_in(vm->value_storage, joined);
    free(joined);
    free(left_text);
    free(right_text);
    if (vm->value_storage->failed) {
        vm->status = USK_BYTECODE_ALLOCATION_FAILURE;
        return false;
    }
    return true;
}

static bool execute_arithmetic(VmState *vm, UskBytecodeOpcode opcode,
                               Value left, Value right, Value *result) {
    if (opcode == USK_BC_ADD &&
        (left.kind == V_STRING || right.kind == V_STRING))
        return string_concatenate(vm, left, right, result);
    if (!is_numeric(left) || !is_numeric(right)) {
        vm->status = USK_BYTECODE_TYPE_MISMATCH;
        return false;
    }
    bool floating = left.kind == V_DOUBLE || right.kind == V_DOUBLE;
    if (floating) {
        double a = numeric(left), b = numeric(right), value = 0.0;
        switch (opcode) {
            case USK_BC_ADD: value = a + b; break;
            case USK_BC_SUBTRACT: value = a - b; break;
            case USK_BC_MULTIPLY: value = a * b; break;
            case USK_BC_DIVIDE:
                if (b == 0.0) { vm->status = USK_BYTECODE_DIVISION_BY_ZERO; return false; }
                value = a / b;
                break;
            default: vm->status = USK_BYTECODE_TYPE_MISMATCH; return false;
        }
        if (!isfinite(value)) {
            vm->status = USK_BYTECODE_LIMIT_EXCEEDED;
            return false;
        }
        *result = double_value(value);
        return true;
    }
    long long value = 0;
    UskMathIntegerStatus status;
    long long a = left.as.i, b = right.as.i;
    switch (opcode) {
        case USK_BC_ADD: status = usk_math_integer_add(a, b, &value); break;
        case USK_BC_SUBTRACT:
            status = usk_math_integer_subtract(a, b, &value); break;
        case USK_BC_MULTIPLY:
            status = usk_math_integer_multiply(a, b, &value); break;
        case USK_BC_DIVIDE:
            status = usk_math_integer_divide(a, b, &value); break;
        case USK_BC_MODULO:
            status = usk_math_integer_remainder(a, b, &value); break;
        default: vm->status = USK_BYTECODE_TYPE_MISMATCH; return false;
    }
    if (status == USK_MATH_INTEGER_DIVISION_BY_ZERO) {
        vm->status = USK_BYTECODE_DIVISION_BY_ZERO;
        return false;
    }
    if (status != USK_MATH_INTEGER_OK) {
        vm->status = opcode == USK_BC_MODULO
            ? USK_BYTECODE_TYPE_MISMATCH : USK_BYTECODE_NUMERIC_RANGE;
        return false;
    }
    *result = int_value(value);
    return true;
}

static bool write_value(FILE *output, Value value) {
    char *text = value_string(value);
    if (!text) return false;
    bool success = fputs(text, output) >= 0;
    free(text);
    return success;
}

static bool builtin_writef(FILE *output, const Value *arguments,
                           size_t argument_count) {
    if (!argument_count || arguments[0].kind != V_STRING) return true;
    const char *format = arguments[0].as.s ? arguments[0].as.s : "";
    size_t argument = 1;
    for (size_t index = 0; format[index];) {
        if (format[index] == '{' && format[index + 1] == '{') {
            fputc('{', output);
            index += 2;
            continue;
        }
        if (format[index] == '}' && format[index + 1] == '}') {
            fputc('}', output);
            index += 2;
            continue;
        }
        if (format[index] == '{') {
            size_t close = index + 1;
            while (format[close] && format[close] != '}') close++;
            if (format[close] == '}') {
                if (argument < argument_count) {
                    if (!write_value(output, arguments[argument++])) return false;
                } else {
                    for (size_t literal = index; literal <= close; ++literal)
                        fputc(format[literal], output);
                }
                index = close + 1;
                continue;
            }
        }
        fputc(format[index++], output);
    }
    return !ferror(output);
}

static bool builtin_call(VmState *vm, const char *name, const Value *arguments,
                         size_t argument_count, Value *result, bool *handled) {
    FILE *output = vm->options.output ? vm->options.output : stdout;
    *handled = true;
    *result = null_value();
    if (usk_fs_builtin_is_name(name)) {
        UskFsStatus status = usk_fs_builtin_call(name, arguments,
            argument_count, vm->value_storage, result);
        if (status == USK_FS_OK) return true;
        switch (status) {
            case USK_FS_ARGUMENT_COUNT:
                vm->status = USK_BYTECODE_INVALID_ARGUMENT;
                break;
            case USK_FS_TYPE_MISMATCH:
                vm->status = USK_BYTECODE_TYPE_MISMATCH;
                break;
            case USK_FS_IO_FAILURE:
                vm->status = USK_BYTECODE_IO_FAILURE;
                break;
            case USK_FS_RANGE_ERROR:
                vm->status = USK_BYTECODE_INDEX_OUT_OF_RANGE;
                break;
            case USK_FS_ALLOCATION_FAILURE:
                vm->status = USK_BYTECODE_ALLOCATION_FAILURE;
                break;
            case USK_FS_UNKNOWN:
            case USK_FS_OK:
                vm->status = USK_BYTECODE_UNKNOWN_FUNCTION;
                break;
        }
        return false;
    }
    if (usk_path_builtin_is_name(name)) {
        UskPathBuiltinStatus status = usk_path_builtin_call(name, arguments,
            argument_count, vm->value_storage, result);
        if (status == USK_PATH_BUILTIN_OK) return true;
        switch (status) {
            case USK_PATH_BUILTIN_ARGUMENT_COUNT:
                vm->status = USK_BYTECODE_INVALID_ARGUMENT;
                break;
            case USK_PATH_BUILTIN_TYPE_MISMATCH:
                vm->status = USK_BYTECODE_TYPE_MISMATCH;
                break;
            case USK_PATH_BUILTIN_RANGE_ERROR:
                vm->status = USK_BYTECODE_INDEX_OUT_OF_RANGE;
                break;
            case USK_PATH_BUILTIN_ALLOCATION_FAILURE:
                vm->status = USK_BYTECODE_ALLOCATION_FAILURE;
                break;
            case USK_PATH_BUILTIN_UNKNOWN:
            case USK_PATH_BUILTIN_OK:
                vm->status = USK_BYTECODE_UNKNOWN_FUNCTION;
                break;
        }
        return false;
    }
    if (usk_array_builtin_is_name(name)) {
        UskArrayBuiltinStatus status = usk_array_builtin_call(name, arguments,
            argument_count, vm->value_storage, result);
        if (status == USK_ARRAY_BUILTIN_OK) return true;
        switch (status) {
            case USK_ARRAY_BUILTIN_ARGUMENT_COUNT:
                vm->status = USK_BYTECODE_INVALID_ARGUMENT;
                break;
            case USK_ARRAY_BUILTIN_TYPE_MISMATCH:
                vm->status = USK_BYTECODE_TYPE_MISMATCH;
                break;
            case USK_ARRAY_BUILTIN_RANGE_ERROR:
                vm->status = USK_BYTECODE_INDEX_OUT_OF_RANGE;
                break;
            case USK_ARRAY_BUILTIN_ALLOCATION_FAILURE:
                vm->status = USK_BYTECODE_ALLOCATION_FAILURE;
                break;
            case USK_ARRAY_BUILTIN_UNKNOWN:
            case USK_ARRAY_BUILTIN_OK:
                vm->status = USK_BYTECODE_UNKNOWN_FUNCTION;
                break;
        }
        return false;
    }
    if (usk_math_builtin_is_name(name)) {
        UskMathBuiltinStatus status = usk_math_builtin_call(name, arguments,
            argument_count, result);
        if (status == USK_MATH_BUILTIN_OK) return true;
        switch (status) {
            case USK_MATH_BUILTIN_ARGUMENT_COUNT:
                vm->status = USK_BYTECODE_INVALID_ARGUMENT;
                break;
            case USK_MATH_BUILTIN_TYPE_MISMATCH:
                vm->status = USK_BYTECODE_TYPE_MISMATCH;
                break;
            case USK_MATH_BUILTIN_DOMAIN_ERROR:
                vm->status = USK_BYTECODE_NUMERIC_DOMAIN;
                break;
            case USK_MATH_BUILTIN_RANGE_ERROR:
                vm->status = USK_BYTECODE_NUMERIC_RANGE;
                break;
            case USK_MATH_BUILTIN_OK:
                break;
            case USK_MATH_BUILTIN_UNKNOWN:
                vm->status = USK_BYTECODE_UNKNOWN_FUNCTION;
                break;
        }
        return false;
    }
    if (usk_string_builtin_is_name(name)) {
        UskStringBuiltinStatus status = usk_string_builtin_call(name,
            arguments, argument_count, vm->value_storage, result);
        if (status == USK_STRING_BUILTIN_OK) return true;
        switch (status) {
            case USK_STRING_BUILTIN_ARGUMENT_COUNT:
                vm->status = USK_BYTECODE_INVALID_ARGUMENT;
                break;
            case USK_STRING_BUILTIN_TYPE_MISMATCH:
                vm->status = USK_BYTECODE_TYPE_MISMATCH;
                break;
            case USK_STRING_BUILTIN_RANGE_ERROR:
                vm->status = USK_BYTECODE_INDEX_OUT_OF_RANGE;
                break;
            case USK_STRING_BUILTIN_ALLOCATION_FAILURE:
                vm->status = USK_BYTECODE_ALLOCATION_FAILURE;
                break;
            case USK_STRING_BUILTIN_OK:
                break;
            case USK_STRING_BUILTIN_UNKNOWN:
                vm->status = USK_BYTECODE_UNKNOWN_FUNCTION;
                break;
        }
        return false;
    }
    if (!strcmp(name, "io::writef") || !strcmp(name, "iostream::io::writef") ||
        !strcmp(name, "writef")) {
        if (!builtin_writef(output, arguments, argument_count))
            vm->status = USK_BYTECODE_OUTPUT_FAILURE;
        return vm->status == USK_BYTECODE_OK;
    }
    if (!strcmp(name, "io::write") || !strcmp(name, "iostream::io::write") ||
        !strcmp(name, "print") || !strcmp(name, "io::writeln") ||
        !strcmp(name, "iostream::io::writeln") || !strcmp(name, "println")) {
        for (size_t index = 0; index < argument_count; ++index)
            if (!write_value(output, arguments[index])) {
                vm->status = USK_BYTECODE_OUTPUT_FAILURE;
                return false;
            }
        if (!strcmp(name, "io::writeln") ||
            !strcmp(name, "iostream::io::writeln") || !strcmp(name, "println"))
            fputc('\n', output);
        if (ferror(output)) {
            vm->status = USK_BYTECODE_OUTPUT_FAILURE;
            return false;
        }
        return true;
    }
    if (!strcmp(name, "typeof")) {
        if (argument_count != 1) {
            vm->status = USK_BYTECODE_INVALID_ARGUMENT;
            return false;
        }
        *result = string_value_in(vm->value_storage, type_name(arguments[0]));
        if (vm->value_storage->failed)
            vm->status = USK_BYTECODE_ALLOCATION_FAILURE;
        return vm->status == USK_BYTECODE_OK;
    }
    if (!strcmp(name, "io::readline") ||
        !strcmp(name, "iostream::io::readline") || !strcmp(name, "readline")) {
        if (argument_count || !vm->options.allow_standard_input) {
            vm->status = USK_BYTECODE_INPUT_FAILURE;
            return false;
        }
        FILE *input = vm->options.input ? vm->options.input : stdin;
        char line[8192];
        if (!fgets(line, sizeof(line), input)) {
            if (ferror(input)) {
                vm->status = USK_BYTECODE_INPUT_FAILURE;
                return false;
            }
            line[0] = '\0';
        }
        line[strcspn(line, "\r\n")] = '\0';
        *result = string_value_in(vm->value_storage, line);
        if (vm->value_storage->failed)
            vm->status = USK_BYTECODE_ALLOCATION_FAILURE;
        return vm->status == USK_BYTECODE_OK;
    }
    *handled = false;
    return true;
}

static bool execute_call(VmState *vm, int32_t argument_count) {
    size_t count = (size_t)argument_count;
    if (count + 1 > vm->stack_count) {
        vm->status = USK_BYTECODE_STACK_UNDERFLOW;
        return false;
    }
    size_t base = vm->stack_count - count - 1;
    Value callee = vm->stack[base];
    if (callee.kind != V_STRING || !callee.as.s) {
        vm->status = USK_BYTECODE_TYPE_MISMATCH;
        return false;
    }
    const Value *arguments = count ? &vm->stack[base + 1] : NULL;
    Value result = null_value();
    bool handled = false;
    if (!builtin_call(vm, callee.as.s, arguments, count, &result, &handled))
        return false;
    if (!handled) {
        const UskBytecodeChunk *function = vm->host &&
            vm->host->resolve_function
            ? vm->host->resolve_function(vm->host->context, callee.as.s)
            : NULL;
        if (function) {
            if (vm->call_depth >= vm->budget->maximum_call_depth) {
                vm->status = USK_BYTECODE_CALL_DEPTH_LIMIT;
                return false;
            }
            UskBytecodeVmResult call_result = execute_chunk_internal(
                function, arguments, count, vm->value_storage, vm->host,
                &vm->options, vm->budget, vm->call_depth + 1);
            if (!call_result.completed) {
                vm->status = call_result.status;
                return false;
            }
            result = call_result.return_value;
        } else if (!vm->host || !vm->host->call ||
                   !vm->host->call(vm->host->context, callee.as.s,
                                   arguments, count, &result)) {
            vm->status = USK_BYTECODE_UNKNOWN_FUNCTION;
            return false;
        }
    }
    vm->stack_count = base;
    return stack_push(vm, result);
}

static bool execute_comparison(VmState *vm, UskBytecodeOpcode opcode,
                               Value left, Value right, Value *result) {
    if (opcode == USK_BC_EQUAL) {
        *result = bool_value(value_equal(left, right));
        return true;
    }
    if (opcode == USK_BC_NOT_EQUAL) {
        *result = bool_value(!value_equal(left, right));
        return true;
    }
    int ordering = 0;
    if (value_compare(left, right, &ordering) != USK_VALUE_OK) {
        vm->status = USK_BYTECODE_TYPE_MISMATCH;
        return false;
    }
    switch (opcode) {
        case USK_BC_LESS: *result = bool_value(ordering < 0); break;
        case USK_BC_LESS_EQUAL: *result = bool_value(ordering <= 0); break;
        case USK_BC_GREATER: *result = bool_value(ordering > 0); break;
        case USK_BC_GREATER_EQUAL: *result = bool_value(ordering >= 0); break;
        default: vm->status = USK_BYTECODE_INVALID_INSTRUCTION; return false;
    }
    return true;
}

static bool execute_instruction(VmState *vm, const UskBytecodeInstruction *item,
                                size_t *next_instruction,
                                Value *return_value, bool *finished) {
    Value left, right, result;
    *next_instruction = vm->instruction + 1;
    *finished = false;
    switch (item->opcode) {
        case USK_BC_CONSTANT:
            if (!usk_bytecode_get_constant(vm->chunk, (uint32_t)item->operand,
                                           &result)) {
                vm->status = USK_BYTECODE_INVALID_CONSTANT;
                return false;
            }
            return stack_push(vm, result);
        case USK_BC_NULL: return stack_push(vm, null_value());
        case USK_BC_TRUE: return stack_push(vm, bool_value(true));
        case USK_BC_FALSE: return stack_push(vm, bool_value(false));
        case USK_BC_POP: return stack_pop(vm, &left);
        case USK_BC_DUPLICATE:
            return stack_peek(vm, 0, &left) && stack_push(vm, left);
        case USK_BC_LOAD_LOCAL:
            return stack_push(vm, vm->locals[item->operand]);
        case USK_BC_STORE_LOCAL:
            return stack_pop(vm, &vm->locals[item->operand]);
        case USK_BC_LOAD_GLOBAL: {
            Value name;
            if (!usk_bytecode_get_constant(vm->chunk, (uint32_t)item->operand,
                                           &name) || name.kind != V_STRING) {
                vm->status = USK_BYTECODE_INVALID_CONSTANT;
                return false;
            }
            if (!vm->host || !vm->host->load_global ||
                !vm->host->load_global(vm->host->context, name.as.s, &result)) {
                vm->status = USK_BYTECODE_UNKNOWN_GLOBAL;
                return false;
            }
            return stack_push(vm, result);
        }
        case USK_BC_STORE_GLOBAL: {
            Value name;
            if (!stack_pop(vm, &result)) return false;
            if (!usk_bytecode_get_constant(vm->chunk, (uint32_t)item->operand,
                                           &name) || name.kind != V_STRING) {
                vm->status = USK_BYTECODE_INVALID_CONSTANT;
                return false;
            }
            if (!vm->host || !vm->host->store_global ||
                !vm->host->store_global(vm->host->context, name.as.s, result)) {
                vm->status = USK_BYTECODE_UNKNOWN_GLOBAL;
                return false;
            }
            return true;
        }
        case USK_BC_ADD:
        case USK_BC_SUBTRACT:
        case USK_BC_MULTIPLY:
        case USK_BC_DIVIDE:
        case USK_BC_MODULO:
            if (!stack_pop(vm, &right) || !stack_pop(vm, &left) ||
                !execute_arithmetic(vm, item->opcode, left, right, &result))
                return false;
            return stack_push(vm, result);
        case USK_BC_NEGATE:
            if (!stack_pop(vm, &left)) return false;
            if (left.kind == V_DOUBLE) result = double_value(-left.as.d);
            else if (left.kind == V_INT) {
                long long negated = 0;
                if (usk_math_integer_negate(left.as.i, &negated) !=
                    USK_MATH_INTEGER_OK) {
                    vm->status = USK_BYTECODE_NUMERIC_RANGE;
                    return false;
                }
                result = int_value(negated);
            }
            else {
                vm->status = left.kind == V_INT
                    ? USK_BYTECODE_LIMIT_EXCEEDED : USK_BYTECODE_TYPE_MISMATCH;
                return false;
            }
            return stack_push(vm, result);
        case USK_BC_NOT:
            if (!stack_pop(vm, &left)) return false;
            return stack_push(vm, bool_value(!truthy(left)));
        case USK_BC_BUILD_ARRAY: {
            if (item->operand < 0) {
                vm->status = USK_BYTECODE_INVALID_INSTRUCTION;
                return false;
            }
            size_t count = (size_t)item->operand;
            if (count > vm->stack_count ||
                count > (size_t)-1 / sizeof(Value)) {
                vm->status = USK_BYTECODE_STACK_UNDERFLOW;
                return false;
            }
            size_t base = vm->stack_count - count;
            Value *items = count ? (Value *)usk_value_arena_allocate(
                vm->value_storage, count * sizeof(*items), false) : NULL;
            if (count && !items) {
                vm->status = USK_BYTECODE_ALLOCATION_FAILURE;
                return false;
            }
            if (count) memcpy(items, &vm->stack[base], count * sizeof(*items));
            Value array = array_value_in(vm->value_storage, items, count);
            if (vm->value_storage->failed) {
                vm->status = USK_BYTECODE_ALLOCATION_FAILURE;
                return false;
            }
            vm->stack_count = base;
            return stack_push(vm, array);
        }
        case USK_BC_INDEX:
            if (!stack_pop(vm, &right) || !stack_pop(vm, &left)) return false;
            if (right.kind != V_INT || right.as.i < 0) {
                vm->status = USK_BYTECODE_TYPE_MISMATCH;
                return false;
            }
            if (left.kind == V_ARRAY) {
                if (value_array_get(left, (size_t)right.as.i, &result) != USK_VALUE_OK) {
                    vm->status = USK_BYTECODE_INDEX_OUT_OF_RANGE;
                    return false;
                }
            } else if (left.kind == V_STRING) {
                const char *string = left.as.s ? left.as.s : "";
                if ((size_t)right.as.i >= strlen(string)) {
                    vm->status = USK_BYTECODE_INDEX_OUT_OF_RANGE;
                    return false;
                }
                result = character_value_in(vm->value_storage,
                    string[(size_t)right.as.i]);
                if (vm->value_storage->failed) {
                    vm->status = USK_BYTECODE_ALLOCATION_FAILURE;
                    return false;
                }
            } else {
                vm->status = USK_BYTECODE_TYPE_MISMATCH;
                return false;
            }
            return stack_push(vm, result);
        case USK_BC_INDEX_STORE:
            if (!stack_pop(vm, &result) || !stack_pop(vm, &right) ||
                !stack_pop(vm, &left)) return false;
            if (right.kind != V_INT || right.as.i < 0) {
                vm->status = USK_BYTECODE_TYPE_MISMATCH;
                return false;
            }
            if (left.kind != V_ARRAY) {
                vm->status = USK_BYTECODE_TYPE_MISMATCH;
                return false;
            }
            if (value_array_set(left, (size_t)right.as.i, result) != USK_VALUE_OK) {
                vm->status = USK_BYTECODE_INDEX_OUT_OF_RANGE;
                return false;
            }
            return stack_push(vm, result);
        case USK_BC_LENGTH: {
            if (!stack_pop(vm, &left)) return false;
            size_t length = 0;
            if (left.kind == V_ARRAY) length = value_array_length(left);
            else if (left.kind == V_STRING)
                length = strlen(left.as.s ? left.as.s : "");
            else {
                vm->status = USK_BYTECODE_TYPE_MISMATCH;
                return false;
            }
            if (length > (size_t)LLONG_MAX) {
                vm->status = USK_BYTECODE_LIMIT_EXCEEDED;
                return false;
            }
            return stack_push(vm, int_value((long long)length));
        }
        case USK_BC_EQUAL:
        case USK_BC_NOT_EQUAL:
        case USK_BC_LESS:
        case USK_BC_LESS_EQUAL:
        case USK_BC_GREATER:
        case USK_BC_GREATER_EQUAL:
            if (!stack_pop(vm, &right) || !stack_pop(vm, &left) ||
                !execute_comparison(vm, item->opcode, left, right, &result))
                return false;
            return stack_push(vm, result);
        case USK_BC_JUMP:
            *next_instruction = (size_t)item->operand;
            return true;
        case USK_BC_JUMP_IF_FALSE:
            if (!stack_pop(vm, &left)) return false;
            if (!truthy(left)) *next_instruction = (size_t)item->operand;
            return true;
        case USK_BC_CALL:
            return execute_call(vm, item->operand);
        case USK_BC_RETURN:
            if (!stack_pop(vm, return_value)) return false;
            *finished = true;
            return true;
        case USK_BC_PRINT:
            if (!stack_pop(vm, &left)) return false;
            if (!write_value(vm->options.output ? vm->options.output : stdout,
                             left)) {
                vm->status = USK_BYTECODE_OUTPUT_FAILURE;
                return false;
            }
            return true;
        case USK_BC_HALT:
            *return_value = null_value();
            *finished = true;
            return true;
        case USK_BC_OPCODE_COUNT:
            vm->status = USK_BYTECODE_INVALID_INSTRUCTION;
            return false;
    }
    vm->status = USK_BYTECODE_INVALID_INSTRUCTION;
    return false;
}

UskBytecodeVmOptions usk_bytecode_vm_default_options(void) {
    return (UskBytecodeVmOptions){
        .maximum_steps = 1000000,
        .maximum_call_depth = 128,
        .input = NULL,
        .output = NULL,
        .allow_standard_input = true
    };
}

static UskBytecodeVmResult execute_chunk_internal(
    const UskBytecodeChunk *chunk, const Value *arguments,
    size_t argument_count, UskValueArena *value_storage,
    const UskBytecodeHost *host, const UskBytecodeVmOptions *options,
    VmBudget *budget, size_t call_depth) {
    UskBytecodeVmResult result = {
        .completed = false,
        .status = USK_BYTECODE_INVALID_ARGUMENT,
        .instruction_index = 0,
        .instructions_executed = 0,
        .return_value = {.kind = V_NULL}
    };
    if (!chunk || !value_storage || !budget || (argument_count && !arguments) ||
        argument_count != chunk->parameter_count ||
        chunk->parameter_count > chunk->local_count) return result;
    UskBytecodeChunk verification = *chunk;
    UskBytecodeStatus verified = usk_bytecode_verify(&verification,
        &result.instruction_index);
    if (verified != USK_BYTECODE_OK) {
        result.status = verified;
        return result;
    }
    UskBytecodeVmOptions vm_options = options
        ? *options : usk_bytecode_vm_default_options();
    UskBytecodeVmOptions defaults = usk_bytecode_vm_default_options();
    if (!vm_options.maximum_steps) vm_options.maximum_steps = defaults.maximum_steps;
    if (!vm_options.maximum_call_depth)
        vm_options.maximum_call_depth = defaults.maximum_call_depth;
    size_t stack_capacity = verification.maximum_stack
        ? verification.maximum_stack : 1;
    if (stack_capacity > (size_t)-1 / sizeof(Value) ||
        chunk->local_count > (size_t)-1 / sizeof(Value)) {
        result.status = USK_BYTECODE_LIMIT_EXCEEDED;
        return result;
    }
    VmState vm = {
        .chunk = chunk,
        .value_storage = value_storage,
        .host = host,
        .options = vm_options,
        .stack = (Value *)calloc(stack_capacity, sizeof(Value)),
        .stack_count = 0,
        .stack_capacity = stack_capacity,
        .locals = chunk->local_count
            ? (Value *)calloc(chunk->local_count, sizeof(Value)) : NULL,
        .instruction = 0,
        .call_depth = call_depth,
        .budget = budget,
        .status = USK_BYTECODE_OK
    };
    if (!vm.stack || (chunk->local_count && !vm.locals)) {
        free(vm.stack);
        free(vm.locals);
        result.status = USK_BYTECODE_ALLOCATION_FAILURE;
        return result;
    }
    for (size_t index = 0; index < chunk->local_count; ++index)
        vm.locals[index] = null_value();
    for (size_t index = 0; index < argument_count; ++index)
        vm.locals[index] = arguments[index];

    bool finished = false;
    while (!finished && vm.status == USK_BYTECODE_OK) {
        if (vm.budget->instructions_executed >= vm.budget->maximum_steps) {
            vm.status = USK_BYTECODE_STEP_LIMIT;
            break;
        }
        if (vm.instruction >= chunk->instruction_count) {
            vm.status = USK_BYTECODE_INVALID_CONTROL_FLOW;
            break;
        }
        const UskBytecodeInstruction *instruction =
            &chunk->instructions[vm.instruction];
        size_t next_instruction = vm.instruction + 1;
        if (!execute_instruction(&vm, instruction, &next_instruction,
                                 &result.return_value, &finished)) break;
        vm.instruction = next_instruction;
        vm.budget->instructions_executed++;
    }
    result.completed = vm.status == USK_BYTECODE_OK && finished;
    result.status = vm.status;
    result.instruction_index = vm.instruction;
    result.instructions_executed = budget->instructions_executed;
    free(vm.stack);
    free(vm.locals);
    return result;
}

UskBytecodeVmResult usk_bytecode_execute(
    const UskBytecodeChunk *chunk, const Value *arguments,
    size_t argument_count, UskValueArena *value_storage,
    const UskBytecodeHost *host, const UskBytecodeVmOptions *options) {
    UskBytecodeVmOptions effective = options
        ? *options : usk_bytecode_vm_default_options();
    UskBytecodeVmOptions defaults = usk_bytecode_vm_default_options();
    if (!effective.maximum_steps) effective.maximum_steps = defaults.maximum_steps;
    if (!effective.maximum_call_depth)
        effective.maximum_call_depth = defaults.maximum_call_depth;
    VmBudget budget = {
        .maximum_steps = effective.maximum_steps,
        .maximum_call_depth = effective.maximum_call_depth,
        .instructions_executed = 0
    };
    return execute_chunk_internal(chunk, arguments, argument_count,
        value_storage, host, &effective, &budget, 0);
}
