#include "usk/diagnostic.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static char *format_message(const char *format, va_list arguments) {
    va_list copy;
    va_copy(copy, arguments);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0) return NULL;
    char *message = (char *)malloc((size_t)length + 1);
    if (!message) return NULL;
    vsnprintf(message, (size_t)length + 1, format, arguments);
    return message;
}

static char *copy_source_name(const char *source_name) {
    if (!source_name) source_name = "<source>";
    size_t length = strlen(source_name);
    char *copy = (char *)malloc(length + 1);
    if (copy) memcpy(copy, source_name, length + 1);
    return copy;
}

void usk_diagnostics_init(UskDiagnosticList *diagnostics) {
    if (diagnostics) memset(diagnostics, 0, sizeof(*diagnostics));
}

void usk_diagnostics_clear(UskDiagnosticList *diagnostics) {
    if (!diagnostics) return;
    for (size_t index = 0; index < diagnostics->count; ++index) {
        free((char *)diagnostics->items[index].source_name);
        free(diagnostics->items[index].message);
    }
    diagnostics->count = 0;
    diagnostics->error_count = 0;
    diagnostics->warning_count = 0;
}

void usk_diagnostics_destroy(UskDiagnosticList *diagnostics) {
    if (!diagnostics) return;
    usk_diagnostics_clear(diagnostics);
    free(diagnostics->items);
    memset(diagnostics, 0, sizeof(*diagnostics));
}

bool usk_diagnostics_add(UskDiagnosticList *diagnostics,
                         UskDiagnosticSeverity severity,
                         UskDiagnosticCode code,
                         const char *source_name,
                         int line,
                         int column,
                         const char *format,
                         ...) {
    if (!diagnostics || !format) return false;
    va_list arguments;
    va_start(arguments, format);
    char *message = format_message(format, arguments);
    va_end(arguments);
    char *source_copy = copy_source_name(source_name);
    if (!message || !source_copy) {
        free(message);
        free(source_copy);
        return false;
    }
    if (diagnostics->count == diagnostics->capacity) {
        size_t capacity = diagnostics->capacity ? diagnostics->capacity * 2 : 16;
        UskDiagnostic *items = (UskDiagnostic *)realloc(
            diagnostics->items, capacity * sizeof(*items));
        if (!items) {
            free(message);
            free(source_copy);
            return false;
        }
        diagnostics->items = items;
        diagnostics->capacity = capacity;
    }
    diagnostics->items[diagnostics->count++] = (UskDiagnostic){
        severity, code, source_copy, line, column, message
    };
    if (severity == USK_DIAGNOSTIC_ERROR || severity == USK_DIAGNOSTIC_FATAL)
        diagnostics->error_count++;
    if (severity == USK_DIAGNOSTIC_WARNING) diagnostics->warning_count++;
    return true;
}

bool usk_diagnostics_has_errors(const UskDiagnosticList *diagnostics) {
    return diagnostics && diagnostics->error_count != 0;
}

const UskDiagnostic *usk_diagnostics_get(const UskDiagnosticList *diagnostics,
                                         size_t index) {
    if (!diagnostics || index >= diagnostics->count) return NULL;
    return &diagnostics->items[index];
}

const char *usk_diagnostic_code_name(UskDiagnosticCode code) {
    switch (code) {
        case USK_DIAG_NONE: return "none";
        case USK_DIAG_OUT_OF_MEMORY: return "out-of-memory";
        case USK_DIAG_UNEXPECTED_TOKEN: return "unexpected-token";
        case USK_DIAG_EXPECTED_TOKEN: return "expected-token";
        case USK_DIAG_INVALID_LITERAL: return "invalid-literal";
        case USK_DIAG_UNTERMINATED_LITERAL: return "unterminated-literal";
        case USK_DIAG_DUPLICATE_NAME: return "duplicate-name";
        case USK_DIAG_UNKNOWN_NAME: return "unknown-name";
        case USK_DIAG_TYPE_MISMATCH: return "type-mismatch";
        case USK_DIAG_ARGUMENT_COUNT: return "argument-count";
        case USK_DIAG_INVALID_CONTROL_FLOW: return "invalid-control-flow";
        case USK_DIAG_INVALID_DECLARATION: return "invalid-declaration";
        case USK_DIAG_UNSUPPORTED_FEATURE: return "unsupported-feature";
        case USK_DIAG_IMPORT_NOT_FOUND: return "import-not-found";
        case USK_DIAG_IO_FAILURE: return "io-failure";
        case USK_DIAG_RESOURCE_LIMIT: return "resource-limit";
        case USK_DIAG_NUMERIC_OVERFLOW: return "numeric-overflow";
        case USK_DIAG_INTERNAL_ERROR: return "internal-error";
    }
    return "unknown-diagnostic";
}

const char *usk_diagnostic_severity_name(UskDiagnosticSeverity severity) {
    switch (severity) {
        case USK_DIAGNOSTIC_NOTE: return "note";
        case USK_DIAGNOSTIC_WARNING: return "warning";
        case USK_DIAGNOSTIC_ERROR: return "error";
        case USK_DIAGNOSTIC_FATAL: return "fatal";
    }
    return "diagnostic";
}

void usk_diagnostics_print(const UskDiagnosticList *diagnostics, FILE *stream) {
    if (!diagnostics || !stream) return;
    for (size_t index = 0; index < diagnostics->count; ++index) {
        const UskDiagnostic *diagnostic = &diagnostics->items[index];
        fprintf(stream, "%s:%d:%d: %s[%s]: %s\n",
                diagnostic->source_name ? diagnostic->source_name : "<source>",
                diagnostic->line, diagnostic->column,
                usk_diagnostic_severity_name(diagnostic->severity),
                usk_diagnostic_code_name(diagnostic->code),
                diagnostic->message ? diagnostic->message : "");
    }
}
