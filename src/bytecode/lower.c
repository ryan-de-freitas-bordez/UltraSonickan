#include "usk/bytecode.h"
#include "usk/arraylib.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct CompilerLocal {
    const char *name;
    size_t slot;
    unsigned scope_depth;
    struct CompilerLocal *next;
} CompilerLocal;

typedef struct {
    size_t *items;
    size_t count;
    size_t capacity;
} JumpList;

typedef struct CompilerLoop {
    JumpList continues;
    JumpList breaks;
    bool is_loop;
    struct CompilerLoop *parent;
} CompilerLoop;

typedef struct {
    UskBytecodeChunk *chunk;
    UskDiagnosticList *diagnostics;
    const char *source_name;
    CompilerLocal *locals;
    unsigned scope_depth;
    size_t next_slot;
    CompilerLoop *loop;
    UskBytecodeStatus status;
} BytecodeCompiler;

static void compile_error(BytecodeCompiler *compiler, UskDiagnosticCode code,
                          UskSourceSpan span, const char *format, ...) {
    char message[768];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    usk_diagnostics_add(compiler->diagnostics, USK_DIAGNOSTIC_ERROR, code,
        compiler->source_name, span.line, span.column, "%s", message);
    if (compiler->status == USK_BYTECODE_OK)
        compiler->status = USK_BYTECODE_INVALID_INSTRUCTION;
}

static bool emit(BytecodeCompiler *compiler, UskBytecodeOpcode opcode,
                 int32_t operand, UskSourceSpan span, size_t *index) {
    UskBytecodeStatus status = usk_bytecode_emit(compiler->chunk, opcode,
        operand, span, index);
    if (status == USK_BYTECODE_OK) return true;
    compile_error(compiler, USK_DIAG_OUT_OF_MEMORY, span,
        "cannot emit bytecode instruction: %s",
        usk_bytecode_status_name(status));
    compiler->status = status;
    return false;
}

static bool add_constant(BytecodeCompiler *compiler, Value value,
                         UskSourceSpan span, int32_t *constant_index) {
    uint32_t index = 0;
    UskBytecodeStatus status = usk_bytecode_add_constant(compiler->chunk,
        value, &index);
    if (value.kind == V_STRING) free(value.as.s);
    if (status != USK_BYTECODE_OK) {
        compile_error(compiler, USK_DIAG_OUT_OF_MEMORY, span,
            "cannot add bytecode constant: %s",
            usk_bytecode_status_name(status));
        compiler->status = status;
        return false;
    }
    *constant_index = (int32_t)index;
    return true;
}

static bool add_string_constant(BytecodeCompiler *compiler, const char *text,
                                UskSourceSpan span, int32_t *index) {
    Value value = string_value(text ? text : "");
    if (value.kind != V_STRING) {
        compile_error(compiler, USK_DIAG_OUT_OF_MEMORY, span,
                      "cannot allocate a bytecode string constant");
        compiler->status = USK_BYTECODE_ALLOCATION_FAILURE;
        return false;
    }
    return add_constant(compiler, value, span, index);
}

static CompilerLocal *find_local(BytecodeCompiler *compiler, const char *name) {
    for (CompilerLocal *local = compiler->locals; local; local = local->next)
        if (!strcmp(local->name, name)) return local;
    return NULL;
}

static bool local_exists_in_scope(BytecodeCompiler *compiler,
                                  const char *name) {
    for (CompilerLocal *local = compiler->locals; local; local = local->next) {
        if (local->scope_depth < compiler->scope_depth) break;
        if (!strcmp(local->name, name)) return true;
    }
    return false;
}

static bool add_local(BytecodeCompiler *compiler, const char *name,
                      UskSourceSpan span, size_t *slot) {
    if (local_exists_in_scope(compiler, name)) {
        compile_error(compiler, USK_DIAG_DUPLICATE_NAME, span,
                      "duplicate local '%s' in the same scope", name);
        return false;
    }
    CompilerLocal *local = (CompilerLocal *)malloc(sizeof(*local));
    if (!local) {
        compile_error(compiler, USK_DIAG_OUT_OF_MEMORY, span,
                      "cannot allocate local variable metadata");
        compiler->status = USK_BYTECODE_ALLOCATION_FAILURE;
        return false;
    }
    local->name = name;
    local->slot = compiler->next_slot++;
    local->scope_depth = compiler->scope_depth;
    local->next = compiler->locals;
    compiler->locals = local;
    *slot = local->slot;
    return true;
}

static CompilerLocal *enter_scope(BytecodeCompiler *compiler) {
    compiler->scope_depth++;
    return compiler->locals;
}

static void leave_scope(BytecodeCompiler *compiler, CompilerLocal *marker) {
    while (compiler->locals != marker) {
        CompilerLocal *local = compiler->locals;
        compiler->locals = local->next;
        free(local);
    }
    if (compiler->scope_depth) compiler->scope_depth--;
}

