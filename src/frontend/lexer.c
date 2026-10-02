#include "usk/lexer.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    UskLexerResult *result;
    UskLexerOptions options;
    const char *source_name;
    const char *source;
    size_t position;
    int line;
    int column;
    bool stopped;
} Lexer;

static void report_error(Lexer *lexer, UskDiagnosticCode code,
                         int line, int column, const char *message) {
    UskLexerResult *result = lexer->result;
    result->succeeded = false;
    result->stats.lexical_errors++;
    usk_diagnostics_add(&result->diagnostics, USK_DIAGNOSTIC_ERROR, code,
        lexer->source_name, line, column, "%s", message);
}

static void report_formatted_error(Lexer *lexer, UskDiagnosticCode code,
                                   int line, int column,
                                   const char *prefix, size_t value) {
    UskLexerResult *result = lexer->result;
    result->succeeded = false;
    result->stats.lexical_errors++;
    usk_diagnostics_add(&result->diagnostics, USK_DIAGNOSTIC_ERROR, code,
        lexer->source_name, line, column, "%s%zu", prefix, value);
}

static char *copy_text(const char *source, size_t length) {
    if (length == SIZE_MAX) return NULL;
    char *copy = (char *)malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, source, length);
    copy[length] = '\0';
    return copy;
}

static bool grow_tokens(Tokens *tokens) {
    if (tokens->capacity > SIZE_MAX / 2 / sizeof(*tokens->items)) return false;
    size_t capacity = tokens->capacity ? tokens->capacity * 2 : 128;
    Token *items = (Token *)realloc(tokens->items, capacity * sizeof(*items));
    if (!items) return false;
    tokens->items = items;
    tokens->capacity = capacity;
    return true;
}

static bool append_token(Lexer *lexer, TokenKind kind, const char *text,
                         size_t text_length, size_t source_offset,
                         size_t source_length, int line, int column,
                         bool is_eof) {
    Tokens *tokens = &lexer->result->tokens;
    if (!is_eof && lexer->options.maximum_tokens &&
        lexer->result->stats.emitted_tokens >= lexer->options.maximum_tokens) {
        report_error(lexer, USK_DIAG_RESOURCE_LIMIT, line, column,
                     "token limit reached; remaining source was not tokenized");
        lexer->result->stats.token_limit_reached = true;
        lexer->stopped = true;
        return false;
    }
    if (tokens->count == tokens->capacity && !grow_tokens(tokens)) {
        report_error(lexer, USK_DIAG_OUT_OF_MEMORY, line, column,
                     "cannot grow the token array");
        lexer->stopped = true;
        return false;
    }
    char *token_text = copy_text(text, text_length);
    if (!token_text) {
        report_error(lexer, USK_DIAG_OUT_OF_MEMORY, line, column,
                     "cannot allocate token text");
        lexer->stopped = true;
        return false;
    }
    TokenKeyword keyword = kind == TK_ID ? usk_keyword_from_text(token_text)
                                         : USK_KW_NONE;
    tokens->items[tokens->count++] = (Token){
        .kind = kind,
        .keyword = keyword,
        .text = token_text,
        .line = line,
        .column = column,
        .source_offset = source_offset,
        .source_length = source_length
    };
    if (!is_eof) lexer->result->stats.emitted_tokens++;
    return true;
}

static bool is_name_character(unsigned char character) {
    return isalnum(character) || character == '_';
}

static bool is_newline_marker(const char *source, size_t position) {
    return source[position] == '/' && source[position + 1] == 'n' &&
           !is_name_character((unsigned char)source[position + 2]);
}

static bool append_string_character(char **buffer, size_t *length,
                                    size_t *capacity, char character) {
    if (*length > SIZE_MAX - 2) return false;
    if (*length + 2 > *capacity) {
        if (*capacity > SIZE_MAX / 2) return false;
        size_t new_capacity = *capacity * 2;
        char *new_buffer = (char *)realloc(*buffer, new_capacity);
        if (!new_buffer) return false;
        *buffer = new_buffer;
        *capacity = new_capacity;
    }
    (*buffer)[(*length)++] = character;
    return true;
}

static void advance_character(Lexer *lexer) {
    unsigned char character = (unsigned char)lexer->source[lexer->position++];
    if (character == '\n') {
        lexer->line++;
        lexer->column = 1;
    } else {
        lexer->column++;
    }
}

static bool scan_line_comment(Lexer *lexer) {
    size_t start = lexer->position;
    while (lexer->source[lexer->position] &&
           lexer->source[lexer->position] != '\n')
        advance_character(lexer);
    lexer->result->stats.comments++;
    return lexer->position > start;
}

