#define _CRT_SECURE_NO_WARNINGS
#include "usk/diagnostic.h"
#include "usk/bytecode.h"
#include "usk/c_backend.h"
#include "usk/evaluator.h"
#include "usk/lexer.h"
#include "usk/loader.h"
#include "usk/parser.h"
#include "usk/sema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    USK_COMMAND_RUN,
    USK_COMMAND_CHECK,
    USK_COMMAND_AST,
    USK_COMMAND_BYTECODE,
    USK_COMMAND_VM,
    USK_COMMAND_EMIT_C,
    USK_COMMAND_HELP,
    USK_COMMAND_VERSION
} UskCommand;

typedef struct {
    UskCommand command;
    const char *source_path;
    int first_argument;
    bool json_diagnostics;
    const char *output_path;
} UskCommandLine;

static void print_usage(FILE *stream) {
    fputs("UltraSonickan compiler/runtime\n"
          "Usage:\n"
          "  usk run <file.usk> [arguments...]\n"
          "  usk check [--json] <file.usk>\n"
          "  usk ast <file.usk>\n"
          "  usk bytecode <file.usk>\n"
          "  usk vm <file.usk> [arguments...]\n"
          "  usk emit-c <file.usk> [-o output.c]\n"
          "  usk <file.usk> [arguments...]\n"
          "  usk --version\n", stream);
}

static UskCommandLine parse_command_line(int argument_count, char **arguments) {
    UskCommandLine result = {USK_COMMAND_HELP, NULL, argument_count, false, NULL};
    if (argument_count < 2) return result;
    const char *command = arguments[1];
    if (!strcmp(command, "--help") || !strcmp(command, "-h")) {
        result.command = USK_COMMAND_HELP;
        return result;
    }
    if (!strcmp(command, "--version") || !strcmp(command, "-V")) {
        result.command = USK_COMMAND_VERSION;
        return result;
    }
    int source_index = 1;
    if (!strcmp(command, "run")) {
        result.command = USK_COMMAND_RUN;
        source_index = 2;
    } else if (!strcmp(command, "check")) {
        result.command = USK_COMMAND_CHECK;
        source_index = 2;
    } else if (!strcmp(command, "ast") || !strcmp(command, "dump-ast")) {
        result.command = USK_COMMAND_AST;
        source_index = 2;
    } else if (!strcmp(command, "bytecode")) {
        result.command = USK_COMMAND_BYTECODE;
        source_index = 2;
    } else if (!strcmp(command, "vm")) {
        result.command = USK_COMMAND_VM;
        source_index = 2;
    } else if (!strcmp(command, "emit-c")) {
        result.command = USK_COMMAND_EMIT_C;
        source_index = 2;
    } else {
        result.command = USK_COMMAND_RUN;
    }
    if (result.command == USK_COMMAND_CHECK &&
        source_index < argument_count &&
        !strcmp(arguments[source_index], "--json")) {
        result.json_diagnostics = true;
        source_index++;
    }
    if (source_index >= argument_count) {
        result.command = USK_COMMAND_HELP;
        return result;
    }
    result.source_path = arguments[source_index];
    result.first_argument = source_index + 1;
    if (result.command == USK_COMMAND_EMIT_C &&
        result.first_argument < argument_count &&
        !strcmp(arguments[result.first_argument], "-o")) {
        if (result.first_argument + 1 >= argument_count) {
            result.command = USK_COMMAND_HELP;
            result.source_path = "emit-c requires a path after -o";
            return result;
        }
        result.output_path = arguments[result.first_argument + 1];
        result.first_argument += 2;
    }
    return result;
}

static char *load_source(const char *path) {
    char error[512];
    char *source = usk_load_program(path, error, sizeof(error));
    if (!source) fprintf(stderr, "usk: %s\n", error[0] ? error : "unable to load source");
    return source;
}

