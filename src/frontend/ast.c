#include "usk/ast.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct UskAstAllocation {
    void *memory;
    UskAstAllocation *next;
};

void usk_ast_program_init(UskAstProgram *program) {
    if (!program) return;
    memset(program, 0, sizeof(*program));
}

void usk_ast_program_destroy(UskAstProgram *program) {
    if (!program) return;
    UskAstAllocation *allocation = program->allocations;
    while (allocation) {
        UskAstAllocation *next = allocation->next;
        free(allocation->memory);
        free(allocation);
        allocation = next;
    }
    memset(program, 0, sizeof(*program));
}

void *usk_ast_allocate(UskAstProgram *program, size_t size) {
    if (!program || !size) return NULL;
    void *memory = calloc(1, size);
    if (!memory) return NULL;
    UskAstAllocation *allocation = (UskAstAllocation *)malloc(sizeof(*allocation));
    if (!allocation) {
        free(memory);
        return NULL;
    }
    allocation->memory = memory;
    allocation->next = program->allocations;
    program->allocations = allocation;
    return memory;
}

char *usk_ast_copy_text(UskAstProgram *program, const char *text) {
    if (!text) text = "";
    size_t length = strlen(text);
    char *copy = (char *)usk_ast_allocate(program, length + 1);
    if (!copy) return NULL;
    memcpy(copy, text, length + 1);
    return copy;
}

UskAstExpr *usk_ast_new_expression(UskAstProgram *program, UskAstExprKind kind,
                                   UskSourceSpan span) {
    UskAstExpr *expression = (UskAstExpr *)usk_ast_allocate(program, sizeof(*expression));
    if (expression) {
        expression->kind = kind;
        expression->span = span;
    }
    return expression;
}

UskAstStmt *usk_ast_new_statement(UskAstProgram *program, UskAstStmtKind kind,
                                  UskSourceSpan span) {
    UskAstStmt *statement = (UskAstStmt *)usk_ast_allocate(program, sizeof(*statement));
    if (statement) {
        statement->kind = kind;
        statement->span = span;
    }
    return statement;
}

UskAstDecl *usk_ast_new_declaration(UskAstProgram *program, UskAstDeclKind kind,
                                    UskSourceSpan span) {
    UskAstDecl *declaration = (UskAstDecl *)usk_ast_allocate(program, sizeof(*declaration));
    if (declaration) {
        declaration->kind = kind;
        declaration->span = span;
        declaration->visibility = USK_VISIBILITY_DEFAULT;
    }
    return declaration;
}

void usk_ast_append_declaration(UskAstProgram *program, UskAstDecl *declaration) {
    if (!program || !declaration) return;
    declaration->next = NULL;
    if (program->last_declaration) program->last_declaration->next = declaration;
    else program->declarations = declaration;
    program->last_declaration = declaration;
    program->declaration_count++;
}

static const UskAstDecl *find_function_in_list(const UskAstDecl *declaration,
                                                const char *name) {
    for (; declaration; declaration = declaration->next) {
        if (declaration->kind == USK_DECL_FUNCTION &&
            declaration->as.function.name &&
            strcmp(declaration->as.function.name, name) == 0)
            return declaration;
        if ((declaration->kind == USK_DECL_CLASS || declaration->kind == USK_DECL_STRUCT) &&
            declaration->as.record.members) {
            const UskAstDecl *nested = find_function_in_list(
                declaration->as.record.members, name);
            if (nested) return nested;
        }
    }
    return NULL;
}

const UskAstDecl *usk_ast_find_function(const UskAstProgram *program,
                                       const char *name) {
    if (!program || !name) return NULL;
    return find_function_in_list(program->declarations, name);
}

const UskAstDecl *usk_ast_find_entry_function(const UskAstProgram *program) {
    return usk_ast_find_function(program, "main");
}

static void dump_indent(FILE *stream, unsigned indentation) {
    for (unsigned index = 0; index < indentation; ++index) fputs("  ", stream);
}

static void dump_quoted(FILE *stream, const char *text) {
    fputc('"', stream);
    for (const unsigned char *cursor = (const unsigned char *)(text ? text : "");
         *cursor; ++cursor) {
        switch (*cursor) {
            case '\n': fputs("\\n", stream); break;
            case '\r': fputs("\\r", stream); break;
            case '\t': fputs("\\t", stream); break;
            case '\\': fputs("\\\\", stream); break;
            case '"': fputs("\\\"", stream); break;
            default: fputc(*cursor, stream); break;
        }
    }
    fputc('"', stream);
}

