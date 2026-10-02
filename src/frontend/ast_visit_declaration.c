#include "ast_visit_internal.h"

#include <string.h>

const char *usk_ast_decl_kind_name(UskAstDeclKind kind) {
    static const char *names[] = {
        "function", "class", "struct", "enum", "variable", "typedef",
        "extern", "import", "directive"
    };
    return kind >= USK_DECL_FUNCTION && kind <= USK_DECL_DIRECTIVE
        ? names[kind] : "unknown-declaration";
}

const char *usk_ast_decl_name(const UskAstDecl *declaration) {
    if (!declaration) return NULL;
    switch (declaration->kind) {
        case USK_DECL_FUNCTION: return declaration->as.function.name;
        case USK_DECL_CLASS:
        case USK_DECL_STRUCT: return declaration->as.record.name;
        case USK_DECL_ENUM: return declaration->as.enumeration.name;
        case USK_DECL_VARIABLE: return declaration->as.variable.name;
        case USK_DECL_TYPEDEF: return declaration->as.alias.name;
        case USK_DECL_EXTERN:
        case USK_DECL_IMPORT:
        case USK_DECL_DIRECTIVE: return NULL;
    }
    return NULL;
}

bool usk_ast_decl_is_callable(const UskAstDecl *declaration) {
    return declaration && declaration->kind == USK_DECL_FUNCTION;
}

bool usk_ast_decl_is_aggregate(const UskAstDecl *declaration) {
    return declaration && (declaration->kind == USK_DECL_CLASS ||
        declaration->kind == USK_DECL_STRUCT || declaration->kind == USK_DECL_ENUM);
}

bool usk_ast_decl_has_body(const UskAstDecl *declaration) {
    return usk_ast_decl_is_callable(declaration) &&
           declaration->as.function.body != NULL;
}

void usk_ast_declaration_metrics_clear(UskAstDeclarationMetrics *metrics) {
    if (metrics) memset(metrics, 0, sizeof(*metrics));
}

static UskAstVisitDecision measure_declaration_callback(
    const UskAstDecl *declaration, unsigned depth, void *user_data) {
    UskAstDeclarationMetrics *metrics = (UskAstDeclarationMetrics *)user_data;
    if (!declaration || !metrics || declaration->kind < USK_DECL_FUNCTION ||
        declaration->kind > USK_DECL_DIRECTIVE)
        return USK_AST_VISIT_STOP;
    metrics->by_kind[declaration->kind]++;
    metrics->total++;
    if (declaration->kind == USK_DECL_FUNCTION) metrics->functions++;
    if (declaration->kind == USK_DECL_CLASS ||
        declaration->kind == USK_DECL_STRUCT) metrics->records++;
    if (declaration->kind == USK_DECL_ENUM) metrics->enumerations++;
    if (declaration->kind == USK_DECL_VARIABLE) metrics->variables++;
    if (depth > metrics->maximum_depth) metrics->maximum_depth = depth;
    return USK_AST_VISIT_CONTINUE;
}

UskAstVisitStatus usk_ast_measure_declarations(
    const UskAstProgram *program, UskAstDeclarationMetrics *metrics) {
    if (!program || !metrics) return USK_AST_VISIT_INVALID_ARGUMENT;
    usk_ast_declaration_metrics_clear(metrics);
    UskAstVisitor visitor;
    usk_ast_visitor_init(&visitor);
    visitor.declaration = measure_declaration_callback;
    visitor.user_data = metrics;
    return usk_ast_visit_program(program, &visitor, NULL);
}

static bool visit_type(UskAstVisitContext *context, const UskAstType *type,
                       unsigned depth) {
    return !type || usk_ast_visit_type_inner(context, type, depth);
}

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

bool usk_ast_visit_declaration_inner(UskAstVisitContext *context,
                                     const UskAstDecl *node, unsigned depth) {
    if (!node) return true;
    UskAstVisitChildren decision = usk_ast_visit_enter_declaration(
        context, node, depth);
    if (decision == USK_AST_VISIT_CHILDREN_ABORT) return false;
    if (decision == USK_AST_VISIT_CHILDREN_SKIP) return true;
    unsigned child_depth = depth + 1;

    switch (node->kind) {
        case USK_DECL_FUNCTION:
            if (!visit_type(context, node->as.function.return_type,
                            child_depth)) return false;
            for (const UskAstParameter *parameter =
                     node->as.function.parameters;
                 parameter && context->status == USK_AST_VISIT_COMPLETE;
                 parameter = parameter->next) {
                if (!visit_type(context, parameter->type, child_depth))
                    return false;
                if (!visit_expression(context, parameter->default_value,
                                      child_depth)) return false;
            }
            return visit_statement(context, node->as.function.body,
                                   child_depth);
        case USK_DECL_CLASS:
        case USK_DECL_STRUCT:
            for (const UskAstDecl *member = node->as.record.members;
                 member && context->status == USK_AST_VISIT_COMPLETE;
                 member = member->next)
                if (!usk_ast_visit_declaration_inner(context, member,
                                                     child_depth)) return false;
            return context->status == USK_AST_VISIT_COMPLETE;
        case USK_DECL_ENUM:
            for (const UskAstEnumValue *value = node->as.enumeration.values;
                 value && context->status == USK_AST_VISIT_COMPLETE;
                 value = value->next)
                if (!visit_expression(context, value->value, child_depth))
                    return false;
            return context->status == USK_AST_VISIT_COMPLETE;
        case USK_DECL_VARIABLE:
            if (!visit_type(context, node->as.variable.type, child_depth))
                return false;
            return visit_expression(context, node->as.variable.initializer,
                                    child_depth);
        case USK_DECL_TYPEDEF:
            return visit_type(context, node->as.alias.type, child_depth);
        case USK_DECL_EXTERN:
        case USK_DECL_IMPORT:
        case USK_DECL_DIRECTIVE:
            return true;
    }
    context->status = USK_AST_VISIT_INVALID_ARGUMENT;
    return false;
}
