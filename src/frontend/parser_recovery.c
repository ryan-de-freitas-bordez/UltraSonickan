#include "parser_internal.h"

#include <string.h>

static bool is_declaration_keyword(const Token *token) {
    if (!token) return false;
    return token->keyword == USK_KW_FN || token->keyword == USK_KW_CLASS ||
           token->keyword == USK_KW_STRUCT || token->keyword == USK_KW_ENUM ||
           token->keyword == USK_KW_VAR || token->keyword == USK_KW_CONST ||
           token->keyword == USK_KW_IMPORT;
}

static bool is_statement_keyword(const Token *token) {
    if (!token) return false;
    return token->keyword == USK_KW_IF || token->keyword == USK_KW_ELIF ||
           token->keyword == USK_KW_ELSE || token->keyword == USK_KW_SWITCH ||
           token->keyword == USK_KW_CASE || token->keyword == USK_KW_DEFAULT ||
           token->keyword == USK_KW_WHILE || token->keyword == USK_KW_FOR ||
           token->keyword == USK_KW_BREAK || token->keyword == USK_KW_CONTINUE ||
           strcmp(token->text, "return") == 0;
}

static bool is_recovery_boundary(const UskParser *parser, bool top_level) {
    const Token *token = usk_parser_peek(parser);
    return is_declaration_keyword(token) ||
           (!top_level && is_statement_keyword(token));
}

bool usk_parser_at_declaration_boundary(const UskParser *parser) {
    return is_declaration_keyword(usk_parser_peek(parser));
}

static bool at_end(const UskParser *parser) {
    const Token *token = usk_parser_peek(parser);
    return !token || token->kind == TK_EOF;
}

static bool at_closing_brace(const UskParser *parser) {
    const Token *token = usk_parser_peek(parser);
    return token && strcmp(token->text, "}") == 0;
}

static bool at_statement_terminator(const UskParser *parser) {
    const Token *token = usk_parser_peek(parser);
    return token && (strcmp(token->text, ";") == 0 ||
                     strcmp(token->text, "\n") == 0);
}

static void skip_to_end(UskParser *parser) {
    while (!at_end(parser)) usk_parser_advance(parser);
}

void usk_parser_synchronize(UskParser *parser) {
    if (!parser || !parser->panic_mode) return;
    parser->panic_mode = false;
    usk_parse_stats_add_recoveries(parser->stats, 1);
    if (!parser->options.recover_after_error) {
        skip_to_end(parser);
        parser->stopped = true;
        return;
    }

    size_t consumed = 0;
    unsigned parentheses = 0;
    unsigned brackets = 0;
    unsigned braces = 0;
    while (!at_end(parser)) {
        const Token *token = usk_parser_peek(parser);
        if (parentheses == 0 && brackets == 0 && braces == 0) {
            if (at_closing_brace(parser)) return;
            if (consumed && is_recovery_boundary(parser, false)) return;
            if (at_statement_terminator(parser)) {
                usk_parser_advance(parser);
                return;
            }
        }

        if (strcmp(token->text, "(") == 0) parentheses++;
        else if (strcmp(token->text, ")") == 0 && parentheses)
            parentheses--;
        else if (strcmp(token->text, "[") == 0) brackets++;
        else if (strcmp(token->text, "]") == 0 && brackets)
            brackets--;
        else if (strcmp(token->text, "{") == 0) braces++;
        else if (strcmp(token->text, "}") == 0 && braces)
            braces--;

        usk_parser_advance(parser);
        consumed++;
    }
}

size_t usk_parser_find_next_declaration(const UskParser *parser,
                                       size_t from_index) {
    if (!parser || !parser->tokens) return 0;
    for (size_t index = from_index; index < parser->tokens->count; ++index) {
        const Token *token = usk_parser_at(parser, index);
        if (!token || token->kind == TK_EOF) return index;
        if (is_declaration_keyword(token) || strcmp(token->text, "[") == 0)
            return index;
    }
    return parser->tokens->count - 1;
}

size_t usk_parser_find_matching_delimiter(const UskParser *parser,
                                          size_t from_index,
                                          const char *opening,
                                          const char *closing) {
    if (!parser || !opening || !closing) return 0;
    unsigned depth = 0;
    for (size_t index = from_index; index < parser->tokens->count; ++index) {
        const Token *token = usk_parser_at(parser, index);
        if (!token || token->kind == TK_EOF) break;
        if (strcmp(token->text, opening) == 0) depth++;
        else if (strcmp(token->text, closing) == 0) {
            if (depth == 0) return index;
            if (--depth == 0) return index;
        }
    }
    return parser->tokens->count ? parser->tokens->count - 1 : 0;
}

bool usk_parser_recovery_can_continue(const UskParser *parser,
                                     size_t start_index) {
    if (!parser || parser->stopped || at_end(parser)) return false;
    return parser->current != start_index ||
           is_recovery_boundary(parser, true) || at_closing_brace(parser);
}

bool usk_parser_recovery_should_unwind(const UskParser *parser,
                                       unsigned open_blocks) {
    if (!parser || parser->stopped || at_end(parser)) return true;
    return open_blocks == 0 && usk_parser_at_declaration_boundary(parser);
}