static bool scan_block_comment(Lexer *lexer, int start_line,
                               int start_column) {
    lexer->position += 2;
    lexer->column += 2;
    unsigned depth = 1;
    while (lexer->source[lexer->position] && depth) {
        if (lexer->source[lexer->position] == '/' &&
            lexer->source[lexer->position + 1] == '*' &&
            lexer->options.allow_nested_block_comments) {
            if (depth == UINT32_MAX) {
                report_error(lexer, USK_DIAG_RESOURCE_LIMIT, lexer->line,
                    lexer->column, "nested block comment depth limit reached");
                return false;
            }
            depth++;
            lexer->position += 2;
            lexer->column += 2;
        } else if (lexer->source[lexer->position] == '*' &&
                   lexer->source[lexer->position + 1] == '/') {
            depth--;
            lexer->position += 2;
            lexer->column += 2;
        } else {
            advance_character(lexer);
        }
    }
    lexer->result->stats.comments++;
    if (depth) {
        report_error(lexer, USK_DIAG_UNTERMINATED_LITERAL,
                     start_line, start_column,
                     "unterminated block comment");
        return false;
    }
    return true;
}

static bool scan_identifier(Lexer *lexer) {
    size_t start = lexer->position;
    int line = lexer->line, column = lexer->column;
    while (is_name_character((unsigned char)lexer->source[lexer->position]))
        advance_character(lexer);
    if (!append_token(lexer, TK_ID, lexer->source + start,
            lexer->position - start, start, lexer->position - start,
            line, column, false)) return false;
    lexer->result->stats.identifiers++;
    return true;
}

static bool scan_number(Lexer *lexer) {
    size_t start = lexer->position;
    int line = lexer->line, column = lexer->column;
    bool has_decimal = false;
    while (isdigit((unsigned char)lexer->source[lexer->position]) ||
           (!has_decimal && lexer->source[lexer->position] == '.' &&
            isdigit((unsigned char)lexer->source[lexer->position + 1]))) {
        if (lexer->source[lexer->position] == '.') has_decimal = true;
        advance_character(lexer);
    }
    if (!append_token(lexer, TK_NUMBER, lexer->source + start,
            lexer->position - start, start, lexer->position - start,
            line, column, false)) return false;
    lexer->result->stats.numeric_literals++;
    return true;
}

static char decode_escape(char escaped, bool *valid) {
    switch (escaped) {
        case 'n': return '\n';
        case 'r': return '\r';
        case 't': return '\t';
        case 'b': return '\b';
        case 'f': return '\f';
        case 'v': return '\v';
        case 'a': return '\a';
        case '\\': return '\\';
        case '"': return '"';
        case '\'': return '\'';
        case '0': *valid = false; return ' ';
        default: return escaped;
    }
}

static bool scan_string(Lexer *lexer) {
    size_t start = lexer->position;
    int start_line = lexer->line, start_column = lexer->column;
    char quote = lexer->source[lexer->position];
    size_t capacity = 32, length = 0;
    char *buffer = (char *)malloc(capacity);
    bool closed = false;
    bool literal_limit_reported = false;
    if (!buffer) {
        report_error(lexer, USK_DIAG_OUT_OF_MEMORY, start_line, start_column,
                     "cannot allocate a string literal buffer");
        lexer->stopped = true;
        return false;
    }
    advance_character(lexer);
    while (lexer->source[lexer->position]) {
        size_t character_offset = lexer->position;
        char current = lexer->source[lexer->position];
        if (current == quote) {
            advance_character(lexer);
            closed = true;
            break;
        }
        if (current == '\\' && lexer->source[lexer->position + 1]) {
            advance_character(lexer);
            bool escape_valid = true;
            current = decode_escape(lexer->source[lexer->position],
                                    &escape_valid);
            advance_character(lexer);
            if (!escape_valid)
                report_error(lexer, USK_DIAG_UNSUPPORTED_FEATURE,
                    start_line, start_column,
                    "embedded NUL characters are not supported in USKString");
        } else if (is_newline_marker(lexer->source, character_offset)) {
            current = '\n';
            advance_character(lexer);
            advance_character(lexer);
            lexer->result->stats.newline_markers++;
        } else {
            advance_character(lexer);
        }

        if (lexer->options.maximum_literal_bytes &&
            length >= lexer->options.maximum_literal_bytes) {
            if (!literal_limit_reported) {
                report_formatted_error(lexer, USK_DIAG_RESOURCE_LIMIT,
                    start_line, start_column,
                    "string literal exceeds byte limit of ",
                    lexer->options.maximum_literal_bytes);
                literal_limit_reported = true;
                lexer->result->stats.literal_limit_reached = true;
            }
            continue;
        }
        if (!append_string_character(&buffer, &length, &capacity, current)) {
            report_error(lexer, USK_DIAG_OUT_OF_MEMORY,
                         start_line, start_column,
                         "cannot grow a string literal buffer");
            lexer->stopped = true;
            free(buffer);
            return false;
        }
    }
    if (!closed) {
        report_error(lexer, USK_DIAG_UNTERMINATED_LITERAL,
                     start_line, start_column, "unterminated string literal");
        free(buffer);
        return false;
    }
    bool appended = append_token(lexer, TK_STRING, buffer, length,
        start, lexer->position - start, start_line, start_column, false);
    free(buffer);
    if (!appended) return false;
    lexer->result->stats.string_literals++;
    return true;
}

