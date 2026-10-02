#include "evaluator_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

UskEvaluatorOptions usk_evaluator_default_options(void) {
    return (UskEvaluatorOptions){
        .maximum_call_depth = 256,
        .maximum_loop_iterations = 1000000,
        .maximum_value_allocations = 1000000,
        .maximum_value_bytes = (size_t)256 * 1024 * 1024,
        .maximum_arguments = 100000,
        .input_stream = NULL,
        .output_stream = NULL,
        .allow_standard_input = true
    };
}

UskFlow usk_flow_normal(void) {
    return (UskFlow){USK_FLOW_NORMAL, {.kind = V_NULL}};
}

UskFlow usk_flow_make(UskFlowKind kind, Value value) {
    return (UskFlow){kind, value};
}

void usk_eval_error(UskEvaluator *evaluator, UskDiagnosticCode code,
                    UskSourceSpan span, const char *format, ...) {
    if (!evaluator || !evaluator->diagnostics || !format) return;
    char message[1024];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    usk_diagnostics_add(evaluator->diagnostics, USK_DIAGNOSTIC_ERROR, code,
                        evaluator->source_name, span.line, span.column,
                        "%s", message);
    evaluator->failed = true;
}

static char *join_qualified_name(const char *prefix, const char *name) {
    if (!prefix || !prefix[0]) {
        size_t length = strlen(name);
        char *copy = (char *)malloc(length + 1);
        if (copy) memcpy(copy, name, length + 1);
        return copy;
    }
    size_t prefix_length = strlen(prefix), name_length = strlen(name);
    char *joined = (char *)malloc(prefix_length + name_length + 3);
    if (!joined) return NULL;
    memcpy(joined, prefix, prefix_length);
    joined[prefix_length] = ':';
    joined[prefix_length + 1] = ':';
    memcpy(joined + prefix_length + 2, name, name_length + 1);
    return joined;
}

static void register_function(UskEvaluator *evaluator,
                              const UskAstDecl *declaration,
                              const char *owner) {
    char *qualified = join_qualified_name(owner, declaration->as.function.name);
    UskRuntimeFunction *function = (UskRuntimeFunction *)calloc(1, sizeof(*function));
    if (!qualified || !function) {
        free(qualified);
        free(function);
        usk_eval_error(evaluator, USK_DIAG_OUT_OF_MEMORY, declaration->span,
                       "cannot register function '%s'",
                       declaration->as.function.name ? declaration->as.function.name : "<unnamed>");
        return;
    }
    function->declaration = declaration;
    function->qualified_name = qualified;
    function->next = evaluator->functions;
    evaluator->functions = function;
    evaluator->functions_registered++;
}

static void register_declarations(UskEvaluator *evaluator,
                                  const UskAstDecl *declarations,
                                  const char *owner) {
    for (const UskAstDecl *declaration = declarations; declaration && !evaluator->failed;
         declaration = declaration->next) {
        if (declaration->kind == USK_DECL_FUNCTION) {
            register_function(evaluator, declaration, owner);
        } else if (declaration->kind == USK_DECL_CLASS ||
                   declaration->kind == USK_DECL_STRUCT) {
            register_declarations(evaluator, declaration->as.record.members,
                                  declaration->as.record.name);
        }
    }
}

UskRuntimeFunction *usk_find_runtime_function(UskEvaluator *evaluator,
                                              const char *name) {
    if (!evaluator || !name) return NULL;
    for (UskRuntimeFunction *function = evaluator->functions; function;
         function = function->next)
        if (strcmp(function->qualified_name, name) == 0) return function;
    if (strstr(name, "::") == NULL) {
        for (UskRuntimeFunction *function = evaluator->functions; function;
             function = function->next) {
            const char *unqualified = strrchr(function->qualified_name, ':');
            unqualified = unqualified ? unqualified + 1 : function->qualified_name;
            if (strcmp(unqualified, name) == 0) return function;
        }
    }
    return NULL;
}