static bool append_jump(JumpList *list, size_t instruction) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 4;
        size_t *items = (size_t *)realloc(list->items, capacity * sizeof(*items));
        if (!items) return false;
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->count++] = instruction;
    return true;
}

static bool patch_jump_list(BytecodeCompiler *compiler, JumpList *list,
                            size_t target, UskSourceSpan span) {
    if (target > INT32_MAX) {
        compile_error(compiler, USK_DIAG_UNSUPPORTED_FEATURE, span,
                      "bytecode jump target exceeds the instruction limit");
        return false;
    }
    for (size_t index = 0; index < list->count; ++index) {
        if (usk_bytecode_patch_operand(compiler->chunk, list->items[index],
                                       (int32_t)target) != USK_BYTECODE_OK) {
            compile_error(compiler, USK_DIAG_INTERNAL_ERROR, span,
                          "cannot patch loop control-flow instruction");
            return false;
        }
    }
    return true;
}

static char *callable_name(const UskAstExpr *expression) {
    if (!expression) return NULL;
    if (expression->kind == USK_EXPR_NAME) {
        size_t length = strlen(expression->as.name);
        char *copy = (char *)malloc(length + 1);
        if (copy) memcpy(copy, expression->as.name, length + 1);
        return copy;
    }
    if (expression->kind != USK_EXPR_MEMBER) return NULL;
    char *base = callable_name(expression->as.member.object);
    if (!base) return NULL;
    const char *separator = expression->as.member.pointer_access ? "->" : "::";
    const char *member = expression->as.member.name;
    size_t length = strlen(base) + strlen(separator) + strlen(member) + 1;
    char *joined = (char *)malloc(length);
    if (joined) snprintf(joined, length, "%s%s%s", base, separator, member);
    free(base);
    return joined;
}

static bool compile_expression(BytecodeCompiler *compiler,
                               const UskAstExpr *expression);
static bool compile_statement(BytecodeCompiler *compiler,
                              const UskAstStmt *statement);

static bool emit_name_load(BytecodeCompiler *compiler, const char *name,
                           UskSourceSpan span) {
    CompilerLocal *local = find_local(compiler, name);
    if (local)
        return emit(compiler, USK_BC_LOAD_LOCAL, (int32_t)local->slot,
                    span, NULL);
    int32_t constant = 0;
    if (!add_string_constant(compiler, name, span, &constant)) return false;
    return emit(compiler, USK_BC_LOAD_GLOBAL, constant, span, NULL);
}

static bool emit_name_store(BytecodeCompiler *compiler, const char *name,
                            UskSourceSpan span) {
    CompilerLocal *local = find_local(compiler, name);
    if (local)
        return emit(compiler, USK_BC_STORE_LOCAL, (int32_t)local->slot,
                    span, NULL);
    int32_t constant = 0;
    if (!add_string_constant(compiler, name, span, &constant)) return false;
    return emit(compiler, USK_BC_STORE_GLOBAL, constant, span, NULL);
}

static UskBytecodeOpcode binary_opcode(const char *operator_text) {
    if (!strcmp(operator_text, "+")) return USK_BC_ADD;
    if (!strcmp(operator_text, "-")) return USK_BC_SUBTRACT;
    if (!strcmp(operator_text, "*")) return USK_BC_MULTIPLY;
    if (!strcmp(operator_text, "/")) return USK_BC_DIVIDE;
    if (!strcmp(operator_text, "%")) return USK_BC_MODULO;
    if (!strcmp(operator_text, "==")) return USK_BC_EQUAL;
    if (!strcmp(operator_text, "!=")) return USK_BC_NOT_EQUAL;
    if (!strcmp(operator_text, "<")) return USK_BC_LESS;
    if (!strcmp(operator_text, "<=")) return USK_BC_LESS_EQUAL;
    if (!strcmp(operator_text, ">")) return USK_BC_GREATER;
    if (!strcmp(operator_text, ">=")) return USK_BC_GREATER_EQUAL;
    return USK_BC_OPCODE_COUNT;
}