static bool scan_operator_or_punctuation(Lexer *lexer) {
    static const char *two_character_tokens[] = {
        "::", "->", "==", "!=", "<=", ">=", "&&", "||", "++", "--",
        "+=", "-=", "*=", "/=", "%=", NULL
    };
    size_t start = lexer->position;
    int line = lexer->line, column = lexer->column;
    for (size_t index = 0; two_character_tokens[index]; ++index) {
        const char *candidate = two_character_tokens[index];
        if (lexer->source[start] == candidate[0] &&
            lexer->source[start + 1] == candidate[1]) {
            lexer->position += 2;
            lexer->column += 2;
            return append_token(lexer, TK_PUNCT, candidate, 2, start, 2,
                                line, column, false);
        }
    }
    unsigned char character = (unsigned char)lexer->source[start];
    if (strchr("{}()[];,:.+-*/%!=<>?", character)) {
        advance_character(lexer);
        return append_token(lexer, TK_PUNCT, lexer->source + start, 1,
                            start, 1, line, column, false);
    }
    char message[96];
    snprintf(message, sizeof(message), "unexpected character byte 0x%02x",
             (unsigned)character);
    report_error(lexer, USK_DIAG_UNEXPECTED_TOKEN, line, column, message);
    advance_character(lexer);
    return true;
}

static void finish_tokens(Lexer *lexer) {
    if (lexer->result->tokens.count &&
        lexer->result->tokens.items[lexer->result->tokens.count - 1].kind == TK_EOF)
        return;
    append_token(lexer, TK_EOF, "<eof>", 5, lexer->position, 0,
                 lexer->line, lexer->column, true);
}

UskLexerResult usk_lex_source_ex(const char *source, const char *source_name,
                                 const UskLexerOptions *options) {
    UskLexerResult result;
    usk_lexer_result_init(&result);
    UskLexerOptions effective;
    usk_lexer_options_init(&effective);
    if (options) effective = *options;
    if (!source) {
        usk_diagnostics_add(&result.diagnostics, USK_DIAGNOSTIC_ERROR,
            USK_DIAG_INVALID_DECLARATION, source_name, 0, 0,
            "source text must not be null");
        result.succeeded = false;
        result.stats.lexical_errors = 1;
        return result;
    }

    size_t source_bytes = strlen(source);
    result.stats.source_bytes = source_bytes;
    Lexer lexer = {
        .result = &result,
        .options = effective,
        .source_name = source_name ? source_name : "<source>",
        .source = source,
        .line = 1,
        .column = 1
    };
    if (effective.maximum_source_bytes &&
        source_bytes > effective.maximum_source_bytes) {
        report_formatted_error(&lexer, USK_DIAG_RESOURCE_LIMIT, 1, 1,
            "source exceeds byte limit of ", effective.maximum_source_bytes);
        result.stats.source_limit_reached = true;
        finish_tokens(&lexer);
        return result;
    }

    while (source[lexer.position] && !lexer.stopped) {
        unsigned char character = (unsigned char)source[lexer.position];
        if (isspace(character)) {
            advance_character(&lexer);
            continue;
        }
        if ((character == '/' && source[lexer.position + 1] == '/') ||
            (character == '\\' && source[lexer.position + 1] == '\\')) {
            scan_line_comment(&lexer);
            continue;
        }
        if (character == '/' && source[lexer.position + 1] == '*') {
            int line = lexer.line, column = lexer.column;
            if (!scan_block_comment(&lexer, line, column)) break;
            continue;
        }
        if (is_newline_marker(source, lexer.position)) {
            size_t start = lexer.position;
            int line = lexer.line, column = lexer.column;
            lexer.position += 2;
            lexer.column += 2;
            result.stats.newline_markers++;
            if (!append_token(&lexer, TK_STRING, "\n", 1, start, 2,
                              line, column, false)) break;
            continue;
        }
        if (isalpha(character) || character == '_') {
            if (!scan_identifier(&lexer)) break;
            continue;
        }
        if (isdigit(character)) {
            if (!scan_number(&lexer)) break;
            continue;
        }
        if (character == '"' || character == '\'') {
            if (!scan_string(&lexer)) break;
            continue;
        }
        if (!scan_operator_or_punctuation(&lexer)) break;
    }
    finish_tokens(&lexer);
    return result;
}

