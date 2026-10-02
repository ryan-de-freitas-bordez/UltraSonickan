#include "evaluator_internal.h"
#include "usk/arraylib.h"
#include "usk/filesystem.h"
#include "usk/mathlib.h"
#include "usk/pathlib.h"
#include "usk/stringlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool write_value(FILE *stream, Value value) {
    char *text = value_string(value);
    if (!text) return false;
    bool success = fputs(text, stream) >= 0;
    free(text);
    return success;
}

static char *callee_name(const UskAstExpr *callee) {
    if (!callee) return NULL;
    if (callee->kind == USK_EXPR_NAME) {
        const char *name = callee->as.name ? callee->as.name : "";
        size_t length = strlen(name);
        char *copy = (char *)malloc(length + 1);
        if (copy) memcpy(copy, name, length + 1);
        return copy;
    }
    if (callee->kind == USK_EXPR_MEMBER) {
        char *owner = callee_name(callee->as.member.object);
        if (!owner) return NULL;
        const char *member = callee->as.member.name ? callee->as.member.name : "";
        const char *separator = callee->as.member.pointer_access ? "->" : "::";
        size_t length = strlen(owner) + strlen(separator) + strlen(member) + 1;
        char *name = (char *)malloc(length);
        if (name) snprintf(name, length, "%s%s%s", owner, separator, member);
        free(owner);
        return name;
    }
    return NULL;
}

static bool write_formatted(FILE *stream, Value *arguments,
                            size_t argument_count) {
    if (!stream || !argument_count || arguments[0].kind != V_STRING)
        return false;
    const char *format = arguments[0].as.s ? arguments[0].as.s : "";
    size_t argument = 1;
    for (size_t index = 0; format[index];) {
        if (format[index] == '{' && format[index + 1] == '{') {
            if (fputc('{', stream) == EOF) return false;
            index += 2;
            continue;
        }
        if (format[index] == '}' && format[index + 1] == '}') {
            if (fputc('}', stream) == EOF) return false;
            index += 2;
            continue;
        }
        if (format[index] == '{') {
            size_t close = index + 1;
            while (format[close] && format[close] != '}') close++;
            if (format[close] == '}') {
                if (argument < argument_count) {
                    if (!write_value(stream, arguments[argument++])) return false;
                } else for (size_t literal = index; literal <= close; ++literal)
                    if (fputc(format[literal], stream) == EOF) return false;
                index = close + 1;
                continue;
            }
        }
        if (fputc(format[index++], stream) == EOF) return false;
    }
    return !ferror(stream);
}

static bool is_builtin_name(const char *name) {
    static const char *builtins[] = {
        "io::writef", "iostream::io::writef", "writef",
        "io::write", "iostream::io::write", "print",
        "io::writeln", "iostream::io::writeln", "println",
        "io::readline", "iostream::io::readline", "readline",
        "typeof", NULL
    };
    if (usk_array_builtin_is_name(name) || usk_path_builtin_is_name(name) ||
        usk_fs_builtin_is_name(name))
        return true;
    for (size_t index = 0; builtins[index]; ++index)
        if (strcmp(name, builtins[index]) == 0) return true;
    return false;
}