static bool compile_logical(BytecodeCompiler *compiler,
                            const UskAstExpr *expression) {
    size_t short_circuit = 0, end = 0;
    const char *operator_text = expression->as.binary.operator;
    if (!strcmp(operator_text, "&&")) {
        if (!compile_expression(compiler, expression->as.binary.left) ||
            !emit(compiler, USK_BC_JUMP_IF_FALSE, -1, expression->span,
                  &short_circuit) ||
            !compile_expression(compiler, expression->as.binary.right) ||
            !emit(compiler, USK_BC_JUMP, -1, expression->span, &end))
            return false;
        size_t false_branch = compiler->chunk->instruction_count;
        if (false_branch > INT32_MAX ||
            usk_bytecode_patch_operand(compiler->chunk, short_circuit,
                (int32_t)false_branch) != USK_BYTECODE_OK ||
            !emit(compiler, USK_BC_FALSE, 0, expression->span, NULL))
            return false;
    } else {
        if (!compile_expression(compiler, expression->as.binary.left) ||
            !emit(compiler, USK_BC_NOT, 0, expression->span, NULL) ||
            !emit(compiler, USK_BC_JUMP_IF_FALSE, -1, expression->span,
                  &short_circuit) ||
            !compile_expression(compiler, expression->as.binary.right) ||
            !emit(compiler, USK_BC_JUMP, -1, expression->span, &end))
            return false;
        size_t true_branch = compiler->chunk->instruction_count;
        if (true_branch > INT32_MAX ||
            usk_bytecode_patch_operand(compiler->chunk, short_circuit,
                (int32_t)true_branch) != USK_BYTECODE_OK ||
            !emit(compiler, USK_BC_TRUE, 0, expression->span, NULL))
            return false;
    }
    size_t finish = compiler->chunk->instruction_count;
    return finish <= INT32_MAX &&
        usk_bytecode_patch_operand(compiler->chunk, end,
                                   (int32_t)finish) == USK_BYTECODE_OK;
}

static bool compile_assignment(BytecodeCompiler *compiler,
                               const UskAstExpr *expression) {
    const UskAstExpr *target = expression->as.assignment.target;
    const char *operator_text = expression->as.assignment.operator;
    if (target && target->kind == USK_EXPR_INDEX) {
        if (strcmp(operator_text, "=")) {
            compile_error(compiler, USK_DIAG_UNSUPPORTED_FEATURE,
                expression->span,
                "compound bytecode assignment to an indexed value is not implemented");
            return false;
        }
        return compile_expression(compiler, target->as.index.object) &&
            compile_expression(compiler, target->as.index.index) &&
            compile_expression(compiler, expression->as.assignment.value) &&
            emit(compiler, USK_BC_INDEX_STORE, 0, expression->span, NULL);
    }
    if (!target || target->kind != USK_EXPR_NAME) {
        compile_error(compiler, USK_DIAG_UNSUPPORTED_FEATURE, expression->span,
                      "bytecode assignment currently requires a named variable");
        return false;
    }
    if (strcmp(operator_text, "=")) {
        char operation[2] = {operator_text[0], '\0'};
        UskBytecodeOpcode opcode = binary_opcode(operation);
        if (opcode == USK_BC_OPCODE_COUNT ||
            !emit_name_load(compiler, target->as.name, target->span) ||
            !compile_expression(compiler, expression->as.assignment.value) ||
            !emit(compiler, opcode, 0, expression->span, NULL))
            return false;
    } else if (!compile_expression(compiler, expression->as.assignment.value)) {
        return false;
    }
    return emit(compiler, USK_BC_DUPLICATE, 0, expression->span, NULL) &&
           emit_name_store(compiler, target->as.name, target->span);
}

static bool compile_call(BytecodeCompiler *compiler,
                         const UskAstExpr *expression) {
    const UskAstExpr *callee = expression->as.call.callee;
    const UskArrayBuiltinInfo *array_method = callee &&
        callee->kind == USK_EXPR_MEMBER
        ? usk_array_method_find(callee->as.member.name) : NULL;
    char *name = array_method ? NULL : callable_name(callee);
    const char *call_name = array_method ? array_method->name : name;
    if (!call_name) {
        compile_error(compiler, USK_DIAG_UNSUPPORTED_FEATURE, expression->span,
                      "bytecode calls require a named function");
        return false;
    }
    int32_t constant = 0;
    bool success = add_string_constant(compiler, call_name, expression->span,
                                       &constant) &&
                   emit(compiler, USK_BC_CONSTANT, constant,
                        expression->as.call.callee->span, NULL);
    free(name);
    if (!success) return false;
    size_t receiver_count = array_method ? 1 : 0;
    if (expression->as.call.count > (size_t)INT32_MAX - receiver_count) {
        compile_error(compiler, USK_DIAG_ARGUMENT_COUNT, expression->span,
                      "call argument count exceeds bytecode limits");
        return false;
    }
    if (array_method && !compile_expression(compiler,
            callee->as.member.object)) return false;
    for (const UskAstExprList *argument = expression->as.call.arguments;
         argument; argument = argument->next)
        if (!compile_expression(compiler, argument->expression)) return false;
    return emit(compiler, USK_BC_CALL,
                (int32_t)(expression->as.call.count + receiver_count),
                expression->span, NULL);
}

