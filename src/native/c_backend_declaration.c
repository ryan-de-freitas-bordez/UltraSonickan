#include "c_backend_internal.h"

#include <stdlib.h>
#include <string.h>

static const char *primitive_c_type(const UskAstType *type,
                                    UskCEmitter *emitter) {
    const char *name = type->name ? type->name : "";
    if (type->is_long && type->is_short) {
        usk_c_emit_error(emitter, USK_DIAG_INVALID_DECLARATION,
            (UskSourceSpan){0}, "USKLong and USKShort cannot be combined");
        return NULL;
    }
    if (!strcmp(name, "USKInt")) {
        if (type->is_long) return type->is_unsigned ? "uint64_t" : "int64_t";
        if (type->is_short) return type->is_unsigned ? "uint8_t" : "int8_t";
        return type->is_unsigned ? "uint32_t" : "int32_t";
    }
    if (!strcmp(name, "USKDouble")) {
        if (type->is_unsigned || type->is_short || type->is_long) {
            usk_c_emit_error(emitter, USK_DIAG_INVALID_DECLARATION,
                (UskSourceSpan){0},
                "modified-width USKDouble is unavailable in the portable C11 backend");
            return NULL;
        }
        return "double";
    }
    if (!strcmp(name, "USKFloat")) {
        if (type->is_unsigned || type->is_short || type->is_long) {
            usk_c_emit_error(emitter, USK_DIAG_INVALID_DECLARATION,
                (UskSourceSpan){0},
                "USKFloat cannot use unsigned or USKShort in the C11 backend");
            return NULL;
        }
        return "float";
    }
    if (!strcmp(name, "USKString")) {
        if (type->is_unsigned || type->is_long || type->is_short) {
            usk_c_emit_error(emitter, USK_DIAG_INVALID_DECLARATION,
                (UskSourceSpan){0},
                "numeric modifiers cannot be applied to USKString");
            return NULL;
        }
        return "USKString";
    }
    if (!strcmp(name, "USKChar")) {
        if (type->is_long || type->is_short) {
            usk_c_emit_error(emitter, USK_DIAG_INVALID_DECLARATION,
                (UskSourceSpan){0}, "USKChar cannot use USKLong or USKShort");
            return NULL;
        }
        return type->is_unsigned ? "unsigned char" : "signed char";
    }
    if (!strcmp(name, "USKBool")) {
        if (type->is_unsigned || type->is_long || type->is_short) {
            usk_c_emit_error(emitter, USK_DIAG_INVALID_DECLARATION,
                (UskSourceSpan){0},
                "numeric modifiers cannot be applied to USKBool");
            return NULL;
        }
        return "USKBool";
    }
    if (!strcmp(name, "USKNull")) return "USKNull";
    if (!strcmp(name, "USKAuto")) return "void *";
    if (!strcmp(name, "USKLong")) return type->is_unsigned ? "uint64_t" : "int64_t";
    if (!strcmp(name, "USKShort")) return type->is_unsigned ? "uint8_t" : "int8_t";
    usk_c_emit_unsupported(emitter, (UskSourceSpan){0},
                           "user-defined types before record layout support");
    return NULL;
}

bool usk_c_emit_type(UskCEmitter *emitter, const UskAstType *type) {
    if (!emitter || !type || emitter->failed) return false;
    const char *c_type = primitive_c_type(type, emitter);
    if (!c_type) return false;
    if (type->is_const) usk_c_emit_write(emitter, "const ");
    if (!usk_c_emit_write(emitter, "%s", c_type)) return false;
    for (size_t dimension = 0; dimension < type->array_dimensions; ++dimension)
        if (!usk_c_emit_write(emitter, " *")) return false;
    return !emitter->failed;
}

static char *make_qualified_name(const char *owner, const char *name) {
    return usk_c_join_name(owner, name);
}