static int parse_source_file(const char *path, const char *source,
                             Tokens *tokens, UskParseResult *parsed) {
    UskLexerResult lexical = usk_lex_source_ex(source, path, NULL);
    bool diagnostics_copied = true;
    for (size_t index = 0; index < lexical.diagnostics.count; ++index) {
        const UskDiagnostic *diagnostic = &lexical.diagnostics.items[index];
        if (!usk_diagnostics_add(&parsed->diagnostics, diagnostic->severity,
                diagnostic->code, diagnostic->source_name,
                diagnostic->line, diagnostic->column, "%s",
                diagnostic->message)) {
            diagnostics_copied = false;
            break;
        }
    }
    *tokens = lexical.tokens;
    memset(&lexical.tokens, 0, sizeof(lexical.tokens));
    bool parse_success = usk_parse_tokens(tokens, path, parsed);
    if (!diagnostics_copied)
        usk_diagnostics_add(&parsed->diagnostics, USK_DIAGNOSTIC_ERROR,
            USK_DIAG_OUT_OF_MEMORY, path, 0, 0,
            "cannot copy lexer diagnostics into the parse result");
    bool success = lexical.succeeded && diagnostics_copied && parse_success;
    usk_lexer_result_destroy(&lexical);
    return success ? 1 : 0;
}

static int run_check_command(const UskCommandLine *command,
                             UskParseResult *parsed) {
    UskSemanticResult semantic = usk_analyze_program(&parsed->program,
        command->source_path, &parsed->diagnostics);
    if (command->json_diagnostics) {
        if (!usk_diagnostics_print_json(&parsed->diagnostics, stdout)) return 2;
        return semantic.valid ? 0 : 1;
    }
    if (!semantic.valid) {
        usk_diagnostics_print(&parsed->diagnostics, stderr);
        return 1;
    }
    printf("%s: syntax and semantics OK (%zu declarations, %zu expressions)\n",
        command->source_path, semantic.declarations_checked,
        semantic.expressions_checked);
    return 0;
}

static int run_bytecode_command(const UskCommandLine *command,
                                UskParseResult *parsed) {
    UskSemanticResult semantic = usk_analyze_program(&parsed->program,
        command->source_path, &parsed->diagnostics);
    if (!semantic.valid) {
        usk_diagnostics_print(&parsed->diagnostics, stderr);
        return 1;
    }
    const UskAstDecl *entry = usk_ast_find_entry_function(&parsed->program);
    UskBytecodeChunk chunk = {0};
    UskBytecodeCompileResult result = usk_bytecode_compile_function(entry,
        command->source_path, &chunk, &parsed->diagnostics);
    if (result.success) usk_bytecode_disassemble(stdout, &chunk);
    else usk_diagnostics_print(&parsed->diagnostics, stderr);
    usk_bytecode_chunk_destroy(&chunk);
    return result.success ? 0 : 1;
}

static int run_emit_c_command(const UskCommandLine *command,
                              UskParseResult *parsed) {
    UskSemanticResult semantic = usk_analyze_program(&parsed->program,
        command->source_path, &parsed->diagnostics);
    if (!semantic.valid) {
        usk_diagnostics_print(&parsed->diagnostics, stderr);
        return 1;
    }
    UskCBackendOptions options;
    usk_c_backend_options_init(&options);
    UskCBackendResult generated = usk_c_backend_emit_file(
        &parsed->program, command->source_path, command->output_path, &options,
        &parsed->diagnostics);
    if (!generated.success) {
        usk_diagnostics_print(&parsed->diagnostics, stderr);
        return 1;
    }
    return 0;
}

typedef struct UskVmCompiledFunction {
    char *name;
    UskBytecodeChunk chunk;
    struct UskVmCompiledFunction *next;
} UskVmCompiledFunction;

typedef struct UskVmGlobal {
    char *name;
    Value value;
    bool is_const;
    bool initialized;
    struct UskVmGlobal *next;
} UskVmGlobal;

typedef struct {
    const UskAstProgram *program;
    const char *source_name;
    UskDiagnosticList *diagnostics;
    UskVmCompiledFunction *functions;
    UskVmGlobal *globals;
} UskVmProgram;

