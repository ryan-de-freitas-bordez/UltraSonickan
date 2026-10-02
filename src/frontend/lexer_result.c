#include "usk/lexer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void usk_lexer_options_init(UskLexerOptions *options) {
    if (!options) return;
    options->maximum_source_bytes = (size_t)16 * 1024 * 1024;
    options->maximum_tokens = 1000000;
    options->maximum_literal_bytes = (size_t)4 * 1024 * 1024;
    options->allow_nested_block_comments = true;
}

void usk_lexer_stats_clear(UskLexerStats *stats) {
    if (stats) memset(stats, 0, sizeof(*stats));
}

void usk_lexer_result_init(UskLexerResult *result) {
    if (!result) return;
    memset(result, 0, sizeof(*result));
    usk_diagnostics_init(&result->diagnostics);
    usk_lexer_stats_clear(&result->stats);
    result->succeeded = true;
}

void usk_lexer_result_destroy(UskLexerResult *result) {
    if (!result) return;
    usk_tokens_free(&result->tokens);
    usk_diagnostics_destroy(&result->diagnostics);
    memset(result, 0, sizeof(*result));
}

Tokens usk_lex_source(const char *source, bool *ok) {
    UskLexerResult result = usk_lex_source_ex(source, "<source>", NULL);
    if (ok) *ok = result.succeeded;
    if (!result.succeeded) usk_diagnostics_print(&result.diagnostics, stderr);
    Tokens tokens = result.tokens;
    memset(&result.tokens, 0, sizeof(result.tokens));
    usk_lexer_result_destroy(&result);
    return tokens;
}

const Token *usk_tokens_get(const Tokens *tokens, size_t index) {
    if (!tokens || index >= tokens->count) return NULL;
    return &tokens->items[index];
}

const Token *usk_tokens_find_at_or_after(const Tokens *tokens,
                                         size_t source_offset) {
    if (!tokens || !tokens->count) return NULL;
    size_t low = 0;
    size_t high = tokens->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (tokens->items[middle].source_offset < source_offset)
            low = middle + 1;
        else
            high = middle;
    }
    return low < tokens->count ? &tokens->items[low] : NULL;
}

size_t usk_tokens_content_count(const Tokens *tokens) {
    if (!tokens || !tokens->count) return 0;
    return tokens->items[tokens->count - 1].kind == TK_EOF
        ? tokens->count - 1 : tokens->count;
}

bool usk_tokens_validate(const Tokens *tokens) {
    if (!tokens || !tokens->items || !tokens->count ||
        tokens->count > tokens->capacity) return false;
    size_t previous_offset = 0;
    for (size_t index = 0; index < tokens->count; ++index) {
        const Token *token = &tokens->items[index];
        if (!token->text || token->kind < TK_EOF || token->kind > TK_PUNCT ||
            token->source_offset > SIZE_MAX - token->source_length ||
            (index && token->source_offset < previous_offset)) return false;
        if (token->kind == TK_EOF && index + 1 != tokens->count) return false;
        if (token->kind == TK_EOF && token->source_length != 0) return false;
        if (token->kind == TK_ID && token->keyword !=
            usk_keyword_from_text(token->text)) return false;
        if (token->kind != TK_ID && token->keyword != USK_KW_NONE) return false;
        previous_offset = token->source_offset;
    }
    return tokens->items[tokens->count - 1].kind == TK_EOF;
}

size_t usk_tokens_count_kind(const Tokens *tokens, TokenKind kind) {
    if (!tokens) return 0;
    size_t count = 0;
    for (size_t index = 0; index < tokens->count; ++index)
        if (tokens->items[index].kind == kind) count++;
    return count;
}

const Token *usk_tokens_find_keyword(const Tokens *tokens,
                                     TokenKeyword keyword,
                                     size_t first_index) {
    if (!tokens || keyword == USK_KW_NONE || first_index >= tokens->count)
        return NULL;
    for (size_t index = first_index; index < tokens->count; ++index)
        if (usk_token_is_keyword(&tokens->items[index], keyword))
            return &tokens->items[index];
    return NULL;
}

