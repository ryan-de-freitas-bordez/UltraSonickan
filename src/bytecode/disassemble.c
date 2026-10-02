#include "usk/bytecode.h"

#include <inttypes.h>
#include <stdlib.h>

const char *usk_bytecode_opcode_name(UskBytecodeOpcode opcode) {
    static const char *names[USK_BC_OPCODE_COUNT] = {
        "CONSTANT", "NULL", "TRUE", "FALSE", "POP", "DUPLICATE",
        "LOAD_LOCAL", "STORE_LOCAL", "LOAD_GLOBAL", "STORE_GLOBAL",
        "ADD", "SUBTRACT", "MULTIPLY", "DIVIDE", "MODULO", "NEGATE",
        "NOT", "BUILD_ARRAY", "INDEX", "INDEX_STORE", "LENGTH",
        "EQUAL", "NOT_EQUAL", "LESS", "LESS_EQUAL", "GREATER",
        "GREATER_EQUAL", "JUMP", "JUMP_IF_FALSE", "CALL", "RETURN",
        "PRINT", "HALT"
    };
    if (opcode < 0 || opcode >= USK_BC_OPCODE_COUNT) return "<invalid-opcode>";
    return names[opcode];
}

static bool has_operand(UskBytecodeOpcode opcode) {
    switch (opcode) {
        case USK_BC_CONSTANT:
        case USK_BC_LOAD_LOCAL:
        case USK_BC_STORE_LOCAL:
        case USK_BC_LOAD_GLOBAL:
        case USK_BC_STORE_GLOBAL:
        case USK_BC_JUMP:
        case USK_BC_JUMP_IF_FALSE:
        case USK_BC_CALL:
        case USK_BC_BUILD_ARRAY:
            return true;
        default:
            return false;
    }
}

static void print_constant(FILE *stream, const UskBytecodeChunk *chunk,
                           int32_t operand) {
    if (operand < 0 || (size_t)operand >= chunk->constant_count) {
        fputs("<invalid-constant>", stream);
        return;
    }
    const Value *value = &chunk->constants[operand];
    char *text = value_string(*value);
    if (!text) {
        fputs("<unprintable>", stream);
        return;
    }
    fprintf(stream, " %s : %s", text, type_name(*value));
    free(text);
}

static void print_escaped_string(FILE *stream, const char *text) {
    fputc('"', stream);
    for (const unsigned char *cursor = (const unsigned char *)text;
         cursor && *cursor; ++cursor) {
        switch (*cursor) {
            case '\\': fputs("\\\\", stream); break;
            case '"': fputs("\\\"", stream); break;
            case '\n': fputs("\\n", stream); break;
            case '\r': fputs("\\r", stream); break;
            case '\t': fputs("\\t", stream); break;
            default:
                if (*cursor < 32 || *cursor == 127)
                    fprintf(stream, "\\x%02X", (unsigned)*cursor);
                else
                    fputc(*cursor, stream);
                break;
        }
    }
    fputc('"', stream);
}

static void print_constant_entry(FILE *stream, size_t index, Value value) {
    fprintf(stream, "  [%04zu] ", index);
    if (value.kind == V_STRING) {
        print_escaped_string(stream, value.as.s ? value.as.s : "");
    } else {
        char *text = value_string(value);
        if (text) {
            fputs(text, stream);
            free(text);
        } else {
            fputs("<unprintable>", stream);
        }
    }
    fprintf(stream, "  ; %s\n", type_name(value));
}

void usk_bytecode_disassemble_constants(FILE *stream,
                                        const UskBytecodeChunk *chunk) {
    if (!stream || !chunk) return;
    fprintf(stream, "constants for %s (%zu)\n",
        chunk->name ? chunk->name : "<anonymous>", chunk->constant_count);
    for (size_t index = 0; index < chunk->constant_count; ++index)
        print_constant_entry(stream, index, chunk->constants[index]);
}

void usk_bytecode_disassemble_instruction(FILE *stream,
                                          const UskBytecodeChunk *chunk,
                                          size_t instruction_index) {
    if (!stream || !chunk) return;
    if (instruction_index >= chunk->instruction_count) {
        fprintf(stream, "%04zu <out-of-range>\n", instruction_index);
        return;
    }
    const UskBytecodeInstruction *instruction =
        &chunk->instructions[instruction_index];
    fprintf(stream, "%04zu  %4d:%-4d %-18s", instruction_index,
        instruction->source.line, instruction->source.column,
        usk_bytecode_opcode_name(instruction->opcode));
    if (has_operand(instruction->opcode))
        fprintf(stream, " %" PRId32, instruction->operand);
    if (instruction->opcode == USK_BC_CONSTANT ||
        instruction->opcode == USK_BC_LOAD_GLOBAL ||
        instruction->opcode == USK_BC_STORE_GLOBAL)
        print_constant(stream, chunk, instruction->operand);
    if (instruction->opcode == USK_BC_JUMP ||
        instruction->opcode == USK_BC_JUMP_IF_FALSE)
        fprintf(stream, " -> %" PRId32, instruction->operand);
    fputc('\n', stream);
}

void usk_bytecode_disassemble(FILE *stream, const UskBytecodeChunk *chunk) {
    if (!stream || !chunk) return;
    fprintf(stream, "chunk %s: %zu instructions, %zu constants, %zu locals",
        chunk->name ? chunk->name : "<anonymous>", chunk->instruction_count,
        chunk->constant_count, chunk->local_count);
    if (chunk->maximum_stack)
        fprintf(stream, ", max stack %zu", chunk->maximum_stack);
    fputc('\n', stream);
    usk_bytecode_disassemble_constants(stream, chunk);
    for (size_t index = 0; index < chunk->instruction_count; ++index)
        usk_bytecode_disassemble_instruction(stream, chunk, index);
}