void usk_ast_dump_type(FILE *stream, const UskAstType *type) {
    if (!stream) return;
    if (!type) { fputs("<inferred>", stream); return; }
    if (type->is_const) fputs("const ", stream);
    if (type->is_unsigned) fputs("unsigned ", stream);
    else if (type->is_signed) fputs("signed ", stream);
    if (type->is_long) fputs("USKLong ", stream);
    if (type->is_short) fputs("USKShort ", stream);
    fputs(type->name ? type->name : "<unknown-type>", stream);
    for (size_t dimension = 0; dimension < type->array_dimensions; ++dimension)
        fputs("[]", stream);
}

void usk_ast_dump_expression(FILE *stream, const UskAstExpr *expression,
                             unsigned indentation) {
    if (!stream) return;
    if (!expression) { fputs("<missing-expression>", stream); return; }
    switch (expression->kind) {
        case USK_EXPR_NULL: fputs("null", stream); break;
        case USK_EXPR_BOOLEAN: fputs(expression->as.boolean ? "true" : "false", stream); break;
        case USK_EXPR_INTEGER: fprintf(stream, "%lld", expression->as.integer); break;
        case USK_EXPR_DOUBLE: fprintf(stream, "%.17g", expression->as.floating); break;
        case USK_EXPR_STRING: dump_quoted(stream, expression->as.string); break;
        case USK_EXPR_NAME: fputs(expression->as.name ? expression->as.name : "<name>", stream); break;
        case USK_EXPR_ARRAY: {
            fputc('[', stream);
            const UskAstExprList *item = expression->as.array.items;
            while (item) {
                usk_ast_dump_expression(stream, item->expression, indentation);
                if (item->next) fputs(", ", stream);
                item = item->next;
            }
            fputc(']', stream);
            break;
        }
        case USK_EXPR_UNARY:
            fprintf(stream, "(%s", expression->as.unary.operator ? expression->as.unary.operator : "?");
            usk_ast_dump_expression(stream, expression->as.unary.operand, indentation);
            fputc(')', stream);
            break;
        case USK_EXPR_BINARY:
            fprintf(stream, "(%s ", expression->as.binary.operator ? expression->as.binary.operator : "?");
            usk_ast_dump_expression(stream, expression->as.binary.left, indentation);
            fputc(' ', stream);
            usk_ast_dump_expression(stream, expression->as.binary.right, indentation);
            fputc(')', stream);
            break;
        case USK_EXPR_ASSIGNMENT:
            fprintf(stream, "(assign %s ", expression->as.assignment.operator ? expression->as.assignment.operator : "?");
            usk_ast_dump_expression(stream, expression->as.assignment.target, indentation);
            fputc(' ', stream);
            usk_ast_dump_expression(stream, expression->as.assignment.value, indentation);
            fputc(')', stream);
            break;
        case USK_EXPR_CALL: {
            fputs("(call ", stream);
            usk_ast_dump_expression(stream, expression->as.call.callee, indentation);
            const UskAstExprList *argument = expression->as.call.arguments;
            while (argument) {
                fputc(' ', stream);
                usk_ast_dump_expression(stream, argument->expression, indentation);
                argument = argument->next;
            }
            fputc(')', stream);
            break;
        }
        case USK_EXPR_MEMBER:
            fprintf(stream, "(member%s ", expression->as.member.pointer_access ? "->" : "::");
            usk_ast_dump_expression(stream, expression->as.member.object, indentation);
            fprintf(stream, " %s)", expression->as.member.name ? expression->as.member.name : "<member>");
            break;
        case USK_EXPR_INDEX:
            fputs("(index ", stream);
            usk_ast_dump_expression(stream, expression->as.index.object, indentation);
            fputc(' ', stream);
            usk_ast_dump_expression(stream, expression->as.index.index, indentation);
            fputc(')', stream);
            break;
        case USK_EXPR_CONDITIONAL:
            fputs("(?: ", stream);
            usk_ast_dump_expression(stream, expression->as.conditional.condition, indentation);
            fputc(' ', stream);
            usk_ast_dump_expression(stream, expression->as.conditional.when_true, indentation);
            fputc(' ', stream);
            usk_ast_dump_expression(stream, expression->as.conditional.when_false, indentation);
            fputc(')', stream);
            break;
    }
}

