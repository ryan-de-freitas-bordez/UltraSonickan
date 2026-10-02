#include "analyzer_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *qualified_name(const char *owner, const char *name) {
    if (!owner || !owner[0]) {
        size_t size = strlen(name) + 1;
        char *copy = (char *)malloc(size);
        if (copy) memcpy(copy, name, size);
        return copy;
    }
    size_t owner_length = strlen(owner), name_length = strlen(name);
    char *result = (char *)malloc(owner_length + name_length + 3);
    if (!result) return NULL;
    memcpy(result, owner, owner_length);
    memcpy(result + owner_length, "::", 2);
    memcpy(result + owner_length + 2, name, name_length + 1);
    return result;
}

void usk_sema_error(UskSemanticAnalyzer *analyzer, UskDiagnosticCode code,
                    UskSourceSpan span, const char *format, ...) {
    char message[1024];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    usk_diagnostics_add(analyzer->diagnostics, USK_DIAGNOSTIC_ERROR, code,
                        analyzer->source_name, span.line, span.column,
                        "%s", message);
}

UskSemanticType usk_sema_type_from_ast(const UskAstType *ast_type) {
    if (!ast_type) return (UskSemanticType){USK_TYPE_UNKNOWN, "USKAuto", 0, false, true, false, false};
    UskTypeInfo primitive;
    bool known = usk_type_resolve(ast_type->name, &primitive);
    return (UskSemanticType){
        known ? primitive.kind : USK_TYPE_UNKNOWN,
        ast_type->name ? ast_type->name : "USKAuto",
        ast_type->array_dimensions,
        ast_type->is_const,
        ast_type->is_signed,
        ast_type->is_long,
        ast_type->is_short
    };
}

bool usk_sema_types_compatible(UskSemanticType target, UskSemanticType value) {
    if (target.kind == USK_TYPE_UNKNOWN || value.kind == USK_TYPE_UNKNOWN) return true;
    if (target.array_dimensions || value.array_dimensions) {
        if (!target.array_dimensions || !value.array_dimensions) return false;
        if (target.kind == USK_TYPE_UNKNOWN || value.kind == USK_TYPE_UNKNOWN) return true;
        return target.kind == value.kind ||
               (target.kind == USK_TYPE_FLOATING && value.kind == USK_TYPE_INTEGER);
    }
    if (target.kind == value.kind) return true;
    if (target.kind == USK_TYPE_FLOATING && value.kind == USK_TYPE_INTEGER) return true;
    if (target.kind == USK_TYPE_STRING && value.kind == USK_TYPE_CHARACTER) return true;
    return false;
}

UskSemanticSymbol *usk_sema_lookup(UskSemanticAnalyzer *analyzer,
                                   const char *name) {
    for (UskSemanticScope *scope = analyzer->scope; scope; scope = scope->parent)
        for (UskSemanticSymbol *symbol = scope->symbols; symbol; symbol = symbol->next)
            if (!strcmp(symbol->name, name)) return symbol;
    return NULL;
}

UskSemanticSymbol *usk_sema_find_function(UskSemanticAnalyzer *analyzer,
                                          const char *name) {
    if (analyzer->current_owner) {
        char *member_name = qualified_name(analyzer->current_owner, name);
        UskSemanticSymbol *member = member_name ? usk_sema_lookup(analyzer, member_name) : NULL;
        free(member_name);
        if (member && member->kind == USK_SYMBOL_FUNCTION) return member;
    }
    UskSemanticSymbol *exact = usk_sema_lookup(analyzer, name);
    if (exact && exact->kind == USK_SYMBOL_FUNCTION) return exact;
    if (!strstr(name, "::")) {
        for (UskSemanticSymbol *symbol = analyzer->global_scope.symbols;
             symbol; symbol = symbol->next) {
            if (symbol->kind != USK_SYMBOL_FUNCTION) continue;
            const char *suffix = strrchr(symbol->name, ':');
            suffix = suffix ? suffix + 1 : symbol->name;
            if (!strcmp(suffix, name)) return symbol;
        }
    }
    return NULL;
}

UskSemanticSymbol *usk_sema_add_symbol(UskSemanticAnalyzer *analyzer,
                                       UskSemanticScope *scope,
                                       const char *name,
                                       UskSymbolKind kind,
                                       UskSemanticType type,
                                       const UskAstDecl *declaration,
                                       bool owns_name,
                                       UskSourceSpan span) {
    for (UskSemanticSymbol *existing = scope->symbols; existing; existing = existing->next) {
        if (!strcmp(existing->name, name)) {
            usk_sema_error(analyzer, USK_DIAG_DUPLICATE_NAME, span,
                           "duplicate declaration of '%s'", name);
            if (owns_name) free((void *)name);
            return existing;
        }
    }
    UskSemanticSymbol *symbol = (UskSemanticSymbol *)calloc(1, sizeof(*symbol));
    if (!symbol) {
        usk_sema_error(analyzer, USK_DIAG_OUT_OF_MEMORY, span,
                       "cannot allocate symbol '%s'", name);
        if (owns_name) free((void *)name);
        return NULL;
    }
    symbol->name = name;
    symbol->kind = kind;
    symbol->type = type;
    symbol->declaration = declaration;
    symbol->owns_name = owns_name;
    symbol->next = scope->symbols;
    scope->symbols = symbol;
    analyzer->result.symbols_registered++;
    return symbol;
}