static char *vm_qualified_name(const char *owner, const char *name) {
    size_t owner_length = owner ? strlen(owner) : 0;
    size_t name_length = strlen(name ? name : "");
    size_t separator_length = owner_length ? 2 : 0;
    if (owner_length > (size_t)-1 - separator_length - name_length - 1)
        return NULL;
    size_t length = owner_length + separator_length + name_length;
    char *result = (char *)malloc(length + 1);
    if (!result) return NULL;
    if (owner_length) {
        memcpy(result, owner, owner_length);
        memcpy(result + owner_length, "::", 2);
    }
    memcpy(result + owner_length + separator_length,
           name ? name : "", name_length + 1);
    return result;
}

static UskVmGlobal *vm_find_global(UskVmProgram *program, const char *name) {
    for (UskVmGlobal *global = program ? program->globals : NULL;
         global; global = global->next)
        if (!strcmp(global->name, name)) return global;
    return NULL;
}

static bool vm_load_global(void *context, const char *name, Value *result) {
    UskVmProgram *program = (UskVmProgram *)context;
    UskVmGlobal *global = vm_find_global(program, name);
    if (!global || !result) return false;
    *result = global->value;
    return true;
}

static bool vm_store_global(void *context, const char *name, Value value) {
    UskVmProgram *program = (UskVmProgram *)context;
    UskVmGlobal *global = vm_find_global(program, name);
    if (!global || (global->is_const && global->initialized)) return false;
    global->value = value;
    global->initialized = true;
    return true;
}

static bool vm_prepare_globals(UskVmProgram *program) {
    for (const UskAstDecl *declaration = program->program->declarations;
         declaration; declaration = declaration->next) {
        if (declaration->kind != USK_DECL_VARIABLE) continue;
        char *name = vm_qualified_name(NULL, declaration->as.variable.name);
        UskVmGlobal *global = (UskVmGlobal *)calloc(1, sizeof(*global));
        if (!name || !global) {
            free(name);
            free(global);
            return false;
        }
        global->name = name;
        global->value = null_value();
        global->is_const = declaration->as.variable.type &&
            declaration->as.variable.type->is_const;
        global->next = program->globals;
        program->globals = global;
    }
    return true;
}

static const UskAstDecl *vm_find_function_in_list(
    const UskAstDecl *declarations, const char *owner, const char *name) {
    for (const UskAstDecl *declaration = declarations; declaration;
         declaration = declaration->next) {
        if (declaration->kind == USK_DECL_FUNCTION) {
            char *qualified = vm_qualified_name(owner,
                declaration->as.function.name);
            bool matched = qualified && !strcmp(qualified, name);
            free(qualified);
            if (matched) return declaration;
        } else if (declaration->kind == USK_DECL_CLASS ||
                   declaration->kind == USK_DECL_STRUCT) {
            char *nested_owner = vm_qualified_name(owner,
                declaration->as.record.name);
            if (!nested_owner) continue;
            const UskAstDecl *found = vm_find_function_in_list(
                declaration->as.record.members, nested_owner, name);
            free(nested_owner);
            if (found) return found;
        }
    }
    return NULL;
}

static const UskAstDecl *vm_find_function_by_suffix(
    const UskAstDecl *declarations, const char *name) {
    for (const UskAstDecl *declaration = declarations; declaration;
         declaration = declaration->next) {
        if (declaration->kind == USK_DECL_FUNCTION &&
            declaration->as.function.name &&
            !strcmp(declaration->as.function.name, name)) return declaration;
        if (declaration->kind == USK_DECL_CLASS ||
            declaration->kind == USK_DECL_STRUCT) {
            const UskAstDecl *found = vm_find_function_by_suffix(
                declaration->as.record.members, name);
            if (found) return found;
        }
    }
    return NULL;
}

