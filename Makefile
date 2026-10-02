CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
LDLIBS ?= -lm

.PHONY: all clean
all: usk

SOURCES = src/usk.c src/native/c_backend.c src/native/c_backend_file.c \
	src/native/c_backend_bindings.c \
	src/native/c_backend_runtime.c src/native/c_backend_expression.c \
	src/native/c_backend_statement.c src/native/c_backend_declaration.c \
	src/module/loader.c src/frontend/lexer.c \
	src/frontend/lexer_keywords.c src/frontend/lexer_result.c \
	src/frontend/ast.c src/frontend/ast_visit.c \
	src/frontend/ast_visit_expression.c src/frontend/ast_visit_statement.c \
	src/frontend/ast_visit_declaration.c \
	src/frontend/parser.c src/frontend/parse_expression.c \
	src/frontend/parser_options.c src/frontend/parser_recovery.c \
	src/frontend/parse_statement.c src/frontend/parse_declaration.c \
	src/common/diagnostic.c src/common/diagnostic_json.c \
	src/runtime/value.c src/runtime/value_arena.c \
	src/runtime/arraylib.c src/runtime/pathlib.c src/runtime/path_lexical.c \
	src/runtime/filesystem.c \
	src/runtime/stringlib.c src/runtime/string_view.c src/runtime/mathlib.c \
	src/runtime/math_integer.c \
	src/runtime/environment.c \
	src/runtime/evaluator.c src/runtime/eval_call.c src/runtime/eval_expression.c \
	src/runtime/eval_statement.c \
	src/sema/typecheck.c src/sema/analyzer.c src/sema/resolve_type.c \
	src/sema/analyze_declaration.c src/sema/analyze_expression.c \
	src/sema/analyze_statement.c src/bytecode/chunk.c \
	src/bytecode/verify.c src/bytecode/disassemble.c src/bytecode/lower.c \
	src/bytecode/vm.c
HEADERS = include/usk/loader.h include/usk/lexer.h include/usk/value.h \
include/usk/c_backend.h \
include/usk/ast_visitor.h \
include/usk/environment.h include/usk/typecheck.h include/usk/stringlib.h \
	include/usk/arraylib.h \
	include/usk/pathlib.h \
	include/usk/filesystem.h \
	include/usk/mathlib.h \
	include/usk/ast.h \
	include/usk/parser.h include/usk/diagnostic.h include/usk/evaluator.h \
	include/usk/sema.h include/usk/bytecode.h src/frontend/parser_internal.h \
	src/runtime/evaluator_internal.h src/sema/analyzer_internal.h

usk: $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) -Iinclude -o $@ $(SOURCES) $(LDLIBS)

clean:
	rm -f usk usk.exe