void usk_sema_destroy_scope(UskSemanticScope *scope) {
    if (!scope) return;
    UskSemanticSymbol *symbol = scope->symbols;
    while (symbol) {
        UskSemanticSymbol *next = symbol->next;
        if (symbol->owns_name) free((void *)symbol->name);
        free(symbol);
        symbol = next;
    }
    scope->symbols = NULL;
}

static void register_declaration(UskSemanticAnalyzer *analyzer,
                                 const UskAstDecl *declaration,
                                 const char *owner) {
    if (declaration->kind == USK_DECL_FUNCTION) {
        char *name = qualified_name(owner, declaration->as.function.name);
        if (!name) {
            usk_sema_error(analyzer, USK_DIAG_OUT_OF_MEMORY, declaration->span,
                           "cannot qualify function name");
            return;
        }
        usk_sema_add_symbol(analyzer, &analyzer->global_scope, name,
            USK_SYMBOL_FUNCTION,
            (UskSemanticType){USK_TYPE_UNKNOWN, "USKAuto", 0, false, true, false, false},
            declaration, true, declaration->span);
    } else if (declaration->kind == USK_DECL_CLASS ||
               declaration->kind == USK_DECL_STRUCT) {
        usk_sema_add_symbol(analyzer, &analyzer->global_scope,
            declaration->as.record.name, USK_SYMBOL_TYPE,
            (UskSemanticType){USK_TYPE_UNKNOWN, declaration->as.record.name, 0, false, true, false, false},
            declaration, false, declaration->span);
        usk_sema_register_declarations(analyzer, declaration->as.record.members,
                                       declaration->as.record.name);
    } else if (declaration->kind == USK_DECL_ENUM) {
        usk_sema_add_symbol(analyzer, &analyzer->global_scope,
            declaration->as.enumeration.name, USK_SYMBOL_TYPE,
            (UskSemanticType){USK_TYPE_INTEGER, "USKInt", 0, true, true, false, false},
            declaration, false, declaration->span);
        for (const UskAstEnumValue *item = declaration->as.enumeration.values;
             item; item = item->next) {
            char *member = qualified_name(declaration->as.enumeration.name, item->name);
            if (member)
                usk_sema_add_symbol(analyzer, &analyzer->global_scope, member,
                    USK_SYMBOL_ENUM_VALUE,
                    (UskSemanticType){USK_TYPE_INTEGER, "USKInt", 0, true, true, false, false},
                    declaration, true, declaration->span);
        }
    } else if (declaration->kind == USK_DECL_VARIABLE) {
        UskSemanticType type = usk_sema_type_from_ast(declaration->as.variable.type);
        char *name = qualified_name(owner, declaration->as.variable.name);
        if (name)
            usk_sema_add_symbol(analyzer, &analyzer->global_scope, name,
                USK_SYMBOL_VARIABLE, type, declaration, true, declaration->span);
    } else if (declaration->kind == USK_DECL_TYPEDEF) {
        usk_sema_add_symbol(analyzer, &analyzer->global_scope,
            declaration->as.alias.name, USK_SYMBOL_TYPE,
            usk_sema_type_from_ast(declaration->as.alias.type),
            declaration, false, declaration->span);
    }
}

void usk_sema_register_declarations(UskSemanticAnalyzer *analyzer,
                                    const UskAstDecl *declarations,
                                    const char *owner) {
    for (const UskAstDecl *declaration = declarations; declaration;
         declaration = declaration->next)
        register_declaration(analyzer, declaration, owner);
}

static bool has_main_function(const UskSemanticAnalyzer *analyzer) {
    for (const UskSemanticSymbol *symbol = analyzer->global_scope.symbols;
         symbol; symbol = symbol->next) {
        if (symbol->kind != USK_SYMBOL_FUNCTION) continue;
        const char *suffix = strrchr(symbol->name, ':');
        suffix = suffix ? suffix + 1 : symbol->name;
        if (!strcmp(suffix, "main")) return true;
    }
    return false;
}

UskSemanticResult usk_analyze_program(const UskAstProgram *program,
                                      const char *source_name,
                                      UskDiagnosticList *diagnostics) {
    UskSemanticAnalyzer analyzer = {0};
    analyzer.program = program;
    analyzer.source_name = source_name ? source_name : "<source>";
    analyzer.diagnostics = diagnostics;
    analyzer.scope = &analyzer.global_scope;
    usk_sema_register_declarations(&analyzer, program ? program->declarations : NULL, NULL);
    if (!has_main_function(&analyzer))
        usk_sema_error(&analyzer, USK_DIAG_INVALID_DECLARATION, (UskSourceSpan){0},
                       "no fn main(...) entry point found");
    usk_sema_declarations(&analyzer, program ? program->declarations : NULL, NULL);
    analyzer.result.valid = !usk_diagnostics_has_errors(diagnostics);
    usk_sema_destroy_scope(&analyzer.global_scope);
    return analyzer.result;
}
