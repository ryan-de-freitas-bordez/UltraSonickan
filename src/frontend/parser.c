#include "parser_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const Token *usk_parser_at(const UskParser *parser, size_t index) {
    if (!parser || !parser->tokens || !parser->tokens->count) return NULL;
    if (index >= parser->tokens->count) index = parser->tokens->count - 1;
    return &parser->tokens->items[index];
}

const Token *usk_parser_peek(const UskParser *parser) {
    return usk_parser_at(parser, parser ? parser->current : 0);
}

const Token *usk_parser_previous(const UskParser *parser) {
    if (!parser || parser->current == 0) return usk_parser_peek(parser);
    return usk_parser_at(parser, parser->current - 1);
}

bool usk_parser_check(const UskParser *parser, const char *text) {
    const Token *token = usk_parser_peek(parser);
    return token && text && strcmp(token->text, text) == 0;
}

const Token *usk_parser_advance(UskParser *parser) {
    const Token *current = usk_parser_peek(parser);
    if (current && current->kind != TK_EOF) {
        parser->current++;
        usk_parse_stats_add_tokens(parser->stats, 1);
    }
    return current;
}

bool usk_parser_match(UskParser *parser, const char *text) {
    if (!usk_parser_check(parser, text)) return false;
    usk_parser_advance(parser);
    return true;
}

void usk_parser_report_limit(UskParser *parser, const char *limit_name) {
    if (!parser || !parser->stats) return;
    parser->stopped = true;
    if (strcmp(limit_name, "nesting depth") == 0)
        parser->stats->nesting_limit_reached = true;
    else if (strcmp(limit_name, "error count") == 0)
        parser->stats->error_limit_reached = true;
    usk_parser_error(parser, USK_DIAG_UNEXPECTED_TOKEN,
                     "parser %s limit reached", limit_name);
}

void usk_parser_error(UskParser *parser, UskDiagnosticCode code,
                      const char *format, ...) {
    if (!parser || !parser->diagnostics || !format) return;
    if (parser->options.maximum_errors &&
        parser->stats->syntax_errors >= parser->options.maximum_errors) {
        parser->stats->error_limit_reached = true;
        parser->stopped = true;
        return;
    }

    char message[1024];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);

    const Token *token = usk_parser_peek(parser);
    if (!usk_diagnostics_add(parser->diagnostics, USK_DIAGNOSTIC_ERROR, code,
                             parser->source_name, token ? token->line : 0,
                             token ? token->column : 0, "%s", message)) {
        parser->stopped = true;
        return;
    }
    usk_parse_stats_add_errors(parser->stats, 1);
    parser->panic_mode = true;
    if (parser->options.maximum_errors &&
        parser->stats->syntax_errors >= parser->options.maximum_errors) {
        parser->stats->error_limit_reached = true;
        parser->stopped = true;
    }
}

bool usk_parser_consume(UskParser *parser, const char *text,
                        UskDiagnosticCode code, const char *message) {
    if (usk_parser_match(parser, text)) return true;
    const Token *found = usk_parser_peek(parser);
    usk_parser_error(parser, code, "%s; found '%s'", message,
                     found ? found->text : "<no token>");
    return false;
}

UskSourceSpan usk_parser_span(UskParser *parser, size_t first, size_t last) {
    const Token *start = usk_parser_at(parser, first);
    UskSourceSpan span = {0};
    span.first_token = first;
    span.last_token = last;
    if (start) {
        span.line = start->line;
        span.column = start->column;
    }
    return span;
}

UskAstType *usk_parser_new_type(UskParser *parser, const char *name) {
    UskAstType *type = (UskAstType *)usk_ast_allocate(parser->program,
                                                       sizeof(*type));
    if (!type) {
        usk_parser_error(parser, USK_DIAG_OUT_OF_MEMORY,
                         "cannot allocate a type node");
        return NULL;
    }
    type->name = usk_ast_copy_text(parser->program, name);
    type->is_signed = true;
    if (!type->name)
        usk_parser_error(parser, USK_DIAG_OUT_OF_MEMORY,
                         "cannot copy a type name");
    return type;
}

static void parse_entry_annotation(UskParser *parser) {
    if (!usk_parser_match(parser, "[")) return;
    unsigned nesting = 1;
    while (nesting && !parser->stopped &&
           usk_parser_peek(parser)->kind != TK_EOF) {
        if (usk_parser_match(parser, "[")) nesting++;
        else if (usk_parser_match(parser, "]")) nesting--;
        else usk_parser_advance(parser);
    }
    if (nesting) {
        usk_parser_error(parser, USK_DIAG_EXPECTED_TOKEN,
                         "unterminated entry-point annotation");
        return;
    }
    usk_parser_match(parser, ":");
}

