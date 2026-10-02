#include "parser_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static UskAstExpr *new_expression(UskParser *parser, UskAstExprKind kind,
                                 size_t first_token) {
    UskAstExpr *expression = usk_ast_new_expression(
        parser->program, kind, usk_parser_span(parser, first_token,
                                               parser->current ? parser->current - 1 : first_token));
    if (!expression)
        usk_parser_error(parser, USK_DIAG_OUT_OF_MEMORY, "cannot allocate an expression node");
    return expression;
}

static UskAstExprList *append_expression(UskParser *parser,
                                         UskAstExprList **first,
                                         UskAstExprList **last,
                                         UskAstExpr *expression,
                                         size_t *count) {
    UskAstExprList *item = (UskAstExprList *)usk_ast_allocate(parser->program, sizeof(*item));
    if (!item) {
        usk_parser_error(parser, USK_DIAG_OUT_OF_MEMORY, "cannot allocate an expression list item");
        return NULL;
    }
    item->expression = expression;
    if (*last) (*last)->next = item;
    else *first = item;
    *last = item;
    if (count) (*count)++;
    return item;
}

static int operator_precedence(const char *operator_text) {
    if (!strcmp(operator_text, "=") || !strcmp(operator_text, "+=") ||
        !strcmp(operator_text, "-=") || !strcmp(operator_text, "*=") ||
        !strcmp(operator_text, "/=") || !strcmp(operator_text, "%=")) return 1;
    if (!strcmp(operator_text, "?")) return 2;
    if (!strcmp(operator_text, "||")) return 3;
    if (!strcmp(operator_text, "&&")) return 4;
    if (!strcmp(operator_text, "==") || !strcmp(operator_text, "!=")) return 5;
    if (!strcmp(operator_text, "<") || !strcmp(operator_text, ">") ||
        !strcmp(operator_text, "<=") || !strcmp(operator_text, ">=")) return 6;
    if (!strcmp(operator_text, "+") || !strcmp(operator_text, "-")) return 7;
    if (!strcmp(operator_text, "*") || !strcmp(operator_text, "/") ||
        !strcmp(operator_text, "%")) return 8;
    return 0;
}

static UskAstExpr *parse_precedence(UskParser *parser, int minimum_precedence);

static UskAstExpr *parse_array_literal(UskParser *parser, size_t start) {
    UskAstExprList *items = NULL, *last = NULL;
    size_t count = 0;
    if (!usk_parser_check(parser, "]")) {
        do {
            UskAstExpr *item = parse_precedence(parser, 1);
            append_expression(parser, &items, &last, item, &count);
        } while (usk_parser_match(parser, ",") && !usk_parser_check(parser, "]"));
    }
    if (!usk_parser_consume(parser, "]", USK_DIAG_EXPECTED_TOKEN,
                            "expected ']' after array elements")) return NULL;
    UskAstExpr *array = new_expression(parser, USK_EXPR_ARRAY, start);
    if (array) {
        array->as.array.items = items;
        array->as.array.count = count;
    }
    return array;
}