static Value call_builtin(UskEvaluator *evaluator, const char *name,
                          const Value *arguments, size_t argument_count,
                          UskSourceSpan span) {
    if (usk_fs_builtin_is_name(name)) {
        Value result = null_value();
        UskFsStatus status = usk_fs_builtin_call(name, arguments,
            argument_count, evaluator->value_storage, &result);
        if (status != USK_FS_OK) {
            UskDiagnosticCode code = status == USK_FS_ARGUMENT_COUNT
                ? USK_DIAG_ARGUMENT_COUNT
                : status == USK_FS_TYPE_MISMATCH
                    ? USK_DIAG_TYPE_MISMATCH
                    : status == USK_FS_ALLOCATION_FAILURE
                        ? USK_DIAG_OUT_OF_MEMORY
                        : status == USK_FS_IO_FAILURE
                            ? USK_DIAG_IO_FAILURE : USK_DIAG_INVALID_LITERAL;
            usk_eval_error(evaluator, code, span,
                "filesystem operation '%s' failed: %s", name,
                usk_fs_status_name(status));
        }
        return result;
    }
    if (usk_path_builtin_is_name(name)) {
        Value result = null_value();
        UskPathBuiltinStatus status = usk_path_builtin_call(name, arguments,
            argument_count, evaluator->value_storage, &result);
        if (status != USK_PATH_BUILTIN_OK) {
            UskDiagnosticCode code = status == USK_PATH_BUILTIN_ARGUMENT_COUNT
                ? USK_DIAG_ARGUMENT_COUNT
                : status == USK_PATH_BUILTIN_TYPE_MISMATCH
                    ? USK_DIAG_TYPE_MISMATCH
                    : status == USK_PATH_BUILTIN_ALLOCATION_FAILURE
                        ? USK_DIAG_OUT_OF_MEMORY : USK_DIAG_INVALID_LITERAL;
            usk_eval_error(evaluator, code, span,
                "path operation '%s' failed: %s", name,
                usk_path_builtin_status_name(status));
        }
        return result;
    }
    if (usk_array_builtin_is_name(name)) {
        Value result = null_value();
        UskArrayBuiltinStatus status = usk_array_builtin_call(name, arguments,
            argument_count, evaluator->value_storage, &result);
        if (status != USK_ARRAY_BUILTIN_OK) {
            UskDiagnosticCode code = status == USK_ARRAY_BUILTIN_ARGUMENT_COUNT
                ? USK_DIAG_ARGUMENT_COUNT
                : status == USK_ARRAY_BUILTIN_TYPE_MISMATCH
                    ? USK_DIAG_TYPE_MISMATCH
                    : status == USK_ARRAY_BUILTIN_ALLOCATION_FAILURE
                        ? USK_DIAG_OUT_OF_MEMORY : USK_DIAG_INVALID_LITERAL;
            usk_eval_error(evaluator, code, span,
                "array operation '%s' failed: %s", name,
                usk_array_builtin_status_name(status));
        }
        return result;
    }
    if (!strcmp(name, "io::writef") || !strcmp(name, "iostream::io::writef") ||
        !strcmp(name, "writef")) {
        FILE *output = evaluator->options.output_stream
            ? evaluator->options.output_stream : stdout;
        if (!write_formatted(output, (Value *)arguments, argument_count))
            usk_eval_error(evaluator, USK_DIAG_IO_FAILURE, span,
                           "formatted output failed");
        return null_value();
    }
    if (!strcmp(name, "io::write") || !strcmp(name, "iostream::io::write") ||
        !strcmp(name, "print")) {
        FILE *output = evaluator->options.output_stream
            ? evaluator->options.output_stream : stdout;
        for (size_t index = 0; index < argument_count; ++index)
            if (!write_value(output, arguments[index])) {
                usk_eval_error(evaluator, USK_DIAG_IO_FAILURE, span,
                               "standard output failed");
                return null_value();
            }
        return null_value();
    }
    if (!strcmp(name, "io::writeln") || !strcmp(name, "iostream::io::writeln") ||
        !strcmp(name, "println")) {
        FILE *output = evaluator->options.output_stream
            ? evaluator->options.output_stream : stdout;
        for (size_t index = 0; index < argument_count; ++index)
            if (!write_value(output, arguments[index])) {
                usk_eval_error(evaluator, USK_DIAG_IO_FAILURE, span,
                               "standard output failed");
                return null_value();
            }
        if (fputc('\n', output) == EOF) {
            usk_eval_error(evaluator, USK_DIAG_IO_FAILURE, span,
                           "standard output failed");
            return null_value();
        }
        return null_value();
    }
    if (!strcmp(name, "io::readline") ||
        !strcmp(name, "iostream::io::readline") ||
        !strcmp(name, "readline")) {
        if (!evaluator->options.allow_standard_input) {
            usk_eval_error(evaluator, USK_DIAG_UNSUPPORTED_FEATURE, span,
                           "standard input is disabled by the evaluator options");
            return null_value();
        }
        FILE *input = evaluator->options.input_stream
            ? evaluator->options.input_stream : stdin;
        char line[8192];
        if (!fgets(line, sizeof(line), input)) {
            if (ferror(input)) {
                usk_eval_error(evaluator, USK_DIAG_IO_FAILURE, span,
                               "standard input failed");
                return null_value();
            }
            return string_value_in(evaluator->value_storage, "");
        }
        line[strcspn(line, "\r\n")] = '\0';
        return string_value_in(evaluator->value_storage, line);
    }
    if (!strcmp(name, "typeof"))
        return string_value_in(evaluator->value_storage,
            argument_count ? type_name(arguments[0]) : "USKNull");
    return null_value();
}