static void evaluate_global_declarations(UskEvaluator *evaluator,
                                         const UskAstDecl *declarations) {
    for (const UskAstDecl *declaration = declarations; declaration && !evaluator->failed;
         declaration = declaration->next) {
        if (declaration->kind == USK_DECL_VARIABLE) {
            Value value = usk_default_value_for_type(evaluator,
                declaration->as.variable.type);
            if (declaration->as.variable.initializer)
                value = usk_eval_expression(evaluator, &evaluator->globals,
                                            declaration->as.variable.initializer);
            const char *type = declaration->as.variable.type
                ? declaration->as.variable.type->name : NULL;
            if (type && !value_matches_type(type, value)) {
                usk_eval_error(evaluator, USK_DIAG_TYPE_MISMATCH, declaration->span,
                               "initializer for global '%s' does not match %s",
                               declaration->as.variable.name, type);
                continue;
            }
            bool immutable = declaration->as.variable.type &&
                             declaration->as.variable.type->is_const;
            if (define_typed_var(&evaluator->globals,
                    declaration->as.variable.name, value, immutable, type) !=
                USK_ENV_ASSIGN_OK) {
                usk_eval_error(evaluator, USK_DIAG_OUT_OF_MEMORY,
                    declaration->span,
                    "cannot allocate storage for global '%s'",
                    declaration->as.variable.name);
                continue;
            }
            evaluator->declarations_evaluated++;
        } else if (declaration->kind == USK_DECL_ENUM) {
            long long next_value = 0;
            for (const UskAstEnumValue *item = declaration->as.enumeration.values;
                 item && !evaluator->failed; item = item->next) {
                if (item->value) {
                    Value explicit_value = usk_eval_expression(evaluator,
                        &evaluator->globals, item->value);
                    if (!is_numeric(explicit_value)) {
                        usk_eval_error(evaluator, USK_DIAG_TYPE_MISMATCH,
                                       item->value->span,
                                       "enum values must be numeric constants");
                        break;
                    }
                    next_value = (long long)numeric(explicit_value);
                }
                char *qualified = join_qualified_name(declaration->as.enumeration.name,
                                                     item->name);
                if (!qualified) {
                    usk_eval_error(evaluator, USK_DIAG_OUT_OF_MEMORY,
                                   declaration->span,
                                   "cannot create enum member symbol");
                    break;
                }
                UskEnvironmentStatus define_status = define_var(
                    &evaluator->globals, qualified, int_value(next_value), true);
                free(qualified);
                if (define_status != USK_ENV_ASSIGN_OK) {
                    usk_eval_error(evaluator, USK_DIAG_OUT_OF_MEMORY,
                        declaration->span,
                        "cannot allocate storage for enum member '%s'",
                        item->name);
                    break;
                }
                next_value++;
                evaluator->declarations_evaluated++;
            }
        }
    }
}

static const UskAstDecl *find_entry_declaration(const UskAstDecl *declarations) {
    for (const UskAstDecl *declaration = declarations; declaration; declaration = declaration->next) {
        if (declaration->kind == USK_DECL_FUNCTION &&
            strcmp(declaration->as.function.name, "main") == 0)
            return declaration;
        if ((declaration->kind == USK_DECL_CLASS || declaration->kind == USK_DECL_STRUCT)) {
            const UskAstDecl *entry = find_entry_declaration(declaration->as.record.members);
            if (entry) return entry;
        }
    }
    return NULL;
}