static bool emit_parameter_list(UskCEmitter *emitter,
                                const UskAstParameter *parameter,
                                size_t expected_count) {
    if (!expected_count) return usk_c_emit_write(emitter, "void");
    size_t count = 0;
    for (; parameter && !emitter->failed; parameter = parameter->next) {
        if (count) usk_c_emit_write(emitter, ", ");
        if (parameter->default_value)
            usk_c_emit_unsupported(emitter, parameter->default_value->span,
                "default parameters in a native C function signature");
        if (!parameter->type || !usk_c_emit_type(emitter, parameter->type))
            return false;
        usk_c_emit_write(emitter, " %s",
            parameter->name ? parameter->name : "usk_invalid_parameter");
        count++;
    }
    if (count != expected_count) {
        usk_c_emit_error(emitter, USK_DIAG_INTERNAL_ERROR,
            (UskSourceSpan){0},
            "function parameter count does not match its AST metadata");
        return false;
    }
    return !emitter->failed;
}

static bool emit_function(UskCEmitter *emitter,
                          const UskAstDecl *declaration,
                          const char *owner, bool definition) {
    /* Runtime imports may declare host functions without providing bodies.
     * Built-ins are lowered at call sites, so they must not become phantom C
     * prototypes or trigger a definition diagnostic. Other FFI calls remain
     * unsupported until an explicit native-linkage contract exists. */
    if (declaration->is_extern && !declaration->as.function.body)
        return true;
    if (!declaration->as.function.body && definition) {
        usk_c_emit_unsupported(emitter, declaration->span,
                               "external or bodyless function definitions");
        return false;
    }
    char *qualified = make_qualified_name(owner,
        declaration->as.function.name);
    char *mangled = qualified ? usk_c_mangle_name(qualified) : NULL;
    free(qualified);
    if (!mangled) {
        usk_c_emit_error(emitter, USK_DIAG_OUT_OF_MEMORY, declaration->span,
                         "cannot allocate a function symbol");
        return false;
    }
    if (declaration->is_static) usk_c_emit_write(emitter, "static ");
    if (declaration->is_inline) usk_c_emit_write(emitter, "inline ");
    if (declaration->as.function.return_type &&
        declaration->as.function.return_type->name &&
        strcmp(declaration->as.function.return_type->name, "USKNull") != 0) {
        if (!usk_c_emit_type(emitter, declaration->as.function.return_type)) {
            free(mangled);
            return false;
        }
    } else usk_c_emit_write(emitter, "void");
    usk_c_emit_write(emitter, " %s(", mangled);
    emit_parameter_list(emitter, declaration->as.function.parameters,
                        declaration->as.function.parameter_count);
    if (!definition) {
        usk_c_emit_write(emitter, ");\n");
        free(mangled);
        return !emitter->failed;
    }
    usk_c_emit_write(emitter, ") ");
    const char *previous_owner = emitter->current_owner;
    UskCBinding *scope = usk_c_scope_begin(emitter);
    UskCValueKind previous_return = emitter->current_return_kind;
    emitter->current_owner = owner;
    emitter->current_return_kind = declaration->as.function.return_type
        ? usk_c_value_kind_from_type(declaration->as.function.return_type)
        : USK_C_VALUE_NULL;
    for (const UskAstParameter *parameter = declaration->as.function.parameters;
         parameter && !emitter->failed; parameter = parameter->next)
        usk_c_bind_local(emitter, parameter->name,
            usk_c_value_kind_from_type(parameter->type));
    bool success = usk_c_emit_statement(emitter,
        declaration->as.function.body);
    usk_c_scope_end(emitter, scope);
    emitter->current_return_kind = previous_return;
    emitter->current_owner = previous_owner;
    usk_c_emit_write(emitter, "\n");
    emitter->result->functions_emitted++;
    free(mangled);
    return success && !emitter->failed;
}

