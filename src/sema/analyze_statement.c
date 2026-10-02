#include "analyzer_internal.h"

#include <string.h>

static bool unknown_type(UskSemanticType type) {
    return type.kind == USK_TYPE_UNKNOWN;
}

static void require_condition(UskSemanticAnalyzer *analyzer,
                              const UskAstExpr *expression,
                              const char *construct) {
    UskSemanticType type = usk_sema_expression(analyzer, expression);
    if (type.kind != USK_TYPE_BOOLEAN && !unknown_type(type))
        usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
            expression ? expression->span : (UskSourceSpan){0},
            "%s condition must have type USKBool", construct);
}

static void analyze_local_variable(UskSemanticAnalyzer *analyzer,
                                   const UskAstStmt *statement) {
    UskSemanticType declared = usk_sema_resolve_ast_type(analyzer,
        statement->as.variable.type, statement->span);
    UskSemanticType initializer = {USK_TYPE_UNKNOWN, "USKAuto", 0, false,
                                   true, false, false};
    if (statement->as.variable.initializer) {
        initializer = usk_sema_expression(analyzer,
            statement->as.variable.initializer);
        if (declared.kind != USK_TYPE_UNKNOWN &&
            !usk_sema_types_compatible(declared, initializer))
            usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
                statement->as.variable.initializer->span,
                "initializer for '%s' is incompatible with %s",
                statement->as.variable.name,
                statement->as.variable.type->name);
    }
    if (declared.kind == USK_TYPE_UNKNOWN && initializer.kind != USK_TYPE_UNKNOWN)
        declared = initializer;
    UskSemanticSymbol *symbol = usk_sema_add_symbol(analyzer, analyzer->scope,
        statement->as.variable.name, USK_SYMBOL_VARIABLE, declared, NULL, false,
        statement->span);
    if (symbol) symbol->is_initialized = statement->as.variable.initializer != NULL;
}

static void analyze_switch(UskSemanticAnalyzer *analyzer,
                           const UskAstStmt *statement) {
    UskSemanticType selector = usk_sema_expression(analyzer,
        statement->as.switch_stmt.selector);
    size_t default_count = 0;
    for (const UskAstSwitchCase *item = statement->as.switch_stmt.cases;
         item; item = item->next) {
        if (item->is_default) {
            if (++default_count > 1)
                usk_sema_error(analyzer, USK_DIAG_DUPLICATE_NAME, item->span,
                               "switch statement has more than one default arm");
        }
        for (const UskAstExprList *label = item->labels; label; label = label->next) {
            UskSemanticType label_type = usk_sema_expression(analyzer,
                label->expression);
            if (!usk_sema_types_compatible(selector, label_type))
                usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
                    label->expression->span,
                    "case label is incompatible with the switch selector");
        }
        UskSemanticScope arm = {.parent = analyzer->scope};
        UskSemanticScope *previous = analyzer->scope;
        analyzer->scope = &arm;
        for (const UskAstStmtList *line = item->statements; line; line = line->next)
            usk_sema_statement(analyzer, line->statement);
        analyzer->scope = previous;
        usk_sema_destroy_scope(&arm);
    }
}

static void analyze_for(UskSemanticAnalyzer *analyzer,
                        const UskAstStmt *statement) {
    UskSemanticScope loop_scope = {.parent = analyzer->scope};
    UskSemanticScope *previous = analyzer->scope;
    analyzer->scope = &loop_scope;
    if (statement->as.for_stmt.initializer)
        usk_sema_statement(analyzer, statement->as.for_stmt.initializer);
    if (statement->as.for_stmt.condition)
        require_condition(analyzer, statement->as.for_stmt.condition, "for");
    if (statement->as.for_stmt.update)
        usk_sema_expression(analyzer, statement->as.for_stmt.update);
    analyzer->loop_depth++;
    usk_sema_statement(analyzer, statement->as.for_stmt.body);
    analyzer->loop_depth--;
    analyzer->scope = previous;
    usk_sema_destroy_scope(&loop_scope);
}

static void analyze_foreach(UskSemanticAnalyzer *analyzer,
                            const UskAstStmt *statement) {
    UskSemanticType collection = usk_sema_expression(analyzer,
        statement->as.foreach_stmt.collection);
    UskSemanticType element = collection;
    if (element.array_dimensions) {
        element.array_dimensions--;
    } else if (element.kind == USK_TYPE_STRING) {
        element.kind = USK_TYPE_CHARACTER;
        element.name = "USKChar";
    } else if (!unknown_type(element)) {
        usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
            statement->as.foreach_stmt.collection->span,
            "for-in collection must be an array or USKString");
    }
    UskSemanticType declared = usk_sema_resolve_ast_type(analyzer,
        statement->as.foreach_stmt.type, statement->span);
    if (declared.kind == USK_TYPE_UNKNOWN) declared = element;
    else if (!usk_sema_types_compatible(declared, element))
        usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH, statement->span,
                       "for-in element is incompatible with variable '%s'",
                       statement->as.foreach_stmt.name);

    UskSemanticScope loop_scope = {.parent = analyzer->scope};
    UskSemanticScope *previous = analyzer->scope;
    analyzer->scope = &loop_scope;
    UskSemanticSymbol *symbol = usk_sema_add_symbol(analyzer, &loop_scope,
        statement->as.foreach_stmt.name, USK_SYMBOL_VARIABLE, declared, NULL,
        false, statement->span);
    if (symbol) symbol->is_initialized = true;
    analyzer->loop_depth++;
    usk_sema_statement(analyzer, statement->as.foreach_stmt.body);
    analyzer->loop_depth--;
    analyzer->scope = previous;
    usk_sema_destroy_scope(&loop_scope);
}

