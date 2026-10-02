#include "evaluator_internal.h"

#include <string.h>

static UskFlow execute_statement_list(UskEvaluator *evaluator,
                                     Environment *environment,
                                     const UskAstStmtList *list) {
    for (const UskAstStmtList *item = list; item && !evaluator->failed;
         item = item->next) {
        UskFlow flow = usk_eval_statement(evaluator, environment, item->statement);
        if (flow.kind != USK_FLOW_NORMAL) return flow;
    }
    return usk_flow_normal();
}

Value usk_default_value_for_type(UskEvaluator *evaluator,
                                 const UskAstType *type) {
    if (!type) return null_value();
    if (type->array_dimensions)
        return array_value_in(evaluator->value_storage, NULL, 0);
    if (!strcmp(type->name, "USKInt") || !strcmp(type->name, "USKLong") ||
        !strcmp(type->name, "USKShort")) return int_value(0);
    if (!strcmp(type->name, "USKDouble") || !strcmp(type->name, "USKFloat"))
        return double_value(0.0);
    if (!strcmp(type->name, "USKBool")) return bool_value(false);
    if (!strcmp(type->name, "USKString"))
        return string_value_in(evaluator->value_storage, "");
    if (!strcmp(type->name, "USKChar"))
        return character_value_in(evaluator->value_storage, ' ');
    return null_value();
}

static bool value_matches_ast_type(const UskAstType *type, Value value) {
    if (!type || !type->name || !strcmp(type->name, "USKAuto")) return true;
    if (type->array_dimensions) return value.kind == V_ARRAY;
    return value_matches_type(type->name, value);
}

static UskFlow execute_block(UskEvaluator *evaluator, Environment *parent,
                             const UskAstStmt *block) {
    Environment local;
    environment_init(&local, parent);
    UskFlow flow = execute_statement_list(evaluator, &local,
                                          block->as.block.items);
    environment_destroy(&local);
    return flow;
}

static UskFlow execute_while(UskEvaluator *evaluator, Environment *environment,
                             const UskAstStmt *statement) {
    for (size_t iteration = 0; iteration < evaluator->options.maximum_loop_iterations;
         ++iteration) {
        Value condition = usk_eval_expression(evaluator, environment,
                                             statement->as.while_stmt.condition);
        if (evaluator->failed || !truthy(condition)) return usk_flow_normal();
        evaluator->loop_depth++;
        UskFlow flow = usk_eval_statement(evaluator, environment,
                                          statement->as.while_stmt.body);
        evaluator->loop_depth--;
        if (flow.kind == USK_FLOW_RETURN) return flow;
        if (flow.kind == USK_FLOW_BREAK) return usk_flow_normal();
        if (evaluator->failed) return usk_flow_normal();
    }
    usk_eval_error(evaluator, USK_DIAG_INVALID_CONTROL_FLOW, statement->span,
                   "while loop exceeded the configured iteration limit (%zu)",
                   evaluator->options.maximum_loop_iterations);
    return usk_flow_normal();
}

static UskFlow execute_for(UskEvaluator *evaluator, Environment *environment,
                           const UskAstStmt *statement) {
    Environment loop_scope;
    environment_init(&loop_scope, environment);
    if (statement->as.for_stmt.initializer) {
        UskFlow init = usk_eval_statement(evaluator, &loop_scope,
                                          statement->as.for_stmt.initializer);
        if (init.kind != USK_FLOW_NORMAL) {
            environment_destroy(&loop_scope);
            return init;
        }
    }
    size_t iteration = 0;
    for (; iteration < evaluator->options.maximum_loop_iterations; ++iteration) {
        if (statement->as.for_stmt.condition) {
            Value condition = usk_eval_expression(evaluator, &loop_scope,
                statement->as.for_stmt.condition);
            if (evaluator->failed || !truthy(condition)) break;
        }
        evaluator->loop_depth++;
        UskFlow flow = usk_eval_statement(evaluator, &loop_scope,
                                          statement->as.for_stmt.body);
        evaluator->loop_depth--;
        if (flow.kind == USK_FLOW_RETURN) {
            environment_destroy(&loop_scope);
            return flow;
        }
        if (flow.kind == USK_FLOW_BREAK || evaluator->failed) break;
        if (statement->as.for_stmt.update)
            usk_eval_expression(evaluator, &loop_scope,
                                statement->as.for_stmt.update);
    }
    if (iteration == evaluator->options.maximum_loop_iterations && !evaluator->failed)
        usk_eval_error(evaluator, USK_DIAG_INVALID_CONTROL_FLOW, statement->span,
                       "for loop exceeded the configured iteration limit (%zu)",
                       evaluator->options.maximum_loop_iterations);
    environment_destroy(&loop_scope);
    return usk_flow_normal();
}

