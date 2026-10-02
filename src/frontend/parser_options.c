#include "parser_internal.h"

#include <string.h>
#include <stdint.h>

void usk_parser_options_init(UskParserOptions *options) {
    if (!options) return;
    options->maximum_tokens = 1000000;
    options->maximum_errors = 100;
    options->maximum_nesting_depth = 512;
    options->recover_after_error = true;
}

UskParserOptions usk_parser_options_editor(void) {
    UskParserOptions options;
    usk_parser_options_init(&options);
    options.maximum_tokens = 250000;
    options.maximum_errors = 25;
    options.maximum_nesting_depth = 256;
    return options;
}

UskParserOptions usk_parser_options_batch(void) {
    UskParserOptions options;
    usk_parser_options_init(&options);
    options.maximum_tokens = 4000000;
    options.maximum_errors = 1000;
    options.maximum_nesting_depth = 1024;
    return options;
}

UskParserOptions usk_parser_options_strict(void) {
    UskParserOptions options;
    usk_parser_options_init(&options);
    options.maximum_errors = 1;
    options.recover_after_error = false;
    return options;
}

bool usk_parser_options_validate(const UskParserOptions *options,
                                 UskDiagnosticList *diagnostics,
                                 const char *source_name) {
    if (!options) {
        if (diagnostics)
            usk_diagnostics_add(diagnostics, USK_DIAGNOSTIC_ERROR,
                USK_DIAG_INVALID_DECLARATION, source_name, 0, 0,
                "parser options must not be null");
        return false;
    }
    if (options->maximum_errors && options->maximum_tokens &&
        options->maximum_errors > options->maximum_tokens) {
        if (diagnostics)
            usk_diagnostics_add(diagnostics, USK_DIAGNOSTIC_ERROR,
                USK_DIAG_INVALID_DECLARATION, source_name, 0, 0,
                "maximum parser errors cannot exceed the token limit");
        return false;
    }
    if (options->maximum_tokens > SIZE_MAX / sizeof(Token) - 1) {
        if (diagnostics)
            usk_diagnostics_add(diagnostics, USK_DIAGNOSTIC_ERROR,
                USK_DIAG_INVALID_DECLARATION, source_name, 0, 0,
                "maximum token limit cannot fit in addressable memory");
        return false;
    }
    return true;
}

bool usk_parser_options_equal(const UskParserOptions *left,
                             const UskParserOptions *right) {
    return left && right && left->maximum_tokens == right->maximum_tokens &&
        left->maximum_errors == right->maximum_errors &&
        left->maximum_nesting_depth == right->maximum_nesting_depth &&
        left->recover_after_error == right->recover_after_error;
}

void usk_parse_stats_clear(UskParseStats *stats) {
    if (stats) memset(stats, 0, sizeof(*stats));
}

bool usk_parse_stats_hit_limit(const UskParseStats *stats) {
    return stats && (stats->token_limit_reached || stats->error_limit_reached ||
                     stats->nesting_limit_reached);
}

static size_t saturating_add(size_t left, size_t right) {
    return right > SIZE_MAX - left ? SIZE_MAX : left + right;
}

void usk_parse_stats_add_tokens(UskParseStats *stats, size_t amount) {
    if (stats) stats->tokens_consumed = saturating_add(stats->tokens_consumed,
                                                       amount);
}

void usk_parse_stats_add_declarations(UskParseStats *stats, size_t amount) {
    if (stats) stats->declarations = saturating_add(stats->declarations,
                                                    amount);
}

void usk_parse_stats_add_errors(UskParseStats *stats, size_t amount) {
    if (stats) stats->syntax_errors = saturating_add(stats->syntax_errors,
                                                     amount);
}

void usk_parse_stats_add_recoveries(UskParseStats *stats, size_t amount) {
    if (stats) stats->recovery_scans = saturating_add(stats->recovery_scans,
                                                      amount);
}

void usk_parse_stats_merge(UskParseStats *destination,
                           const UskParseStats *source) {
    if (!destination || !source) return;
    destination->token_count = saturating_add(destination->token_count,
                                              source->token_count);
    destination->tokens_consumed = saturating_add(destination->tokens_consumed,
                                                  source->tokens_consumed);
    destination->declarations = saturating_add(destination->declarations,
                                               source->declarations);
    destination->syntax_errors = saturating_add(destination->syntax_errors,
                                                source->syntax_errors);
    destination->recovery_scans = saturating_add(destination->recovery_scans,
                                                 source->recovery_scans);
    if (source->maximum_nesting_depth > destination->maximum_nesting_depth)
        destination->maximum_nesting_depth = source->maximum_nesting_depth;
    destination->token_limit_reached |= source->token_limit_reached;
    destination->error_limit_reached |= source->error_limit_reached;
    destination->nesting_limit_reached |= source->nesting_limit_reached;
}

bool usk_parser_enter_recursion(UskParser *parser) {
    if (!parser) return false;
    parser->recursion_depth++;
    if (parser->stats && parser->recursion_depth >
                         parser->stats->maximum_nesting_depth)
        parser->stats->maximum_nesting_depth = parser->recursion_depth;
    if (parser->options.maximum_nesting_depth &&
        parser->recursion_depth > parser->options.maximum_nesting_depth) {
        parser->recursion_depth--;
        usk_parser_report_limit(parser, "nesting depth");
        return false;
    }
    return true;
}

void usk_parser_leave_recursion(UskParser *parser) {
    if (parser && parser->recursion_depth) parser->recursion_depth--;
}

size_t usk_parser_remaining_tokens(const UskParser *parser) {
    if (!parser || !parser->tokens || parser->current >= parser->tokens->count)
        return 0;
    return parser->tokens->count - parser->current - 1;
}

bool usk_parser_has_room_for(const UskParser *parser, size_t lookahead) {
    return parser && parser->tokens &&
           lookahead <= usk_parser_remaining_tokens(parser);
}

const char *usk_parser_expected_context(const UskParser *parser) {
    if (!parser || !parser->source_name) return "<source>";
    return parser->source_name;
}

bool usk_parser_stopped(const UskParser *parser) {
    return !parser || parser->stopped;
}

void usk_parser_stop(UskParser *parser) {
    if (parser) parser->stopped = true;
}

bool usk_parser_options_has_finite_error_limit(
    const UskParserOptions *options) {
    return options && options->maximum_errors != 0;
}

bool usk_parser_options_has_finite_token_limit(
    const UskParserOptions *options) {
    return options && options->maximum_tokens != 0;
}

bool usk_parser_options_has_finite_nesting_limit(
    const UskParserOptions *options) {
    return options && options->maximum_nesting_depth != 0;
}
