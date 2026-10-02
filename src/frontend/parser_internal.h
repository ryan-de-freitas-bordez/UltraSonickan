#ifndef USK_PARSER_INTERNAL_H
#define USK_PARSER_INTERNAL_H

#include "usk/parser.h"

typedef struct {
    const Tokens *tokens;
    size_t current;
    const char *source_name;
    UskAstProgram *program;
    UskDiagnosticList *diagnostics;
    UskParserOptions options;
    UskParseStats *stats;
    unsigned recursion_depth;
    bool panic_mode;
    bool stopped;
} UskParser;

const Token *usk_parser_peek(const UskParser *parser);
const Token *usk_parser_previous(const UskParser *parser);
const Token *usk_parser_at(const UskParser *parser, size_t index);
bool usk_parser_check(const UskParser *parser, const char *text);
bool usk_parser_match(UskParser *parser, const char *text);
const Token *usk_parser_advance(UskParser *parser);
bool usk_parser_consume(UskParser *parser, const char *text,
                        UskDiagnosticCode code, const char *message);
void usk_parser_error(UskParser *parser, UskDiagnosticCode code,
                      const char *format, ...);
void usk_parser_synchronize(UskParser *parser);
void usk_parser_report_limit(UskParser *parser, const char *limit_name);
bool usk_parser_at_declaration_boundary(const UskParser *parser);
bool usk_parser_enter_recursion(UskParser *parser);
void usk_parser_leave_recursion(UskParser *parser);
size_t usk_parser_remaining_tokens(const UskParser *parser);
bool usk_parser_has_room_for(const UskParser *parser, size_t lookahead);
const char *usk_parser_expected_context(const UskParser *parser);
bool usk_parser_stopped(const UskParser *parser);
void usk_parser_stop(UskParser *parser);
bool usk_parser_options_has_finite_error_limit(
    const UskParserOptions *options);
bool usk_parser_options_has_finite_token_limit(
    const UskParserOptions *options);
bool usk_parser_options_has_finite_nesting_limit(
    const UskParserOptions *options);
size_t usk_parser_find_next_declaration(const UskParser *parser,
                                       size_t from_index);
size_t usk_parser_find_matching_delimiter(const UskParser *parser,
                                          size_t from_index,
                                          const char *opening,
                                          const char *closing);
bool usk_parser_recovery_can_continue(const UskParser *parser,
                                     size_t start_index);
bool usk_parser_recovery_should_unwind(const UskParser *parser,
                                       unsigned open_blocks);
UskSourceSpan usk_parser_span(UskParser *parser, size_t first, size_t last);
UskAstType *usk_parser_new_type(UskParser *parser, const char *name);
UskAstExpr *usk_parser_expression(UskParser *parser);
UskAstStmt *usk_parser_statement(UskParser *parser);
UskAstDecl *usk_parser_declaration(UskParser *parser, bool class_member);
UskAstDecl *usk_parser_variable_declaration(UskParser *parser);
UskAstStmt *usk_parser_block(UskParser *parser);

#endif