static void dump_statement_list(FILE *stream, const UskAstStmtList *list,
                                unsigned indentation) {
    for (const UskAstStmtList *item = list; item; item = item->next)
        usk_ast_dump_statement(stream, item->statement, indentation);
}

void usk_ast_dump_statement(FILE *stream, const UskAstStmt *statement,
                            unsigned indentation) {
    if (!stream || !statement) return;
    dump_indent(stream, indentation);
    switch (statement->kind) {
        case USK_STMT_EMPTY: fputs("Empty\n", stream); break;
        case USK_STMT_BLOCK:
            fputs("Block\n", stream);
            dump_statement_list(stream, statement->as.block.items, indentation + 1);
            break;
        case USK_STMT_VARIABLE:
            fputs("Variable ", stream); usk_ast_dump_type(stream, statement->as.variable.type);
            fprintf(stream, " %s", statement->as.variable.name ? statement->as.variable.name : "<name>");
            if (statement->as.variable.initializer) {
                fputs(" = ", stream);
                usk_ast_dump_expression(stream, statement->as.variable.initializer, indentation);
            }
            fputc('\n', stream);
            break;
        case USK_STMT_EXPRESSION:
            fputs("Expression ", stream); usk_ast_dump_expression(stream, statement->as.expression, indentation); fputc('\n', stream); break;
        case USK_STMT_IF:
            fputs("If ", stream); usk_ast_dump_expression(stream, statement->as.if_stmt.condition, indentation); fputc('\n', stream);
            usk_ast_dump_statement(stream, statement->as.if_stmt.then_branch, indentation + 1);
            if (statement->as.if_stmt.else_branch) {
                dump_indent(stream, indentation); fputs("Else\n", stream);
                usk_ast_dump_statement(stream, statement->as.if_stmt.else_branch, indentation + 1);
            }
            break;
        case USK_STMT_WHILE:
            fputs("While ", stream); usk_ast_dump_expression(stream, statement->as.while_stmt.condition, indentation); fputc('\n', stream);
            usk_ast_dump_statement(stream, statement->as.while_stmt.body, indentation + 1); break;
        case USK_STMT_FOR:
            fputs("For\n", stream);
            if (statement->as.for_stmt.initializer) usk_ast_dump_statement(stream, statement->as.for_stmt.initializer, indentation + 1);
            if (statement->as.for_stmt.condition) { dump_indent(stream, indentation + 1); fputs("Condition ", stream); usk_ast_dump_expression(stream, statement->as.for_stmt.condition, indentation); fputc('\n', stream); }
            if (statement->as.for_stmt.update) { dump_indent(stream, indentation + 1); fputs("Update ", stream); usk_ast_dump_expression(stream, statement->as.for_stmt.update, indentation); fputc('\n', stream); }
            usk_ast_dump_statement(stream, statement->as.for_stmt.body, indentation + 1); break;
        case USK_STMT_FOREACH:
            fputs("ForEach ", stream); usk_ast_dump_type(stream, statement->as.foreach_stmt.type);
            fprintf(stream, " %s in ", statement->as.foreach_stmt.name ? statement->as.foreach_stmt.name : "<name>");
            usk_ast_dump_expression(stream, statement->as.foreach_stmt.collection, indentation); fputc('\n', stream);
            usk_ast_dump_statement(stream, statement->as.foreach_stmt.body, indentation + 1); break;
        case USK_STMT_SWITCH:
            fputs("Switch ", stream); usk_ast_dump_expression(stream, statement->as.switch_stmt.selector, indentation); fputc('\n', stream);
            for (const UskAstSwitchCase *item = statement->as.switch_stmt.cases; item; item = item->next) {
                dump_indent(stream, indentation + 1);
                if (item->is_default) fputs("Default\n", stream);
                else {
                    fputs("Case", stream);
                    for (const UskAstExprList *label = item->labels; label; label = label->next) {
                        fputc(' ', stream);
                        usk_ast_dump_expression(stream, label->expression, indentation + 2);
                    }
                    fputc('\n', stream);
                }
                dump_statement_list(stream, item->statements, indentation + 2);
            }
            break;
        case USK_STMT_BREAK: fputs("Break\n", stream); break;
        case USK_STMT_CONTINUE: fputs("Continue\n", stream); break;
        case USK_STMT_RETURN:
            fputs("Return", stream);
            if (statement->as.return_value) { fputc(' ', stream); usk_ast_dump_expression(stream, statement->as.return_value, indentation); }
            fputc('\n', stream); break;
    }
}

