cmake_minimum_required(VERSION 3.16)
project(UltraSonickan LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(usk
    src/usk.c
    src/native/c_backend.c
    src/native/c_backend_file.c
    src/native/c_backend_bindings.c
    src/native/c_backend_runtime.c
    src/native/c_backend_expression.c
    src/native/c_backend_statement.c
    src/native/c_backend_declaration.c
    src/module/loader.c
    src/frontend/lexer.c
    src/frontend/lexer_keywords.c
    src/frontend/lexer_result.c
    src/frontend/ast.c
    src/frontend/ast_visit.c
    src/frontend/ast_visit_expression.c
    src/frontend/ast_visit_statement.c
    src/frontend/ast_visit_declaration.c
    src/frontend/parser.c
    src/frontend/parser_options.c
    src/frontend/parser_recovery.c
    src/frontend/parse_expression.c
    src/frontend/parse_statement.c
    src/frontend/parse_declaration.c
    src/common/diagnostic.c
    src/common/diagnostic_json.c
    src/runtime/value.c
    src/runtime/value_arena.c
    src/runtime/arraylib.c
    src/runtime/pathlib.c
    src/runtime/path_lexical.c
    src/runtime/filesystem.c
    src/runtime/stringlib.c
    src/runtime/string_view.c
    src/runtime/mathlib.c
    src/runtime/math_integer.c
    src/runtime/environment.c
    src/runtime/evaluator.c
    src/runtime/eval_call.c
    src/runtime/eval_expression.c
    src/runtime/eval_statement.c
    src/sema/typecheck.c
    src/sema/analyzer.c
    src/sema/resolve_type.c
    src/sema/analyze_declaration.c
    src/sema/analyze_expression.c
    src/sema/analyze_statement.c
    src/bytecode/chunk.c
    src/bytecode/verify.c
    src/bytecode/disassemble.c
    src/bytecode/lower.c
    src/bytecode/vm.c)
target_include_directories(usk PRIVATE include)
if (MSVC)
  target_compile_options(usk PRIVATE /W4)
else()
  target_compile_options(usk PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(usk PRIVATE m)
endif()
