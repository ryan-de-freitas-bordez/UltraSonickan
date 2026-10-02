#include "usk/filesystem.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define usk_make_directory(path) _mkdir(path)
#define usk_get_current_directory(buffer, size) _getcwd(buffer, (int)(size))
#define usk_mode_is_directory(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#else
#include <unistd.h>
#define usk_make_directory(path) mkdir((path), 0777)
#define usk_get_current_directory(buffer, size) getcwd(buffer, (size))
#define usk_mode_is_directory(mode) S_ISDIR(mode)
#endif

static const UskFsBuiltinInfo builtins[] = {
    {"fs::exists", 1, USK_FS_RESULT_BOOLEAN, {USK_FS_ARGUMENT_TEXT}},
    {"fs::is_file", 1, USK_FS_RESULT_BOOLEAN, {USK_FS_ARGUMENT_TEXT}},
    {"fs::is_directory", 1, USK_FS_RESULT_BOOLEAN, {USK_FS_ARGUMENT_TEXT}},
    {"fs::file_size", 1, USK_FS_RESULT_INTEGER, {USK_FS_ARGUMENT_TEXT}},
    {"fs::read_text", 1, USK_FS_RESULT_TEXT, {USK_FS_ARGUMENT_TEXT}},
    {"fs::read_lines", 1, USK_FS_RESULT_ARRAY, {USK_FS_ARGUMENT_TEXT}},
    {"fs::write_text", 2, USK_FS_RESULT_BOOLEAN,
        {USK_FS_ARGUMENT_TEXT, USK_FS_ARGUMENT_TEXT}},
    {"fs::append_text", 2, USK_FS_RESULT_BOOLEAN,
        {USK_FS_ARGUMENT_TEXT, USK_FS_ARGUMENT_TEXT}},
    {"fs::write_lines", 2, USK_FS_RESULT_BOOLEAN,
        {USK_FS_ARGUMENT_TEXT, USK_FS_ARGUMENT_ARRAY}},
    {"fs::remove_file", 1, USK_FS_RESULT_BOOLEAN, {USK_FS_ARGUMENT_TEXT}},
    {"fs::create_directory", 1, USK_FS_RESULT_BOOLEAN,
        {USK_FS_ARGUMENT_TEXT}},
    {"fs::current_directory", 0, USK_FS_RESULT_TEXT, {0}},
    {"fs::copy_file", 2, USK_FS_RESULT_BOOLEAN,
        {USK_FS_ARGUMENT_TEXT, USK_FS_ARGUMENT_TEXT}},
    {"fs::move_file", 2, USK_FS_RESULT_BOOLEAN,
        {USK_FS_ARGUMENT_TEXT, USK_FS_ARGUMENT_TEXT}}
};

size_t usk_fs_builtin_count(void) {
    return sizeof(builtins) / sizeof(builtins[0]);
}

const UskFsBuiltinInfo *usk_fs_builtin_at(size_t index) {
    return index < usk_fs_builtin_count() ? &builtins[index] : NULL;
}

const UskFsBuiltinInfo *usk_fs_builtin_find(const char *name) {
    if (!name) return NULL;
    for (size_t index = 0; index < usk_fs_builtin_count(); ++index)
        if (!strcmp(name, builtins[index].name)) return &builtins[index];
    return NULL;
}

bool usk_fs_builtin_is_name(const char *name) {
    return usk_fs_builtin_find(name) != NULL;
}

static const char *text_at(const Value *arguments, size_t index) {
    return arguments[index].as.s ? arguments[index].as.s : "";
}

static UskFsStatus validate_call(const UskFsBuiltinInfo *info,
    const Value *arguments, size_t count) {
    if (count != info->argument_count) return USK_FS_ARGUMENT_COUNT;
    if (count && !arguments) return USK_FS_TYPE_MISMATCH;
    for (size_t index = 0; index < count; ++index) {
        if (info->arguments[index] == USK_FS_ARGUMENT_TEXT &&
            arguments[index].kind != V_STRING)
            return USK_FS_TYPE_MISMATCH;
        if (info->arguments[index] == USK_FS_ARGUMENT_ARRAY &&
            arguments[index].kind != V_ARRAY)
            return USK_FS_TYPE_MISMATCH;
    }
    return USK_FS_OK;
}

static bool stat_path(const char *path, struct stat *status) {
    if (!path || !path[0] || !status) return false;
    return stat(path, status) == 0;
}

static UskFsStatus file_size_value(const char *path, Value *result) {
    struct stat status;
    if (!stat_path(path, &status) || status.st_size < 0)
        return USK_FS_IO_FAILURE;
    if ((uintmax_t)status.st_size > (uintmax_t)LLONG_MAX)
        return USK_FS_RANGE_ERROR;
    *result = int_value((long long)status.st_size);
    return USK_FS_OK;
}