Value usk_call_function(UskEvaluator *evaluator,
                        UskRuntimeFunction *function,
                        const Value *arguments,
                        size_t argument_count,
                        UskSourceSpan call_span) {
    if (!function) {
        usk_eval_error(evaluator, USK_DIAG_UNKNOWN_NAME, call_span,
                       "attempted to call a missing function");
        return null_value();
    }
    if (evaluator->call_depth >= evaluator->options.maximum_call_depth) {
        usk_eval_error(evaluator, USK_DIAG_INVALID_CONTROL_FLOW, call_span,
                       "maximum function call depth (%zu) exceeded",
                       evaluator->options.maximum_call_depth);
        return null_value();
    }
    const UskAstDecl *declaration = function->declaration;
    size_t required = 0;
    for (const UskAstParameter *parameter = declaration->as.function.parameters;
         parameter; parameter = parameter->next)
        if (!parameter->default_value) required++;
    if (argument_count < required || argument_count > declaration->as.function.parameter_count) {
        usk_eval_error(evaluator, USK_DIAG_ARGUMENT_COUNT, call_span,
                       "function '%s' expects %zu to %zu arguments; received %zu",
                       declaration->as.function.name,
                       required, declaration->as.function.parameter_count, argument_count);
        return null_value();
    }
    if (!declaration->as.function.body) {
        usk_eval_error(evaluator, USK_DIAG_UNSUPPORTED_FEATURE, call_span,
                       "external function '%s' has no registered native binding",
                       declaration->as.function.name);
        return null_value();
    }

    Environment local;
    environment_init(&local, &evaluator->globals);
    size_t index = 0;
    for (const UskAstParameter *parameter = declaration->as.function.parameters;
         parameter && !evaluator->failed; parameter = parameter->next, ++index) {
        Value value = index < argument_count ? arguments[index]
            : usk_eval_expression(evaluator, &local, parameter->default_value);
        if (parameter->type && !parameter->type->array_dimensions &&
            !value_matches_type(parameter->type->name, value)) {
            usk_eval_error(evaluator, USK_DIAG_TYPE_MISMATCH, call_span,
                           "argument '%s' does not match parameter type %s",
                           parameter->name, parameter->type->name);
            break;
        }
        if (define_typed_var(&local, parameter->name, value,
                         parameter->type && parameter->type->is_const,
                         parameter->type && !parameter->type->array_dimensions
                            ? parameter->type->name : NULL) != USK_ENV_ASSIGN_OK)
            usk_eval_error(evaluator, USK_DIAG_OUT_OF_MEMORY, call_span,
                           "cannot allocate parameter '%s'",
                           parameter->name);
    }

    evaluator->call_depth++;
    UskFlow flow = usk_eval_statement(evaluator, &local, declaration->as.function.body);
    evaluator->call_depth--;
    Value result = flow.kind == USK_FLOW_RETURN ? flow.value : null_value();
    if (flow.kind == USK_FLOW_BREAK || flow.kind == USK_FLOW_CONTINUE)
        usk_eval_error(evaluator, USK_DIAG_INVALID_CONTROL_FLOW, call_span,
                       "break or continue escaped function '%s'",
                       declaration->as.function.name);
    if (declaration->as.function.return_type &&
        (declaration->as.function.return_type->array_dimensions
            ? result.kind != V_ARRAY
            : !value_matches_type(declaration->as.function.return_type->name,
                                  result)))
        usk_eval_error(evaluator, USK_DIAG_TYPE_MISMATCH, call_span,
                       "return value does not match declared return type %s%s",
                       declaration->as.function.return_type->name,
                       declaration->as.function.return_type->array_dimensions
                           ? "[]" : "");
    environment_destroy(&local);
    return result;
}

