#include "parser_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    UskAstVisibility visibility;
    bool is_static;
    bool is_inline;
    bool is_extern;
    const char *wrapped_kind;
} DeclarationModifiers;

static bool is_builtin_type(const char *text) {
    static const char *types[] = {"USKInt", "USKDouble", "USKString", "USKChar",
        "USKFloat", "USKBool", "USKNull", "USKAuto", "USKLong", "USKShort", NULL};
    for (size_t index = 0; types[index]; ++index)
        if (strcmp(text, types[index]) == 0) return true;
    return false;
}

static const char *copy_token_text(UskParser *parser, const Token *token) {
    return usk_ast_copy_text(parser->program, token ? token->text : "");
}

static bool parse_visibility_brackets(UskParser *parser,
                                      UskAstVisibility *visibility) {
    if (!usk_parser_match(parser, "[")) return false;
    if (usk_parser_match(parser, "public") || usk_parser_match(parser, "pub"))
        *visibility = USK_VISIBILITY_PUBLIC;
    else if (usk_parser_match(parser, "private"))
        *visibility = USK_VISIBILITY_PRIVATE;
    else
        usk_parser_error(parser, USK_DIAG_INVALID_DECLARATION,
                         "expected public, pub, or private in visibility annotation");
    usk_parser_consume(parser, "]", USK_DIAG_EXPECTED_TOKEN,
                       "expected ']' after visibility annotation");
    return true;
}

static DeclarationModifiers parse_modifiers(UskParser *parser) {
    DeclarationModifiers modifiers = {0};
    bool scanning = true;
    while (scanning) {
        if (usk_parser_match(parser, "-")) continue;
        if (parse_visibility_brackets(parser, &modifiers.visibility)) continue;
        if (usk_parser_match(parser, "public") || usk_parser_match(parser, "pub")) {
            modifiers.visibility = USK_VISIBILITY_PUBLIC;
            continue;
        }
        if (usk_parser_match(parser, "private")) {
            modifiers.visibility = USK_VISIBILITY_PRIVATE;
            continue;
        }
        if (usk_parser_match(parser, "static")) { modifiers.is_static = true; continue; }
        if (usk_parser_match(parser, "inline")) { modifiers.is_inline = true; continue; }
        if (usk_parser_match(parser, "extern")) { modifiers.is_extern = true; continue; }
        if (usk_parser_match(parser, "(")) {
            parse_visibility_brackets(parser, &modifiers.visibility);
            const Token *kind = usk_parser_peek(parser);
            if (kind && kind->kind == TK_ID) {
                modifiers.wrapped_kind = copy_token_text(parser, kind);
                usk_parser_advance(parser);
                while (!usk_parser_check(parser, ")") &&
                       usk_parser_peek(parser)->kind != TK_EOF)
                    usk_parser_advance(parser);
                usk_parser_consume(parser, ")", USK_DIAG_EXPECTED_TOKEN,
                                   "expected ')' after wrapped declaration keyword");
                continue;
            }
            parser->current--;
        }
        scanning = false;
    }
    return modifiers;
}

static UskAstType *parse_type(UskParser *parser, bool is_const) {
    bool is_signed = true, is_unsigned = false, is_long = false, is_short = false;
    bool scanning = true;
    while (scanning) {
        if (usk_parser_match(parser, "signed")) { is_signed = true; is_unsigned = false; }
        else if (usk_parser_match(parser, "unsigned")) { is_unsigned = true; is_signed = false; }
        else if (usk_parser_match(parser, "USKLong")) is_long = true;
        else if (usk_parser_match(parser, "USKShort")) is_short = true;
        else scanning = false;
    }
    const Token *name = usk_parser_peek(parser);
    if (!name || name->kind != TK_ID) {
        usk_parser_error(parser, USK_DIAG_EXPECTED_TOKEN, "expected a type name");
        return usk_parser_new_type(parser, "USKAuto");
    }
    if (is_long && is_short)
        usk_parser_error(parser, USK_DIAG_INVALID_DECLARATION,
                         "USKLong and USKShort cannot modify the same type");
    usk_parser_advance(parser);
    UskAstType *type = usk_parser_new_type(parser, name->text);
    if (type) {
        type->is_const = is_const;
        type->is_signed = is_signed;
        type->is_unsigned = is_unsigned;
        type->is_long = is_long;
        type->is_short = is_short;
    }
    return type;
}