static bool emit_enum(UskCEmitter *emitter,
                      const UskAstDecl *declaration) {
    usk_c_emit_write(emitter, "typedef enum {");
    size_t count = 0;
    for (const UskAstEnumValue *value = declaration->as.enumeration.values;
         value && !emitter->failed; value = value->next) {
        char *qualified = make_qualified_name(declaration->as.enumeration.name,
                                               value->name);
        char *mangled = qualified ? usk_c_mangle_name(qualified) : NULL;
        free(qualified);
        if (!mangled) {
            usk_c_emit_error(emitter, USK_DIAG_OUT_OF_MEMORY,
                declaration->span, "cannot allocate an enum constant name");
            return false;
        }
        usk_c_emit_write(emitter, "%s\n    %s", count ? "," : "", mangled);
        free(mangled);
        if (value->value) {
            usk_c_emit_write(emitter, " = ");
            usk_c_emit_expression(emitter, value->value);
        }
        count++;
    }
    if (count != declaration->as.enumeration.value_count) {
        usk_c_emit_error(emitter, USK_DIAG_INTERNAL_ERROR, declaration->span,
                         "enum value count does not match its AST metadata");
        return false;
    }
    char *name = usk_c_mangle_name(declaration->as.enumeration.name);
    if (!name) {
        usk_c_emit_error(emitter, USK_DIAG_OUT_OF_MEMORY, declaration->span,
                         "cannot allocate an enum type name");
        return false;
    }
    usk_c_emit_write(emitter, "\n} %s;\n", name);
    free(name);
    return !emitter->failed;
}

static bool emit_record(UskCEmitter *emitter,
                        const UskAstDecl *declaration,
                        const char *owner, bool definition) {
    if (declaration->as.record.base_count) {
        usk_c_emit_unsupported(emitter, declaration->span,
                               "record inheritance in the C11 backend");
        return false;
    }
    char *next_owner = make_qualified_name(owner,
        declaration->as.record.name);
    if (!next_owner) {
        usk_c_emit_error(emitter, USK_DIAG_OUT_OF_MEMORY, declaration->span,
                         "cannot allocate a record owner name");
        return false;
    }
    for (const UskAstDecl *member = declaration->as.record.members;
         member && !emitter->failed; member = member->next) {
        if (member->kind == USK_DECL_VARIABLE) {
            if (definition)
                usk_c_emit_unsupported(emitter, member->span,
                                       "record field layout");
            continue;
        }
        usk_c_emit_declaration(emitter, member, next_owner, definition);
    }
    free(next_owner);
    if (definition) usk_c_emit_write(emitter, "\n");
    return !emitter->failed;
}

bool usk_c_emit_declaration(UskCEmitter *emitter,
                            const UskAstDecl *declaration,
                            const char *owner, bool definition) {
    if (!emitter || !declaration || emitter->failed) return false;
    switch (declaration->kind) {
        case USK_DECL_FUNCTION:
            return emit_function(emitter, declaration, owner, definition);
        case USK_DECL_CLASS:
        case USK_DECL_STRUCT:
            return emit_record(emitter, declaration, owner, definition);
        case USK_DECL_ENUM:
            if (definition) return true;
            return emit_enum(emitter, declaration);
        case USK_DECL_TYPEDEF:
            if (definition) return true;
            if (!declaration->as.alias.type || !declaration->as.alias.name) {
                usk_c_emit_error(emitter, USK_DIAG_INVALID_DECLARATION,
                    declaration->span, "typedef is missing its type or name");
                return false;
            }
            usk_c_emit_write(emitter, "typedef ");
            usk_c_emit_type(emitter, declaration->as.alias.type);
            usk_c_emit_write(emitter, " %s;\n",
                             declaration->as.alias.name);
            return !emitter->failed;
        case USK_DECL_IMPORT:
            return true;
        case USK_DECL_VARIABLE:
            if (!definition) return true;
            usk_c_emit_unsupported(emitter, declaration->span,
                                   "file-scope variable storage");
            return false;
        case USK_DECL_EXTERN:
            if (!definition) return true;
            usk_c_emit_unsupported(emitter, declaration->span,
                                   "extern declarations and FFI");
            return false;
        case USK_DECL_DIRECTIVE:
            if (!definition) return true;
            usk_c_emit_unsupported(emitter, declaration->span,
                                   "preprocessor directives");
            return false;
    }
    usk_c_emit_error(emitter, USK_DIAG_INTERNAL_ERROR, declaration->span,
                     "AST contains an unknown declaration node");
    return false;
}
