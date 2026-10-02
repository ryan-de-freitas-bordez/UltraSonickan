#include "usk/bytecode.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t required;
    int64_t delta;
    bool terminal;
    bool conditional_jump;
    bool unconditional_jump;
} StackEffect;

static bool opcode_valid(UskBytecodeOpcode opcode) {
    return opcode >= 0 && opcode < USK_BC_OPCODE_COUNT;
}

static StackEffect stack_effect(UskBytecodeOpcode opcode, int32_t operand) {
    StackEffect effect = {0, 0, false, false, false};
    switch (opcode) {
        case USK_BC_CONSTANT:
        case USK_BC_NULL:
        case USK_BC_TRUE:
        case USK_BC_FALSE:
        case USK_BC_LOAD_LOCAL:
        case USK_BC_LOAD_GLOBAL:
            effect.delta = 1;
            break;
        case USK_BC_POP:
        case USK_BC_STORE_LOCAL:
        case USK_BC_STORE_GLOBAL:
        case USK_BC_PRINT:
            effect.required = 1;
            effect.delta = -1;
            break;
        case USK_BC_DUPLICATE:
            effect.required = 1;
            effect.delta = 1;
            break;
        case USK_BC_NEGATE:
        case USK_BC_NOT:
            effect.required = 1;
            break;
        case USK_BC_BUILD_ARRAY:
            if (operand < 0)
                return (StackEffect){(size_t)-1, 0, false, false, false};
            effect.required = (size_t)operand;
            effect.delta = 1 - (int64_t)operand;
            break;
        case USK_BC_INDEX:
            effect.required = 2;
            effect.delta = -1;
            break;
        case USK_BC_INDEX_STORE:
            effect.required = 3;
            effect.delta = -2;
            break;
        case USK_BC_LENGTH:
            effect.required = 1;
            break;
        case USK_BC_ADD:
        case USK_BC_SUBTRACT:
        case USK_BC_MULTIPLY:
        case USK_BC_DIVIDE:
        case USK_BC_MODULO:
        case USK_BC_EQUAL:
        case USK_BC_NOT_EQUAL:
        case USK_BC_LESS:
        case USK_BC_LESS_EQUAL:
        case USK_BC_GREATER:
        case USK_BC_GREATER_EQUAL:
            effect.required = 2;
            effect.delta = -1;
            break;
        case USK_BC_JUMP:
            effect.unconditional_jump = true;
            break;
        case USK_BC_JUMP_IF_FALSE:
            effect.required = 1;
            effect.delta = -1;
            effect.conditional_jump = true;
            break;
        case USK_BC_CALL:
            if (operand < 0) return (StackEffect){(size_t)-1, 0, false, false, false};
            effect.required = (size_t)operand + 1;
            effect.delta = -(int64_t)operand;
            break;
        case USK_BC_RETURN:
            effect.required = 1;
            effect.terminal = true;
            break;
        case USK_BC_HALT:
            effect.terminal = true;
            break;
        case USK_BC_OPCODE_COUNT:
            break;
    }
    return effect;
}

static bool operand_is_constant(UskBytecodeOpcode opcode) {
    return opcode == USK_BC_CONSTANT || opcode == USK_BC_LOAD_GLOBAL ||
           opcode == USK_BC_STORE_GLOBAL;
}

static bool operand_is_local(UskBytecodeOpcode opcode) {
    return opcode == USK_BC_LOAD_LOCAL || opcode == USK_BC_STORE_LOCAL;
}

static UskBytecodeStatus validate_instruction(const UskBytecodeChunk *chunk,
                                              size_t index) {
    const UskBytecodeInstruction *instruction = &chunk->instructions[index];
    if (!opcode_valid(instruction->opcode)) return USK_BYTECODE_INVALID_INSTRUCTION;
    if (operand_is_constant(instruction->opcode)) {
        if (instruction->operand < 0 ||
            (size_t)instruction->operand >= chunk->constant_count)
            return USK_BYTECODE_INVALID_CONSTANT;
    }
    if (operand_is_local(instruction->opcode)) {
        if (instruction->operand < 0 ||
            (size_t)instruction->operand >= chunk->local_count)
            return USK_BYTECODE_LOCAL_OUT_OF_RANGE;
    }
    if (instruction->opcode == USK_BC_JUMP ||
        instruction->opcode == USK_BC_JUMP_IF_FALSE) {
        if (instruction->operand < 0 ||
            (size_t)instruction->operand >= chunk->instruction_count)
            return USK_BYTECODE_INVALID_CONTROL_FLOW;
    }
    if (instruction->opcode == USK_BC_CALL && instruction->operand < 0)
        return USK_BYTECODE_INVALID_INSTRUCTION;
    StackEffect effect = stack_effect(instruction->opcode, instruction->operand);
    if (effect.required == (size_t)-1)
        return USK_BYTECODE_INVALID_INSTRUCTION;
    return USK_BYTECODE_OK;
}

