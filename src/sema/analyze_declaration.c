#include "analyzer_internal.h"

#include <string.h>

static bool semantic_type_matches(const UskAstType *target,
                                  UskSemanticType source,
                                  UskSemanticAnalyzer *analyzer,
                                  UskSourceSpan span) {
    return usk_sema_types_compatible(
        usk_sema_resolve_ast_type(analyzer, target, span), source);
}

static void analyze_function(UskSemanticAnalyzer *analyzer,
                             const UskAstDecl *declaration,
                             const char *owner) {
    const UskAstDecl *previous_function = analyzer->current_function;
    const char *previous_owner = analyzer->current_owner;
    UskSemanticScope local = {.parent = &analyzer->global_scope};
    analyzer->scope = &local;
    analyzer->current_function = declaration;
    analyzer->current_owner = owner;
    usk_sema_resolve_ast_type(analyzer,
        declaration->as.function.return_type, declaration->span);

    for (const UskAstParameter *parameter = declaration->as.function.parameters;
         parameter; parameter = parameter->next) {
        UskSemanticType type = usk_sema_resolve_ast_type(analyzer,
            parameter->type, declaration->span);
        UskSemanticSymbol *symbol = usk_sema_add_symbol(analyzer, &local,
            parameter->name, USK_SYMBOL_VARIABLE, type, NULL, false,
            declaration->span);
        if (symbol) symbol->is_initialized = true;
        if (parameter->default_value) {
            UskSemanticType default_type = usk_sema_expression(analyzer,
                parameter->default_value);
            if (!semantic_type_matches(parameter->type, default_type,
                                       analyzer, declaration->span))
                usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
                    parameter->default_value->span,
                    "default value for parameter '%s' does not match %s",
                    parameter->name, parameter->type->name);
        }
    }

    if (declaration->as.function.body) {
        usk_sema_statement(analyzer, declaration->as.function.body);
        UskSemanticType return_type = usk_sema_resolve_ast_type(analyzer,
            declaration->as.function.return_type, declaration->span);
        if (return_type.kind != USK_TYPE_NULL &&
            return_type.kind != USK_TYPE_UNKNOWN &&
            !usk_sema_statement_returns(declaration->as.function.body))
            usk_sema_error(analyzer, USK_DIAG_INVALID_CONTROL_FLOW,
                declaration->span,
                "not all paths in function '%s' return a value",
                declaration->as.function.name);
    }
    analyzer->result.declarations_checked++;
    usk_sema_destroy_scope(&local);
    analyzer->scope = &analyzer->global_scope;
    analyzer->current_function = previous_function;
    analyzer->current_owner = previous_owner;
}

static void analyze_global_variable(UskSemanticAnalyzer *analyzer,
                                    const UskAstDecl *declaration,
                                    const char *owner) {
    UskSemanticType declared = usk_sema_resolve_ast_type(analyzer,
        declaration->as.variable.type, declaration->span);
    if (!declaration->as.variable.initializer) return;
    UskSemanticType initializer_type = usk_sema_expression(analyzer,
        declaration->as.variable.initializer);
    if (!usk_sema_types_compatible(declared, initializer_type))
        usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
            declaration->as.variable.initializer->span,
            "initializer for '%s%s%s' has incompatible type",
            owner ? owner : "", owner ? "::" : "",
            declaration->as.variable.name);
}

static void analyze_enum(UskSemanticAnalyzer *analyzer,
                         const UskAstDecl *declaration) {
    for (const UskAstEnumValue *item = declaration->as.enumeration.values;
         item; item = item->next) {
        if (!item->value) continue;
        UskSemanticType type = usk_sema_expression(analyzer, item->value);
        if (type.kind != USK_TYPE_INTEGER && type.kind != USK_TYPE_UNKNOWN)
            usk_sema_error(analyzer, USK_DIAG_TYPE_MISMATCH,
                item->value->span,
                "enum value '%s' must be an integer constant", item->name);
    }
}

void usk_sema_declarations(UskSemanticAnalyzer *analyzer,
                           const UskAstDecl *declarations,
                           const char *owner) {
    for (const UskAstDecl *declaration = declarations; declaration;
         declaration = declaration->next) {
        switch (declaration->kind) {
            case USK_DECL_FUNCTION:
                analyze_function(analyzer, declaration, owner);
                break;
            case USK_DECL_CLASS:
            case USK_DECL_STRUCT: {
                const char *previous_owner = analyzer->current_owner;
                analyzer->current_owner = declaration->as.record.name;
                usk_sema_declarations(analyzer, declaration->as.record.members,
                                      declaration->as.record.name);
                analyzer->current_owner = previous_owner;
                analyzer->result.declarations_checked++;
                break;
            }
            case USK_DECL_ENUM:
                analyze_enum(analyzer, declaration);
                analyzer->result.declarations_checked++;
                break;
            case USK_DECL_VARIABLE:
                analyze_global_variable(analyzer, declaration, owner);
                analyzer->result.declarations_checked++;
                break;
            case USK_DECL_TYPEDEF:
                usk_sema_resolve_ast_type(analyzer,
                    declaration->as.alias.type, declaration->span);
                analyzer->result.declarations_checked++;
                break;
            case USK_DECL_EXTERN:
            case USK_DECL_IMPORT:
            case USK_DECL_DIRECTIVE:
                break;
        }
    }
}
