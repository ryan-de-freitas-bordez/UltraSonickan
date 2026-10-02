#ifndef USK_AST_VISIT_INTERNAL_H
#define USK_AST_VISIT_INTERNAL_H

#include "usk/ast_visitor.h"

typedef struct {
    const UskAstVisitor *visitor;
    UskAstVisitStats *stats;
    UskAstVisitStatus status;
} UskAstVisitContext;

typedef enum {
    USK_AST_VISIT_CHILDREN_ABORT,
    USK_AST_VISIT_CHILDREN_SKIP,
    USK_AST_VISIT_CHILDREN_DESCEND
} UskAstVisitChildren;

UskAstVisitChildren usk_ast_visit_enter_expression(
    UskAstVisitContext *context, const UskAstExpr *node, unsigned depth);
UskAstVisitChildren usk_ast_visit_enter_statement(
    UskAstVisitContext *context, const UskAstStmt *node, unsigned depth);
UskAstVisitChildren usk_ast_visit_enter_declaration(
    UskAstVisitContext *context, const UskAstDecl *node, unsigned depth);
UskAstVisitChildren usk_ast_visit_enter_type(
    UskAstVisitContext *context, const UskAstType *node, unsigned depth);
UskAstVisitStatus usk_ast_visit_finish(UskAstVisitContext *context);
bool usk_ast_visit_expression_inner(UskAstVisitContext *context,
                                    const UskAstExpr *node, unsigned depth);
bool usk_ast_visit_statement_inner(UskAstVisitContext *context,
                                   const UskAstStmt *node, unsigned depth);
bool usk_ast_visit_declaration_inner(UskAstVisitContext *context,
                                     const UskAstDecl *node, unsigned depth);
bool usk_ast_visit_type_inner(UskAstVisitContext *context,
                              const UskAstType *node, unsigned depth);

#endif