static void dump_parameter_list(FILE *stream, const UskAstParameter *parameter) {
    for (; parameter; parameter = parameter->next) {
        fputs("\n    Parameter ", stream);
        usk_ast_dump_type(stream, parameter->type);
        fprintf(stream, " %s", parameter->name ? parameter->name : "<name>");
        if (parameter->default_value) {
            fputs(" = ", stream);
            usk_ast_dump_expression(stream, parameter->default_value, 0);
        }
    }
}

void usk_ast_dump_declaration(FILE *stream, const UskAstDecl *declaration,
                              unsigned indentation) {
    if (!stream || !declaration) return;
    dump_indent(stream, indentation);
    if (declaration->visibility == USK_VISIBILITY_PUBLIC) fputs("public ", stream);
    if (declaration->visibility == USK_VISIBILITY_PRIVATE) fputs("private ", stream);
    if (declaration->is_static) fputs("static ", stream);
    if (declaration->is_inline) fputs("inline ", stream);
    switch (declaration->kind) {
        case USK_DECL_FUNCTION:
            fprintf(stream, "Function %s", declaration->as.function.name ? declaration->as.function.name : "<name>");
            if (declaration->as.function.return_type) { fputs(" -> ", stream); usk_ast_dump_type(stream, declaration->as.function.return_type); }
            dump_parameter_list(stream, declaration->as.function.parameters);
            fputc('\n', stream);
            if (declaration->as.function.body) usk_ast_dump_statement(stream, declaration->as.function.body, indentation + 1);
            break;
        case USK_DECL_CLASS:
        case USK_DECL_STRUCT:
            fprintf(stream, "%s %s\n", declaration->kind == USK_DECL_CLASS ? "Class" : "Struct",
                    declaration->as.record.name ? declaration->as.record.name : "<name>");
            for (const UskAstDecl *member = declaration->as.record.members; member; member = member->next)
                usk_ast_dump_declaration(stream, member, indentation + 1);
            break;
        case USK_DECL_ENUM:
            fprintf(stream, "Enum %s\n", declaration->as.enumeration.name ? declaration->as.enumeration.name : "<name>");
            for (const UskAstEnumValue *value = declaration->as.enumeration.values; value; value = value->next) {
                dump_indent(stream, indentation + 1); fprintf(stream, "%s", value->name ? value->name : "<name>");
                if (value->value) { fputs(" = ", stream); usk_ast_dump_expression(stream, value->value, indentation); }
                fputc('\n', stream);
            }
            break;
        case USK_DECL_VARIABLE:
            fputs("Variable ", stream); usk_ast_dump_type(stream, declaration->as.variable.type);
            fprintf(stream, " %s", declaration->as.variable.name ? declaration->as.variable.name : "<name>");
            if (declaration->as.variable.initializer) { fputs(" = ", stream); usk_ast_dump_expression(stream, declaration->as.variable.initializer, indentation); }
            fputc('\n', stream); break;
        case USK_DECL_TYPEDEF:
            fprintf(stream, "Typedef %s as ", declaration->as.alias.name ? declaration->as.alias.name : "<name>");
            usk_ast_dump_type(stream, declaration->as.alias.type); fputc('\n', stream); break;
        case USK_DECL_EXTERN: fputs("Extern\n", stream); break;
        case USK_DECL_IMPORT: fprintf(stream, "Import %s\n", declaration->as.import_decl.path ? declaration->as.import_decl.path : "<path>"); break;
        case USK_DECL_DIRECTIVE: fprintf(stream, "Directive %s\n", declaration->as.directive.text ? declaration->as.directive.text : ""); break;
    }
}

void usk_ast_dump_program(FILE *stream, const UskAstProgram *program) {
    if (!stream || !program) return;
    fprintf(stream, "Program (%zu declarations)\n", program->declaration_count);
    for (const UskAstDecl *declaration = program->declarations;
         declaration; declaration = declaration->next)
        usk_ast_dump_declaration(stream, declaration, 1);
}