static const UskAstDecl *vm_find_function(const UskVmProgram *program,
                                          const char *name) {
    if (!program || !program->program || !name) return NULL;
    const UskAstDecl *function = vm_find_function_in_list(
        program->program->declarations, NULL, name);
    if (!function && !strstr(name, "::"))
        function = vm_find_function_by_suffix(program->program->declarations,
                                              name);
    return function;
}

static const UskBytecodeChunk *vm_resolve_function(void *context,
                                                    const char *name) {
    UskVmProgram *program = (UskVmProgram *)context;
    if (!program || !name) return NULL;
    for (UskVmCompiledFunction *function = program->functions; function;
         function = function->next)
        if (!strcmp(function->name, name)) return &function->chunk;

    const UskAstDecl *declaration = vm_find_function(program, name);
    if (!declaration) return NULL;
    UskVmCompiledFunction *function =
        (UskVmCompiledFunction *)calloc(1, sizeof(*function));
    if (!function) return NULL;
    function->name = (char *)malloc(strlen(name) + 1);
    if (!function->name) {
        free(function);
        return NULL;
    }
    strcpy(function->name, name);
    UskBytecodeCompileResult compiled = usk_bytecode_compile_function(
        declaration, program->source_name, &function->chunk,
        program->diagnostics);
    if (!compiled.success) {
        free(function->name);
        usk_bytecode_chunk_destroy(&function->chunk);
        free(function);
        return NULL;
    }
    function->next = program->functions;
    program->functions = function;
    return &function->chunk;
}

static void vm_destroy_functions(UskVmProgram *program) {
    UskVmCompiledFunction *function = program->functions;
    while (function) {
        UskVmCompiledFunction *next = function->next;
        usk_bytecode_chunk_destroy(&function->chunk);
        free(function->name);
        free(function);
        function = next;
    }
    program->functions = NULL;
}

static void vm_destroy_globals(UskVmProgram *program) {
    UskVmGlobal *global = program->globals;
    while (global) {
        UskVmGlobal *next = global->next;
        free(global->name);
        free(global);
        global = next;
    }
    program->globals = NULL;
}

typedef struct {
    UskAstStmt block;
    UskAstStmt *statements;
    UskAstStmtList *items;
    UskAstExpr *expressions;
    size_t count;
} UskVmInitializer;

static bool vm_build_global_initializer(const UskVmProgram *program,
                                        UskVmInitializer *initializer,
                                        UskAstDecl *function) {
    memset(initializer, 0, sizeof(*initializer));
    size_t count = 0;
    for (const UskAstDecl *declaration = program->program->declarations;
         declaration; declaration = declaration->next)
        if (declaration->kind == USK_DECL_VARIABLE &&
            declaration->as.variable.initializer) count++;
    if (!count) return true;
    if (count > (size_t)-1 / sizeof(*initializer->statements) ||
        count > (size_t)-1 / sizeof(*initializer->items) ||
        count > (size_t)-1 / (2 * sizeof(*initializer->expressions)))
        return false;
    initializer->statements = (UskAstStmt *)calloc(count,
        sizeof(*initializer->statements));
    initializer->items = (UskAstStmtList *)calloc(count,
        sizeof(*initializer->items));
    initializer->expressions = (UskAstExpr *)calloc(count * 2,
        sizeof(*initializer->expressions));
    if (!initializer->statements || !initializer->items ||
        !initializer->expressions) return false;
    initializer->count = count;

    size_t index = 0;
    for (const UskAstDecl *declaration = program->program->declarations;
         declaration; declaration = declaration->next) {
        if (declaration->kind != USK_DECL_VARIABLE ||
            !declaration->as.variable.initializer) continue;
        UskAstExpr *target = &initializer->expressions[index * 2];
        UskAstExpr *assignment = &initializer->expressions[index * 2 + 1];
        target->kind = USK_EXPR_NAME;
        target->span = declaration->span;
        target->as.name = declaration->as.variable.name;
        assignment->kind = USK_EXPR_ASSIGNMENT;
        assignment->span = declaration->span;
        assignment->as.assignment.operator = "=";
        assignment->as.assignment.target = target;
        assignment->as.assignment.value = declaration->as.variable.initializer;
        initializer->statements[index].kind = USK_STMT_EXPRESSION;
        initializer->statements[index].span = declaration->span;
        initializer->statements[index].as.expression = assignment;
        initializer->items[index].statement = &initializer->statements[index];
        initializer->items[index].next = index + 1 < count
            ? &initializer->items[index + 1] : NULL;
        index++;
    }
    initializer->block.kind = USK_STMT_BLOCK;
    initializer->block.as.block.items = initializer->items;
    initializer->block.as.block.count = count;
    memset(function, 0, sizeof(*function));
    function->kind = USK_DECL_FUNCTION;
    function->as.function.name = "__usk_global_init";
    function->as.function.body = &initializer->block;
    return true;
}