UskAstDecl *usk_parser_variable_declaration(UskParser *parser) {
    size_t start = parser->current;
    bool immutable = false;
    bool explicit_var = false;
    if (usk_parser_match(parser, "-")) {
        if (usk_parser_match(parser, "(")) {
            if (usk_parser_match(parser, "const")) immutable = true;
            else if (usk_parser_match(parser, "var")) explicit_var = true;
            else if (is_builtin_type(usk_parser_peek(parser)->text)) { }
            else usk_parser_error(parser, USK_DIAG_INVALID_DECLARATION,
                                  "expected var, const, or a type in declaration marker");
            while (!usk_parser_check(parser, ")") &&
                   usk_parser_peek(parser)->kind != TK_EOF)
                usk_parser_advance(parser);
            usk_parser_consume(parser, ")", USK_DIAG_EXPECTED_TOKEN,
                               "expected ')' after variable declaration marker");
        } else {
            parser->current--;
        }
    }
    if (usk_parser_match(parser, "const")) immutable = true;
    else if (usk_parser_match(parser, "var")) explicit_var = true;

    UskAstType *type = NULL;
    const Token *first = usk_parser_peek(parser);
    bool starts_with_type = first && first->kind == TK_ID &&
                            (is_builtin_type(first->text) ||
                             usk_parser_at(parser, parser->current + 1)->kind == TK_ID);
    if (!explicit_var && !immutable && !starts_with_type) {
        usk_parser_error(parser, USK_DIAG_INVALID_DECLARATION,
                         "expected var, const, or an explicit type declaration");
        return NULL;
    }
    if (starts_with_type || (!explicit_var && !immutable))
        type = parse_type(parser, immutable);
    else if (immutable)
        type = usk_parser_new_type(parser, "USKAuto");

    const Token *name = usk_parser_peek(parser);
    if (name && name->kind == TK_ID && !is_builtin_type(name->text)) {
        usk_parser_advance(parser);
    } else {
        usk_parser_error(parser, USK_DIAG_EXPECTED_TOKEN,
                         "expected a variable name, found '%s'",
                         name ? name->text : "<missing>");
        return NULL;
    }
    const char *variable_name = copy_token_text(parser, name);

    if (usk_parser_match(parser, ":")) type = parse_type(parser, immutable);
    if (!type) type = usk_parser_new_type(parser, "USKAuto");
    while (usk_parser_match(parser, "[")) {
        if (!usk_parser_match(parser, "]")) {
            usk_parser_error(parser, USK_DIAG_UNSUPPORTED_FEATURE,
                             "fixed-size array declarations are not implemented yet");
            while (!usk_parser_check(parser, "]") && usk_parser_peek(parser)->kind != TK_EOF)
                usk_parser_advance(parser);
            usk_parser_match(parser, "]");
        }
        type->array_dimensions++;
    }
    UskAstExpr *initializer = NULL;
    if (usk_parser_match(parser, "=")) initializer = usk_parser_expression(parser);
    if (immutable && !initializer)
        usk_parser_error(parser, USK_DIAG_INVALID_DECLARATION,
                         "const variables require an initializer");

    UskAstDecl *declaration = usk_ast_new_declaration(
        parser->program, USK_DECL_VARIABLE,
        usk_parser_span(parser, start, parser->current ? parser->current - 1 : start));
    if (!declaration) return NULL;
    declaration->as.variable.type = type;
    declaration->as.variable.name = variable_name;
    declaration->as.variable.initializer = initializer;
    if (immutable && type) type->is_const = true;
    return declaration;
}