static bool compile_expression(BytecodeCompiler *compiler,
                               const UskAstExpr *expression) {
    if (!expression) return false;
    int32_t constant = 0;
    switch (expression->kind) {
        case USK_EXPR_NULL:
            return emit(compiler, USK_BC_NULL, 0, expression->span, NULL);
        case USK_EXPR_BOOLEAN:
            return emit(compiler, expression->as.boolean ? USK_BC_TRUE : USK_BC_FALSE,
                        0, expression->span, NULL);
        case USK_EXPR_INTEGER:
            return add_constant(compiler, int_value(expression->as.integer),
                expression->span, &constant) &&
                emit(compiler, USK_BC_CONSTANT, constant, expression->span, NULL);
        case USK_EXPR_DOUBLE:
            return add_constant(compiler, double_value(expression->as.floating),
                expression->span, &constant) &&
                emit(compiler, USK_BC_CONSTANT, constant, expression->span, NULL);
        case USK_EXPR_STRING:
            return add_string_constant(compiler, expression->as.string,
                expression->span, &constant) &&
                emit(compiler, USK_BC_CONSTANT, constant, expression->span, NULL);
        case USK_EXPR_NAME:
            return emit_name_load(compiler, expression->as.name, expression->span);
        case USK_EXPR_UNARY: {
            const char *operator_text = expression->as.unary.operator;
            const UskAstExpr *operand = expression->as.unary.operand;
            if (!strcmp(operator_text, "++") || !strcmp(operator_text, "--") ||
                !strcmp(operator_text, "post++") || !strcmp(operator_text, "post--")) {
                if (!operand || operand->kind != USK_EXPR_NAME) {
                    compile_error(compiler, USK_DIAG_UNSUPPORTED_FEATURE,
                        expression->span,
                        "bytecode increment requires a named variable");
                    return false;
                }
                bool decrement = strstr(operator_text, "--") != NULL;
                bool postfix = operator_text[0] == 'p';
                if (!emit_name_load(compiler, operand->as.name, operand->span))
                    return false;
                if (postfix && !emit(compiler, USK_BC_DUPLICATE, 0,
                                     expression->span, NULL)) return false;
                int32_t one_index = 0;
                if (!add_constant(compiler, int_value(1), expression->span,
                                  &one_index) ||
                    !emit(compiler, USK_BC_CONSTANT, one_index,
                          expression->span, NULL)) return false;
                if (!emit(compiler, decrement ? USK_BC_SUBTRACT : USK_BC_ADD,
                          0, expression->span, NULL)) return false;
                if (!postfix && !emit(compiler, USK_BC_DUPLICATE, 0,
                                      expression->span, NULL)) return false;
                return emit_name_store(compiler, operand->as.name, operand->span);
            }
            if (!compile_expression(compiler, operand)) return false;
            if (!strcmp(operator_text, "!"))
                return emit(compiler, USK_BC_NOT, 0, expression->span, NULL);
            if (!strcmp(operator_text, "-"))
                return emit(compiler, USK_BC_NEGATE, 0, expression->span, NULL);
            if (!strcmp(operator_text, "+")) return true;
            compile_error(compiler, USK_DIAG_UNSUPPORTED_FEATURE,
                expression->span, "unsupported bytecode unary operator '%s'",
                operator_text);
            return false;
        }
        case USK_EXPR_BINARY: {
            const char *operator_text = expression->as.binary.operator;
            if (!strcmp(operator_text, "&&") || !strcmp(operator_text, "||"))
                return compile_logical(compiler, expression);
            UskBytecodeOpcode opcode = binary_opcode(operator_text);
            if (opcode == USK_BC_OPCODE_COUNT) {
                compile_error(compiler, USK_DIAG_UNSUPPORTED_FEATURE,
                    expression->span, "unsupported bytecode binary operator '%s'",
                    operator_text);
                return false;
            }
            return compile_expression(compiler, expression->as.binary.left) &&
                   compile_expression(compiler, expression->as.binary.right) &&
                   emit(compiler, opcode, 0, expression->span, NULL);
        }
        case USK_EXPR_ASSIGNMENT:
            return compile_assignment(compiler, expression);
        case USK_EXPR_CALL:
            return compile_call(compiler, expression);
        case USK_EXPR_ARRAY:
            if (expression->as.array.count > INT32_MAX) {
                compile_error(compiler, USK_DIAG_UNSUPPORTED_FEATURE,
                    expression->span, "array literal exceeds bytecode limits");
                return false;
            }
            for (const UskAstExprList *item = expression->as.array.items;
                 item; item = item->next)
                if (!compile_expression(compiler, item->expression)) return false;
            return emit(compiler, USK_BC_BUILD_ARRAY,
                (int32_t)expression->as.array.count, expression->span, NULL);
        case USK_EXPR_MEMBER:
            if (expression->as.member.name &&
                (!strcmp(expression->as.member.name, "length") ||
                 !strcmp(expression->as.member.name, "size")))
                return compile_expression(compiler, expression->as.member.object) &&
                    emit(compiler, USK_BC_LENGTH, 0, expression->span, NULL);
            compile_error(compiler, USK_DIAG_UNSUPPORTED_FEATURE,
                expression->span,
                "bytecode member access is not implemented for this member");
            return false;
        case USK_EXPR_INDEX:
            return compile_expression(compiler, expression->as.index.object) &&
                compile_expression(compiler, expression->as.index.index) &&
                emit(compiler, USK_BC_INDEX, 0, expression->span, NULL);
        case USK_EXPR_CONDITIONAL: {
            size_t otherwise = 0, finish_jump = 0;
            if (!compile_expression(compiler, expression->as.conditional.condition) ||
                !emit(compiler, USK_BC_JUMP_IF_FALSE, -1, expression->span, &otherwise) ||
                !compile_expression(compiler, expression->as.conditional.when_true) ||
                !emit(compiler, USK_BC_JUMP, -1, expression->span, &finish_jump))
                return false;
            size_t alternate = compiler->chunk->instruction_count;
            if (alternate > INT32_MAX ||
                usk_bytecode_patch_operand(compiler->chunk, otherwise,
                    (int32_t)alternate) != USK_BYTECODE_OK ||
                !compile_expression(compiler, expression->as.conditional.when_false))
                return false;
            size_t finish = compiler->chunk->instruction_count;
            return finish <= INT32_MAX &&
                usk_bytecode_patch_operand(compiler->chunk, finish_jump,
                    (int32_t)finish) == USK_BYTECODE_OK;
        }
    }
    return false;
}

