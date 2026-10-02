#include "ast_visit_internal.h"

static bool visit_expression(UskAstVisitContext *context,
                             const UskAstExpr *expression, unsigned depth) {
    return !expression || usk_ast_visit_expression_inner(context, expression,
                                                          depth);
}

static bool visit_statement(UskAstVisitContext *context,
                            const UskAstStmt *statement, unsigned depth) {
    return !statement || usk_ast_visit_statement_inner(context, statement,
                                                        depth);
}

static bool visit_statement_list(UskAstVisitContext *context,
                                 const UskAstStmtList *item, unsigned depth) {
    for (; item && context->status == USK_AST_VISIT_COMPLETE; item = item->next)
        if (item->statement &&
            !usk_ast_visit_statement_inner(context, item->statement, depth))
            return false;
    return context->status == USK_AST_VISIT_COMPLETE;
}

static bool visit_expression_list(UskAstVisitContext *context,
                                  const UskAstExprList *item,
                                  unsigned depth) {
    for (; item && context->status == USK_AST_VISIT_COMPLETE; item = item->next)
        if (item->expression &&
            !usk_ast_visit_expression_inner(context, item->expression, depth))
            return false;
    return context->status == USK_AST_VISIT_COMPLETE;
}

bool usk_ast_visit_statement_inner(UskAstVisitContext *context,
                                   const UskAstStmt *node, unsigned depth) {
    if (!node) return true;
    UskAstVisitChildren decision = usk_ast_visit_enter_statement(
        context, node, depth);
    if (decision == USK_AST_VISIT_CHILDREN_ABORT) return false;
    if (decision == USK_AST_VISIT_CHILDREN_SKIP) return true;
    unsigned child_depth = depth + 1;

    switch (node->kind) {
        case USK_STMT_EMPTY:
        case USK_STMT_BREAK:
        case USK_STMT_CONTINUE:
            return true;
        case USK_STMT_BLOCK:
            return visit_statement_list(context, node->as.block.items,
                                        child_depth);
        case USK_STMT_VARIABLE:
            if (node->as.variable.type &&
                !usk_ast_visit_type_inner(context, node->as.variable.type,
                                          child_depth)) return false;
            return visit_expression(context, node->as.variable.initializer,
                                    child_depth);
        case USK_STMT_EXPRESSION:
            return visit_expression(context, node->as.expression, child_depth);
        case USK_STMT_IF:
            if (!visit_expression(context, node->as.if_stmt.condition,
                                  child_depth)) return false;
            if (!visit_statement(context, node->as.if_stmt.then_branch,
                                 child_depth)) return false;
            return visit_statement(context, node->as.if_stmt.else_branch,
                                   child_depth);
        case USK_STMT_WHILE:
            if (!visit_expression(context, node->as.while_stmt.condition,
                                  child_depth)) return false;
            return visit_statement(context, node->as.while_stmt.body,
                                   child_depth);
        case USK_STMT_FOR:
            if (!visit_statement(context, node->as.for_stmt.initializer,
                                 child_depth)) return false;
            if (!visit_expression(context, node->as.for_stmt.condition,
                                  child_depth)) return false;
            if (!visit_expression(context, node->as.for_stmt.update,
                                  child_depth)) return false;
            return visit_statement(context, node->as.for_stmt.body,
                                   child_depth);
        case USK_STMT_FOREACH:
            if (node->as.foreach_stmt.type &&
                !usk_ast_visit_type_inner(context,
                    node->as.foreach_stmt.type, child_depth)) return false;
            if (!visit_expression(context, node->as.foreach_stmt.collection,
                                  child_depth)) return false;
            return visit_statement(context, node->as.foreach_stmt.body,
                                   child_depth);
        case USK_STMT_SWITCH:
            if (!visit_expression(context, node->as.switch_stmt.selector,
                                  child_depth)) return false;
            for (const UskAstSwitchCase *item = node->as.switch_stmt.cases;
                 item && context->status == USK_AST_VISIT_COMPLETE;
                 item = item->next) {
                if (!visit_expression_list(context, item->labels,
                                           child_depth)) return false;
                if (!visit_statement_list(context, item->statements,
                                          child_depth)) return false;
            }
            return context->status == USK_AST_VISIT_COMPLETE;
        case USK_STMT_RETURN:
            return visit_expression(context, node->as.return_value,
                                    child_depth);
    }
    context->status = USK_AST_VISIT_INVALID_ARGUMENT;
    return false;
}