UskEvaluatorResult usk_evaluate_program(const UskAstProgram *program,
                                       const char *source_name,
                                       const char *const *arguments,
                                       size_t argument_count,
                                       const UskEvaluatorOptions *options,
                                       UskDiagnosticList *diagnostics) {
    UskEvaluator evaluator = {0};
    evaluator.program = program;
    evaluator.source_name = source_name ? source_name : "<source>";
    evaluator.diagnostics = diagnostics;
    evaluator.options = options ? *options : usk_evaluator_default_options();
    if (evaluator.options.maximum_arguments &&
        argument_count > evaluator.options.maximum_arguments)
        usk_eval_error(&evaluator, USK_DIAG_ARGUMENT_COUNT, (UskSourceSpan){0},
                       "received %zu arguments; limit is %zu",
                       argument_count, evaluator.options.maximum_arguments);
    evaluator.value_storage = (UskValueArena *)calloc(1,
        sizeof(*evaluator.value_storage));
    if (evaluator.value_storage)
        usk_value_arena_init_limited(evaluator.value_storage,
            evaluator.options.maximum_value_allocations,
            evaluator.options.maximum_value_bytes);
    else
        usk_eval_error(&evaluator, USK_DIAG_OUT_OF_MEMORY, (UskSourceSpan){0},
                       "cannot create explicit runtime value storage");
    environment_init(&evaluator.globals, NULL);
    register_declarations(&evaluator, program ? program->declarations : NULL, NULL);
    evaluate_global_declarations(&evaluator, program ? program->declarations : NULL);

    const UskAstDecl *entry = find_entry_declaration(program ? program->declarations : NULL);
    Value return_value = null_value();
    int process_status = 0;
    if (!entry && !evaluator.failed) {
        usk_eval_error(&evaluator, USK_DIAG_INVALID_DECLARATION, (UskSourceSpan){0},
                       "no fn main(...) entry point found");
    }
    if (entry && !evaluator.failed) {
        UskRuntimeFunction *function = usk_find_runtime_function(&evaluator,
            entry->as.function.name);
        const UskAstParameter *entry_parameter = entry->as.function.parameters;
        Value *call_arguments = NULL;
        size_t call_argument_count = 0;
        if (entry_parameter && entry_parameter->type &&
            entry_parameter->type->array_dimensions) {
            if (argument_count > (size_t)-1 / sizeof(Value)) {
                usk_eval_error(&evaluator, USK_DIAG_OUT_OF_MEMORY, entry->span,
                               "command-line argument count exceeds runtime limits");
                argument_count = 0;
            }
            Value *items = argument_count
                ? (Value *)usk_value_arena_allocate(evaluator.value_storage,
                    argument_count * sizeof(*items), true) : NULL;
            if (argument_count && !items) {
                usk_eval_error(&evaluator, USK_DIAG_OUT_OF_MEMORY, entry->span,
                               "cannot allocate command-line argument values");
                items = NULL;
            }
            for (size_t index = 0; items && index < argument_count; ++index)
                items[index] = string_value_in(evaluator.value_storage,
                    arguments[index] ? arguments[index] : "");
            call_arguments = items || !argument_count
                ? (Value *)calloc(1, sizeof(*call_arguments)) : NULL;
            if (call_arguments) {
                call_arguments[0] = array_value_in(evaluator.value_storage,
                    items, argument_count);
                call_argument_count = 1;
            } else {
                usk_eval_error(&evaluator, USK_DIAG_OUT_OF_MEMORY, entry->span,
                               "cannot allocate entry-point arguments");
            }
        } else if (entry_parameter && argument_count) {
            call_arguments = (Value *)calloc(1, sizeof(*call_arguments));
            if (call_arguments) {
                call_arguments[0] = string_value_in(evaluator.value_storage,
                    arguments[0] ? arguments[0] : "");
                call_argument_count = 1;
            } else {
                usk_eval_error(&evaluator, USK_DIAG_OUT_OF_MEMORY, entry->span,
                               "cannot allocate entry-point argument");
            }
        }
        return_value = usk_call_function(&evaluator, function, call_arguments,
                                         call_argument_count, entry->span);
        free(call_arguments);
        if (return_value.kind == V_INT) process_status = (int)return_value.as.i;
    }

    UskRuntimeFunction *function = evaluator.functions;
    while (function) {
        UskRuntimeFunction *next = function->next;
        free((void *)function->qualified_name);
        free(function);
        function = next;
    }
    environment_destroy(&evaluator.globals);
    if (evaluator.value_storage && evaluator.value_storage->failed &&
        !evaluator.failed)
        usk_eval_error(&evaluator, USK_DIAG_OUT_OF_MEMORY, (UskSourceSpan){0},
                       "runtime value storage was exhausted");
    size_t value_allocations = evaluator.value_storage
        ? usk_value_arena_allocation_count(evaluator.value_storage) : 0;
    size_t value_bytes = evaluator.value_storage
        ? usk_value_arena_allocation_bytes(evaluator.value_storage) : 0;
    UskEvaluatorResult result = {
        .completed = !evaluator.failed,
        .process_status = evaluator.failed ? 1 : process_status,
        .return_value = return_value,
        .functions_registered = evaluator.functions_registered,
        .declarations_evaluated = evaluator.declarations_evaluated,
        .value_allocations = value_allocations,
        .value_bytes = value_bytes,
        .value_storage = evaluator.value_storage
    };
    evaluator.value_storage = NULL;
    return result;
}

void usk_evaluator_result_destroy(UskEvaluatorResult *result) {
    if (!result) return;
    if (result->value_storage) {
        usk_value_arena_destroy(result->value_storage);
        free(result->value_storage);
        result->value_storage = NULL;
    }
    result->return_value = null_value();
    result->completed = false;
    result->process_status = 0;
    result->functions_registered = 0;
    result->declarations_evaluated = 0;
    result->value_allocations = 0;
    result->value_bytes = 0;
}
