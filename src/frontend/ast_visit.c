#include "ast_visit_internal.h"

#include <string.h>

void usk_ast_visitor_init(UskAstVisitor *visitor) {
    if (!visitor) return;
    memset(visitor, 0, sizeof(*visitor));
    visitor->maximum_nodes = 1000000;
    visitor->maximum_depth = 1024;
}

void usk_ast_visit_stats_clear(UskAstVisitStats *stats) {
    if (stats) memset(stats, 0, sizeof(*stats));
}

static UskAstVisitChildren record_node(UskAstVisitContext *context,
                                      const void *node, unsigned depth) {
    if (!context || !context->visitor || !context->stats || !node ||
        context->status != USK_AST_VISIT_COMPLETE)
        return USK_AST_VISIT_CHILDREN_ABORT;
    if (context->visitor->maximum_depth &&
        depth > context->visitor->maximum_depth) {
        context->status = USK_AST_VISIT_DEPTH_LIMIT;
        return USK_AST_VISIT_CHILDREN_ABORT;
    }
    if (context->visitor->maximum_nodes &&
        context->stats->visited_nodes >= context->visitor->maximum_nodes) {
        context->status = USK_AST_VISIT_NODE_LIMIT;
        return USK_AST_VISIT_CHILDREN_ABORT;
    }
    context->stats->visited_nodes++;
    if (depth > context->stats->maximum_depth_reached)
        context->stats->maximum_depth_reached = depth;
    return USK_AST_VISIT_CHILDREN_DESCEND;
}

static UskAstVisitChildren apply_decision(UskAstVisitContext *context,
                                          UskAstVisitDecision decision) {
    if (decision == USK_AST_VISIT_STOP) {
        context->status = USK_AST_VISIT_STOPPED;
        return USK_AST_VISIT_CHILDREN_ABORT;
    }
    return decision == USK_AST_VISIT_SKIP_CHILDREN
        ? USK_AST_VISIT_CHILDREN_SKIP : USK_AST_VISIT_CHILDREN_DESCEND;
}

UskAstVisitChildren usk_ast_visit_enter_expression(
    UskAstVisitContext *context, const UskAstExpr *node, unsigned depth) {
    UskAstVisitChildren result = record_node(context, node, depth);
    if (result != USK_AST_VISIT_CHILDREN_DESCEND) return result;
    if (!context->visitor->expression) return result;
    return apply_decision(context, context->visitor->expression(
        node, depth, context->visitor->user_data));
}

UskAstVisitChildren usk_ast_visit_enter_statement(
    UskAstVisitContext *context, const UskAstStmt *node, unsigned depth) {
    UskAstVisitChildren result = record_node(context, node, depth);
    if (result != USK_AST_VISIT_CHILDREN_DESCEND) return result;
    if (!context->visitor->statement) return result;
    return apply_decision(context, context->visitor->statement(
        node, depth, context->visitor->user_data));
}

UskAstVisitChildren usk_ast_visit_enter_declaration(
    UskAstVisitContext *context, const UskAstDecl *node, unsigned depth) {
    UskAstVisitChildren result = record_node(context, node, depth);
    if (result != USK_AST_VISIT_CHILDREN_DESCEND) return result;
    if (!context->visitor->declaration) return result;
    return apply_decision(context, context->visitor->declaration(
        node, depth, context->visitor->user_data));
}

UskAstVisitChildren usk_ast_visit_enter_type(
    UskAstVisitContext *context, const UskAstType *node, unsigned depth) {
    UskAstVisitChildren result = record_node(context, node, depth);
    if (result != USK_AST_VISIT_CHILDREN_DESCEND) return result;
    if (!context->visitor->type) return result;
    return apply_decision(context, context->visitor->type(
        node, depth, context->visitor->user_data));
}

bool usk_ast_visit_type_inner(UskAstVisitContext *context,
                              const UskAstType *node, unsigned depth) {
    return node && usk_ast_visit_enter_type(context, node, depth) !=
                   USK_AST_VISIT_CHILDREN_ABORT;
}

