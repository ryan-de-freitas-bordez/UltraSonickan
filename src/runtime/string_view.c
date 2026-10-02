#include "usk/stringlib.h"

#include <string.h>

UskStringView usk_string_view_from_bytes(const char *data, size_t length) {
    return (UskStringView){data, length};
}

UskStringView usk_string_view_from_cstr(const char *text) {
    if (!text) return (UskStringView){NULL, 0};
    return (UskStringView){text, strlen(text)};
}

bool usk_string_view_is_valid(UskStringView view) {
    return view.length == 0 || view.data != NULL;
}

bool usk_string_view_is_ascii(UskStringView view) {
    if (!usk_string_view_is_valid(view)) return false;
    for (size_t index = 0; index < view.length; ++index)
        if ((unsigned char)view.data[index] >= 0x80) return false;
    return true;
}

bool usk_string_view_equal(UskStringView left, UskStringView right) {
    if (!usk_string_view_is_valid(left) || !usk_string_view_is_valid(right) ||
        left.length != right.length) return false;
    return !left.length || memcmp(left.data, right.data, left.length) == 0;
}

int usk_string_view_compare(UskStringView left, UskStringView right) {
    if (!usk_string_view_is_valid(left))
        return usk_string_view_is_valid(right) ? -1 : 0;
    if (!usk_string_view_is_valid(right)) return 1;
    size_t shared = left.length < right.length ? left.length : right.length;
    int comparison = shared ? memcmp(left.data, right.data, shared) : 0;
    if (comparison) return comparison < 0 ? -1 : 1;
    return left.length < right.length ? -1 : left.length > right.length ? 1 : 0;
}

bool usk_string_view_starts_with(UskStringView text, UskStringView prefix) {
    if (!usk_string_view_is_valid(text) ||
        !usk_string_view_is_valid(prefix) || prefix.length > text.length)
        return false;
    return !prefix.length || memcmp(text.data, prefix.data, prefix.length) == 0;
}

bool usk_string_view_ends_with(UskStringView text, UskStringView suffix) {
    if (!usk_string_view_is_valid(text) ||
        !usk_string_view_is_valid(suffix) || suffix.length > text.length)
        return false;
    return !suffix.length || memcmp(text.data + text.length - suffix.length,
                                    suffix.data, suffix.length) == 0;
}

bool usk_string_view_find(UskStringView text, UskStringView needle,
                          size_t start, size_t *match_offset) {
    if (!usk_string_view_is_valid(text) ||
        !usk_string_view_is_valid(needle) || !match_offset ||
        start > text.length || needle.length > text.length - start)
        return false;
    size_t last = text.length - needle.length;
    for (size_t index = start; index <= last; ++index) {
        if (!needle.length ||
            memcmp(text.data + index, needle.data, needle.length) == 0) {
            *match_offset = index;
            return true;
        }
    }
    return false;
}

bool usk_string_view_contains(UskStringView text, UskStringView needle) {
    size_t offset = 0;
    return usk_string_view_find(text, needle, 0, &offset);
}

bool usk_string_view_slice(UskStringView text, size_t start, size_t length,
                           UskStringView *slice) {
    if (!slice || !usk_string_view_is_valid(text) || start > text.length ||
        length > text.length - start) return false;
    slice->data = text.data ? text.data + start : NULL;
    slice->length = length;
    return true;
}

static bool decode_utf8(const char *data, size_t length, size_t offset,
                        uint32_t *codepoint, size_t *byte_width) {
    if (!data || offset >= length || !codepoint || !byte_width) return false;
    const unsigned char first = (unsigned char)data[offset];
    if (first < 0x80) {
        *codepoint = first;
        *byte_width = 1;
        return true;
    }

    size_t width;
    uint32_t value;
    uint32_t minimum;
    if (first >= 0xc2 && first <= 0xdf) {
        width = 2;
        value = first & 0x1f;
        minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
        width = 3;
        value = first & 0x0f;
        minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
        width = 4;
        value = first & 0x07;
        minimum = 0x10000;
    } else {
        return false;
    }
    if (width > length - offset) return false;
    for (size_t index = 1; index < width; ++index) {
        unsigned char continuation = (unsigned char)data[offset + index];
        if ((continuation & 0xc0) != 0x80) return false;
        value = (value << 6) | (continuation & 0x3f);
    }
    if (value < minimum || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff)) return false;
    *codepoint = value;
    *byte_width = width;
    return true;
}

bool usk_string_view_utf8_valid(UskStringView text) {
    if (!usk_string_view_is_valid(text)) return false;
    for (size_t offset = 0; offset < text.length;) {
        uint32_t codepoint;
        size_t width;
        if (!decode_utf8(text.data, text.length, offset, &codepoint, &width))
            return false;
        offset += width;
    }
    return true;
}

bool usk_string_view_utf8_count(UskStringView text, size_t *codepoint_count) {
    if (!codepoint_count || !usk_string_view_is_valid(text)) return false;
    size_t count = 0;
    for (size_t offset = 0; offset < text.length;) {
        uint32_t codepoint;
        size_t width;
        if (!decode_utf8(text.data, text.length, offset, &codepoint, &width))
            return false;
        if (count == (size_t)-1) return false;
        count++;
        offset += width;
    }
    *codepoint_count = count;
    return true;
}

bool usk_string_view_utf8_offset(UskStringView text, size_t codepoint_index,
                                 size_t *byte_offset) {
    if (!byte_offset || !usk_string_view_is_valid(text)) return false;
    size_t offset = 0, index = 0;
    while (index < codepoint_index && offset < text.length) {
        uint32_t codepoint;
        size_t width;
        if (!decode_utf8(text.data, text.length, offset, &codepoint, &width))
            return false;
        offset += width;
        index++;
    }
    if (index != codepoint_index) return false;
    *byte_offset = offset;
    return true;
}

bool usk_string_view_utf8_at(UskStringView text, size_t codepoint_index,
                             uint32_t *codepoint) {
    if (!codepoint || !usk_string_view_is_valid(text)) return false;
    size_t offset = 0;
    if (!usk_string_view_utf8_offset(text, codepoint_index, &offset))
        return false;
    size_t width;
    return decode_utf8(text.data, text.length, offset, codepoint, &width);
}

bool usk_string_view_utf8_slice(UskStringView text, size_t start,
                                size_t count, UskStringView *slice) {
    if (!slice || count > (size_t)-1 - start ||
        !usk_string_view_utf8_valid(text)) return false;
    size_t first = 0, last = 0;
    if (!usk_string_view_utf8_offset(text, start, &first) ||
        !usk_string_view_utf8_offset(text, start + count, &last)) return false;
    return usk_string_view_slice(text, first, last - first, slice);
}