static UskFlow execute_foreach(UskEvaluator *evaluator, Environment *environment,
                               const UskAstStmt *statement) {
    Value collection = usk_eval_expression(evaluator, environment,
        statement->as.foreach_stmt.collection);
    size_t collection_count = 0;
    const char *string_items = NULL;
    if (collection.kind == V_ARRAY && collection.as.a) {
        if (collection.as.a->count && !collection.as.a->items) {
            usk_eval_error(evaluator, USK_DIAG_INVALID_DECLARATION,
                           statement->span,
                           "for-in array has invalid element storage");
            return usk_flow_normal();
        }
        collection_count = collection.as.a->count;
    } else if (collection.kind == V_STRING) {
        string_items = collection.as.s ? collection.as.s : "";
        collection_count = strlen(string_items);
    } else {
        usk_eval_error(evaluator, USK_DIAG_TYPE_MISMATCH, statement->span,
                       "for-in collection must be an array or USKString");
        return usk_flow_normal();
    }
    Environment loop_scope;
    environment_init(&loop_scope, environment);
    size_t iteration = 0;
    bool broke = false;
    for (; iteration < collection_count &&
         iteration < evaluator->options.maximum_loop_iterations; ++iteration) {
        Value item = collection.kind == V_ARRAY
            ? collection.as.a->items[iteration]
            : character_value_in(evaluator->value_storage,
                                 string_items[iteration]);
        if (evaluator->value_storage->failed) {
            usk_eval_error(evaluator, USK_DIAG_OUT_OF_MEMORY, statement->span,
                           "cannot allocate string iteration character");
            break;
        }
        const UskAstType *type = statement->as.foreach_stmt.type;
        if (type && !value_matches_ast_type(type, item)) {
            usk_eval_error(evaluator, USK_DIAG_TYPE_MISMATCH, statement->span,
                           "array item does not match for-in variable type %s", type->name);
            break;
        }
        if (define_typed_var(&loop_scope, statement->as.foreach_stmt.name, item,
                         type && type->is_const,
                         type && !type->array_dimensions ? type->name : NULL)
            != USK_ENV_ASSIGN_OK) {
            usk_eval_error(evaluator, USK_DIAG_OUT_OF_MEMORY, statement->span,
                           "cannot allocate foreach variable '%s'",
                           statement->as.foreach_stmt.name);
            break;
        }
        evaluator->loop_depth++;
        UskFlow flow = usk_eval_statement(evaluator, &loop_scope,
                                          statement->as.foreach_stmt.body);
        evaluator->loop_depth--;
        if (flow.kind == USK_FLOW_RETURN) {
            environment_destroy(&loop_scope);
            return flow;
        }
        if (flow.kind == USK_FLOW_BREAK) {
            broke = true;
            break;
        }
        if (evaluator->failed) break;
    }
    if (!broke && iteration == evaluator->options.maximum_loop_iterations &&
        iteration < collection_count && !evaluator->failed)
        usk_eval_error(evaluator, USK_DIAG_INVALID_CONTROL_FLOW, statement->span,
                       "for-in loop exceeded the configured iteration limit (%zu)",
                       evaluator->options.maximum_loop_iterations);
    environment_destroy(&loop_scope);
    return usk_flow_normal();
}

