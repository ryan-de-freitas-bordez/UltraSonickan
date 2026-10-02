#include "parser_internal.h"

#include <string.h>

static bool is_type_name(const char *text) {
    static const char *types[] = {"USKInt", "USKDouble", "USKString", "USKChar",
        "USKFloat", "USKBool", "USKNull", "USKAuto", "USKLong", "USKShort", NULL};
    for (size_t index = 0; types[index]; ++index)
        if (strcmp(text, types[index]) == 0) return true;
    return false;
}

static bool starts_variable_declaration(const UskParser *parser) {
    if (usk_parser_check(parser, "var") || usk_parser_check(parser, "const") ||
        is_type_name(usk_parser_peek(parser)->text)) return true;
    if (usk_parser_peek(parser)->kind == TK_ID &&
        usk_parser_at(parser, parser->current + 1)->kind == TK_ID) return true;
    return usk_parser_check(parser, "-") &&
        strcmp(usk_parser_at(parser, parser->current + 1)->text, "(") == 0;
}

static UskAstStmt *new_statement(UskParser *parser, UskAstStmtKind kind,
                                size_t start) {
    UskAstStmt *statement = usk_ast_new_statement(
        parser->program, kind,
        usk_parser_span(parser, start, parser->current ? parser->current - 1 : start));
    if (!statement)
        usk_parser_error(parser, USK_DIAG_OUT_OF_MEMORY, "cannot allocate a statement node");
    return statement;
}

static UskAstStmtList *append_statement(UskParser *parser,
                                       UskAstStmtList **first,
                                       UskAstStmtList **last,
                                       UskAstStmt *statement,
                                       size_t *count) {
    UskAstStmtList *item = (UskAstStmtList *)usk_ast_allocate(parser->program, sizeof(*item));
    if (!item) {
        usk_parser_error(parser, USK_DIAG_OUT_OF_MEMORY, "cannot allocate a statement list item");
        return NULL;
    }
    item->statement = statement;
    if (*last) (*last)->next = item;
    else *first = item;
    *last = item;
    if (count) (*count)++;
    return item;
}

UskAstStmt *usk_parser_block(UskParser *parser) {
    size_t start = parser->current;
    if (!usk_parser_consume(parser, "{", USK_DIAG_EXPECTED_TOKEN,
                            "expected '{' to begin a block")) return NULL;
    UskAstStmt *block = new_statement(parser, USK_STMT_BLOCK, start);
    UskAstStmtList *first = NULL, *last = NULL;
    size_t count = 0;
    while (!parser->stopped && !usk_parser_check(parser, "}") &&
           usk_parser_peek(parser)->kind != TK_EOF) {
        size_t before = parser->current;
        UskAstStmt *item = usk_parser_statement(parser);
        if (item) append_statement(parser, &first, &last, item, &count);
        if (parser->current == before) {
            usk_parser_error(parser, USK_DIAG_UNEXPECTED_TOKEN,
                             "parser cannot continue at '%s'",
                             usk_parser_peek(parser)->text);
            usk_parser_advance(parser);
        }
        if (parser->panic_mode) usk_parser_synchronize(parser);
    }
    usk_parser_consume(parser, "}", USK_DIAG_EXPECTED_TOKEN,
                       "expected '}' after block statements");
    if (block) {
        block->as.block.items = first;
        block->as.block.count = count;
        block->span.last_token = parser->current ? parser->current - 1 : start;
    }
    return block;
}

static UskAstStmt *parse_if_statement(UskParser *parser, size_t start) {
    if (!usk_parser_consume(parser, "(", USK_DIAG_EXPECTED_TOKEN,
                            "expected '(' after if")) return NULL;
    UskAstExpr *condition = usk_parser_expression(parser);
    usk_parser_consume(parser, ")", USK_DIAG_EXPECTED_TOKEN,
                       "expected ')' after if condition");
    UskAstStmt *then_branch = usk_parser_statement(parser);
    UskAstStmt *else_branch = NULL;
    if (usk_parser_match(parser, "elif")) {
        else_branch = parse_if_statement(parser, parser->current - 1);
    } else if (usk_parser_match(parser, "else")) {
        if (usk_parser_match(parser, "if"))
            else_branch = parse_if_statement(parser, parser->current - 1);
        else else_branch = usk_parser_statement(parser);
    }
    UskAstStmt *statement = new_statement(parser, USK_STMT_IF, start);
    if (statement) {
        statement->as.if_stmt.condition = condition;
        statement->as.if_stmt.then_branch = then_branch;
        statement->as.if_stmt.else_branch = else_branch;
    }
    return statement;
}