static const char *parse_qualified_name(UskParser *parser) {
    const Token *first = usk_parser_peek(parser);
    if (!first || first->kind != TK_ID) {
        usk_parser_error(parser, USK_DIAG_EXPECTED_TOKEN, "expected a qualified name");
        return usk_ast_copy_text(parser->program, "<error>");
    }
    size_t size = strlen(first->text) + 1;
    size_t capacity = size + 32;
    char *name = (char *)malloc(capacity);
    if (!name) return NULL;
    strcpy(name, first->text);
    usk_parser_advance(parser);
    while (usk_parser_match(parser, "::")) {
        const Token *part = usk_parser_peek(parser);
        if (!part || part->kind != TK_ID) {
            usk_parser_error(parser, USK_DIAG_EXPECTED_TOKEN,
                             "expected a name after '::'");
            break;
        }
        size = strlen(name) + strlen(part->text) + 3;
        if (size > capacity) {
            capacity = size + 32;
            char *resized = (char *)realloc(name, capacity);
            if (!resized) { free(name); return NULL; }
            name = resized;
        }
        strcat(name, "::");
        strcat(name, part->text);
        usk_parser_advance(parser);
    }
    const char *owned = usk_ast_copy_text(parser->program, name);
    free(name);
    return owned;
}

static void append_base(const char ***items, size_t *count, size_t *capacity,
                        const char *base) {
    if (*count == *capacity) {
        size_t next_capacity = *capacity ? *capacity * 2 : 4;
        const char **next = (const char **)realloc((void *)*items,
                                                   next_capacity * sizeof(**items));
        if (!next) return;
        *items = next;
        *capacity = next_capacity;
    }
    (*items)[(*count)++] = base;
}

static void parse_bases(UskParser *parser, const char ***bases, size_t *count) {
    size_t capacity = 0;
    if (!usk_parser_match(parser, ":")) return;
    if (usk_parser_match(parser, "[")) {
        usk_parser_match(parser, "[");
        if (usk_parser_match(parser, "use"))
            usk_parser_consume(parser, "]", USK_DIAG_EXPECTED_TOKEN,
                               "expected ']' after use marker");
        if (usk_parser_peek(parser)->kind == TK_ID) {
            append_base(bases, count, &capacity, parse_qualified_name(parser));
        }
        while (!usk_parser_check(parser, "]") && usk_parser_peek(parser)->kind != TK_EOF)
            usk_parser_advance(parser);
        usk_parser_match(parser, "]");
        return;
    }
    do {
        append_base(bases, count, &capacity, parse_qualified_name(parser));
    } while (usk_parser_match(parser, ","));
}

static UskAstParameter *parse_parameters(UskParser *parser, size_t *count) {
    UskAstParameter *first = NULL, *last = NULL;
    *count = 0;
    usk_parser_consume(parser, "(", USK_DIAG_EXPECTED_TOKEN,
                       "expected '(' before function parameters");
    while (!usk_parser_check(parser, ")") && usk_parser_peek(parser)->kind != TK_EOF) {
        size_t start = parser->current;
        bool immutable = usk_parser_match(parser, "const");
        const Token *first_word = usk_parser_peek(parser);
        if (!first_word || first_word->kind != TK_ID) {
            usk_parser_error(parser, USK_DIAG_EXPECTED_TOKEN, "expected a parameter name or type");
            break;
        }
        UskAstType *type = NULL;
        const Token *name = first_word;
        bool type_first = is_builtin_type(first_word->text) ||
            usk_parser_at(parser, parser->current + 1)->kind == TK_ID;
        if (type_first) {
            type = parse_type(parser, immutable);
            name = usk_parser_peek(parser);
            if (!name || name->kind != TK_ID) {
                usk_parser_error(parser, USK_DIAG_EXPECTED_TOKEN,
                                 "expected a parameter name after its type");
                break;
            }
            usk_parser_advance(parser);
        } else {
            usk_parser_advance(parser);
            type = usk_parser_new_type(parser, "USKAuto");
            if (usk_parser_match(parser, ":")) type = parse_type(parser, immutable);
        }
        const char *parameter_name = copy_token_text(parser, name);
        while (usk_parser_match(parser, "[")) {
            usk_parser_consume(parser, "]", USK_DIAG_EXPECTED_TOKEN,
                               "expected ']' after parameter array marker");
            type->array_dimensions++;
        }
        UskAstExpr *default_value = NULL;
        if (usk_parser_match(parser, "=")) default_value = usk_parser_expression(parser);
        UskAstParameter *parameter = (UskAstParameter *)usk_ast_allocate(parser->program, sizeof(*parameter));
        if (!parameter) { usk_parser_error(parser, USK_DIAG_OUT_OF_MEMORY, "cannot allocate parameter"); break; }
        parameter->type = type;
        parameter->name = parameter_name;
        parameter->default_value = default_value;
        if (last) last->next = parameter;
        else first = parameter;
        last = parameter;
        (*count)++;
        if (!usk_parser_match(parser, ",")) break;
        if (parser->current == start) break;
    }
    usk_parser_consume(parser, ")", USK_DIAG_EXPECTED_TOKEN,
                       "expected ')' after function parameters");
    return first;
}