static UskFlow execute_switch(UskEvaluator *evaluator, Environment *environment,
                              const UskAstStmt *statement) {
    Value selector = usk_eval_expression(evaluator, environment,
                                         statement->as.switch_stmt.selector);
    const UskAstSwitchCase *default_case = NULL;
    const UskAstSwitchCase *selected = NULL;
    for (const UskAstSwitchCase *item = statement->as.switch_stmt.cases;
         item && !evaluator->failed; item = item->next) {
        if (item->is_default) { default_case = item; continue; }
        for (const UskAstExprList *label = item->labels; label; label = label->next) {
            Value candidate = usk_eval_expression(evaluator, environment,
                                                   label->expression);
            if (value_equal(selector, candidate)) { selected = item; break; }
        }
        if (selected) break;
    }
    if (!selected) selected = default_case;
    if (!selected) return usk_flow_normal();
    evaluator->switch_depth++;
    UskFlow flow = execute_statement_list(evaluator, environment,
                                          selected->statements);
    evaluator->switch_depth--;
    if (flow.kind == USK_FLOW_BREAK) return usk_flow_normal();
    return flow;
}

UskFlow usk_eval_statement(UskEvaluator *evaluator, Environment *environment,
                           const UskAstStmt *statement) {
    if (!statement || evaluator->failed) return usk_flow_normal();
    switch (statement->kind) {
        case USK_STMT_EMPTY:
            return usk_flow_normal();
        case USK_STMT_BLOCK:
            return execute_block(evaluator, environment, statement);
        case USK_STMT_VARIABLE: {
            Value value = statement->as.variable.initializer
                ? usk_eval_expression(evaluator, environment,
                    statement->as.variable.initializer)
                : usk_default_value_for_type(evaluator,
                    statement->as.variable.type);
            const UskAstType *type = statement->as.variable.type;
            if (!value_matches_ast_type(type, value)) {
                usk_eval_error(evaluator, USK_DIAG_TYPE_MISMATCH, statement->span,
                               "initializer for '%s' does not match type %s",
                               statement->as.variable.name,
                               type && type->name ? type->name : "USKAuto");
                return usk_flow_normal();
            }
            if (define_typed_var(environment, statement->as.variable.name, value,
                             type && type->is_const,
                             type && !type->array_dimensions ? type->name : NULL)
                != USK_ENV_ASSIGN_OK) {
                usk_eval_error(evaluator, USK_DIAG_OUT_OF_MEMORY,
                               statement->span,
                               "cannot allocate local variable '%s'",
                               statement->as.variable.name);
            }
            return usk_flow_normal();
        }
        case USK_STMT_EXPRESSION:
            usk_eval_expression(evaluator, environment, statement->as.expression);
            return usk_flow_normal();
        case USK_STMT_IF: {
            Value condition = usk_eval_expression(evaluator, environment,
                                                   statement->as.if_stmt.condition);
            if (truthy(condition))
                return usk_eval_statement(evaluator, environment,
                                          statement->as.if_stmt.then_branch);
            if (statement->as.if_stmt.else_branch)
                return usk_eval_statement(evaluator, environment,
                                          statement->as.if_stmt.else_branch);
            return usk_flow_normal();
        }
        case USK_STMT_WHILE:
            return execute_while(evaluator, environment, statement);
        case USK_STMT_FOR:
            return execute_for(evaluator, environment, statement);
        case USK_STMT_FOREACH:
            return execute_foreach(evaluator, environment, statement);
        case USK_STMT_SWITCH:
            return execute_switch(evaluator, environment, statement);
        case USK_STMT_BREAK:
            if (!evaluator->loop_depth && !evaluator->switch_depth) {
                usk_eval_error(evaluator, USK_DIAG_INVALID_CONTROL_FLOW, statement->span,
                               "break is only valid inside a loop or switch");
                return usk_flow_normal();
            }
            return usk_flow_make(USK_FLOW_BREAK, null_value());
        case USK_STMT_CONTINUE:
            if (!evaluator->loop_depth) {
                usk_eval_error(evaluator, USK_DIAG_INVALID_CONTROL_FLOW, statement->span,
                               "continue is only valid inside a loop");
                return usk_flow_normal();
            }
            return usk_flow_make(USK_FLOW_CONTINUE, null_value());
        case USK_STMT_RETURN:
            return usk_flow_make(USK_FLOW_RETURN,
                statement->as.return_value
                    ? usk_eval_expression(evaluator, environment,
                        statement->as.return_value)
                    : null_value());
    }
    usk_eval_error(evaluator, USK_DIAG_INTERNAL_ERROR, statement->span,
                   "unknown AST statement kind");
    return usk_flow_normal();
}
