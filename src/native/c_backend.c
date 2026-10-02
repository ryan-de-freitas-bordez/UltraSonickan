#include "c_backend_internal.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void usk_c_backend_options_init(UskCBackendOptions *options) {
    if (!options) return;
    options->maximum_expression_depth = 512;
    options->maximum_declarations = 100000;
    options->entry_function_name = "main";
    options->emit_runtime_support = true;
    options->emit_c_main_wrapper = true;
    options->emit_source_line_directives = false;
}

bool usk_c_emit_vwrite(UskCEmitter *emitter, const char *format,
                       va_list arguments) {
    if (!emitter || !emitter->output || !format || emitter->failed) return false;
    int written = vfprintf(emitter->output, format, arguments);
    if (written < 0) {
        emitter->failed = true;
        return false;
    }
    if ((size_t)written > SIZE_MAX - emitter->result->bytes_emitted) {
        emitter->failed = true;
        return false;
    }
    emitter->result->bytes_emitted += (size_t)written;
    return true;
}

bool usk_c_emit_write(UskCEmitter *emitter, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    bool result = usk_c_emit_vwrite(emitter, format, arguments);
    va_end(arguments);
    return result;
}

void usk_c_emit_indent(UskCEmitter *emitter) {
    for (size_t index = 0; index < emitter->indentation; ++index)
        usk_c_emit_write(emitter, "    ");
}

void usk_c_emit_error(UskCEmitter *emitter, UskDiagnosticCode code,
                      UskSourceSpan span, const char *format, ...) {
    if (!emitter || !emitter->diagnostics || !format) return;
    char message[1024];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    usk_diagnostics_add(emitter->diagnostics, USK_DIAGNOSTIC_ERROR, code,
        emitter->source_name, span.line, span.column, "%s", message);
    emitter->failed = true;
}

void usk_c_emit_unsupported(UskCEmitter *emitter, UskSourceSpan span,
                            const char *feature) {
    if (!emitter || !feature) return;
    emitter->result->unsupported_constructs++;
    usk_c_emit_error(emitter, USK_DIAG_UNSUPPORTED_FEATURE, span,
                     "C11 backend does not support %s", feature);
}

bool usk_c_name_is_entry(const UskCEmitter *emitter,
                         const UskAstDecl *declaration) {
    return emitter && declaration && declaration->kind == USK_DECL_FUNCTION &&
        declaration->as.function.name && emitter->options.entry_function_name &&
        strcmp(declaration->as.function.name,
               emitter->options.entry_function_name) == 0;
}

char *usk_c_join_name(const char *owner, const char *name) {
    if (!name) return NULL;
    size_t owner_length = owner ? strlen(owner) : 0;
    size_t name_length = strlen(name);
    size_t separator = owner_length ? 2 : 0;
    if (owner_length > SIZE_MAX - separator - name_length - 1) return NULL;
    size_t length = owner_length + separator + name_length;
    char *joined = (char *)malloc(length + 1);
    if (!joined) return NULL;
    if (owner_length) {
        memcpy(joined, owner, owner_length);
        joined[owner_length] = ':';
        joined[owner_length + 1] = ':';
    }
    memcpy(joined + owner_length + separator, name, name_length + 1);
    return joined;
}

char *usk_c_mangle_name(const char *qualified_name) {
    if (!qualified_name) return NULL;
    size_t length = strlen(qualified_name);
    if (length > (SIZE_MAX - 5) / 2) return NULL;
    char *mangled = (char *)malloc(length * 2 + 5);
    if (!mangled) return NULL;
    memcpy(mangled, "usk_", 4);
    size_t output = 4;
    for (size_t index = 0; index < length; ++index) {
        unsigned char character = (unsigned char)qualified_name[index];
        if (character == ':' && qualified_name[index + 1] == ':') {
            mangled[output++] = '_';
            mangled[output++] = '_';
            index++;
        } else if (isalnum(character) || character == '_') {
            mangled[output++] = (char)character;
        } else {
            mangled[output++] = '_';
        }
    }
    mangled[output] = '\0';
    return mangled;
}

static bool find_entry_in_list(UskCEmitter *emitter,
                               const UskAstDecl *declaration,
                               const char *owner, const UskAstDecl **entry,
                               char **entry_name) {
    for (; declaration && !emitter->failed; declaration = declaration->next) {
        if (usk_c_name_is_entry(emitter, declaration) &&
            declaration->as.function.body) {
            char *qualified = usk_c_join_name(owner,
                declaration->as.function.name);
            char *mangled = qualified ? usk_c_mangle_name(qualified) : NULL;
            free(qualified);
            if (!mangled) {
                usk_c_emit_error(emitter, USK_DIAG_OUT_OF_MEMORY,
                    declaration->span, "cannot allocate the entry symbol name");
                return false;
            }
            if (*entry) {
                free(mangled);
                usk_c_emit_error(emitter, USK_DIAG_DUPLICATE_NAME,
                    declaration->span,
                    "more than one C backend entry function named '%s'",
                    emitter->options.entry_function_name);
                return false;
            }
            *entry = declaration;
            *entry_name = mangled;
        } else if (declaration->kind == USK_DECL_CLASS ||
                   declaration->kind == USK_DECL_STRUCT) {
            char *next_owner = usk_c_join_name(owner,
                declaration->as.record.name);
            if (!next_owner) {
                usk_c_emit_error(emitter, USK_DIAG_OUT_OF_MEMORY,
                    declaration->span, "cannot allocate a record name");
                return false;
            }
            bool success = find_entry_in_list(emitter,
                declaration->as.record.members, next_owner, entry, entry_name);
            free(next_owner);
            if (!success) return false;
        }
    }
    return !emitter->failed;
}

