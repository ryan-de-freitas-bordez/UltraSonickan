#include "analyzer_internal.h"

#include <string.h>

#define USK_TYPE_ALIAS_LIMIT 64

typedef struct {
    const char *names[USK_TYPE_ALIAS_LIMIT];
    size_t count;
} AliasTrail;

static UskSemanticType unknown_named_type(const char *name, size_t dimensions,
                                          bool is_const, bool is_signed,
                                          bool is_long, bool is_short) {
    return (UskSemanticType){USK_TYPE_UNKNOWN, name, dimensions, is_const,
                             is_signed, is_long, is_short};
}

static bool alias_seen(const AliasTrail *trail, const char *name) {
    for (size_t index = 0; index < trail->count; ++index)
        if (!strcmp(trail->names[index], name)) return true;
    return false;
}

static UskSemanticType resolve_named_type(UskSemanticAnalyzer *analyzer,
                                          const UskAstType *ast_type,
                                          UskSourceSpan span,
                                          AliasTrail *trail) {
    UskTypeInfo descriptor;
    if (usk_type_resolve_modified(ast_type->name, ast_type->is_long,
            ast_type->is_short, ast_type->is_unsigned, &descriptor)) {
        return (UskSemanticType){descriptor.kind, ast_type->name,
            ast_type->array_dimensions, ast_type->is_const,
            descriptor.is_signed, ast_type->is_long, ast_type->is_short};
    }
    UskTypeInfo unmodified;
    if (usk_type_resolve(ast_type->name, &unmodified)) {
        usk_sema_error(analyzer, USK_DIAG_INVALID_DECLARATION, span,
                       "invalid size or signedness modifier for type '%s'",
                       ast_type->name);
        return unknown_named_type(ast_type->name, ast_type->array_dimensions,
            ast_type->is_const, ast_type->is_signed, ast_type->is_long,
            ast_type->is_short);
    }

    UskSemanticSymbol *symbol = usk_sema_lookup(analyzer, ast_type->name);
    if (!symbol || symbol->kind != USK_SYMBOL_TYPE) {
        usk_sema_error(analyzer, USK_DIAG_UNKNOWN_NAME, span,
                       "unknown type '%s'", ast_type->name);
        return unknown_named_type(ast_type->name, ast_type->array_dimensions,
            ast_type->is_const, ast_type->is_signed, ast_type->is_long,
            ast_type->is_short);
    }

    const UskAstDecl *declaration = symbol->declaration;
    if (declaration && declaration->kind == USK_DECL_TYPEDEF) {
        if (trail->count >= USK_TYPE_ALIAS_LIMIT || alias_seen(trail, ast_type->name)) {
            usk_sema_error(analyzer, USK_DIAG_INVALID_DECLARATION, span,
                           "typedef cycle detected while resolving '%s'",
                           ast_type->name);
            return unknown_named_type(ast_type->name, ast_type->array_dimensions,
                ast_type->is_const, ast_type->is_signed, ast_type->is_long,
                ast_type->is_short);
        }
        trail->names[trail->count++] = ast_type->name;
        UskSemanticType resolved = resolve_named_type(analyzer,
            declaration->as.alias.type, declaration->span, trail);
        trail->count--;
        if (resolved.array_dimensions > (size_t)-1 - ast_type->array_dimensions) {
            usk_sema_error(analyzer, USK_DIAG_INVALID_DECLARATION, span,
                           "array dimensions overflow while resolving '%s'",
                           ast_type->name);
            return unknown_named_type(ast_type->name, ast_type->array_dimensions,
                ast_type->is_const, ast_type->is_signed, ast_type->is_long,
                ast_type->is_short);
        }
        resolved.array_dimensions += ast_type->array_dimensions;
        resolved.is_const = resolved.is_const || ast_type->is_const;
        if (ast_type->is_long || ast_type->is_short || ast_type->is_unsigned) {
            if (resolved.kind != USK_TYPE_INTEGER &&
                !(resolved.kind == USK_TYPE_FLOATING && !ast_type->is_unsigned)) {
                usk_sema_error(analyzer, USK_DIAG_INVALID_DECLARATION, span,
                    "numeric modifiers cannot be applied to typedef '%s'",
                    ast_type->name);
            }
            if (ast_type->is_long) resolved.is_long = true;
            if (ast_type->is_short) resolved.is_short = true;
            if (ast_type->is_unsigned) resolved.is_signed = false;
        }
        return resolved;
    }

    /* Record types remain nominal and are resolved by the later layout pass. */
    if (declaration && (declaration->kind == USK_DECL_CLASS ||
                        declaration->kind == USK_DECL_STRUCT)) {
        if (ast_type->is_long || ast_type->is_short || ast_type->is_unsigned)
            usk_sema_error(analyzer, USK_DIAG_INVALID_DECLARATION, span,
                "numeric modifiers cannot be applied to record type '%s'",
                ast_type->name);
        return unknown_named_type(ast_type->name, ast_type->array_dimensions,
            ast_type->is_const, ast_type->is_signed, ast_type->is_long,
            ast_type->is_short);
    }

    if (declaration && declaration->kind == USK_DECL_ENUM)
        return (UskSemanticType){USK_TYPE_INTEGER, ast_type->name,
            ast_type->array_dimensions, ast_type->is_const,
            ast_type->is_signed, ast_type->is_long, ast_type->is_short};

    return unknown_named_type(ast_type->name, ast_type->array_dimensions,
        ast_type->is_const, ast_type->is_signed, ast_type->is_long,
        ast_type->is_short);
}

UskSemanticType usk_sema_resolve_ast_type(UskSemanticAnalyzer *analyzer,
                                          const UskAstType *ast_type,
                                          UskSourceSpan span) {
    if (!ast_type)
        return (UskSemanticType){USK_TYPE_NULL, "USKNull", 0, false,
                                true, false, false};
    if (!ast_type->name || !ast_type->name[0]) {
        usk_sema_error(analyzer, USK_DIAG_INVALID_DECLARATION, span,
                       "type has an empty name");
        return unknown_named_type("USKAuto", 0, false, true, false, false);
    }
    if (ast_type->is_long && ast_type->is_short) {
        usk_sema_error(analyzer, USK_DIAG_INVALID_DECLARATION, span,
                       "USKLong and USKShort cannot modify the same type");
        return unknown_named_type(ast_type->name, ast_type->array_dimensions,
            ast_type->is_const, ast_type->is_signed, ast_type->is_long,
            ast_type->is_short);
    }
    AliasTrail trail = {0};
    return resolve_named_type(analyzer, ast_type, span, &trail);
}