static UskFsStatus read_all(FILE *stream, UskValueArena *arena, Value *result) {
    if (fseek(stream, 0, SEEK_END) != 0) return USK_FS_IO_FAILURE;
    long measured = ftell(stream);
    if (measured < 0 || fseek(stream, 0, SEEK_SET) != 0)
        return USK_FS_IO_FAILURE;
    unsigned long long bytes = (unsigned long long)measured;
    if (bytes > (unsigned long long)((size_t)-1 - 1) ||
        bytes > (unsigned long long)LLONG_MAX)
        return USK_FS_RANGE_ERROR;
    size_t length = (size_t)bytes;
    char *buffer = (char *)usk_value_arena_allocate(arena, length + 1, false);
    if (!buffer) return USK_FS_ALLOCATION_FAILURE;
    size_t actual = length ? fread(buffer, 1, length, stream) : 0;
    if (actual != length && ferror(stream)) return USK_FS_IO_FAILURE;
    if (memchr(buffer, '\0', actual)) return USK_FS_RANGE_ERROR;
    buffer[actual] = '\0';
    *result = (Value){.kind = V_STRING, .as.s = buffer};
    return USK_FS_OK;
}

static UskFsStatus read_text(const char *path, UskValueArena *arena,
                             Value *result) {
    FILE *stream = fopen(path, "rb");
    if (!stream) return USK_FS_IO_FAILURE;
    UskFsStatus status = read_all(stream, arena, result);
    if (fclose(stream) != 0 && status == USK_FS_OK)
        status = USK_FS_IO_FAILURE;
    return status;
}

static UskFsStatus read_lines(const char *path, UskValueArena *arena,
                              Value *result) {
    Value text = null_value();
    UskFsStatus status = read_text(path, arena, &text);
    if (status != USK_FS_OK) return status;
    size_t length = strlen(text.as.s ? text.as.s : "");
    size_t count = 0;
    for (size_t index = 0; index < length; ++index)
        if (text.as.s[index] == '\n') count++;
    if (length && text.as.s[length - 1] != '\n') count++;
    if (length && text.as.s[length - 1] == '\n') {
        /* A trailing newline terminates the previous line; it adds no phantom
         * empty line, while an empty file maps to an empty array. */
    }
    if (count > (size_t)-1 / sizeof(Value)) return USK_FS_RANGE_ERROR;
    Value *items = count ? (Value *)usk_value_arena_allocate(arena,
        count * sizeof(*items), true) : NULL;
    if (count && !items) return USK_FS_ALLOCATION_FAILURE;
    size_t start = 0, output = 0;
    for (size_t index = 0; index <= length; ++index) {
        if (index != length && text.as.s[index] != '\n') continue;
        if (index == length && start == length) break;
        size_t end = index;
        if (end > start && text.as.s[end - 1] == '\r') end--;
        size_t line_length = end - start;
        char *line = (char *)usk_value_arena_allocate(arena,
            line_length + 1, false);
        if (!line) return USK_FS_ALLOCATION_FAILURE;
        memcpy(line, text.as.s + start, line_length);
        line[line_length] = '\0';
        items[output++] = (Value){.kind = V_STRING, .as.s = line};
        start = index + 1;
    }
    *result = array_value_in(arena, items, output);
    return arena->failed ? USK_FS_ALLOCATION_FAILURE : USK_FS_OK;
}

static UskFsStatus write_text(const char *path, const char *text,
                              const char *mode, Value *result) {
    FILE *stream = fopen(path, mode);
    if (!stream) return USK_FS_IO_FAILURE;
    size_t length = strlen(text);
    bool success = length == 0 || fwrite(text, 1, length, stream) == length;
    if (fclose(stream) != 0) success = false;
    *result = bool_value(success);
    return success ? USK_FS_OK : USK_FS_IO_FAILURE;
}

static UskFsStatus write_lines(const char *path, Value array,
                               Value *result) {
    if (!array.as.a || (array.as.a->count && !array.as.a->items))
        return USK_FS_TYPE_MISMATCH;
    for (size_t index = 0; index < array.as.a->count; ++index)
        if (array.as.a->items[index].kind != V_STRING)
            return USK_FS_TYPE_MISMATCH;
    FILE *stream = fopen(path, "wb");
    if (!stream) return USK_FS_IO_FAILURE;
    bool success = true;
    for (size_t index = 0; index < array.as.a->count && success; ++index) {
        Value line = array.as.a->items[index];
        const char *text = line.as.s ? line.as.s : "";
        size_t length = strlen(text);
        success = (!length || fwrite(text, 1, length, stream) == length) &&
                  fputc('\n', stream) != EOF;
    }
    if (fclose(stream) != 0) success = false;
    *result = bool_value(success);
    return success ? USK_FS_OK : USK_FS_IO_FAILURE;
}