static bool compile_block(BytecodeCompiler *compiler,
                          const UskAstStmt *statement) {
    CompilerLocal *marker = enter_scope(compiler);
    bool success = true;
    for (const UskAstStmtList *item = statement->as.block.items;
         item; item = item->next) {
        if (!compile_statement(compiler, item->statement)) success = false;
    }
    leave_scope(compiler, marker);
    return success;
}

static bool compile_if(BytecodeCompiler *compiler,
                       const UskAstStmt *statement) {
    size_t alternate = 0;
    if (!compile_expression(compiler, statement->as.if_stmt.condition) ||
        !emit(compiler, USK_BC_JUMP_IF_FALSE, -1, statement->span, &alternate) ||
        !compile_statement(compiler, statement->as.if_stmt.then_branch))
        return false;
    if (!statement->as.if_stmt.else_branch) {
        size_t finish = compiler->chunk->instruction_count;
        return finish <= INT32_MAX &&
            usk_bytecode_patch_operand(compiler->chunk, alternate,
                                       (int32_t)finish) == USK_BYTECODE_OK;
    }
    size_t finish_jump = 0;
    if (!emit(compiler, USK_BC_JUMP, -1, statement->span, &finish_jump)) return false;
    size_t otherwise = compiler->chunk->instruction_count;
    if (otherwise > INT32_MAX ||
        usk_bytecode_patch_operand(compiler->chunk, alternate,
                                   (int32_t)otherwise) != USK_BYTECODE_OK ||
        !compile_statement(compiler, statement->as.if_stmt.else_branch))
        return false;
    size_t finish = compiler->chunk->instruction_count;
    return finish <= INT32_MAX &&
        usk_bytecode_patch_operand(compiler->chunk, finish_jump,
                                   (int32_t)finish) == USK_BYTECODE_OK;
}

static bool compile_while(BytecodeCompiler *compiler,
                          const UskAstStmt *statement) {
    CompilerLoop loop = {.is_loop = true, .parent = compiler->loop};
    compiler->loop = &loop;
    size_t start = compiler->chunk->instruction_count;
    size_t exit_jump = 0;
    bool success = compile_expression(compiler, statement->as.while_stmt.condition) &&
        emit(compiler, USK_BC_JUMP_IF_FALSE, -1, statement->span, &exit_jump) &&
        compile_statement(compiler, statement->as.while_stmt.body) &&
        patch_jump_list(compiler, &loop.continues, start, statement->span) &&
        start <= INT32_MAX && emit(compiler, USK_BC_JUMP, (int32_t)start,
                                   statement->span, NULL);
    size_t finish = compiler->chunk->instruction_count;
    if (success && finish <= INT32_MAX)
        success = usk_bytecode_patch_operand(compiler->chunk, exit_jump,
            (int32_t)finish) == USK_BYTECODE_OK &&
            patch_jump_list(compiler, &loop.breaks, finish, statement->span);
    else success = false;
    free(loop.continues.items);
    free(loop.breaks.items);
    compiler->loop = loop.parent;
    return success;
}