static UskAstDecl *parse_function(UskParser *parser, DeclarationModifiers modifiers,
                                 size_t start) {
    const Token *name = usk_parser_peek(parser);
    if (!name || name->kind != TK_ID) {
        usk_parser_error(parser, USK_DIAG_EXPECTED_TOKEN, "expected a function name");
        return NULL;
    }
    usk_parser_advance(parser);
    const char *function_name = copy_token_text(parser, name);
    size_t parameter_count = 0;
    UskAstParameter *parameters = parse_parameters(parser, &parameter_count);
    UskAstType *return_type = NULL;
    if (usk_parser_match(parser, ":") || usk_parser_match(parser, "->")) {
        return_type = parse_type(parser, false);
        while (return_type && usk_parser_match(parser, "[")) {
            if (!usk_parser_consume(parser, "]", USK_DIAG_EXPECTED_TOKEN,
                                    "expected ']' after return array marker"))
                break;
            if (return_type->array_dimensions == (size_t)-1) {
                usk_parser_error(parser, USK_DIAG_INVALID_DECLARATION,
                                 "return type has too many array dimensions");
                break;
            }
            return_type->array_dimensions++;
        }
    }
    if (!return_type) return_type = usk_parser_new_type(parser, "USKNull");
    UskAstStmt *body = NULL;
    if (usk_parser_check(parser, "{")) body = usk_parser_block(parser);
    else if (modifiers.is_extern)
        usk_parser_consume(parser, ";", USK_DIAG_EXPECTED_TOKEN,
                           "expected ';' after external function declaration");
    else
        usk_parser_consume(parser, ";", USK_DIAG_EXPECTED_TOKEN,
                           "expected a function body or ';'");
    UskAstDecl *declaration = usk_ast_new_declaration(
        parser->program, USK_DECL_FUNCTION,
        usk_parser_span(parser, start, parser->current ? parser->current - 1 : start));
    if (!declaration) return NULL;
    declaration->visibility = modifiers.visibility;
    declaration->is_static = modifiers.is_static;
    declaration->is_inline = modifiers.is_inline;
    declaration->is_extern = modifiers.is_extern;
    declaration->as.function.name = function_name;
    declaration->as.function.return_type = return_type;
    declaration->as.function.parameters = parameters;
    declaration->as.function.parameter_count = parameter_count;
    declaration->as.function.body = body;
    return declaration;
}

static UskAstDecl *parse_record(UskParser *parser, DeclarationModifiers modifiers,
                                size_t start, UskAstDeclKind kind) {
    const Token *name = usk_parser_peek(parser);
    if (!name || name->kind != TK_ID) {
        usk_parser_error(parser, USK_DIAG_EXPECTED_TOKEN, "expected a class or struct name");
        return NULL;
    }
    usk_parser_advance(parser);
    const char *record_name = copy_token_text(parser, name);
    const char **bases = NULL;
    size_t base_count = 0;
    parse_bases(parser, &bases, &base_count);
    if (!usk_parser_consume(parser, "{", USK_DIAG_EXPECTED_TOKEN,
                            "expected '{' before class members")) {
        free((void *)bases);
        return NULL;
    }
    UskAstDecl *members = NULL, *last = NULL;
    while (!parser->stopped && !usk_parser_check(parser, "}") &&
           usk_parser_peek(parser)->kind != TK_EOF) {
        size_t before = parser->current;
        UskAstDecl *member = usk_parser_declaration(parser, true);
        if (member) {
            if (last) last->next = member;
            else members = member;
            last = member;
        }
        if (parser->current == before) {
            usk_parser_error(parser, USK_DIAG_UNEXPECTED_TOKEN,
                             "expected a class member declaration");
            usk_parser_advance(parser);
        }
        if (parser->panic_mode) usk_parser_synchronize(parser);
    }
    usk_parser_consume(parser, "}", USK_DIAG_EXPECTED_TOKEN,
                       "expected '}' after class members");
    if (usk_parser_match(parser, ";")) { }
    UskAstDecl *declaration = usk_ast_new_declaration(
        parser->program, kind,
        usk_parser_span(parser, start, parser->current ? parser->current - 1 : start));
    if (!declaration) { free((void *)bases); return NULL; }
    declaration->visibility = modifiers.visibility;
    declaration->as.record.name = record_name;
    if (base_count) {
        const char **owned_bases = (const char **)usk_ast_allocate(
            parser->program, base_count * sizeof(*owned_bases));
        if (owned_bases) memcpy((void *)owned_bases, bases, base_count * sizeof(*bases));
        declaration->as.record.bases = owned_bases;
        declaration->as.record.base_count = base_count;
    }
    declaration->as.record.members = members;
    free((void *)bases);
    return declaration;
}