static UskAstStmt *parse_switch_statement(UskParser *parser, size_t start) {
    usk_parser_consume(parser, "(", USK_DIAG_EXPECTED_TOKEN,
                       "expected '(' after switch");
    UskAstExpr *selector = usk_parser_expression(parser);
    usk_parser_consume(parser, ")", USK_DIAG_EXPECTED_TOKEN,
                       "expected ')' after switch expression");
    usk_parser_consume(parser, "{", USK_DIAG_EXPECTED_TOKEN,
                       "expected '{' before switch cases");
    UskAstSwitchCase *first_case = NULL, *last_case = NULL;
    while (!parser->stopped && !usk_parser_check(parser, "}") &&
           usk_parser_peek(parser)->kind != TK_EOF) {
        size_t label_start = parser->current;
        bool is_default = usk_parser_match(parser, "default");
        UskAstExprList *labels = NULL, *last_label = NULL;
        if (!is_default) {
            if (!usk_parser_consume(parser, "case", USK_DIAG_EXPECTED_TOKEN,
                                    "expected case or default label")) break;
            do {
                UskAstExpr *label = usk_parser_expression(parser);
                UskAstExprList *item = (UskAstExprList *)usk_ast_allocate(parser->program, sizeof(*item));
                if (!item) { usk_parser_error(parser, USK_DIAG_OUT_OF_MEMORY, "cannot allocate a case label"); break; }
                item->expression = label;
                if (last_label) last_label->next = item;
                else labels = item;
                last_label = item;
            } while (usk_parser_match(parser, ","));
        }
        usk_parser_consume(parser, ":", USK_DIAG_EXPECTED_TOKEN,
                           "expected ':' after switch label");
        UskAstStmtList *statements = NULL, *last_statement = NULL;
        while (!parser->stopped && !usk_parser_check(parser, "case") &&
               !usk_parser_check(parser, "default") &&
               !usk_parser_check(parser, "}") && usk_parser_peek(parser)->kind != TK_EOF) {
            size_t before = parser->current;
            UskAstStmt *statement = usk_parser_statement(parser);
            if (statement) append_statement(parser, &statements, &last_statement, statement, NULL);
            if (parser->current == before) {
                usk_parser_error(parser, USK_DIAG_UNEXPECTED_TOKEN,
                                 "expected a statement in switch arm");
                usk_parser_advance(parser);
            }
        }
        UskAstSwitchCase *switch_case = (UskAstSwitchCase *)usk_ast_allocate(parser->program, sizeof(*switch_case));
        if (!switch_case) { usk_parser_error(parser, USK_DIAG_OUT_OF_MEMORY, "cannot allocate a switch arm"); break; }
        switch_case->labels = labels;
        switch_case->statements = statements;
        switch_case->is_default = is_default;
        switch_case->span = usk_parser_span(parser, label_start,
                                             parser->current ? parser->current - 1 : label_start);
        if (last_case) last_case->next = switch_case;
        else first_case = switch_case;
        last_case = switch_case;
    }
    usk_parser_consume(parser, "}", USK_DIAG_EXPECTED_TOKEN,
                       "expected '}' after switch cases");
    UskAstStmt *statement = new_statement(parser, USK_STMT_SWITCH, start);
    if (statement) {
        statement->as.switch_stmt.selector = selector;
        statement->as.switch_stmt.cases = first_case;
    }
    return statement;
}

