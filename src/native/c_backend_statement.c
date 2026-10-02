#include "c_backend_internal.h"

#include <string.h>

static bool statement_has_semicolon(const UskAstStmt *statement) {
    if (!statement) return false;
    switch (statement->kind) {
        case USK_STMT_EMPTY:
        case USK_STMT_VARIABLE:
        case USK_STMT_EXPRESSION:
        case USK_STMT_BREAK:
        case USK_STMT_CONTINUE:
        case USK_STMT_RETURN:
            return true;
        case USK_STMT_BLOCK:
        case USK_STMT_IF:
        case USK_STMT_WHILE:
        case USK_STMT_FOR:
        case USK_STMT_FOREACH:
        case USK_STMT_SWITCH:
            return false;
    }
    return false;
}

static bool emit_expression_statement(UskCEmitter *emitter,
                                      const UskAstExpr *expression) {
    if (!expression) return usk_c_emit_write(emitter, ";\n");
    if (!usk_c_emit_expression(emitter, expression)) return false;
    return usk_c_emit_write(emitter, ";\n");
}

static bool emit_variable_statement(UskCEmitter *emitter,
                                    const UskAstType *type,
                                    const char *name,
                                    const UskAstExpr *initializer) {
    UskCValueKind kind = type ? usk_c_value_kind_from_type(type)
        : usk_c_infer_expression_kind(emitter, initializer);
    UskAstType inferred_type = {0};
    if (!type) {
        const char *inferred = usk_c_value_kind_name(kind);
        if (kind == USK_C_VALUE_UNKNOWN || kind == USK_C_VALUE_INT8 ||
            kind == USK_C_VALUE_UINT8 || kind == USK_C_VALUE_UINT32 ||
            kind == USK_C_VALUE_UINT64) {
            usk_c_emit_unsupported(emitter, initializer ? initializer->span :
                (UskSourceSpan){0}, "variables with non-primitive inferred types");
            return false;
        }
        inferred_type.name = inferred;
        inferred_type.is_signed = true;
        type = &inferred_type;
    }
    if (!usk_c_emit_type(emitter, type) || !usk_c_emit_write(emitter, " %s",
            name ? name : "usk_invalid_variable")) return false;
    if (initializer) {
        usk_c_emit_write(emitter, " = ");
        UskCValueKind previous_expected = emitter->expected_value_kind;
        emitter->expected_value_kind = kind;
        if (!usk_c_emit_expression(emitter, initializer)) return false;
        emitter->expected_value_kind = previous_expected;
    }
    if (!usk_c_emit_write(emitter, ";\n")) return false;
    return usk_c_bind_local(emitter, name, kind);
}

static bool emit_block_contents(UskCEmitter *emitter,
                                const UskAstStmtList *item) {
    size_t visited = 0;
    const UskAstStmtList *slow = item, *fast = item;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) {
            usk_c_emit_error(emitter, USK_DIAG_INTERNAL_ERROR,
                (UskSourceSpan){0}, "statement list contains a cycle");
            return false;
        }
    }
    for (; item && !emitter->failed; item = item->next) {
        usk_c_emit_indent(emitter);
        if (!usk_c_emit_statement(emitter, item->statement)) return false;
        visited++;
    }
    (void)visited;
    return !emitter->failed;
}

static bool emit_block(UskCEmitter *emitter, const UskAstStmt *block) {
    UskCBinding *scope = usk_c_scope_begin(emitter);
    usk_c_emit_write(emitter, "{\n");
    emitter->indentation++;
    bool success = emit_block_contents(emitter, block->as.block.items);
    emitter->indentation--;
    usk_c_emit_indent(emitter);
    usk_c_emit_write(emitter, "}\n");
    usk_c_scope_end(emitter, scope);
    return success && !emitter->failed;
}

static bool emit_scoped_statement(UskCEmitter *emitter,
                                  const UskAstStmt *statement) {
    if (!statement) return usk_c_emit_write(emitter, ";\n");
    if (statement->kind == USK_STMT_BLOCK)
        return usk_c_emit_statement(emitter, statement);
    UskCBinding *scope = usk_c_scope_begin(emitter);
    usk_c_emit_write(emitter, "{\n");
    emitter->indentation++;
    usk_c_emit_indent(emitter);
    bool success = usk_c_emit_statement(emitter, statement);
    emitter->indentation--;
    usk_c_emit_indent(emitter);
    usk_c_emit_write(emitter, "}\n");
    usk_c_scope_end(emitter, scope);
    return success && !emitter->failed;
}

