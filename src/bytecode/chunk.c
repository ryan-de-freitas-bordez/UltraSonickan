#include "usk/bytecode.h"

#include <stdlib.h>
#include <string.h>

#define USK_BYTECODE_MAX_ITEMS ((size_t)1 << 24)

static char *copy_text(const char *text) {
    if (!text) text = "<anonymous>";
    size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1);
    if (copy) memcpy(copy, text, length + 1);
    return copy;
}

static void destroy_constant(Value value, unsigned depth) {
    if (depth > 128) return;
    if (value.kind == V_STRING) {
        free(value.as.s);
    } else if (value.kind == V_ARRAY && value.as.a) {
        for (size_t index = 0; index < value.as.a->count; ++index)
            destroy_constant(value.as.a->items[index], depth + 1);
        free(value.as.a->items);
        free(value.as.a);
    }
}

static Value clone_constant(Value value, UskBytecodeStatus *status,
                            unsigned depth) {
    if (depth > 128) {
        *status = USK_BYTECODE_INVALID_CONSTANT;
        return null_value();
    }
    if (value.kind == V_STRING) {
        Value clone = string_value(value.as.s ? value.as.s : "");
        if (clone.kind != V_STRING)
            *status = USK_BYTECODE_ALLOCATION_FAILURE;
        return clone;
    }
    if (value.kind != V_ARRAY) return value;
    if (!value.as.a) {
        *status = USK_BYTECODE_INVALID_CONSTANT;
        return null_value();
    }
    size_t count = value.as.a->count;
    if (count > USK_BYTECODE_MAX_ITEMS / sizeof(Value)) {
        *status = USK_BYTECODE_LIMIT_EXCEEDED;
        return null_value();
    }
    Value *items = count ? (Value *)calloc(count, sizeof(*items)) : NULL;
    if (count && !items) {
        *status = USK_BYTECODE_ALLOCATION_FAILURE;
        return null_value();
    }
    for (size_t index = 0; index < count; ++index) {
        items[index] = clone_constant(value.as.a->items[index], status, depth + 1);
        if (*status != USK_BYTECODE_OK) {
            for (size_t cleanup = 0; cleanup <= index; ++cleanup)
                destroy_constant(items[cleanup], depth + 1);
            free(items);
            return null_value();
        }
    }
    Array *array = (Array *)malloc(sizeof(*array));
    if (!array) {
        for (size_t index = 0; index < count; ++index)
            destroy_constant(items[index], depth + 1);
        free(items);
        *status = USK_BYTECODE_ALLOCATION_FAILURE;
        return null_value();
    }
    array->items = items;
    array->count = count;
    array->capacity = count;
    array->arena = NULL;
    return (Value){.kind = V_ARRAY, .as.a = array};
}

void usk_bytecode_chunk_init(UskBytecodeChunk *chunk, const char *name,
                             size_t local_count) {
    if (!chunk) return;
    memset(chunk, 0, sizeof(*chunk));
    chunk->name = copy_text(name);
    chunk->local_count = local_count;
}

void usk_bytecode_chunk_destroy(UskBytecodeChunk *chunk) {
    if (!chunk) return;
    for (size_t index = 0; index < chunk->constant_count; ++index)
        destroy_constant(chunk->constants[index], 0);
    free(chunk->constants);
    free(chunk->instructions);
    free(chunk->name);
    memset(chunk, 0, sizeof(*chunk));
}

static UskBytecodeStatus reserve_items(void **storage, size_t *capacity,
                                       size_t count, size_t item_size) {
    if (count >= USK_BYTECODE_MAX_ITEMS) return USK_BYTECODE_LIMIT_EXCEEDED;
    if (count < *capacity) return USK_BYTECODE_OK;
    size_t next = *capacity ? *capacity * 2 : 16;
    if (next > USK_BYTECODE_MAX_ITEMS) next = USK_BYTECODE_MAX_ITEMS;
    if (next <= count || item_size > ((size_t)-1) / next)
        return USK_BYTECODE_LIMIT_EXCEEDED;
    void *grown = realloc(*storage, next * item_size);
    if (!grown) return USK_BYTECODE_ALLOCATION_FAILURE;
    *storage = grown;
    *capacity = next;
    return USK_BYTECODE_OK;
}