Value usk_eval_call(UskEvaluator *evaluator, Environment *environment,
                    const UskAstExpr *callee, const Value *arguments,
                    size_t argument_count, UskSourceSpan span) {
    const UskArrayBuiltinInfo *array_method = callee &&
        callee->kind == USK_EXPR_MEMBER
        ? usk_array_method_find(callee->as.member.name) : NULL;
    size_t name_length = array_method ? strlen(array_method->name) + 1 : 0;
    char *name = array_method ? (char *)malloc(name_length) : callee_name(callee);
    if (array_method && name) memcpy(name, array_method->name, name_length);
    if (!name) {
        usk_eval_error(evaluator, USK_DIAG_UNSUPPORTED_FEATURE, span,
                       "call target is not a named function or method");
        return null_value();
    }
    Value result = null_value();
    if (usk_math_builtin_is_name(name)) {
        UskMathBuiltinStatus status = usk_math_builtin_call(name, arguments,
            argument_count, &result);
        if (status != USK_MATH_BUILTIN_OK) {
            UskDiagnosticCode code = status == USK_MATH_BUILTIN_ARGUMENT_COUNT
                ? USK_DIAG_ARGUMENT_COUNT
                : status == USK_MATH_BUILTIN_TYPE_MISMATCH
                    ? USK_DIAG_TYPE_MISMATCH : USK_DIAG_INVALID_LITERAL;
            usk_eval_error(evaluator, code, span,
                "math operation '%s' failed: %s", name,
                usk_math_builtin_status_name(status));
        }
    } else if (usk_string_builtin_is_name(name)) {
        UskStringBuiltinStatus status = usk_string_builtin_call(name,
            arguments, argument_count, evaluator->value_storage, &result);
        if (status != USK_STRING_BUILTIN_OK) {
            UskDiagnosticCode code = status == USK_STRING_BUILTIN_ARGUMENT_COUNT
                ? USK_DIAG_ARGUMENT_COUNT
                : status == USK_STRING_BUILTIN_TYPE_MISMATCH
                    ? USK_DIAG_TYPE_MISMATCH
                    : status == USK_STRING_BUILTIN_ALLOCATION_FAILURE
                        ? USK_DIAG_OUT_OF_MEMORY : USK_DIAG_INVALID_LITERAL;
            usk_eval_error(evaluator, code, span,
                "string operation '%s' failed: %s", name,
                usk_string_builtin_status_name(status));
        }
    } else if (is_builtin_name(name)) {
        result = call_builtin(evaluator, name, arguments, argument_count, span);
    } else {
        UskRuntimeFunction *function = usk_find_runtime_function(evaluator, name);
        if (function) result = usk_call_function(evaluator, function,
                                                 arguments, argument_count, span);
        else if (lookup_var(environment, name))
            usk_eval_error(evaluator, USK_DIAG_UNSUPPORTED_FEATURE, span,
                           "callable values and closures are not available for '%s'", name);
        else
            usk_eval_error(evaluator, USK_DIAG_UNKNOWN_NAME, span,
                           "unknown function '%s'", name);
    }
    free(name);
    return result;
}