static bool compile_for(BytecodeCompiler *compiler,
                        const UskAstStmt *statement) {
    CompilerLocal *scope = enter_scope(compiler);
    CompilerLoop loop = {.is_loop = true, .parent = compiler->loop};
    compiler->loop = &loop;
    bool success = true;
    if (statement->as.for_stmt.initializer)
        success = compile_statement(compiler, statement->as.for_stmt.initializer);
    size_t condition = compiler->chunk->instruction_count;
    size_t exit_jump = 0;
    if (success && statement->as.for_stmt.condition)
        success = compile_expression(compiler, statement->as.for_stmt.condition) &&
            emit(compiler, USK_BC_JUMP_IF_FALSE, -1, statement->span, &exit_jump);
    if (success) success = compile_statement(compiler, statement->as.for_stmt.body);
    size_t update = compiler->chunk->instruction_count;
    if (success) success = patch_jump_list(compiler, &loop.continues, update,
                                           statement->span);
    if (success && statement->as.for_stmt.update)
        success = compile_expression(compiler, statement->as.for_stmt.update) &&
            emit(compiler, USK_BC_POP, 0, statement->span, NULL);
    if (success && condition <= INT32_MAX)
        success = emit(compiler, USK_BC_JUMP, (int32_t)condition,
                       statement->span, NULL);
    size_t finish = compiler->chunk->instruction_count;
    if (success && finish <= INT32_MAX) {
        if (statement->as.for_stmt.condition)
            success = usk_bytecode_patch_operand(compiler->chunk, exit_jump,
                (int32_t)finish) == USK_BYTECODE_OK;
        success = success && patch_jump_list(compiler, &loop.breaks, finish,
                                              statement->span);
    } else success = false;
    free(loop.continues.items);
    free(loop.breaks.items);
    compiler->loop = loop.parent;
    leave_scope(compiler, scope);
    return success;
}

static bool compile_foreach(BytecodeCompiler *compiler,
                            const UskAstStmt *statement) {
    CompilerLocal *scope = enter_scope(compiler);
    CompilerLoop loop = {.is_loop = true, .parent = compiler->loop};
    compiler->loop = &loop;
    size_t collection_slot = 0, index_slot = 0, item_slot = 0;
    bool success = add_local(compiler, "$foreach_collection", statement->span,
                             &collection_slot) &&
        add_local(compiler, "$foreach_index", statement->span, &index_slot) &&
        add_local(compiler, statement->as.foreach_stmt.name, statement->span,
                  &item_slot);
    if (success && (collection_slot > INT32_MAX || index_slot > INT32_MAX ||
                    item_slot > INT32_MAX)) {
        compile_error(compiler, USK_DIAG_UNSUPPORTED_FEATURE, statement->span,
                      "foreach locals exceed bytecode limits");
        success = false;
    }
    int32_t zero_constant = 0;
    if (success)
        success = compile_expression(compiler,
                statement->as.foreach_stmt.collection) &&
            emit(compiler, USK_BC_STORE_LOCAL, (int32_t)collection_slot,
                 statement->span, NULL) &&
            add_constant(compiler, int_value(0), statement->span,
                         &zero_constant) &&
            emit(compiler, USK_BC_CONSTANT, zero_constant,
                 statement->span, NULL) &&
            emit(compiler, USK_BC_STORE_LOCAL, (int32_t)index_slot,
                 statement->span, NULL);

    size_t condition = compiler->chunk->instruction_count;
    size_t exit_jump = 0;
    if (success)
        success = emit(compiler, USK_BC_LOAD_LOCAL, (int32_t)index_slot,
                statement->span, NULL) &&
            emit(compiler, USK_BC_LOAD_LOCAL, (int32_t)collection_slot,
                 statement->span, NULL) &&
            emit(compiler, USK_BC_LENGTH, 0, statement->span, NULL) &&
            emit(compiler, USK_BC_LESS, 0, statement->span, NULL) &&
            emit(compiler, USK_BC_JUMP_IF_FALSE, -1, statement->span,
                 &exit_jump) &&
            emit(compiler, USK_BC_LOAD_LOCAL, (int32_t)collection_slot,
                 statement->span, NULL) &&
            emit(compiler, USK_BC_LOAD_LOCAL, (int32_t)index_slot,
                 statement->span, NULL) &&
            emit(compiler, USK_BC_INDEX, 0, statement->span, NULL) &&
            emit(compiler, USK_BC_STORE_LOCAL, (int32_t)item_slot,
                 statement->span, NULL) &&
            compile_statement(compiler, statement->as.foreach_stmt.body);

    size_t update = compiler->chunk->instruction_count;
    if (success)
        success = patch_jump_list(compiler, &loop.continues, update,
                                  statement->span);
    int32_t one_constant = 0;
    if (success)
        success = emit(compiler, USK_BC_LOAD_LOCAL, (int32_t)index_slot,
                statement->span, NULL) &&
            add_constant(compiler, int_value(1), statement->span,
                         &one_constant) &&
            emit(compiler, USK_BC_CONSTANT, one_constant,
                 statement->span, NULL) &&
            emit(compiler, USK_BC_ADD, 0, statement->span, NULL) &&
            emit(compiler, USK_BC_STORE_LOCAL, (int32_t)index_slot,
                 statement->span, NULL) &&
            condition <= INT32_MAX &&
            emit(compiler, USK_BC_JUMP, (int32_t)condition,
                 statement->span, NULL);
    size_t finish = compiler->chunk->instruction_count;
    if (success && finish <= INT32_MAX)
        success = usk_bytecode_patch_operand(compiler->chunk, exit_jump,
                (int32_t)finish) == USK_BYTECODE_OK &&
            patch_jump_list(compiler, &loop.breaks, finish, statement->span);
    else success = false;

    free(loop.continues.items);
    free(loop.breaks.items);
    compiler->loop = loop.parent;
    leave_scope(compiler, scope);
    return success;
}