const Token *usk_tokens_find_on_line(const Tokens *tokens, int line,
                                     size_t first_index) {
    if (!tokens || line < 1 || first_index >= tokens->count) return NULL;
    for (size_t index = first_index; index < tokens->count; ++index) {
        const Token *token = &tokens->items[index];
        if (token->line == line) return token;
        if (token->line > line) break;
    }
    return NULL;
}

bool usk_token_text_is(const Token *token, const char *text) {
    return token && token->text && text && strcmp(token->text, text) == 0;
}

bool usk_token_is_identifier(const Token *token) {
    return token && token->kind == TK_ID && token->keyword == USK_KW_NONE;
}

bool usk_tokens_source_range(const Tokens *tokens, size_t first_index,
                             size_t last_index, size_t *offset,
                             size_t *length) {
    if (!usk_tokens_validate(tokens) || !offset || !length ||
        first_index > last_index || last_index >= tokens->count) return false;
    const Token *first = &tokens->items[first_index];
    const Token *last = &tokens->items[last_index];
    if (last->source_offset > SIZE_MAX - last->source_length ||
        last->source_offset + last->source_length < first->source_offset)
        return false;
    *offset = first->source_offset;
    *length = last->source_offset + last->source_length - first->source_offset;
    return true;
}

static bool is_opening_delimiter(const char *text) {
    return strcmp(text, "(") == 0 || strcmp(text, "[") == 0 ||
           strcmp(text, "{") == 0;
}

static bool is_closing_delimiter(const char *text) {
    return strcmp(text, ")") == 0 || strcmp(text, "]") == 0 ||
           strcmp(text, "}") == 0;
}

static bool delimiters_match(const char *opening, const char *closing) {
    return (strcmp(opening, "(") == 0 && strcmp(closing, ")") == 0) ||
           (strcmp(opening, "[") == 0 && strcmp(closing, "]") == 0) ||
           (strcmp(opening, "{") == 0 && strcmp(closing, "}") == 0);
}

bool usk_tokens_find_matching_delimiter(const Tokens *tokens,
                                        size_t opening_index,
                                        size_t *closing_index) {
    if (!usk_tokens_validate(tokens) || !closing_index ||
        opening_index >= usk_tokens_content_count(tokens)) return false;
    const Token *first = &tokens->items[opening_index];
    if (!is_opening_delimiter(first->text)) return false;
    size_t capacity = 32, depth = 0;
    char *stack = (char *)malloc(capacity);
    if (!stack) return false;
    stack[depth++] = first->text[0];
    for (size_t index = opening_index + 1;
         index < usk_tokens_content_count(tokens); ++index) {
        const Token *token = &tokens->items[index];
        if (token->kind != TK_PUNCT) continue;
        if (is_opening_delimiter(token->text)) {
            if (depth == capacity) {
                if (capacity > SIZE_MAX / 2) { free(stack); return false; }
                size_t next_capacity = capacity * 2;
                char *grown = (char *)realloc(stack, next_capacity);
                if (!grown) { free(stack); return false; }
                stack = grown;
                capacity = next_capacity;
            }
            stack[depth++] = token->text[0];
        } else if (is_closing_delimiter(token->text)) {
            if (!depth) { free(stack); return false; }
            char opening[2] = {stack[depth - 1], '\0'};
            if (!delimiters_match(opening, token->text)) {
                free(stack);
                return false;
            }
            if (--depth == 0) {
                *closing_index = index;
                free(stack);
                return true;
            }
        }
    }
    free(stack);
    return false;
}

void usk_tokens_free(Tokens *tokens) {
    if (!tokens) return;
    for (size_t index = 0; index < tokens->count; ++index)
        free(tokens->items[index].text);
    free(tokens->items);
    memset(tokens, 0, sizeof(*tokens));
}