static void vm_destroy_global_initializer(UskVmInitializer *initializer) {
    free(initializer->statements);
    free(initializer->items);
    free(initializer->expressions);
    memset(initializer, 0, sizeof(*initializer));
}

static bool vm_is_string_type(const UskAstType *type) {
    return type && type->name &&
        (!strcmp(type->name, "USKString") ||
         !strcmp(type->name, "USKAuto"));
}

static int run_vm_command(const UskCommandLine *command, int argument_count,
                          char **arguments, UskParseResult *parsed) {
    UskSemanticResult semantic = usk_analyze_program(&parsed->program,
        command->source_path, &parsed->diagnostics);
    if (!semantic.valid) {
        usk_diagnostics_print(&parsed->diagnostics, stderr);
        return 1;
    }
    const UskAstDecl *entry = usk_ast_find_entry_function(&parsed->program);
    if (!entry) return 1;
    UskBytecodeChunk chunk = {0};
    UskBytecodeCompileResult compiled = usk_bytecode_compile_function(entry,
        command->source_path, &chunk, &parsed->diagnostics);
    if (!compiled.success) {
        usk_diagnostics_print(&parsed->diagnostics, stderr);
        usk_bytecode_chunk_destroy(&chunk);
        return 1;
    }

    UskValueArena storage;
    UskEvaluatorOptions runtime_limits = usk_evaluator_default_options();
    usk_value_arena_init_limited(&storage,
        runtime_limits.maximum_value_allocations,
        runtime_limits.maximum_value_bytes);
    UskVmProgram vm_program = {
        .program = &parsed->program,
        .source_name = command->source_path,
        .diagnostics = &parsed->diagnostics,
        .functions = NULL,
        .globals = NULL
    };
    UskBytecodeHost host = {
        .context = &vm_program,
        .load_global = vm_load_global,
        .store_global = vm_store_global,
        .resolve_function = vm_resolve_function
    };
    if (!vm_prepare_globals(&vm_program)) {
        fprintf(stderr, "usk: cannot allocate VM global-variable table\n");
        vm_destroy_globals(&vm_program);
        usk_value_arena_destroy(&storage);
        usk_bytecode_chunk_destroy(&chunk);
        return 1;
    }
    UskVmInitializer initializer;
    UskAstDecl initializer_function;
    if (!vm_build_global_initializer(&vm_program, &initializer,
                                     &initializer_function)) {
        fprintf(stderr, "usk: cannot construct VM global initializers\n");
        vm_destroy_global_initializer(&initializer);
        vm_destroy_globals(&vm_program);
        usk_value_arena_destroy(&storage);
        usk_bytecode_chunk_destroy(&chunk);
        return 1;
    }
    if (initializer.count) {
        UskBytecodeChunk initializer_chunk = {0};
        UskBytecodeCompileResult initializer_compile =
            usk_bytecode_compile_function(&initializer_function,
                command->source_path, &initializer_chunk,
                &parsed->diagnostics);
        UskBytecodeVmResult initializer_result = {0};
        if (initializer_compile.success)
            initializer_result = usk_bytecode_execute(&initializer_chunk,
                NULL, 0, &storage, &host, NULL);
        if (!initializer_compile.success || !initializer_result.completed) {
            if (initializer_compile.success)
                fprintf(stderr, "usk: global initialization failed: %s\n",
                    usk_bytecode_status_name(initializer_result.status));
            else
                fprintf(stderr, "usk: cannot compile global initializers\n");
            usk_diagnostics_print(&parsed->diagnostics, stderr);
            usk_bytecode_chunk_destroy(&initializer_chunk);
            vm_destroy_global_initializer(&initializer);
            vm_destroy_functions(&vm_program);
            vm_destroy_globals(&vm_program);
            usk_value_arena_destroy(&storage);
            usk_bytecode_chunk_destroy(&chunk);
            return 1;
        }
        usk_bytecode_chunk_destroy(&initializer_chunk);
    }
    vm_destroy_global_initializer(&initializer);
    Value entry_argument = null_value();
    const Value *vm_arguments = NULL;
    size_t vm_argument_count = 0;
    size_t extra_count = command->first_argument < argument_count
        ? (size_t)(argument_count - command->first_argument) : 0;
    if (runtime_limits.maximum_arguments &&
        extra_count > runtime_limits.maximum_arguments) {
        fprintf(stderr, "usk: received %zu arguments; limit is %zu\n",
                extra_count, runtime_limits.maximum_arguments);
        vm_destroy_functions(&vm_program);
        vm_destroy_globals(&vm_program);
        usk_value_arena_destroy(&storage);
        usk_bytecode_chunk_destroy(&chunk);
        return 1;
    }
    const UskAstParameter *parameter = entry->as.function.parameters;
    if (chunk.parameter_count > 1) {
        fprintf(stderr, "usk: bytecode VM supports zero or one entry parameter\n");
        vm_destroy_functions(&vm_program);
        vm_destroy_globals(&vm_program);
        usk_value_arena_destroy(&storage);
        usk_bytecode_chunk_destroy(&chunk);
        return 1;
    }
    if (chunk.parameter_count == 1 && parameter && parameter->type &&
        parameter->type->array_dimensions) {
        if (parameter->type->array_dimensions != 1 ||
            !vm_is_string_type(parameter->type)) {
            fprintf(stderr, "usk: VM entry argument arrays must be USKString[]\n");
            vm_destroy_functions(&vm_program);
            vm_destroy_globals(&vm_program);
            usk_value_arena_destroy(&storage);
            usk_bytecode_chunk_destroy(&chunk);
            return 1;
        }
        if (extra_count > (size_t)-1 / sizeof(Value)) {
            fprintf(stderr, "usk: too many command-line arguments\n");
            vm_destroy_functions(&vm_program);
            vm_destroy_globals(&vm_program);
            usk_value_arena_destroy(&storage);
            usk_bytecode_chunk_destroy(&chunk);
            return 1;
        }
        Value *items = extra_count ? (Value *)usk_value_arena_allocate(&storage,
            extra_count * sizeof(*items), true) : NULL;
        if (extra_count && !items) {
            fprintf(stderr, "usk: cannot allocate VM argument array\n");
            vm_destroy_functions(&vm_program);
            vm_destroy_globals(&vm_program);
            usk_value_arena_destroy(&storage);
            usk_bytecode_chunk_destroy(&chunk);
            return 1;
        }
        for (size_t index = 0; index < extra_count; ++index)
            items[index] = string_value_in(&storage,
                arguments[command->first_argument + (int)index]);
        entry_argument = array_value_in(&storage, items, extra_count);
        vm_arguments = &entry_argument;
        vm_argument_count = 1;
    } else if (chunk.parameter_count == 1 && parameter) {
        if (!parameter->type || parameter->type->array_dimensions ||
            !vm_is_string_type(parameter->type)) {
            fprintf(stderr, "usk: VM scalar entry argument must be USKString\n");
            vm_destroy_functions(&vm_program);
            vm_destroy_globals(&vm_program);
            usk_value_arena_destroy(&storage);
            usk_bytecode_chunk_destroy(&chunk);
            return 1;
        }
        const char *text = extra_count
            ? arguments[command->first_argument] : "";
        entry_argument = string_value_in(&storage, text);
        vm_arguments = &entry_argument;
        vm_argument_count = 1;
    }

    if (storage.failed) {
        fprintf(stderr, "usk: cannot allocate VM entry arguments\n");
        vm_destroy_functions(&vm_program);
        vm_destroy_globals(&vm_program);
        usk_value_arena_destroy(&storage);
        usk_bytecode_chunk_destroy(&chunk);
        return 1;
    }

    UskBytecodeVmResult execution = usk_bytecode_execute(&chunk,
        vm_arguments, vm_argument_count, &storage, &host, NULL);
    int process_status = 0;
    if (!execution.completed) {
        fprintf(stderr, "usk: bytecode VM failed at instruction %zu: %s\n",
            execution.instruction_index,
            usk_bytecode_status_name(execution.status));
        if (usk_diagnostics_has_errors(&parsed->diagnostics))
            usk_diagnostics_print(&parsed->diagnostics, stderr);
        process_status = 1;
    } else if (execution.return_value.kind == V_INT) {
        process_status = (int)execution.return_value.as.i;
    }
    vm_destroy_functions(&vm_program);
    vm_destroy_globals(&vm_program);
    usk_value_arena_destroy(&storage);
    usk_bytecode_chunk_destroy(&chunk);
    return process_status;
}

