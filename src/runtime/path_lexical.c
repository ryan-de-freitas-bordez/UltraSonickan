#include "usk/pathlib.h"

#include <string.h>

bool usk_path_is_separator(char character) {
    return character == '/' || character == '\\';
}

static bool ascii_letter(char character) {
    unsigned char byte = (unsigned char)character;
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

UskPathRootInfo usk_path_classify_root(const char *path) {
    UskPathRootInfo root = {USK_PATH_ROOT_RELATIVE, 0, '\0'};
    if (!path || !path[0]) return root;
    if (ascii_letter(path[0]) && path[1] == ':') {
        root.drive_letter = path[0];
        if (usk_path_is_separator(path[2])) {
            root.kind = USK_PATH_ROOT_DRIVE_ABSOLUTE;
            root.prefix_length = 3;
        } else {
            root.kind = USK_PATH_ROOT_DRIVE_RELATIVE;
            root.prefix_length = 2;
        }
        return root;
    }
    if (!usk_path_is_separator(path[0])) return root;
    if (usk_path_is_separator(path[1])) {
        root.kind = USK_PATH_ROOT_NETWORK;
        root.prefix_length = 2;
    } else {
        root.kind = USK_PATH_ROOT_POSIX;
        root.prefix_length = 1;
    }
    return root;
}

size_t usk_path_root_length(const char *path) {
    return usk_path_classify_root(path).prefix_length;
}

bool usk_path_is_absolute(const char *path) {
    UskPathRootKind kind = usk_path_classify_root(path).kind;
    return kind == USK_PATH_ROOT_POSIX || kind == USK_PATH_ROOT_NETWORK ||
           kind == USK_PATH_ROOT_DRIVE_ABSOLUTE;
}

bool usk_path_component_at(const char *path, size_t requested,
                           size_t *first, size_t *last) {
    if (!path || !first || !last) return false;
    size_t length = strlen(path), offset = usk_path_root_length(path);
    size_t index = 0;
    while (offset < length) {
        while (offset < length && usk_path_is_separator(path[offset])) offset++;
        if (offset == length) break;
        size_t start = offset;
        while (offset < length && !usk_path_is_separator(path[offset])) offset++;
        if (index++ == requested) {
            *first = start;
            *last = offset;
            return true;
        }
    }
    return false;
}

size_t usk_path_component_count(const char *path) {
    if (!path) return 0;
    size_t length = strlen(path), offset = usk_path_root_length(path);
    size_t count = 0;
    while (offset < length) {
        while (offset < length && usk_path_is_separator(path[offset])) offset++;
        if (offset == length) break;
        count++;
        while (offset < length && !usk_path_is_separator(path[offset])) offset++;
    }
    return count;
}

bool usk_path_component_bounds(const char *path, size_t *first, size_t *last) {
    if (!path || !first || !last) return false;
    size_t length = strlen(path), end = length;
    size_t root = usk_path_root_length(path);
    while (end > root && usk_path_is_separator(path[end - 1])) end--;
    if (end == root) {
        if (!root) return false;
        *first = 0;
        *last = root;
        return true;
    }
    size_t start = end;
    while (start > root && !usk_path_is_separator(path[start - 1])) start--;
    *first = start;
    *last = end;
    return true;
}

bool usk_path_directory_bounds(const char *path, size_t *first, size_t *last) {
    if (!path || !first || !last) return false;
    size_t component_first = 0, component_last = 0;
    size_t root = usk_path_root_length(path);
    if (!usk_path_component_bounds(path, &component_first, &component_last))
        return false;
    if (component_first == 0 && component_last == root) return false;
    size_t end = component_first;
    while (end > root && usk_path_is_separator(path[end - 1])) end--;
    if (end <= root) {
        if (!root) return false;
        *first = 0;
        *last = root;
        return true;
    }
    *first = 0;
    *last = end;
    return true;
}

bool usk_path_extension_bounds(const char *path, size_t *first, size_t *last) {
    size_t component_first = 0, component_last = 0;
    if (!path || !first || !last ||
        !usk_path_component_bounds(path, &component_first, &component_last))
        return false;
    size_t dot = component_last;
    while (dot > component_first && path[dot - 1] != '.') dot--;
    if (dot == component_first || path[dot - 1] != '.' ||
        dot == component_last || dot - 1 == component_first)
        return false;
    *first = dot;
    *last = component_last;
    return true;
}

bool usk_path_is_dotfile(const char *path) {
    size_t first = 0, last = 0;
    if (!usk_path_component_bounds(path, &first, &last)) return false;
    size_t length = last - first;
    if (length < 2 || path[first] != '.') return false;
    return !(length == 1 ||
             (length == 2 && path[first + 1] == '.'));
}

bool usk_path_has_extension(const char *path, const char *extension) {
    if (!path || !extension || !extension[0]) return false;
    size_t first = 0, last = 0;
    if (!usk_path_extension_bounds(path, &first, &last)) return false;
    if (extension[0] == '.') {
        size_t dot = first - 1;
        size_t wanted = strlen(extension);
        return last - dot == wanted && memcmp(path + dot, extension, wanted) == 0;
    }
    size_t wanted = strlen(extension);
    return last - first == wanted && memcmp(path + first, extension, wanted) == 0;
}