static bool emit_if(UskCEmitter *emitter, const UskAstStmt *statement) {
    usk_c_emit_write(emitter, "if (");
    usk_c_emit_condition(emitter, statement->as.if_stmt.condition);
    usk_c_emit_write(emitter, ") ");
    emit_scoped_statement(emitter, statement->as.if_stmt.then_branch);
    if (statement->as.if_stmt.else_branch) {
        usk_c_emit_indent(emitter);
        usk_c_emit_write(emitter, "else ");
        emit_scoped_statement(emitter, statement->as.if_stmt.else_branch);
    }
    return !emitter->failed;
}

static bool emit_while(UskCEmitter *emitter, const UskAstStmt *statement) {
    usk_c_emit_write(emitter, "while (");
    usk_c_emit_condition(emitter, statement->as.while_stmt.condition);
    usk_c_emit_write(emitter, ") ");
    emit_scoped_statement(emitter, statement->as.while_stmt.body);
    return !emitter->failed;
}

static bool emit_for(UskCEmitter *emitter, const UskAstStmt *statement) {
    UskCBinding *scope = usk_c_scope_begin(emitter);
    usk_c_emit_write(emitter, "for (");
    if (statement->as.for_stmt.initializer) {
        if (statement->as.for_stmt.initializer->kind == USK_STMT_VARIABLE) {
            const UskAstStmt *variable = statement->as.for_stmt.initializer;
            if (variable->as.variable.type) {
                usk_c_emit_type(emitter, variable->as.variable.type);
                usk_c_emit_write(emitter, " %s",
                    variable->as.variable.name ? variable->as.variable.name : "usk_invalid");
            } else {
                UskCValueKind kind = usk_c_infer_expression_kind(emitter,
                    variable->as.variable.initializer);
                const char *inferred = usk_c_value_kind_name(kind);
                if (kind == USK_C_VALUE_UNKNOWN || kind == USK_C_VALUE_INT8 ||
                    kind == USK_C_VALUE_UINT8 || kind == USK_C_VALUE_UINT32 ||
                    kind == USK_C_VALUE_UINT64) {
                    usk_c_emit_unsupported(emitter, variable->span,
                        "for-loop variables without a primitive inferred type");
                    usk_c_scope_end(emitter, scope);
                    return false;
                }
                UskAstType inferred_type = {.name = inferred, .is_signed = true};
                usk_c_emit_type(emitter, &inferred_type);
                usk_c_emit_write(emitter, " %s",
                    variable->as.variable.name ? variable->as.variable.name : "usk_invalid");
            }
            if (variable->as.variable.initializer) {
                usk_c_emit_write(emitter, " = ");
                UskCValueKind expected = variable->as.variable.type
                    ? usk_c_value_kind_from_type(variable->as.variable.type)
                    : usk_c_infer_expression_kind(emitter,
                        variable->as.variable.initializer);
                UskCValueKind previous_expected = emitter->expected_value_kind;
                emitter->expected_value_kind = expected;
                usk_c_emit_expression(emitter, variable->as.variable.initializer);
                emitter->expected_value_kind = previous_expected;
            }
            UskCValueKind local_kind = variable->as.variable.type
                ? usk_c_value_kind_from_type(variable->as.variable.type)
                : usk_c_infer_expression_kind(emitter,
                    variable->as.variable.initializer);
            usk_c_bind_local(emitter, variable->as.variable.name, local_kind);
            usk_c_emit_write(emitter, "; ");
        } else if (statement->as.for_stmt.initializer->kind ==
                   USK_STMT_EXPRESSION) {
            usk_c_emit_expression(emitter,
                statement->as.for_stmt.initializer->as.expression);
            usk_c_emit_write(emitter, "; ");
        } else {
            usk_c_emit_unsupported(emitter,
                statement->as.for_stmt.initializer->span,
                "this for-loop initializer statement");
            usk_c_scope_end(emitter, scope);
            return false;
        }
    } else usk_c_emit_write(emitter, "; ");
    if (statement->as.for_stmt.condition)
        usk_c_emit_condition(emitter, statement->as.for_stmt.condition);
    usk_c_emit_write(emitter, "; ");
    if (statement->as.for_stmt.update)
        usk_c_emit_expression(emitter, statement->as.for_stmt.update);
    usk_c_emit_write(emitter, ") ");
    emit_scoped_statement(emitter, statement->as.for_stmt.body);
    usk_c_scope_end(emitter, scope);
    return !emitter->failed;
}

