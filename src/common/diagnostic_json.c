#include "usk/diagnostic.h"

#include <inttypes.h>
#include <string.h>

static bool write_byte(FILE *stream, unsigned char byte) {
    return fputc((int)byte, stream) != EOF;
}

static bool write_hex_escape(FILE *stream, unsigned char byte) {
    static const char digits[] = "0123456789abcdef";
    return fputs("\\u00", stream) >= 0 &&
           write_byte(stream, (unsigned char)digits[byte >> 4]) &&
           write_byte(stream, (unsigned char)digits[byte & 15]);
}

static bool write_json_string(FILE *stream, const char *text) {
    if (!text) text = "";
    if (!write_byte(stream, '"')) return false;
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor; ++cursor) {
        unsigned char byte = *cursor;
        if (byte == '"' || byte == '\\') {
            if (!write_byte(stream, '\\') || !write_byte(stream, byte))
                return false;
        } else if (byte == '\b') {
            if (fputs("\\b", stream) < 0) return false;
        } else if (byte == '\f') {
            if (fputs("\\f", stream) < 0) return false;
        } else if (byte == '\n') {
            if (fputs("\\n", stream) < 0) return false;
        } else if (byte == '\r') {
            if (fputs("\\r", stream) < 0) return false;
        } else if (byte == '\t') {
            if (fputs("\\t", stream) < 0) return false;
        } else if (byte < 0x20) {
            if (!write_hex_escape(stream, byte)) return false;
        } else if (!write_byte(stream, byte)) {
            return false;
        }
    }
    return write_byte(stream, '"');
}

size_t usk_diagnostics_count_severity(
    const UskDiagnosticList *diagnostics, UskDiagnosticSeverity severity) {
    if (!diagnostics || (diagnostics->count && !diagnostics->items)) return 0;
    size_t count = 0;
    for (size_t index = 0; index < diagnostics->count; ++index)
        if (diagnostics->items[index].severity == severity) count++;
    return count;
}

bool usk_diagnostics_has_severity(
    const UskDiagnosticList *diagnostics, UskDiagnosticSeverity severity) {
    if (!diagnostics || (diagnostics->count && !diagnostics->items)) return false;
    for (size_t index = 0; index < diagnostics->count; ++index)
        if (diagnostics->items[index].severity == severity) return true;
    return false;
}

bool usk_diagnostic_print_json(const UskDiagnostic *diagnostic, FILE *stream) {
    if (!stream) return false;
    if (!diagnostic || fputs("{\"severity\":", stream) < 0 ||
        !write_json_string(stream,
            usk_diagnostic_severity_name(diagnostic->severity)) ||
        fputs(",\"code\":", stream) < 0 ||
        !write_json_string(stream, usk_diagnostic_code_name(diagnostic->code)) ||
        fputs(",\"source\":", stream) < 0 ||
        !write_json_string(stream, diagnostic->source_name) ||
        fprintf(stream, ",\"line\":%d,\"column\":%d,\"message\":",
                diagnostic->line, diagnostic->column) < 0 ||
        !write_json_string(stream, diagnostic->message) ||
        write_byte(stream, '}') == false)
        return false;
    return true;
}

bool usk_diagnostics_print_json(const UskDiagnosticList *diagnostics,
                                FILE *stream) {
    if (!diagnostics || !stream ||
        (diagnostics->count && !diagnostics->items) ||
        !write_byte(stream, '[')) return false;
    for (size_t index = 0; index < diagnostics->count; ++index) {
        if (index && !write_byte(stream, ',')) return false;
        if (!usk_diagnostic_print_json(&diagnostics->items[index], stream))
            return false;
    }
    if (fputs("]\n", stream) < 0) return false;
    return !ferror(stream);
}

bool usk_diagnostics_print_json_lines(
    const UskDiagnosticList *diagnostics, FILE *stream) {
    if (!diagnostics || !stream ||
        (diagnostics->count && !diagnostics->items)) return false;
    for (size_t index = 0; index < diagnostics->count; ++index) {
        if (!usk_diagnostic_print_json(&diagnostics->items[index], stream) ||
            !write_byte(stream, '\n')) return false;
    }
    return !ferror(stream);
}