UskBytecodeStatus usk_bytecode_add_constant(UskBytecodeChunk *chunk,
                                            Value constant,
                                            uint32_t *index) {
    if (!chunk || !index) return USK_BYTECODE_INVALID_ARGUMENT;
    if (chunk->constant_count > UINT32_MAX)
        return USK_BYTECODE_LIMIT_EXCEEDED;
    UskBytecodeStatus status = reserve_items((void **)&chunk->constants,
        &chunk->constant_capacity, chunk->constant_count, sizeof(*chunk->constants));
    if (status != USK_BYTECODE_OK) return status;
    Value owned = clone_constant(constant, &status, 0);
    if (status != USK_BYTECODE_OK) return status;
    *index = (uint32_t)chunk->constant_count;
    chunk->constants[chunk->constant_count++] = owned;
    return USK_BYTECODE_OK;
}

UskBytecodeStatus usk_bytecode_emit(UskBytecodeChunk *chunk,
                                    UskBytecodeOpcode opcode,
                                    int32_t operand,
                                    UskSourceSpan source,
                                    size_t *instruction_index) {
    if (!chunk || opcode < 0 || opcode >= USK_BC_OPCODE_COUNT)
        return USK_BYTECODE_INVALID_ARGUMENT;
    UskBytecodeStatus status = reserve_items((void **)&chunk->instructions,
        &chunk->instruction_capacity, chunk->instruction_count,
        sizeof(*chunk->instructions));
    if (status != USK_BYTECODE_OK) return status;
    if (instruction_index) *instruction_index = chunk->instruction_count;
    chunk->instructions[chunk->instruction_count++] =
        (UskBytecodeInstruction){opcode, operand, source};
    return USK_BYTECODE_OK;
}

UskBytecodeStatus usk_bytecode_patch_operand(UskBytecodeChunk *chunk,
                                             size_t instruction_index,
                                             int32_t operand) {
    if (!chunk || instruction_index >= chunk->instruction_count)
        return USK_BYTECODE_INVALID_ARGUMENT;
    chunk->instructions[instruction_index].operand = operand;
    return USK_BYTECODE_OK;
}

bool usk_bytecode_get_constant(const UskBytecodeChunk *chunk, uint32_t index,
                               Value *result) {
    if (!chunk || !result || (size_t)index >= chunk->constant_count) return false;
    *result = chunk->constants[index];
    return true;
}

const UskBytecodeInstruction *usk_bytecode_get_instruction(
    const UskBytecodeChunk *chunk, size_t instruction_index) {
    if (!chunk || instruction_index >= chunk->instruction_count) return NULL;
    return &chunk->instructions[instruction_index];
}

const char *usk_bytecode_status_name(UskBytecodeStatus status) {
    switch (status) {
        case USK_BYTECODE_OK: return "ok";
        case USK_BYTECODE_INVALID_ARGUMENT: return "invalid argument";
        case USK_BYTECODE_ALLOCATION_FAILURE: return "allocation failure";
        case USK_BYTECODE_LIMIT_EXCEEDED: return "bytecode size limit exceeded";
        case USK_BYTECODE_INVALID_CONSTANT: return "invalid constant value";
        case USK_BYTECODE_INVALID_INSTRUCTION: return "invalid instruction";
        case USK_BYTECODE_INVALID_CONTROL_FLOW: return "invalid control flow";
        case USK_BYTECODE_STACK_UNDERFLOW: return "operand stack underflow";
        case USK_BYTECODE_STACK_MISMATCH: return "inconsistent stack height";
        case USK_BYTECODE_LOCAL_OUT_OF_RANGE: return "local slot out of range";
        case USK_BYTECODE_TYPE_MISMATCH: return "runtime type mismatch";
        case USK_BYTECODE_DIVISION_BY_ZERO: return "division by zero";
        case USK_BYTECODE_UNKNOWN_GLOBAL: return "unknown global variable";
        case USK_BYTECODE_UNKNOWN_FUNCTION: return "unknown function";
        case USK_BYTECODE_STEP_LIMIT: return "instruction step limit exceeded";
        case USK_BYTECODE_INPUT_FAILURE: return "standard input failed";
        case USK_BYTECODE_CALL_DEPTH_LIMIT: return "maximum call depth exceeded";
        case USK_BYTECODE_OUTPUT_FAILURE: return "standard output failed";
        case USK_BYTECODE_INDEX_OUT_OF_RANGE: return "array or string index is out of range";
        case USK_BYTECODE_NUMERIC_DOMAIN: return "numeric argument is outside the function domain";
        case USK_BYTECODE_NUMERIC_RANGE: return "numeric result is out of range";
        case USK_BYTECODE_IO_FAILURE: return "filesystem operation failed";
    }
    return "unknown bytecode status";
}

bool usk_bytecode_opcode_terminates_block(UskBytecodeOpcode opcode) {
    return opcode == USK_BC_JUMP || opcode == USK_BC_RETURN ||
           opcode == USK_BC_HALT;
}
