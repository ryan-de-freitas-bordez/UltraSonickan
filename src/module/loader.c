#define _CRT_SECURE_NO_WARNINGS
#include "usk/loader.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} PathSet;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} TextBuffer;

typedef struct {
    PathSet visited;
    char *error;
    size_t error_capacity;
    unsigned depth;
    size_t source_bytes;
    const UskLoaderOptions *options;
    UskLoaderReport *report;
    UskLoaderStatus status;
} LoadContext;

static char *copy_range(const char *text, size_t length) {
    char *copy = (char *)malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static int buffer_append(TextBuffer *buffer, const char *text, size_t length) {
    if (length > (size_t)-1 - buffer->length - 1) return 0;
    size_t needed = buffer->length + length + 1;
    if (needed > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 1024;
        while (capacity < needed) {
            if (capacity > ((size_t)-1) / 2) return 0;
            capacity *= 2;
        }
        char *data = (char *)realloc(buffer->data, capacity);
        if (!data) return 0;
        buffer->data = data;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return 1;
}

static char *read_text_file(const char *path, size_t maximum_bytes,
                            bool *over_limit, size_t *length) {
    if (over_limit) *over_limit = false;
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long file_length = ftell(file);
    if (file_length < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }
    if (maximum_bytes && (unsigned long long)file_length >
                         (unsigned long long)maximum_bytes) {
        if (over_limit) *over_limit = true;
        fclose(file);
        return NULL;
    }
    char *data = (char *)malloc((size_t)file_length + 1);
    if (!data) { fclose(file); return NULL; }
    size_t bytes_read = fread(data, 1, (size_t)file_length, file);
    if (ferror(file)) { free(data); fclose(file); return NULL; }
    fclose(file);
    data[bytes_read] = '\0';
    *length = bytes_read;
    return data;
}

static int path_set_contains(const PathSet *set, const char *path) {
    for (size_t i = 0; i < set->count; ++i)
        if (strcmp(set->items[i], path) == 0) return 1;
    return 0;
}

static int path_set_add(PathSet *set, const char *path) {
    if (path_set_contains(set, path)) return 1;
    if (set->count == set->capacity) {
        if (set->capacity > (size_t)-1 / 2 / sizeof(*set->items)) return 0;
        size_t capacity = set->capacity ? set->capacity * 2 : 8;
        char **items = (char **)realloc(set->items, capacity * sizeof(*items));
        if (!items) return 0;
        set->items = items;
        set->capacity = capacity;
    }
    set->items[set->count] = copy_range(path, strlen(path));
    if (!set->items[set->count]) return 0;
    set->count++;
    return 1;
}

static char *path_directory(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    return slash ? copy_range(path, (size_t)(slash - path)) : copy_range(".", 1);
}

static char *join_path(const char *directory, const char *name) {
    size_t directory_length = strlen(directory), name_length = strlen(name);
    int has_separator = directory_length &&
        (directory[directory_length - 1] == '/' || directory[directory_length - 1] == '\\');
    size_t separator_length = has_separator ? 0 : 1;
    if (name_length > (size_t)-1 - separator_length - 1 ||
        directory_length > (size_t)-1 - separator_length - name_length - 1)
        return NULL;
    size_t total = directory_length + separator_length + name_length;
    char *path = (char *)malloc(total + 1);
    if (!path) return NULL;
    memcpy(path, directory, directory_length);
    size_t offset = directory_length;
    if (!has_separator) path[offset++] = '/';
    memcpy(path + offset, name, name_length + 1);
    return path;
}

static int file_readable(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    fclose(file);
    return 1;
}

static char *resolve_import(const char *importer, const char *requested,
                            const UskLoaderOptions *options) {
    if (requested[0] == '/' || requested[0] == '\\' ||
        (strlen(requested) > 1 && requested[1] == ':'))
        return file_readable(requested) ? copy_range(requested, strlen(requested)) : NULL;

    char *directory = path_directory(importer);
    if (!directory) return NULL;
    char *local_path = join_path(directory, requested);
    free(directory);
    if (local_path && file_readable(local_path)) return local_path;
    free(local_path);

    if (options && options->search_paths) {
        for (size_t index = 0; index < options->search_path_count; ++index) {
            const char *directory = options->search_paths[index];
            if (!directory || !directory[0]) continue;
            char *candidate = join_path(directory, requested);
            if (candidate && file_readable(candidate)) return candidate;
            free(candidate);
        }
    }

    char *stdlib_path = join_path("stdlib", requested);
    if (stdlib_path && file_readable(stdlib_path)) return stdlib_path;
    free(stdlib_path);
    return NULL;
}

static void set_error(LoadContext *context, UskLoaderStatus status,
                      const char *format, const char *detail) {
    context->status = status;
    if (context->error_capacity)
        snprintf(context->error, context->error_capacity, format, detail);
}

static int parse_import_line(const char *line, size_t length,
                             size_t *directive_length, char **requested_path) {
    size_t i = 0;
    while (i < length && (line[i] == ' ' || line[i] == '\t')) i++;
    if (length - i < 6 || memcmp(line + i, "import", 6) != 0) return 0;
    if (i + 6 < length && line[i + 6] != ' ' && line[i + 6] != '\t') return 0;
    i += 6;
    while (i < length && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= length || (line[i] != '<' && line[i] != '"' && line[i] != '\'')) return 0;
    char close = line[i] == '<' ? '>' : line[i];
    size_t start = ++i;
    while (i < length && line[i] != close) i++;
    if (i == length || i == start) return 0;
    *requested_path = copy_range(line + start, i - start);
    if (!*requested_path) return -1;
    i++;
    while (i < length && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) i++;
    if (i < length && line[i] == ';') i++;
    while (i < length && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) i++;
    if (i < length && line[i] != '\n' && line[i] != '/' && !(line[i] == '\\' && i + 1 < length && line[i+1] == '\\')) {
        free(*requested_path);
        *requested_path = NULL;
        return 0;
    }
    *directive_length = i;
    return 1;
}

static int load_recursive(LoadContext *context, const char *path, TextBuffer *output) {
    unsigned maximum_depth = context->options->maximum_import_depth;
    if (context->depth >= maximum_depth) {
        context->status = USK_LOADER_DEPTH_LIMIT;
        if (context->error_capacity)
            snprintf(context->error, context->error_capacity,
                     "import nesting exceeds %u files near '%s'",
                     maximum_depth, path);
        return 0;
    }
    if (path_set_contains(&context->visited, path)) return 1;
    if (!path_set_add(&context->visited, path)) {
        set_error(context, USK_LOADER_ALLOCATION_FAILURE,
                  "out of memory loading '%s'", path);
        return 0;
    }
    size_t source_length = 0;
    size_t remaining = 0;
    if (context->options->maximum_source_bytes) {
        if (context->source_bytes >= context->options->maximum_source_bytes) {
            context->status = USK_LOADER_SOURCE_LIMIT;
            if (context->error_capacity)
                snprintf(context->error, context->error_capacity,
                         "source graph exceeds the configured %zu-byte limit",
                         context->options->maximum_source_bytes);
            return 0;
        }
        remaining = context->options->maximum_source_bytes -
                    context->source_bytes;
    }
    bool over_limit = false;
    char *source = read_text_file(path, remaining, &over_limit, &source_length);
    if (!source) {
        UskLoaderStatus status = over_limit
            ? USK_LOADER_SOURCE_LIMIT : USK_LOADER_IO_FAILURE;
        if (over_limit) {
            context->status = status;
            if (context->error_capacity)
                snprintf(context->error, context->error_capacity,
                         "source graph exceeds the configured %zu-byte limit",
                         context->options->maximum_source_bytes);
        } else {
            set_error(context, status, "cannot read source file '%s'", path);
        }
        return 0;
    }

    if (source_length > (size_t)-1 - context->source_bytes) {
        free(source);
        context->status = USK_LOADER_SOURCE_LIMIT;
        if (context->error_capacity)
            snprintf(context->error, context->error_capacity,
                     "combined source size exceeds the platform limit");
        return 0;
    }
    context->source_bytes += source_length;
    if (context->report) {
        context->report->files_loaded++;
        context->report->source_bytes = context->source_bytes;
    }

    context->depth++;
    if (context->report && context->depth >
            context->report->maximum_depth_reached)
        context->report->maximum_depth_reached = context->depth;
    size_t position = 0;
    while (position < source_length) {
        size_t line_start = position;
        while (position < source_length && source[position] != '\n') position++;
        size_t line_length = position - line_start;
        size_t full_line_length = line_length + (position < source_length ? 1 : 0);
        size_t directive_length = 0;
        char *requested = NULL;
        int import_status = parse_import_line(source + line_start, full_line_length,
                                              &directive_length, &requested);
        if (import_status < 0) {
            set_error(context, USK_LOADER_ALLOCATION_FAILURE,
                      "out of memory parsing imports in '%s'", path);
            free(source); context->depth--; return 0;
        }
        if (import_status) {
            char *resolved = resolve_import(path, requested,
                                             context->options);
            if (!resolved) {
                context->status = USK_LOADER_IMPORT_NOT_FOUND;
                if (context->error_capacity)
                    snprintf(context->error, context->error_capacity,
                             "cannot resolve import '%s' from '%s'", requested, path);
                free(requested); free(source); context->depth--; return 0;
            }
            if (!load_recursive(context, resolved, output)) {
                free(resolved); free(requested); free(source); context->depth--; return 0;
            }
            if (context->report) context->report->imports_expanded++;
            free(resolved);
            free(requested);
            if (directive_length < full_line_length &&
                !buffer_append(output, source + line_start + directive_length,
                               full_line_length - directive_length)) {
                set_error(context, USK_LOADER_ALLOCATION_FAILURE,
                          "out of memory assembling '%s'", path);
                free(source); context->depth--; return 0;
            }
        } else if (!buffer_append(output, source + line_start, full_line_length)) {
            set_error(context, USK_LOADER_ALLOCATION_FAILURE,
                      "out of memory assembling '%s'", path);
            free(source); context->depth--; return 0;
        }
        if (position < source_length) position++;
    }
    free(source);
    context->depth--;
    return 1;
}

UskLoaderOptions usk_loader_default_options(void) {
    return (UskLoaderOptions){
        .search_paths = NULL,
        .search_path_count = 0,
        .maximum_import_depth = 128,
        .maximum_source_bytes = 0
    };
}

static void destroy_path_set(PathSet *paths) {
    if (!paths) return;
    for (size_t index = 0; index < paths->count; ++index)
        free(paths->items[index]);
    free(paths->items);
    memset(paths, 0, sizeof(*paths));
}

UskLoadedProgram usk_load_program_ex(const char *entry_path,
                                     const UskLoaderOptions *options) {
    UskLoadedProgram result = {0};
    result.status = USK_LOADER_INVALID_ARGUMENT;
    if (!entry_path || !entry_path[0]) {
        snprintf(result.error, sizeof(result.error),
                 "an entry source path is required");
        return result;
    }
    UskLoaderOptions effective = options
        ? *options : usk_loader_default_options();
    if (effective.search_path_count && !effective.search_paths) {
        snprintf(result.error, sizeof(result.error),
                 "search_path_count requires a search_paths array");
        return result;
    }
    if (!effective.maximum_import_depth)
        effective.maximum_import_depth =
            usk_loader_default_options().maximum_import_depth;

    LoadContext context = {
        .error = result.error,
        .error_capacity = sizeof(result.error),
        .options = &effective,
        .report = &result.report,
        .status = USK_LOADER_OK
    };
    TextBuffer output = {0};
    if (!load_recursive(&context, entry_path, &output)) {
        result.status = context.status;
        destroy_path_set(&context.visited);
        free(output.data);
        return result;
    }
    destroy_path_set(&context.visited);
    if (!output.data) {
        output.data = (char *)calloc(1, 1);
        if (!output.data) {
            result.status = USK_LOADER_ALLOCATION_FAILURE;
            snprintf(result.error, sizeof(result.error),
                     "out of memory creating empty source buffer");
            return result;
        }
    }
    result.source = output.data;
    result.source_length = output.length;
    result.status = USK_LOADER_OK;
    return result;
}

void usk_loaded_program_destroy(UskLoadedProgram *program) {
    if (!program) return;
    free(program->source);
    memset(program, 0, sizeof(*program));
}

const char *usk_loader_status_name(UskLoaderStatus status) {
    switch (status) {
        case USK_LOADER_OK: return "ok";
        case USK_LOADER_INVALID_ARGUMENT: return "invalid loader arguments";
        case USK_LOADER_IO_FAILURE: return "source file could not be read";
        case USK_LOADER_IMPORT_NOT_FOUND: return "import path could not be resolved";
        case USK_LOADER_DEPTH_LIMIT: return "import depth limit exceeded";
        case USK_LOADER_SOURCE_LIMIT: return "source byte limit exceeded";
        case USK_LOADER_ALLOCATION_FAILURE: return "loader allocation failed";
    }
    return "unknown loader status";
}

char *usk_load_program(const char *entry_path, char *error,
                       size_t error_capacity) {
    if (error_capacity && error) error[0] = '\0';
    UskLoadedProgram program = usk_load_program_ex(entry_path, NULL);
    if (program.status != USK_LOADER_OK) {
        if (error_capacity && error)
            snprintf(error, error_capacity, "%s",
                     program.error[0] ? program.error
                     : usk_loader_status_name(program.status));
        usk_loaded_program_destroy(&program);
        return NULL;
    }
    char *source = program.source;
    program.source = NULL;
    usk_loaded_program_destroy(&program);
    return source;
}