static bool compile_switch_case_body(BytecodeCompiler *compiler,
                                     const UskAstSwitchCase *item) {
    CompilerLocal *scope = enter_scope(compiler);
    bool success = true;
    for (const UskAstStmtList *statement = item->statements; statement;
         statement = statement->next)
        if (!compile_statement(compiler, statement->statement)) success = false;
    leave_scope(compiler, scope);
    return success;
}

static bool compile_switch(BytecodeCompiler *compiler,
                           const UskAstStmt *statement) {
    CompilerLocal *scope = enter_scope(compiler);
    CompilerLoop context = {.is_loop = false, .parent = compiler->loop};
    compiler->loop = &context;
    size_t selector_slot = 0;
    bool success = add_local(compiler, "$switch_selector", statement->span,
                             &selector_slot) && selector_slot <= INT32_MAX &&
        compile_expression(compiler, statement->as.switch_stmt.selector) &&
        emit(compiler, USK_BC_STORE_LOCAL, (int32_t)selector_slot,
             statement->span, NULL);
    const UskAstSwitchCase *default_case = NULL;
    for (const UskAstSwitchCase *item = statement->as.switch_stmt.cases;
         item && success; item = item->next) {
        if (item->is_default) {
            default_case = item;
            continue;
        }
        if (!item->labels) continue;
        for (const UskAstExprList *label = item->labels;
             label && success; label = label->next) {
            size_t mismatch = 0;
            success = emit(compiler, USK_BC_LOAD_LOCAL,
                    (int32_t)selector_slot, item->span, NULL) &&
                compile_expression(compiler, label->expression) &&
                emit(compiler, USK_BC_EQUAL, 0, label->expression->span, NULL) &&
                emit(compiler, USK_BC_JUMP_IF_FALSE, -1, item->span,
                     &mismatch) &&
                compile_switch_case_body(compiler, item);
            if (success) {
                size_t finish_jump = 0;
                success = emit(compiler, USK_BC_JUMP, -1, item->span,
                               &finish_jump) &&
                    append_jump(&context.breaks, finish_jump);
                size_t next_label = compiler->chunk->instruction_count;
                success = success && next_label <= INT32_MAX &&
                    usk_bytecode_patch_operand(compiler->chunk, mismatch,
                        (int32_t)next_label) == USK_BYTECODE_OK;
            }
        }
    }
    if (success && default_case)
        success = compile_switch_case_body(compiler, default_case);
    size_t finish = compiler->chunk->instruction_count;
    if (success && finish <= INT32_MAX)
        success = patch_jump_list(compiler, &context.breaks, finish,
                                  statement->span);
    else success = false;
    free(context.continues.items);
    free(context.breaks.items);
    compiler->loop = context.parent;
    leave_scope(compiler, scope);
    return success;
}