static bool statement_list_returns(const UskAstStmtList *statements) {
    for (const UskAstStmtList *item = statements; item; item = item->next)
        if (usk_sema_statement_returns(item->statement)) return true;
    return false;
}

bool usk_sema_statement_returns(const UskAstStmt *statement) {
    if (!statement) return false;
    switch (statement->kind) {
        case USK_STMT_RETURN:
            return true;
        case USK_STMT_BLOCK:
            return statement_list_returns(statement->as.block.items);
        case USK_STMT_IF:
            return statement->as.if_stmt.else_branch &&
                usk_sema_statement_returns(statement->as.if_stmt.then_branch) &&
                usk_sema_statement_returns(statement->as.if_stmt.else_branch);
        case USK_STMT_SWITCH: {
            bool has_default = false;
            if (!statement->as.switch_stmt.cases) return false;
            for (const UskAstSwitchCase *item = statement->as.switch_stmt.cases;
                 item; item = item->next) {
                if (item->is_default) has_default = true;
                if (!statement_list_returns(item->statements)) return false;
            }
            return has_default;
        }
        case USK_STMT_EMPTY:
        case USK_STMT_VARIABLE:
        case USK_STMT_EXPRESSION:
        case USK_STMT_WHILE:
        case USK_STMT_FOR:
        case USK_STMT_FOREACH:
        case USK_STMT_BREAK:
        case USK_STMT_CONTINUE:
            return false;
    }
    return false;
}

void usk_sema_statement(UskSemanticAnalyzer *analyzer,
                        const UskAstStmt *statement) {
    if (!statement) return;
    switch (statement->kind) {
        case USK_STMT_EMPTY:
            return;
        case USK_STMT_BLOCK: {
            UskSemanticScope block = {.parent = analyzer->scope};
            UskSemanticScope *previous = analyzer->scope;
            analyzer->scope = &block;
            for (const UskAstStmtList *item = statement->as.block.items;
                 item; item = item->next)
                usk_sema_statement(analyzer, item->statement);
            analyzer->scope = previous;
            usk_sema_destroy_scope(&block);
            return;
        }
        case USK_STMT_VARIABLE:
            analyze_local_variable(analyzer, statement);
            return;
        case USK_STMT_EXPRESSION:
            usk_sema_expression(analyzer, statement->as.expression);
            return;
        case USK_STMT_IF:
            require_condition(analyzer, statement->as.if_stmt.condition, "if");
            usk_sema_statement(analyzer, statement->as.if_stmt.then_branch);
            usk_sema_statement(analyzer, statement->as.if_stmt.else_branch);
            return;
        case USK_STMT_WHILE:
            require_condition(analyzer, statement->as.while_stmt.condition, "while");
            analyzer->loop_depth++;
            usk_sema_statement(analyzer, statement->as.while_stmt.body);
            analyzer->loop_depth--;
            return;
        case USK_STMT_FOR:
            analyze_for(analyzer, statement);
            return;
        case USK_STMT_FOREACH:
            analyze_foreach(analyzer, statement);
            return;
        case USK_STMT_SWITCH:
            analyzer->switch_depth++;
            analyze_switch(analyzer, statement);
            analyzer->switch_depth--;
            return;
        case USK_STMT_BREAK:
            if (!analyzer->loop_depth && !analyzer->switch_depth)
                usk_sema_error(analyzer, USK_DIAG_INVALID_CONTROL_FLOW,
                    statement->span, "break is only valid inside a loop or switch");
            return;
        case USK_STMT_CONTINUE:
            if (!analyzer->loop_depth)
                usk_sema_error(analyzer, USK_DIAG_INVALID_CONTROL_FLOW,
                    statement->span, "continue is only valid inside a loop");
            return;
        case USK_STMT_RETURN: {
            if (!analyzer->current_function) {
                usk_sema_error(analyzer, USK_DIAG_INVALID_CONTROL_FLOW,
                    statement->span, "return is only valid inside a function");
                if (statement->as.return_value)
                    usk_sema_expression(analyzer, statement->as.return_value);
                return;
            }
            UskSemanticType expected = usk_sema_resolve_ast_type(analyzer,
                analyzer->current_function->as.function.return_type,
                analyzer->current_function->span);
            if (!statement->as.return_value) {
                if (expected.kind != USK_TYPE_NULL && !unknown_type(expected))
                    usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
                        statement->span,
                        "function returning %s must return a value", expected.name);
            } else {
                UskSemanticType actual = usk_sema_expression(analyzer,
                    statement->as.return_value);
                if (expected.kind == USK_TYPE_NULL)
                    usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
                        statement->as.return_value->span,
                        "USKNull function cannot return a value");
                else if (!usk_sema_types_compatible(expected, actual))
                    usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
                        statement->as.return_value->span,
                        "returned value is incompatible with %s", expected.name);
            }
            return;
        }
    }
}