static UskFsStatus copy_file(const char *source, const char *destination,
                             Value *result) {
    if (!strcmp(source, destination)) {
        *result = bool_value(true);
        return USK_FS_OK;
    }
    FILE *input = fopen(source, "rb");
    if (!input) return USK_FS_IO_FAILURE;
    FILE *output = fopen(destination, "wb");
    if (!output) { fclose(input); return USK_FS_IO_FAILURE; }
    unsigned char buffer[65536];
    bool success = true;
    while (success) {
        size_t count = fread(buffer, 1, sizeof(buffer), input);
        if (count && fwrite(buffer, 1, count, output) != count) success = false;
        if (count < sizeof(buffer)) {
            if (ferror(input)) success = false;
            break;
        }
    }
    if (fclose(input) != 0) success = false;
    if (fclose(output) != 0) success = false;
    *result = bool_value(success);
    return success ? USK_FS_OK : USK_FS_IO_FAILURE;
}

static UskFsStatus current_directory(UskValueArena *arena, Value *result) {
    size_t capacity = 256;
    while (capacity <= ((size_t)1 << 20)) {
        char *buffer = (char *)malloc(capacity);
        if (!buffer) return USK_FS_ALLOCATION_FAILURE;
        if (usk_get_current_directory(buffer, capacity)) {
            *result = string_value_in(arena, buffer);
            free(buffer);
            return arena->failed ? USK_FS_ALLOCATION_FAILURE : USK_FS_OK;
        }
        int error = errno;
        free(buffer);
        if (error != ERANGE) return USK_FS_IO_FAILURE;
        capacity *= 2;
    }
    return USK_FS_RANGE_ERROR;
}

static UskFsStatus create_directory(const char *path, Value *result) {
    if (usk_make_directory(path) == 0) {
        *result = bool_value(true);
        return USK_FS_OK;
    }
    struct stat status;
    if (errno == EEXIST && stat_path(path, &status) &&
        usk_mode_is_directory(status.st_mode)) {
        *result = bool_value(true);
        return USK_FS_OK;
    }
    return USK_FS_IO_FAILURE;
}

UskFsStatus usk_fs_builtin_call(const char *name, const Value *arguments,
    size_t argument_count, UskValueArena *arena, Value *result) {
    const UskFsBuiltinInfo *info = usk_fs_builtin_find(name);
    if (!info) return USK_FS_UNKNOWN;
    if (!result || !arena) return USK_FS_TYPE_MISMATCH;
    UskFsStatus status = validate_call(info, arguments, argument_count);
    if (status != USK_FS_OK) return status;
    *result = null_value();
    const char *first = argument_count ? text_at(arguments, 0) : "";
    const char *second = argument_count > 1 && arguments[1].kind == V_STRING
        ? text_at(arguments, 1) : "";

    if (!strcmp(name, "fs::exists") || !strcmp(name, "fs::is_file") ||
        !strcmp(name, "fs::is_directory")) {
        struct stat file_status;
        bool found = stat_path(first, &file_status);
        bool answer = found;
        if (!strcmp(name, "fs::is_file"))
            answer = found && !usk_mode_is_directory(file_status.st_mode);
        else if (!strcmp(name, "fs::is_directory"))
            answer = found && usk_mode_is_directory(file_status.st_mode);
        *result = bool_value(answer);
    } else if (!strcmp(name, "fs::file_size")) {
        return file_size_value(first, result);
    } else if (!strcmp(name, "fs::read_text")) {
        return read_text(first, arena, result);
    } else if (!strcmp(name, "fs::read_lines")) {
        return read_lines(first, arena, result);
    } else if (!strcmp(name, "fs::write_text")) {
        return write_text(first, second, "wb", result);
    } else if (!strcmp(name, "fs::append_text")) {
        return write_text(first, second, "ab", result);
    } else if (!strcmp(name, "fs::write_lines")) {
        return write_lines(first, arguments[1], result);
    } else if (!strcmp(name, "fs::remove_file")) {
        *result = bool_value(remove(first) == 0);
    } else if (!strcmp(name, "fs::create_directory")) {
        return create_directory(first, result);
    } else if (!strcmp(name, "fs::current_directory")) {
        return current_directory(arena, result);
    } else if (!strcmp(name, "fs::copy_file")) {
        return copy_file(first, second, result);
    } else if (!strcmp(name, "fs::move_file")) {
        *result = bool_value(rename(first, second) == 0);
    } else {
        return USK_FS_UNKNOWN;
    }
    return USK_FS_OK;
}

const char *usk_fs_status_name(UskFsStatus status) {
    switch (status) {
        case USK_FS_OK: return "ok";
        case USK_FS_UNKNOWN: return "not a filesystem built-in";
        case USK_FS_ARGUMENT_COUNT: return "wrong argument count";
        case USK_FS_TYPE_MISMATCH: return "filesystem argument type mismatch";
        case USK_FS_IO_FAILURE: return "filesystem operation failed";
        case USK_FS_RANGE_ERROR: return "file size or path exceeds runtime limits";
        case USK_FS_ALLOCATION_FAILURE: return "allocation failure";
    }
    return "unknown filesystem status";
}