static UskBytecodeStatus enqueue(size_t target, int64_t depth,
                                 int64_t *depths, size_t *queue,
                                 size_t *tail, size_t instruction_count) {
    if (target >= instruction_count) return USK_BYTECODE_INVALID_CONTROL_FLOW;
    if (depths[target] == -1) {
        depths[target] = depth;
        queue[(*tail)++] = target;
        return USK_BYTECODE_OK;
    }
    return depths[target] == depth ? USK_BYTECODE_OK : USK_BYTECODE_STACK_MISMATCH;
}

UskBytecodeStatus usk_bytecode_verify(UskBytecodeChunk *chunk,
                                      size_t *error_instruction) {
    if (error_instruction) *error_instruction = 0;
    if (!chunk || (!chunk->instructions && chunk->instruction_count) ||
        (!chunk->constants && chunk->constant_count))
        return USK_BYTECODE_INVALID_ARGUMENT;
    if (!chunk->instruction_count) return USK_BYTECODE_INVALID_CONTROL_FLOW;
    if (chunk->instruction_count > ((size_t)-1) / sizeof(size_t))
        return USK_BYTECODE_LIMIT_EXCEEDED;

    for (size_t index = 0; index < chunk->instruction_count; ++index) {
        UskBytecodeStatus status = validate_instruction(chunk, index);
        if (status != USK_BYTECODE_OK) {
            if (error_instruction) *error_instruction = index;
            return status;
        }
    }

    size_t count = chunk->instruction_count;
    int64_t *depths = (int64_t *)malloc(count * sizeof(*depths));
    size_t *queue = (size_t *)malloc(count * sizeof(*queue));
    if (!depths || !queue) {
        free(depths);
        free(queue);
        return USK_BYTECODE_ALLOCATION_FAILURE;
    }
    for (size_t index = 0; index < count; ++index) depths[index] = -1;
    depths[0] = 0;
    queue[0] = 0;
    size_t head = 0, tail = 1, maximum = 0;
    UskBytecodeStatus status = USK_BYTECODE_OK;

    while (head < tail && status == USK_BYTECODE_OK) {
        size_t index = queue[head++];
        const UskBytecodeInstruction *instruction = &chunk->instructions[index];
        StackEffect effect = stack_effect(instruction->opcode, instruction->operand);
        int64_t before = depths[index];
        if (effect.required == (size_t)-1) {
            status = USK_BYTECODE_INVALID_INSTRUCTION;
        } else if (before < (int64_t)effect.required) {
            status = USK_BYTECODE_STACK_UNDERFLOW;
        } else {
            int64_t after = before + effect.delta;
            if (after < 0 || after > (int64_t)count * 4 + 1024) {
                status = USK_BYTECODE_LIMIT_EXCEEDED;
            } else {
                if ((size_t)after > maximum) maximum = (size_t)after;
                if (effect.terminal) continue;
                if (effect.unconditional_jump) {
                    status = enqueue((size_t)instruction->operand, after,
                        depths, queue, &tail, count);
                } else {
                    if (effect.conditional_jump)
                        status = enqueue((size_t)instruction->operand, after,
                            depths, queue, &tail, count);
                    if (status == USK_BYTECODE_OK) {
                        if (index + 1 >= count)
                            status = USK_BYTECODE_INVALID_CONTROL_FLOW;
                        else
                            status = enqueue(index + 1, after, depths,
                                queue, &tail, count);
                    }
                }
            }
        }
        if (status != USK_BYTECODE_OK && error_instruction)
            *error_instruction = index;
    }
    if (status == USK_BYTECODE_OK) chunk->maximum_stack = maximum;
    free(depths);
    free(queue);
    return status;
}