UskAstVisitStatus usk_ast_visit_finish(UskAstVisitContext *context) {
    return context ? context->status : USK_AST_VISIT_INVALID_ARGUMENT;
}

static UskAstVisitContext begin_visit(const UskAstVisitor *visitor,
                                      UskAstVisitStats *stats,
                                      UskAstVisitStats *scratch) {
    UskAstVisitStats *active_stats = stats ? stats : scratch;
    usk_ast_visit_stats_clear(active_stats);
    UskAstVisitContext context = {
        .visitor = visitor,
        .stats = active_stats,
        .status = visitor && active_stats ? USK_AST_VISIT_COMPLETE
                                   : USK_AST_VISIT_INVALID_ARGUMENT
    };
    return context;
}

UskAstVisitStatus usk_ast_visit_program(const UskAstProgram *program,
                                       const UskAstVisitor *visitor,
                                       UskAstVisitStats *stats) {
    UskAstVisitStats scratch;
    UskAstVisitContext context = begin_visit(visitor, stats, &scratch);
    if (!program || context.status != USK_AST_VISIT_COMPLETE)
        return USK_AST_VISIT_INVALID_ARGUMENT;
    for (const UskAstDecl *declaration = program->declarations;
         declaration && context.status == USK_AST_VISIT_COMPLETE;
         declaration = declaration->next)
        usk_ast_visit_declaration_inner(&context, declaration, 1);
    return usk_ast_visit_finish(&context);
}

UskAstVisitStatus usk_ast_visit_declaration(const UskAstDecl *declaration,
                                            const UskAstVisitor *visitor,
                                            UskAstVisitStats *stats) {
    UskAstVisitStats scratch;
    UskAstVisitContext context = begin_visit(visitor, stats, &scratch);
    if (!declaration || context.status != USK_AST_VISIT_COMPLETE)
        return USK_AST_VISIT_INVALID_ARGUMENT;
    usk_ast_visit_declaration_inner(&context, declaration, 1);
    return usk_ast_visit_finish(&context);
}

UskAstVisitStatus usk_ast_visit_statement(const UskAstStmt *statement,
                                          const UskAstVisitor *visitor,
                                          UskAstVisitStats *stats) {
    UskAstVisitStats scratch;
    UskAstVisitContext context = begin_visit(visitor, stats, &scratch);
    if (!statement || context.status != USK_AST_VISIT_COMPLETE)
        return USK_AST_VISIT_INVALID_ARGUMENT;
    usk_ast_visit_statement_inner(&context, statement, 1);
    return usk_ast_visit_finish(&context);
}

UskAstVisitStatus usk_ast_visit_expression(const UskAstExpr *expression,
                                           const UskAstVisitor *visitor,
                                           UskAstVisitStats *stats) {
    UskAstVisitStats scratch;
    UskAstVisitContext context = begin_visit(visitor, stats, &scratch);
    if (!expression || context.status != USK_AST_VISIT_COMPLETE)
        return USK_AST_VISIT_INVALID_ARGUMENT;
    usk_ast_visit_expression_inner(&context, expression, 1);
    return usk_ast_visit_finish(&context);
}

const char *usk_ast_visit_status_name(UskAstVisitStatus status) {
    switch (status) {
        case USK_AST_VISIT_COMPLETE: return "complete";
        case USK_AST_VISIT_STOPPED: return "stopped";
        case USK_AST_VISIT_NODE_LIMIT: return "node-limit";
        case USK_AST_VISIT_DEPTH_LIMIT: return "depth-limit";
        case USK_AST_VISIT_INVALID_ARGUMENT: return "invalid-argument";
    }
    return "unknown-visit-status";
}

const char *usk_ast_visit_decision_name(UskAstVisitDecision decision) {
    switch (decision) {
        case USK_AST_VISIT_CONTINUE: return "continue";
        case USK_AST_VISIT_SKIP_CHILDREN: return "skip-children";
        case USK_AST_VISIT_STOP: return "stop";
    }
    return "unknown-visit-decision";
}