static UskAstExpr *parse_primary(UskParser *parser) {
    if (!usk_parser_enter_recursion(parser)) return NULL;
    size_t start = parser->current;
    const Token *token = usk_parser_peek(parser);
    UskAstExpr *expression = NULL;

    if (usk_parser_match(parser, "(")) {
        expression = parse_precedence(parser, 1);
        usk_parser_consume(parser, ")", USK_DIAG_EXPECTED_TOKEN,
                           "expected ')' after grouped expression");
    } else if (usk_parser_match(parser, "[")) {
        expression = parse_array_literal(parser, start);
    } else if (token && token->kind == TK_NUMBER) {
        token = usk_parser_advance(parser);
        expression = new_expression(parser,
            strchr(token->text, '.') ? USK_EXPR_DOUBLE : USK_EXPR_INTEGER, start);
        if (expression) {
            errno = 0;
            if (expression->kind == USK_EXPR_INTEGER) {
                expression->as.integer = strtoll(token->text, NULL, 10);
                if (errno == ERANGE)
                    usk_parser_error(parser, USK_DIAG_INVALID_LITERAL,
                                     "integer literal is outside the supported range");
            } else {
                expression->as.floating = strtod(token->text, NULL);
                if (errno == ERANGE)
                    usk_parser_error(parser, USK_DIAG_INVALID_LITERAL,
                                     "floating-point literal is outside the supported range");
            }
        }
    } else if (token && token->kind == TK_STRING) {
        token = usk_parser_advance(parser);
        expression = new_expression(parser, USK_EXPR_STRING, start);
        if (expression) expression->as.string = usk_ast_copy_text(parser->program, token->text);
    } else if (token && token->kind == TK_ID && token->keyword == USK_KW_TRUE) {
        usk_parser_advance(parser);
        expression = new_expression(parser, USK_EXPR_BOOLEAN, start);
        if (expression) expression->as.boolean = true;
    } else if (token && token->kind == TK_ID && token->keyword == USK_KW_FALSE) {
        usk_parser_advance(parser);
        expression = new_expression(parser, USK_EXPR_BOOLEAN, start);
        if (expression) expression->as.boolean = false;
    } else if (token && token->kind == TK_ID && token->keyword == USK_KW_NULL_TYPE) {
        usk_parser_advance(parser);
        expression = new_expression(parser, USK_EXPR_NULL, start);
    } else if (token && token->kind == TK_ID && token->keyword != USK_KW_NONE) {
        /* Keywords are names in type/member positions; statement parsing still
         * reserves their control-flow and declaration forms. */
        usk_parser_advance(parser);
        expression = new_expression(parser, USK_EXPR_NAME, start);
        if (expression) expression->as.name = usk_ast_copy_text(parser->program, token->text);
    } else {
        usk_parser_error(parser, USK_DIAG_UNEXPECTED_TOKEN,
                         "expected an expression, found '%s'",
                         token ? token->text : "<missing>");
        if (token && token->kind != TK_EOF) usk_parser_advance(parser);
    }

    while (expression && !parser->panic_mode) {
        if (usk_parser_match(parser, "(")) {
            UskAstExprList *arguments = NULL, *last = NULL;
            size_t count = 0;
            if (!usk_parser_check(parser, ")")) {
                do {
                    UskAstExpr *argument = parse_precedence(parser, 1);
                    append_expression(parser, &arguments, &last, argument, &count);
                } while (usk_parser_match(parser, ",") && !usk_parser_check(parser, ")"));
            }
            if (!usk_parser_consume(parser, ")", USK_DIAG_EXPECTED_TOKEN,
                                    "expected ')' after function arguments")) break;
            UskAstExpr *call = new_expression(parser, USK_EXPR_CALL, start);
            if (call) {
                call->as.call.callee = expression;
                call->as.call.arguments = arguments;
                call->as.call.count = count;
            }
            expression = call;
        } else if (usk_parser_match(parser, "[")) {
            UskAstExpr *index = parse_precedence(parser, 1);
            if (!usk_parser_consume(parser, "]", USK_DIAG_EXPECTED_TOKEN,
                                    "expected ']' after index expression")) break;
            UskAstExpr *indexed = new_expression(parser, USK_EXPR_INDEX, start);
            if (indexed) { indexed->as.index.object = expression; indexed->as.index.index = index; }
            expression = indexed;
        } else if (usk_parser_check(parser, ".") || usk_parser_check(parser, "->") ||
                   usk_parser_check(parser, "::")) {
            const char *separator = usk_parser_advance(parser)->text;
            bool pointer_access = strcmp(separator, "->") == 0;
            const Token *member = usk_parser_peek(parser);
            if (!member || member->kind != TK_ID) {
                usk_parser_error(parser, USK_DIAG_EXPECTED_TOKEN,
                                 "expected a member name after '%s'", separator);
                break;
            }
            usk_parser_advance(parser);
            UskAstExpr *access = new_expression(parser, USK_EXPR_MEMBER, start);
            if (access) {
                access->as.member.object = expression;
                access->as.member.name = usk_ast_copy_text(parser->program, member->text);
                access->as.member.pointer_access = pointer_access;
            }
            expression = access;
        } else if (usk_parser_check(parser, "++") || usk_parser_check(parser, "--")) {
            const char *operator_text = usk_parser_advance(parser)->text;
            operator_text = usk_ast_copy_text(parser->program,
                strcmp(operator_text, "++") == 0 ? "post++" : "post--");
            UskAstExpr *increment = new_expression(parser, USK_EXPR_UNARY, start);
            if (increment) {
                increment->as.unary.operator = operator_text;
                increment->as.unary.operand = expression;
            }
            expression = increment;
        } else break;
    }
    usk_parser_leave_recursion(parser);
    return expression;
}

static UskAstExpr *parse_unary(UskParser *parser) {
    if (usk_parser_check(parser, "!") || usk_parser_check(parser, "-") ||
        usk_parser_check(parser, "+") || usk_parser_check(parser, "++") ||
        usk_parser_check(parser, "--")) {
        size_t start = parser->current;
        const char *operator_text = usk_ast_copy_text(
            parser->program, usk_parser_advance(parser)->text);
        UskAstExpr *operand = parse_unary(parser);
        UskAstExpr *expression = new_expression(parser, USK_EXPR_UNARY, start);
        if (expression) {
            expression->as.unary.operator = operator_text;
            expression->as.unary.operand = operand;
        }
        return expression;
    }
    return parse_primary(parser);
}

static UskAstExpr *parse_precedence(UskParser *parser, int minimum_precedence) {
    UskAstExpr *left = parse_unary(parser);
    while (left && !parser->panic_mode) {
        const char *operator_text = usk_parser_peek(parser)->text;
        int precedence = operator_precedence(operator_text);
        if (!precedence || precedence < minimum_precedence) break;
        size_t start = left->span.first_token;
        usk_parser_advance(parser);

        if (!strcmp(operator_text, "?")) {
            UskAstExpr *when_true = parse_precedence(parser, 1);
            usk_parser_consume(parser, ":", USK_DIAG_EXPECTED_TOKEN,
                               "expected ':' in conditional expression");
            UskAstExpr *when_false = parse_precedence(parser, precedence);
            UskAstExpr *conditional = new_expression(parser, USK_EXPR_CONDITIONAL, start);
            if (conditional) {
                conditional->as.conditional.condition = left;
                conditional->as.conditional.when_true = when_true;
                conditional->as.conditional.when_false = when_false;
            }
            left = conditional;
            continue;
        }

        bool assignment = precedence == 1;
        UskAstExpr *right = parse_precedence(parser,
                                              assignment ? precedence : precedence + 1);
        UskAstExpr *combined = new_expression(parser,
            assignment ? USK_EXPR_ASSIGNMENT : USK_EXPR_BINARY, start);
        if (combined && assignment) {
            combined->as.assignment.operator = usk_ast_copy_text(parser->program, operator_text);
            combined->as.assignment.target = left;
            combined->as.assignment.value = right;
        } else if (combined) {
            combined->as.binary.operator = usk_ast_copy_text(parser->program, operator_text);
            combined->as.binary.left = left;
            combined->as.binary.right = right;
        }
        left = combined;
    }
    return left;
}

UskAstExpr *usk_parser_expression(UskParser *parser) {
    return parse_precedence(parser, 1);
}
