#ifndef USK_ANALYZER_INTERNAL_H
#define USK_ANALYZER_INTERNAL_H

#include "usk/sema.h"
#include "usk/typecheck.h"

typedef enum {
    USK_SYMBOL_VARIABLE,
    USK_SYMBOL_FUNCTION,
    USK_SYMBOL_TYPE,
    USK_SYMBOL_ENUM_VALUE
} UskSymbolKind;

typedef struct {
    UskTypeKind kind;
    const char *name;
    size_t array_dimensions;
    bool is_const;
    bool is_signed;
    bool is_long;
    bool is_short;
} UskSemanticType;

typedef struct UskSemanticSymbol {
    const char *name;
    UskSymbolKind kind;
    UskSemanticType type;
    const UskAstDecl *declaration;
    bool is_initialized;
    bool owns_name;
    struct UskSemanticSymbol *next;
} UskSemanticSymbol;

typedef struct UskSemanticScope {
    UskSemanticSymbol *symbols;
    struct UskSemanticScope *parent;
} UskSemanticScope;

typedef struct {
    const UskAstProgram *program;
    const char *source_name;
    UskDiagnosticList *diagnostics;
    UskSemanticResult result;
    UskSemanticScope global_scope;
    UskSemanticScope *scope;
    const UskAstDecl *current_function;
    const char *current_owner;
    size_t loop_depth;
    size_t switch_depth;
} UskSemanticAnalyzer;

void usk_sema_error(UskSemanticAnalyzer *analyzer, UskDiagnosticCode code,
                    UskSourceSpan span, const char *format, ...);
UskSemanticType usk_sema_type_from_ast(const UskAstType *type);
UskSemanticType usk_sema_resolve_ast_type(UskSemanticAnalyzer *analyzer,
                                          const UskAstType *type,
                                          UskSourceSpan span);
bool usk_sema_types_compatible(UskSemanticType target, UskSemanticType value);
UskSemanticSymbol *usk_sema_lookup(UskSemanticAnalyzer *analyzer,
                                   const char *name);
UskSemanticSymbol *usk_sema_find_function(UskSemanticAnalyzer *analyzer,
                                          const char *name);
UskSemanticSymbol *usk_sema_add_symbol(UskSemanticAnalyzer *analyzer,
                                       UskSemanticScope *scope,
                                       const char *name,
                                       UskSymbolKind kind,
                                       UskSemanticType type,
                                       const UskAstDecl *declaration,
                                       bool owns_name,
                                       UskSourceSpan span);
UskSemanticType usk_sema_expression(UskSemanticAnalyzer *analyzer,
                                   const UskAstExpr *expression);
void usk_sema_statement(UskSemanticAnalyzer *analyzer,
                        const UskAstStmt *statement);
bool usk_sema_statement_returns(const UskAstStmt *statement);
void usk_sema_declarations(UskSemanticAnalyzer *analyzer,
                           const UskAstDecl *declarations,
                           const char *owner);
void usk_sema_register_declarations(UskSemanticAnalyzer *analyzer,
                                    const UskAstDecl *declarations,
                                    const char *owner);
void usk_sema_destroy_scope(UskSemanticScope *scope);

#endif