static int execute_command(const UskCommandLine *command, int argument_count,
                           char **arguments) {
    char *source = load_source(command->source_path);
    if (!source) return 2;
    UskParseResult parsed;
    usk_parse_result_init(&parsed);
    Tokens tokens = {0};
    int parse_success = parse_source_file(command->source_path, source, &tokens, &parsed);
    free(source);
    if (!parse_success) {
        if (command->json_diagnostics)
            usk_diagnostics_print_json(&parsed.diagnostics, stdout);
        else usk_diagnostics_print(&parsed.diagnostics, stderr);
        usk_tokens_free(&tokens);
        usk_parse_result_destroy(&parsed);
        return 1;
    }

    int status = 0;
    if (command->command == USK_COMMAND_CHECK) {
        status = run_check_command(command, &parsed);
    } else if (command->command == USK_COMMAND_AST) {
        usk_ast_dump_program(stdout, &parsed.program);
    } else if (command->command == USK_COMMAND_BYTECODE) {
        status = run_bytecode_command(command, &parsed);
    } else if (command->command == USK_COMMAND_VM) {
        status = run_vm_command(command, argument_count, arguments, &parsed);
    } else if (command->command == USK_COMMAND_EMIT_C) {
        status = run_emit_c_command(command, &parsed);
    } else {
        UskSemanticResult semantic = usk_analyze_program(&parsed.program,
            command->source_path, &parsed.diagnostics);
        if (!semantic.valid) {
            usk_diagnostics_print(&parsed.diagnostics, stderr);
            status = 1;
        } else {
            size_t extra_count = command->first_argument < argument_count
                ? (size_t)(argument_count - command->first_argument) : 0;
            const char *const *extra_arguments = extra_count
                ? (const char *const *)&arguments[command->first_argument] : NULL;
            UskEvaluatorResult result = usk_evaluate_program(
                &parsed.program, command->source_path, extra_arguments,
                extra_count, NULL, &parsed.diagnostics);
            usk_diagnostics_print(&parsed.diagnostics, stderr);
            status = result.process_status;
            usk_evaluator_result_destroy(&result);
        }
    }
    usk_tokens_free(&tokens);
    usk_parse_result_destroy(&parsed);
    return status;
}

int main(int argc, char **argv) {
    UskCommandLine command = parse_command_line(argc, argv);
    if (command.command == USK_COMMAND_HELP) {
        print_usage(stdout);
        return command.source_path ? 2 : 0;
    }
    if (command.command == USK_COMMAND_VERSION) {
        puts("UltraSonickan 0.2.0");
        return 0;
    }
    return execute_command(&command, argc, argv);
}