void usk_parse_result_init(UskParseResult *result) {
    if (!result) return;
    memset(result, 0, sizeof(*result));
    usk_ast_program_init(&result->program);
    usk_diagnostics_init(&result->diagnostics);
    usk_parse_stats_clear(&result->stats);
}

void usk_parse_result_destroy(UskParseResult *result) {
    if (!result) return;
    usk_ast_program_destroy(&result->program);
    usk_diagnostics_destroy(&result->diagnostics);
    usk_parse_stats_clear(&result->stats);
    result->parsed = false;
}

static bool valid_token_stream(const Tokens *tokens) {
    return usk_tokens_validate(tokens);
}

bool usk_parse_tokens_ex(const Tokens *tokens, const char *source_name,
                         const UskParserOptions *options,
                         UskParseResult *result) {
    if (!valid_token_stream(tokens) || !result) return false;
    usk_parse_stats_clear(&result->stats);
    UskParserOptions effective;
    usk_parser_options_init(&effective);
    if (options) effective = *options;
    if (!usk_parser_options_validate(&effective, &result->diagnostics,
                                     source_name)) {
        result->parsed = false;
        return false;
    }

    UskParser parser = {
        .tokens = tokens,
        .source_name = source_name ? source_name : "<source>",
        .program = &result->program,
        .diagnostics = &result->diagnostics,
        .options = effective,
        .stats = &result->stats
    };
    result->stats.token_count = tokens->count - 1;
    if (effective.maximum_tokens &&
        result->stats.token_count > effective.maximum_tokens) {
        result->stats.token_limit_reached = true;
        usk_parser_error(&parser, USK_DIAG_UNEXPECTED_TOKEN,
                         "source contains %zu tokens; configured maximum is %zu",
                         result->stats.token_count, effective.maximum_tokens);
        result->parsed = false;
        return false;
    }

    while (!parser.stopped && usk_parser_peek(&parser)->kind != TK_EOF) {
        if (usk_parser_check(&parser, "[")) {
            parse_entry_annotation(&parser);
            continue;
        }
        size_t start = parser.current;
        UskAstDecl *declaration = usk_parser_declaration(&parser, false);
        if (declaration) {
            usk_ast_append_declaration(&result->program, declaration);
            usk_parse_stats_add_declarations(&result->stats, 1);
        } else if (parser.current == start && !parser.stopped) {
            usk_parser_error(&parser, USK_DIAG_UNEXPECTED_TOKEN,
                             "expected a declaration, found '%s'",
                             usk_parser_peek(&parser)->text);
            if (!parser.stopped) usk_parser_advance(&parser);
        }
        if (parser.panic_mode && !parser.stopped)
            usk_parser_synchronize(&parser);
    }
    result->parsed = !usk_diagnostics_has_errors(&result->diagnostics);
    return result->parsed;
}

bool usk_parse_tokens(const Tokens *tokens, const char *source_name,
                      UskParseResult *result) {
    return usk_parse_tokens_ex(tokens, source_name, NULL, result);
}

bool usk_parse_source_ex(const char *source, const char *source_name,
                         const UskParserOptions *options,
                         UskParseResult *result) {
    if (!source || !result) return false;
    UskLexerOptions lexer_options;
    usk_lexer_options_init(&lexer_options);
    if (options) lexer_options.maximum_tokens = options->maximum_tokens;
    UskLexerResult lexical = usk_lex_source_ex(source, source_name,
                                               &lexer_options);
    bool diagnostics_copied = true;
    for (size_t index = 0; index < lexical.diagnostics.count; ++index) {
        const UskDiagnostic *diagnostic = &lexical.diagnostics.items[index];
        if (!usk_diagnostics_add(&result->diagnostics, diagnostic->severity,
            diagnostic->code, diagnostic->source_name, diagnostic->line,
            diagnostic->column, "%s", diagnostic->message)) {
            diagnostics_copied = false;
            break;
        }
    }
    bool parse_success = usk_parse_tokens_ex(&lexical.tokens, source_name,
                                              options, result);
    if (!diagnostics_copied)
        usk_diagnostics_add(&result->diagnostics, USK_DIAGNOSTIC_ERROR,
            USK_DIAG_OUT_OF_MEMORY, source_name, 0, 0,
            "cannot copy lexer diagnostics into the parse result");
    bool success = lexical.succeeded && diagnostics_copied && parse_success;
    usk_lexer_result_destroy(&lexical);
    return success;
}

bool usk_parse_source(const char *source, const char *source_name,
                      UskParseResult *result) {
    return usk_parse_source_ex(source, source_name, NULL, result);
}