static bool emit_switch(UskCEmitter *emitter, const UskAstStmt *statement) {
    usk_c_emit_write(emitter, "switch (");
    usk_c_emit_expression(emitter, statement->as.switch_stmt.selector);
    usk_c_emit_write(emitter, ") {\n");
    emitter->indentation++;
    const UskAstSwitchCase *slow_case = statement->as.switch_stmt.cases;
    const UskAstSwitchCase *fast_case = slow_case;
    while (fast_case && fast_case->next) {
        slow_case = slow_case->next;
        fast_case = fast_case->next->next;
        if (slow_case == fast_case) {
            usk_c_emit_error(emitter, USK_DIAG_INTERNAL_ERROR, statement->span,
                             "switch-case list contains a cycle");
            return false;
        }
    }
    for (const UskAstSwitchCase *item = statement->as.switch_stmt.cases;
         item && !emitter->failed; item = item->next) {
        UskCBinding *case_scope = usk_c_scope_begin(emitter);
        if (item->is_default) {
            usk_c_emit_indent(emitter);
            usk_c_emit_write(emitter, "default:\n");
        } else {
            for (const UskAstExprList *label = item->labels; label;
                 label = label->next) {
                usk_c_emit_indent(emitter);
                usk_c_emit_write(emitter, "case ");
                usk_c_emit_expression(emitter, label->expression);
                usk_c_emit_write(emitter, ":\n");
            }
        }
        usk_c_emit_indent(emitter);
        usk_c_emit_write(emitter, "{\n");
        emitter->indentation++;
        for (const UskAstStmtList *body = item->statements;
             body && !emitter->failed; body = body->next) {
            usk_c_emit_indent(emitter);
            usk_c_emit_statement(emitter, body->statement);
        }
        usk_c_emit_indent(emitter);
        usk_c_emit_write(emitter, "}\nbreak;\n");
        emitter->indentation--;
        usk_c_scope_end(emitter, case_scope);
    }
    emitter->indentation--;
    usk_c_emit_indent(emitter);
    usk_c_emit_write(emitter, "}\n");
    return !emitter->failed;
}

static bool emit_statement_body(UskCEmitter *emitter,
                                const UskAstStmt *statement) {
    switch (statement->kind) {
        case USK_STMT_EMPTY:
            return usk_c_emit_write(emitter, ";\n");
        case USK_STMT_BLOCK:
            return emit_block(emitter, statement);
        case USK_STMT_VARIABLE:
            return emit_variable_statement(emitter,
                statement->as.variable.type, statement->as.variable.name,
                statement->as.variable.initializer);
        case USK_STMT_EXPRESSION:
            return emit_expression_statement(emitter,
                                              statement->as.expression);
        case USK_STMT_IF:
            return emit_if(emitter, statement);
        case USK_STMT_WHILE:
            return emit_while(emitter, statement);
        case USK_STMT_FOR:
            return emit_for(emitter, statement);
        case USK_STMT_FOREACH:
            usk_c_emit_unsupported(emitter, statement->span,
                                   "for-in loops in the native C backend");
            return false;
        case USK_STMT_SWITCH:
            return emit_switch(emitter, statement);
        case USK_STMT_BREAK:
            return usk_c_emit_write(emitter, "break;\n");
        case USK_STMT_CONTINUE:
            return usk_c_emit_write(emitter, "continue;\n");
        case USK_STMT_RETURN:
            usk_c_emit_write(emitter, "return");
            if (statement->as.return_value) {
                usk_c_emit_write(emitter, " ");
                UskCValueKind previous_expected = emitter->expected_value_kind;
                emitter->expected_value_kind = emitter->current_return_kind;
                usk_c_emit_expression(emitter, statement->as.return_value);
                emitter->expected_value_kind = previous_expected;
            }
            return usk_c_emit_write(emitter, ";\n");
    }
    usk_c_emit_error(emitter, USK_DIAG_INTERNAL_ERROR, statement->span,
                     "AST contains an unknown statement node");
    return false;
}

bool usk_c_emit_statement(UskCEmitter *emitter,
                          const UskAstStmt *statement) {
    if (!emitter || !statement || emitter->failed) return false;
    emitter->result->statements_emitted++;
    bool success = emit_statement_body(emitter, statement);
    if (success && !statement_has_semicolon(statement) &&
        statement->kind != USK_STMT_BLOCK)
        usk_c_emit_write(emitter, "\n");
    return success && !emitter->failed;
}