static bool emit_entry_wrapper(UskCEmitter *emitter,
                               const UskAstDecl *entry,
                               const char *entry_name) {
    const UskAstParameter *parameter = entry->as.function.parameters;
    if (entry->as.function.parameter_count > 1) {
        usk_c_emit_unsupported(emitter, entry->span,
                               "entry functions with multiple parameters");
        return false;
    }
    if (parameter && (!parameter->type ||
        strcmp(parameter->type->name, "USKString") != 0 ||
        parameter->type->array_dimensions != 1)) {
        usk_c_emit_unsupported(emitter, entry->span,
                               "entry parameters other than USKString[]");
        return false;
    }
    bool returns_value = entry->as.function.return_type &&
        strcmp(entry->as.function.return_type->name, "USKNull") != 0;
    if (returns_value) {
        const char *return_type = entry->as.function.return_type->name;
        if (strcmp(return_type, "USKInt") && strcmp(return_type, "USKLong") &&
            strcmp(return_type, "USKShort") && strcmp(return_type, "USKBool")) {
            usk_c_emit_unsupported(emitter, entry->span,
                "entry return types other than signed integers and USKBool");
            return false;
        }
    }
    usk_c_emit_write(emitter, "\nint main(int argc, char **argv) {\n");
    usk_c_emit_write(emitter, "    (void)argc;\n");
    usk_c_emit_write(emitter, "    (void)argv;\n");
    if (parameter)
        usk_c_emit_write(emitter,
            "    const char **usk_arguments = (const char **)(argv + 1);\n");
    if (returns_value)
        usk_c_emit_write(emitter, "    return (int)%s(", entry_name);
    else
        usk_c_emit_write(emitter, "    %s(", entry_name);
    if (parameter) usk_c_emit_write(emitter, "usk_arguments");
    usk_c_emit_write(emitter, ");\n");
    if (!returns_value) usk_c_emit_write(emitter, "    return 0;\n");
    usk_c_emit_write(emitter, "}\n");
    return !emitter->failed;
}

void usk_c_backend_options_init(UskCBackendOptions *options);

UskCBackendResult usk_c_backend_emit_program(
    const UskAstProgram *program, const char *source_name,
    FILE *output, const UskCBackendOptions *options,
    UskDiagnosticList *diagnostics) {
    UskCBackendResult result = {0};
    UskCBackendOptions effective;
    usk_c_backend_options_init(&effective);
    if (options) effective = *options;
    if (!program || !output || !diagnostics ||
        !effective.entry_function_name || !effective.entry_function_name[0]) {
        result.success = false;
        return result;
    }
    UskCEmitter emitter = {
        .program = program,
        .source_name = source_name ? source_name : "<source>",
        .output = output,
        .options = effective,
        .diagnostics = diagnostics,
        .result = &result
    };
    const UskAstDecl *entry = NULL;
    char *entry_name = NULL;
    find_entry_in_list(&emitter, program->declarations, NULL, &entry,
                       &entry_name);
    if (!entry && !emitter.failed)
        usk_c_emit_error(&emitter, USK_DIAG_UNKNOWN_NAME, (UskSourceSpan){0},
                         "program has no function named '%s'",
                         effective.entry_function_name);
    if (!emitter.failed && effective.emit_runtime_support)
        usk_c_emit_runtime_support(&emitter);

    for (int definition = 0; definition <= 1 && !emitter.failed;
         ++definition) {
        for (const UskAstDecl *declaration = program->declarations;
             declaration && !emitter.failed;
             declaration = declaration->next) {
            if (definition && effective.maximum_declarations &&
                result.declarations_emitted >= effective.maximum_declarations) {
                usk_c_emit_error(&emitter, USK_DIAG_RESOURCE_LIMIT,
                    declaration->span,
                    "C backend declaration limit reached");
                break;
            }
            usk_c_emit_declaration(&emitter, declaration, NULL,
                                   definition != 0);
            if (definition && !emitter.failed)
                result.declarations_emitted++;
        }
    }
    if (!emitter.failed && effective.emit_c_main_wrapper)
        emit_entry_wrapper(&emitter, entry, entry_name);
    if (ferror(output)) {
        usk_c_emit_error(&emitter, USK_DIAG_IO_FAILURE, (UskSourceSpan){0},
                         "failed while writing generated C source");
    }
    free(entry_name);
    result.success = !emitter.failed &&
                     result.unsupported_constructs == 0;
    return result;
}

bool usk_c_backend_result_succeeded(const UskCBackendResult *result) {
    return result && result->success;
}

const char *usk_c_backend_status_name(bool success,
                                      size_t unsupported_constructs) {
    if (success) return "success";
    return unsupported_constructs ? "unsupported-language-feature"
                                  : "code-generation-failure";
}