static UskAstDecl *parse_enum(UskParser *parser, DeclarationModifiers modifiers,
                              size_t start) {
    const Token *name = usk_parser_peek(parser);
    if (!name || name->kind != TK_ID) {
        usk_parser_error(parser, USK_DIAG_EXPECTED_TOKEN, "expected an enum name");
        return NULL;
    }
    usk_parser_advance(parser);
    usk_parser_consume(parser, "{", USK_DIAG_EXPECTED_TOKEN,
                       "expected '{' before enum values");
    UskAstEnumValue *first = NULL, *last = NULL;
    size_t count = 0;
    while (!parser->stopped && !usk_parser_check(parser, "}") &&
           usk_parser_peek(parser)->kind != TK_EOF) {
        const Token *value_name = usk_parser_peek(parser);
        if (!value_name || value_name->kind != TK_ID) {
            usk_parser_error(parser, USK_DIAG_EXPECTED_TOKEN, "expected an enum member name");
            break;
        }
        usk_parser_advance(parser);
        UskAstExpr *value = NULL;
        if (usk_parser_match(parser, "=")) value = usk_parser_expression(parser);
        UskAstEnumValue *item = (UskAstEnumValue *)usk_ast_allocate(parser->program, sizeof(*item));
        if (!item) { usk_parser_error(parser, USK_DIAG_OUT_OF_MEMORY, "cannot allocate enum value"); break; }
        item->name = copy_token_text(parser, value_name);
        item->value = value;
        if (last) last->next = item;
        else first = item;
        last = item;
        count++;
        if (!usk_parser_match(parser, ",")) break;
    }
    usk_parser_consume(parser, "}", USK_DIAG_EXPECTED_TOKEN,
                       "expected '}' after enum values");
    usk_parser_match(parser, ";");
    UskAstDecl *declaration = usk_ast_new_declaration(
        parser->program, USK_DECL_ENUM,
        usk_parser_span(parser, start, parser->current ? parser->current - 1 : start));
    if (declaration) {
        declaration->visibility = modifiers.visibility;
        declaration->as.enumeration.name = copy_token_text(parser, name);
        declaration->as.enumeration.values = first;
        declaration->as.enumeration.value_count = count;
    }
    return declaration;
}

static UskAstDecl *parse_typedef(UskParser *parser, size_t start) {
    UskAstType *type = parse_type(parser, false);
    const Token *name = usk_parser_peek(parser);
    if (!name || name->kind != TK_ID) {
        usk_parser_error(parser, USK_DIAG_EXPECTED_TOKEN, "expected a typedef name");
        return NULL;
    }
    usk_parser_advance(parser);
    usk_parser_consume(parser, ";", USK_DIAG_EXPECTED_TOKEN,
                       "expected ';' after typedef");
    UskAstDecl *declaration = usk_ast_new_declaration(
        parser->program, USK_DECL_TYPEDEF,
        usk_parser_span(parser, start, parser->current ? parser->current - 1 : start));
    if (declaration) {
        declaration->as.alias.name = copy_token_text(parser, name);
        declaration->as.alias.type = type;
    }
    return declaration;
}

