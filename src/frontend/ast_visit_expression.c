#include "ast_visit_internal.h"

#include <string.h>

const char *usk_ast_expr_kind_name(UskAstExprKind kind) {
    static const char *names[] = {
        "null", "boolean", "integer", "double", "string", "name",
        "array", "unary", "binary", "assignment", "call", "member",
        "index", "conditional"
    };
    return kind >= USK_EXPR_NULL && kind <= USK_EXPR_CONDITIONAL
        ? names[kind] : "unknown-expression";
}

bool usk_ast_expr_is_literal(const UskAstExpr *expression) {
    if (!expression) return false;
    return expression->kind >= USK_EXPR_NULL &&
           expression->kind <= USK_EXPR_STRING;
}

bool usk_ast_expr_is_numeric(const UskAstExpr *expression) {
    return expression && (expression->kind == USK_EXPR_INTEGER ||
                          expression->kind == USK_EXPR_DOUBLE);
}

bool usk_ast_expr_is_lvalue(const UskAstExpr *expression) {
    if (!expression) return false;
    return expression->kind == USK_EXPR_NAME ||
           expression->kind == USK_EXPR_MEMBER ||
           expression->kind == USK_EXPR_INDEX;
}

void usk_ast_expression_metrics_clear(UskAstExpressionMetrics *metrics) {
    if (metrics) memset(metrics, 0, sizeof(*metrics));
}

static UskAstVisitDecision measure_expression_callback(
    const UskAstExpr *expression, unsigned depth, void *user_data) {
    UskAstExpressionMetrics *metrics = (UskAstExpressionMetrics *)user_data;
    if (!expression || !metrics || expression->kind < USK_EXPR_NULL ||
        expression->kind > USK_EXPR_CONDITIONAL)
        return USK_AST_VISIT_STOP;
    metrics->by_kind[expression->kind]++;
    metrics->total++;
    if (usk_ast_expr_is_literal(expression)) metrics->literals++;
    if (expression->kind == USK_EXPR_CALL) metrics->calls++;
    if (expression->kind == USK_EXPR_ASSIGNMENT) metrics->assignments++;
    if (expression->kind == USK_EXPR_MEMBER) metrics->member_accesses++;
    if (depth > metrics->maximum_depth) metrics->maximum_depth = depth;
    return USK_AST_VISIT_CONTINUE;
}

UskAstVisitStatus usk_ast_measure_expression(
    const UskAstExpr *expression, UskAstExpressionMetrics *metrics) {
    if (!expression || !metrics) return USK_AST_VISIT_INVALID_ARGUMENT;
    usk_ast_expression_metrics_clear(metrics);
    UskAstVisitor visitor;
    usk_ast_visitor_init(&visitor);
    visitor.expression = measure_expression_callback;
    visitor.user_data = metrics;
    return usk_ast_visit_expression(expression, &visitor, NULL);
}

static bool visit_expression_list(UskAstVisitContext *context,
                                  const UskAstExprList *item,
                                  size_t expected_count,
                                  unsigned depth) {
    size_t observed = 0;
    const UskAstExprList *slow = item;
    const UskAstExprList *fast = item;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) {
            context->status = USK_AST_VISIT_INVALID_ARGUMENT;
            return false;
        }
    }
    for (; item && context->status == USK_AST_VISIT_COMPLETE; item = item->next) {
        observed++;
        if (observed > expected_count) {
            context->status = USK_AST_VISIT_INVALID_ARGUMENT;
            return false;
        }
        if (item->expression)
            usk_ast_visit_expression_inner(context, item->expression, depth);
        else {
            context->status = USK_AST_VISIT_INVALID_ARGUMENT;
            return false;
        }
    }
    if (observed != expected_count) {
        context->status = USK_AST_VISIT_INVALID_ARGUMENT;
        return false;
    }
    return context->status == USK_AST_VISIT_COMPLETE;
}

bool usk_ast_visit_expression_inner(UskAstVisitContext *context,
                                    const UskAstExpr *node, unsigned depth) {
    if (!node) return true;
    UskAstVisitChildren decision = usk_ast_visit_enter_expression(
        context, node, depth);
    if (decision == USK_AST_VISIT_CHILDREN_ABORT) return false;
    if (decision == USK_AST_VISIT_CHILDREN_SKIP) return true;
    unsigned child_depth = depth + 1;

    switch (node->kind) {
        case USK_EXPR_NULL:
        case USK_EXPR_BOOLEAN:
        case USK_EXPR_INTEGER:
        case USK_EXPR_DOUBLE:
        case USK_EXPR_STRING:
        case USK_EXPR_NAME:
            return true;
        case USK_EXPR_ARRAY:
            return visit_expression_list(context, node->as.array.items,
                                         node->as.array.count,
                                         child_depth);
        case USK_EXPR_UNARY:
            return usk_ast_visit_expression_inner(context,
                node->as.unary.operand, child_depth);
        case USK_EXPR_BINARY:
            if (!usk_ast_visit_expression_inner(context,
                    node->as.binary.left, child_depth)) return false;
            return usk_ast_visit_expression_inner(context,
                node->as.binary.right, child_depth);
        case USK_EXPR_ASSIGNMENT:
            if (!usk_ast_visit_expression_inner(context,
                    node->as.assignment.target, child_depth)) return false;
            return usk_ast_visit_expression_inner(context,
                node->as.assignment.value, child_depth);
        case USK_EXPR_CALL:
            if (!usk_ast_visit_expression_inner(context,
                    node->as.call.callee, child_depth)) return false;
            return visit_expression_list(context, node->as.call.arguments,
                                         node->as.call.count,
                                         child_depth);
        case USK_EXPR_MEMBER:
            return usk_ast_visit_expression_inner(context,
                node->as.member.object, child_depth);
        case USK_EXPR_INDEX:
            if (!usk_ast_visit_expression_inner(context,
                    node->as.index.object, child_depth)) return false;
            return usk_ast_visit_expression_inner(context,
                node->as.index.index, child_depth);
        case USK_EXPR_CONDITIONAL:
            if (!usk_ast_visit_expression_inner(context,
                    node->as.conditional.condition, child_depth)) return false;
            if (!usk_ast_visit_expression_inner(context,
                    node->as.conditional.when_true, child_depth)) return false;
            return usk_ast_visit_expression_inner(context,
                node->as.conditional.when_false, child_depth);
    }
    context->status = USK_AST_VISIT_INVALID_ARGUMENT;
    return false;
}