static bool compile_statement(BytecodeCompiler *compiler,
                              const UskAstStmt *statement) {
    if (!statement) return true;
    switch (statement->kind) {
        case USK_STMT_EMPTY:
            return true;
        case USK_STMT_BLOCK:
            return compile_block(compiler, statement);
        case USK_STMT_VARIABLE: {
            if (statement->as.variable.initializer) {
                if (!compile_expression(compiler, statement->as.variable.initializer))
                    return false;
            } else if (!emit(compiler, USK_BC_NULL, 0, statement->span, NULL)) {
                return false;
            }
            size_t slot = 0;
            return add_local(compiler, statement->as.variable.name,
                             statement->span, &slot) && slot <= INT32_MAX &&
                emit(compiler, USK_BC_STORE_LOCAL, (int32_t)slot,
                     statement->span, NULL);
        }
        case USK_STMT_EXPRESSION:
            return compile_expression(compiler, statement->as.expression) &&
                emit(compiler, USK_BC_POP, 0, statement->span, NULL);
        case USK_STMT_IF:
            return compile_if(compiler, statement);
        case USK_STMT_WHILE:
            return compile_while(compiler, statement);
        case USK_STMT_FOR:
            return compile_for(compiler, statement);
        case USK_STMT_FOREACH:
            return compile_foreach(compiler, statement);
        case USK_STMT_SWITCH:
            return compile_switch(compiler, statement);
        case USK_STMT_BREAK:
        case USK_STMT_CONTINUE: {
            CompilerLoop *target = compiler->loop;
            if (statement->kind == USK_STMT_CONTINUE)
                while (target && !target->is_loop) target = target->parent;
            if (!target) {
                compile_error(compiler, USK_DIAG_INVALID_CONTROL_FLOW,
                    statement->span,
                    statement->kind == USK_STMT_CONTINUE
                        ? "continue has no enclosing loop"
                        : "break has no enclosing loop or switch");
                return false;
            }
            size_t jump = 0;
            if (!emit(compiler, USK_BC_JUMP, -1, statement->span, &jump))
                return false;
            JumpList *list = statement->kind == USK_STMT_BREAK
                ? &target->breaks : &target->continues;
            if (!append_jump(list, jump)) {
                compile_error(compiler, USK_DIAG_OUT_OF_MEMORY, statement->span,
                              "cannot allocate loop jump patch list");
                compiler->status = USK_BYTECODE_ALLOCATION_FAILURE;
                return false;
            }
            return true;
        }
        case USK_STMT_RETURN:
            if (statement->as.return_value) {
                if (!compile_expression(compiler, statement->as.return_value)) return false;
            } else if (!emit(compiler, USK_BC_NULL, 0, statement->span, NULL)) {
                return false;
            }
            return emit(compiler, USK_BC_RETURN, 0, statement->span, NULL);
    }
    return false;
}

static void destroy_locals(CompilerLocal *locals) {
    while (locals) {
        CompilerLocal *next = locals->next;
        free(locals);
        locals = next;
    }
}

UskBytecodeCompileResult usk_bytecode_compile_function(
    const UskAstDecl *function, const char *source_name,
    UskBytecodeChunk *chunk, UskDiagnosticList *diagnostics) {
    UskBytecodeCompileResult result = {false, USK_BYTECODE_INVALID_ARGUMENT, 0, 0};
    if (!function || function->kind != USK_DECL_FUNCTION || !chunk || !diagnostics ||
        !function->as.function.body)
        return result;
    const char *name = function->as.function.name ? function->as.function.name : "<function>";
    usk_bytecode_chunk_init(chunk, name, function->as.function.parameter_count);
    chunk->parameter_count = function->as.function.parameter_count;
    BytecodeCompiler compiler = {
        .chunk = chunk,
        .diagnostics = diagnostics,
        .source_name = source_name ? source_name : "<source>",
        .status = USK_BYTECODE_OK
    };
    for (const UskAstParameter *parameter = function->as.function.parameters;
         parameter; parameter = parameter->next) {
        size_t slot = 0;
        if (!add_local(&compiler, parameter->name, function->span, &slot)) break;
    }
    if (compiler.status == USK_BYTECODE_OK) {
        bool body_compiled = compile_statement(&compiler,
            function->as.function.body);
        if (!body_compiled && compiler.status == USK_BYTECODE_OK)
            compile_error(&compiler, USK_DIAG_INTERNAL_ERROR, function->span,
                          "bytecode lowering stopped without a diagnostic");
    }
    if (compiler.status == USK_BYTECODE_OK) {
        emit(&compiler, USK_BC_NULL, 0, function->span, NULL);
        emit(&compiler, USK_BC_RETURN, 0, function->span, NULL);
    }
    chunk->local_count = compiler.next_slot;
    if (compiler.status == USK_BYTECODE_OK) {
        size_t error_instruction = 0;
        UskBytecodeStatus verify = usk_bytecode_verify(chunk, &error_instruction);
        if (verify != USK_BYTECODE_OK) {
            UskSourceSpan span = error_instruction < chunk->instruction_count
                ? chunk->instructions[error_instruction].source : function->span;
            compile_error(&compiler, USK_DIAG_INVALID_CONTROL_FLOW, span,
                "bytecode verification failed at instruction %zu: %s",
                error_instruction, usk_bytecode_status_name(verify));
            compiler.status = verify;
        }
    }
    destroy_locals(compiler.locals);
    result.status = compiler.status;
    result.success = compiler.status == USK_BYTECODE_OK;
    result.instructions_emitted = chunk->instruction_count;
    result.locals_allocated = compiler.next_slot;
    return result;
}