static UskAstDecl *parse_import(UskParser *parser, size_t start) {
    const Token *path = usk_parser_peek(parser);
    if (usk_parser_match(parser, "<")) {
        size_t capacity = 64, length = 0;
        char *text = (char *)malloc(capacity);
        while (!usk_parser_check(parser, ">") && usk_parser_peek(parser)->kind != TK_EOF) {
            const char *part = usk_parser_advance(parser)->text;
            size_t part_length = strlen(part);
            while (length + part_length + 1 > capacity) capacity *= 2;
            text = (char *)realloc(text, capacity);
            memcpy(text + length, part, part_length);
            length += part_length;
        }
        text[length] = '\0';
        usk_parser_consume(parser, ">", USK_DIAG_EXPECTED_TOKEN,
                           "expected '>' after import path");
        path = NULL;
        UskAstDecl *declaration = usk_ast_new_declaration(
            parser->program, USK_DECL_IMPORT,
            usk_parser_span(parser, start, parser->current ? parser->current - 1 : start));
        if (declaration) declaration->as.import_decl.path = usk_ast_copy_text(parser->program, text);
        free(text);
        usk_parser_match(parser, ";");
        return declaration;
    }
    if (path && path->kind == TK_STRING) {
        usk_parser_advance(parser);
        UskAstDecl *declaration = usk_ast_new_declaration(
            parser->program, USK_DECL_IMPORT,
            usk_parser_span(parser, start, parser->current - 1));
        if (declaration) declaration->as.import_decl.path = copy_token_text(parser, path);
        usk_parser_match(parser, ";");
        return declaration;
    }
    usk_parser_error(parser, USK_DIAG_EXPECTED_TOKEN,
                     "expected '<path>' or a string after import");
    return NULL;
}

UskAstDecl *usk_parser_declaration(UskParser *parser, bool class_member) {
    size_t start = parser->current;
    DeclarationModifiers modifiers = parse_modifiers(parser);
    const Token *keyword = usk_parser_peek(parser);
    if (!keyword || keyword->kind == TK_EOF) return NULL;
    const char *kind = modifiers.wrapped_kind ? modifiers.wrapped_kind : keyword->text;
    if (modifiers.wrapped_kind) {
        /* parse_modifiers consumed the wrapped declaration keyword itself */
        keyword = usk_parser_previous(parser);
    } else if (keyword->keyword == USK_KW_FN || keyword->keyword == USK_KW_CLASS ||
               keyword->keyword == USK_KW_STRUCT || keyword->keyword == USK_KW_ENUM ||
               keyword->keyword == USK_KW_TYPEDEF || keyword->keyword == USK_KW_IMPORT ||
               keyword->keyword == USK_KW_VAR || keyword->keyword == USK_KW_CONST) {
        usk_parser_advance(parser);
    } else if (is_builtin_type(keyword->text) || class_member) {
        /* Explicitly typed fields can omit `var`; the variable parser expects
         * its starting token still to be in the stream. */
        return usk_parser_variable_declaration(parser);
    } else {
        return NULL;
    }

    if (!strcmp(kind, "fn")) return parse_function(parser, modifiers, start);
    if (!strcmp(kind, "class")) return parse_record(parser, modifiers, start, USK_DECL_CLASS);
    if (!strcmp(kind, "struct")) return parse_record(parser, modifiers, start, USK_DECL_STRUCT);
    if (!strcmp(kind, "enum")) return parse_enum(parser, modifiers, start);
    if (!strcmp(kind, "typedef")) return parse_typedef(parser, start);
    if (!strcmp(kind, "import")) return parse_import(parser, start);
    if (!strcmp(kind, "var") || !strcmp(kind, "const")) {
        if (modifiers.wrapped_kind) parser->current = start;
        else parser->current--;
        UskAstDecl *declaration = usk_parser_variable_declaration(parser);
        if (declaration)
            usk_parser_consume(parser, ";", USK_DIAG_EXPECTED_TOKEN,
                               "expected ';' after variable declaration");
        return declaration;
    }
    if (class_member && is_builtin_type(kind)) {
        parser->current = start;
        UskAstDecl *declaration = usk_parser_variable_declaration(parser);
        if (declaration)
            usk_parser_consume(parser, ";", USK_DIAG_EXPECTED_TOKEN,
                               "expected ';' after field declaration");
        return declaration;
    }
    usk_parser_error(parser, USK_DIAG_UNSUPPORTED_FEATURE,
                     "declaration form '%s' is not implemented", kind);
    return NULL;
}
