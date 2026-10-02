#include "usk/c_backend.h"

#include <errno.h>
#include <string.h>

static void report_file_error(UskDiagnosticList *diagnostics,
                              const char *source_name,
                              const char *operation,
                              const char *path) {
    const char *description = strerror(errno);
    if (path)
        usk_diagnostics_add(diagnostics, USK_DIAGNOSTIC_ERROR,
            USK_DIAG_IO_FAILURE, source_name, 0, 0,
            "C backend %s '%s': %s", operation, path,
            description ? description : "unknown I/O error");
    else
        usk_diagnostics_add(diagnostics, USK_DIAGNOSTIC_ERROR,
            USK_DIAG_IO_FAILURE, source_name, 0, 0,
            "C backend %s: %s", operation,
            description ? description : "unknown I/O error");
}

static bool copy_staged_source(FILE *staging, FILE *destination) {
    unsigned char buffer[16384];
    for (;;) {
        size_t count = fread(buffer, 1, sizeof(buffer), staging);
        if (count && fwrite(buffer, 1, count, destination) != count)
            return false;
        if (count < sizeof(buffer)) {
            if (ferror(staging)) return false;
            if (feof(staging)) break;
        }
    }
    return fflush(destination) == 0;
}

/* This front door is intentionally separate from AST emission: it defines
 * stream ownership, stages generation, and commits bytes to one destination. */
UskCBackendResult usk_c_backend_emit_file(
    const UskAstProgram *program, const char *source_name,
    const char *output_path, const UskCBackendOptions *options,
    UskDiagnosticList *diagnostics) {
    UskCBackendResult result = {0};
    const char *reported_source = source_name ? source_name : "<source>";
    if (!program || !diagnostics) {
        result.success = false;
        return result;
    }

    FILE *staging = tmpfile();
    if (!staging) {
        report_file_error(diagnostics, reported_source,
                          "cannot create temporary output", NULL);
        return result;
    }

    result = usk_c_backend_emit_program(program, reported_source, staging,
                                        options, diagnostics);
    if (!result.success) {
        if (fclose(staging) != 0)
            report_file_error(diagnostics, reported_source,
                              "cannot close temporary output", NULL);
        result.success = false;
        return result;
    }

    if (fflush(staging) != 0 || fseek(staging, 0, SEEK_SET) != 0) {
        report_file_error(diagnostics, reported_source,
                          "cannot rewind temporary output", NULL);
        fclose(staging);
        result.success = false;
        return result;
    }

    FILE *destination = stdout;
    bool close_destination = false;
    if (output_path) {
        destination = fopen(output_path, "wb");
        if (!destination) {
            report_file_error(diagnostics, reported_source,
                              "cannot open destination", output_path);
            fclose(staging);
            result.success = false;
            return result;
        }
        close_destination = true;
    }

    bool copied = copy_staged_source(staging, destination);
    if (!copied)
        report_file_error(diagnostics, reported_source,
                          "cannot copy generated source", output_path);
    if (close_destination && fclose(destination) != 0) {
        report_file_error(diagnostics, reported_source,
                          "cannot close destination", output_path);
        copied = false;
    }
    if (fclose(staging) != 0) {
        report_file_error(diagnostics, reported_source,
                          "cannot close temporary output", NULL);
        copied = false;
    }
    result.success = copied;
    return result;
}