static UskAstStmt *parse_for_statement(UskParser *parser, size_t start) {
    usk_parser_consume(parser, "(", USK_DIAG_EXPECTED_TOKEN,
                       "expected '(' after for");
    UskAstStmt *initializer = NULL;
    if (!usk_parser_check(parser, ";") && !usk_parser_check(parser, ")")) {
        if (starts_variable_declaration(parser)) {
            size_t declaration_start = parser->current;
            UskAstDecl *declaration = usk_parser_variable_declaration(parser);
            if (declaration && declaration->kind == USK_DECL_VARIABLE) {
                initializer = new_statement(parser, USK_STMT_VARIABLE, declaration_start);
                if (initializer) {
                    initializer->as.variable.type = declaration->as.variable.type;
                    initializer->as.variable.name = declaration->as.variable.name;
                    initializer->as.variable.initializer = declaration->as.variable.initializer;
                }
            }
            if (usk_parser_match(parser, "in")) {
                UskAstExpr *collection = usk_parser_expression(parser);
                usk_parser_consume(parser, ")", USK_DIAG_EXPECTED_TOKEN,
                                   "expected ')' after for-in collection");
                UskAstStmt *body = usk_parser_statement(parser);
                UskAstStmt *foreach_statement = new_statement(parser, USK_STMT_FOREACH, start);
                if (foreach_statement) {
                    foreach_statement->as.foreach_stmt.type = initializer ? initializer->as.variable.type : NULL;
                    foreach_statement->as.foreach_stmt.name = initializer ? initializer->as.variable.name : NULL;
                    foreach_statement->as.foreach_stmt.collection = collection;
                    foreach_statement->as.foreach_stmt.body = body;
                }
                return foreach_statement;
            }
        } else {
            UskAstExpr *expression = usk_parser_expression(parser);
            initializer = new_statement(parser, USK_STMT_EXPRESSION, start);
            if (initializer) initializer->as.expression = expression;
        }
    }

    if (!usk_parser_check(parser, ";")) {
        usk_parser_error(parser, USK_DIAG_EXPECTED_TOKEN,
                         "expected ';' after for initializer");
        return NULL;
    }
    usk_parser_advance(parser);
    UskAstExpr *condition = NULL;
    if (!usk_parser_check(parser, ";")) condition = usk_parser_expression(parser);
    usk_parser_consume(parser, ";", USK_DIAG_EXPECTED_TOKEN,
                       "expected ';' after for condition");
    UskAstExpr *update = NULL;
    if (!usk_parser_check(parser, ")")) update = usk_parser_expression(parser);
    usk_parser_consume(parser, ")", USK_DIAG_EXPECTED_TOKEN,
                       "expected ')' after for clauses");
    UskAstStmt *body = usk_parser_statement(parser);
    UskAstStmt *statement = new_statement(parser, USK_STMT_FOR, start);
    if (statement) {
        statement->as.for_stmt.initializer = initializer;
        statement->as.for_stmt.condition = condition;
        statement->as.for_stmt.update = update;
        statement->as.for_stmt.body = body;
    }
    return statement;
}

UskAstStmt *usk_parser_statement(UskParser *parser) {
    size_t start = parser->current;
    if (usk_parser_check(parser, "{")) return usk_parser_block(parser);
    if (usk_parser_match(parser, ";")) return new_statement(parser, USK_STMT_EMPTY, start);
    if (usk_parser_match(parser, "if")) return parse_if_statement(parser, start);
    if (usk_parser_match(parser, "switch")) return parse_switch_statement(parser, start);
    if (usk_parser_match(parser, "while")) {
        usk_parser_consume(parser, "(", USK_DIAG_EXPECTED_TOKEN,
                           "expected '(' after while");
        UskAstExpr *condition = usk_parser_expression(parser);
        usk_parser_consume(parser, ")", USK_DIAG_EXPECTED_TOKEN,
                           "expected ')' after while condition");
        UskAstStmt *body = usk_parser_statement(parser);
        UskAstStmt *statement = new_statement(parser, USK_STMT_WHILE, start);
        if (statement) { statement->as.while_stmt.condition = condition; statement->as.while_stmt.body = body; }
        return statement;
    }
    if (usk_parser_match(parser, "for")) return parse_for_statement(parser, start);
    if (usk_parser_match(parser, "break")) {
        usk_parser_consume(parser, ";", USK_DIAG_EXPECTED_TOKEN, "expected ';' after break");
        return new_statement(parser, USK_STMT_BREAK, start);
    }
    if (usk_parser_match(parser, "continue")) {
        usk_parser_consume(parser, ";", USK_DIAG_EXPECTED_TOKEN, "expected ';' after continue");
        return new_statement(parser, USK_STMT_CONTINUE, start);
    }
    if (usk_parser_match(parser, "return")) {
        UskAstExpr *value = NULL;
        if (!usk_parser_check(parser, ";") && !usk_parser_check(parser, "}"))
            value = usk_parser_expression(parser);
        usk_parser_consume(parser, ";", USK_DIAG_EXPECTED_TOKEN, "expected ';' after return");
        UskAstStmt *statement = new_statement(parser, USK_STMT_RETURN, start);
        if (statement) statement->as.return_value = value;
        return statement;
    }
    if (starts_variable_declaration(parser)) {
        UskAstDecl *declaration = usk_parser_variable_declaration(parser);
        usk_parser_consume(parser, ";", USK_DIAG_EXPECTED_TOKEN,
                           "expected ';' after variable declaration");
        if (declaration && declaration->kind == USK_DECL_VARIABLE) {
            UskAstStmt *statement = new_statement(parser, USK_STMT_VARIABLE, start);
            if (statement) {
                statement->as.variable.type = declaration->as.variable.type;
                statement->as.variable.name = declaration->as.variable.name;
                statement->as.variable.initializer = declaration->as.variable.initializer;
            }
            return statement;
        }
        return NULL;
    }
    UskAstExpr *expression = usk_parser_expression(parser);
    usk_parser_consume(parser, ";", USK_DIAG_EXPECTED_TOKEN,
                       "expected ';' after expression statement");
    UskAstStmt *statement = new_statement(parser, USK_STMT_EXPRESSION, start);
    if (statement) statement->as.expression = expression;
    return statement;
}
