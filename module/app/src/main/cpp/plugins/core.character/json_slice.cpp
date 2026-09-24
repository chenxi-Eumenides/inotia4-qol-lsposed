#include "json_slice.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

namespace qol::features::core_character {
namespace {

void skip_ws(const char* json, uint32_t size, uint32_t* cursor) {
    while (*cursor < size) {
        const char c = json[*cursor];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        ++*cursor;
    }
}

// cursor 指向字符串起始 '"'；成功后 cursor 指向闭合引号之后。
bool skip_string(const char* json, uint32_t size, uint32_t* cursor) {
    if (*cursor >= size || json[*cursor] != '"') return false;
    ++*cursor;
    while (*cursor < size) {
        const char c = json[*cursor];
        if (c == '\\') {
            *cursor += 2;
            continue;
        }
        ++*cursor;
        if (c == '"') return true;
    }
    return false;
}

bool skip_value(const char* json, uint32_t size, uint32_t* cursor);

bool skip_object(const char* json, uint32_t size, uint32_t* cursor) {
    if (*cursor >= size || json[*cursor] != '{') return false;
    ++*cursor;
    skip_ws(json, size, cursor);
    if (*cursor < size && json[*cursor] == '}') {
        ++*cursor;
        return true;
    }
    while (*cursor < size) {
        if (!skip_string(json, size, cursor)) return false;
        skip_ws(json, size, cursor);
        if (*cursor >= size || json[*cursor] != ':') return false;
        ++*cursor;
        if (!skip_value(json, size, cursor)) return false;
        skip_ws(json, size, cursor);
        if (*cursor < size && json[*cursor] == ',') {
            ++*cursor;
            skip_ws(json, size, cursor);
            continue;
        }
        if (*cursor < size && json[*cursor] == '}') {
            ++*cursor;
            return true;
        }
        return false;
    }
    return false;
}

bool skip_array(const char* json, uint32_t size, uint32_t* cursor) {
    if (*cursor >= size || json[*cursor] != '[') return false;
    ++*cursor;
    skip_ws(json, size, cursor);
    if (*cursor < size && json[*cursor] == ']') {
        ++*cursor;
        return true;
    }
    while (*cursor < size) {
        if (!skip_value(json, size, cursor)) return false;
        skip_ws(json, size, cursor);
        if (*cursor < size && json[*cursor] == ',') {
            ++*cursor;
            skip_ws(json, size, cursor);
            continue;
        }
        if (*cursor < size && json[*cursor] == ']') {
            ++*cursor;
            return true;
        }
        return false;
    }
    return false;
}

bool skip_value(const char* json, uint32_t size, uint32_t* cursor) {
    skip_ws(json, size, cursor);
    if (*cursor >= size) return false;
    const char c = json[*cursor];
    if (c == '"') return skip_string(json, size, cursor);
    if (c == '{') return skip_object(json, size, cursor);
    if (c == '[') return skip_array(json, size, cursor);
    // number / true / false / null
    while (*cursor < size) {
        const char d = json[*cursor];
        if (d == ',' || d == '}' || d == ']' || d == ':' || d == ' ' || d == '\t' ||
            d == '\n' || d == '\r') {
            break;
        }
        ++*cursor;
    }
    return true;
}

}  // namespace

bool json_member(const char* json, uint32_t size, const char* key, JsonSlice* out) {
    if (json == nullptr || key == nullptr || out == nullptr) return false;
    uint32_t cursor = 0;
    skip_ws(json, size, &cursor);
    if (cursor >= size || json[cursor] != '{') return false;
    ++cursor;
    skip_ws(json, size, &cursor);
    if (cursor < size && json[cursor] == '}') return false;
    const uint32_t key_length = static_cast<uint32_t>(std::strlen(key));
    JsonSlice last_value{};
    bool found = false;
    while (cursor < size) {
        if (json[cursor] != '"') return false;
        const uint32_t key_start = cursor + 1;
        if (!skip_string(json, size, &cursor)) return false;
        const uint32_t key_end = cursor - 1;
        skip_ws(json, size, &cursor);
        if (cursor >= size || json[cursor] != ':') return false;
        ++cursor;
        skip_ws(json, size, &cursor);
        const uint32_t value_start = cursor;
        if (!skip_value(json, size, &cursor)) return false;
        if (key_end - key_start == key_length &&
            std::strncmp(json + key_start, key, key_length) == 0) {
            last_value.data = json + value_start;
            last_value.size = cursor - value_start;
            found = true;
        }
        skip_ws(json, size, &cursor);
        if (cursor < size && json[cursor] == ',') {
            ++cursor;
            skip_ws(json, size, &cursor);
            continue;
        }
        break;
    }
    if (found) *out = last_value;
    return found;
}

bool json_element(const char* json, uint32_t size, int32_t index, JsonSlice* out) {
    if (json == nullptr || out == nullptr || index < 0) return false;
    uint32_t cursor = 0;
    skip_ws(json, size, &cursor);
    if (cursor >= size || json[cursor] != '[') return false;
    ++cursor;
    skip_ws(json, size, &cursor);
    if (cursor < size && json[cursor] == ']') return false;
    int32_t current = 0;
    while (cursor < size) {
        const uint32_t start = cursor;
        if (!skip_value(json, size, &cursor)) return false;
        if (current == index) {
            out->data = json + start;
            out->size = cursor - start;
            return true;
        }
        ++current;
        skip_ws(json, size, &cursor);
        if (cursor < size && json[cursor] == ',') {
            ++cursor;
            skip_ws(json, size, &cursor);
            continue;
        }
        break;
    }
    return false;
}

bool json_array_size(const char* json, uint32_t size, int32_t* out) {
    if (json == nullptr || out == nullptr) return false;
    uint32_t cursor = 0;
    skip_ws(json, size, &cursor);
    if (cursor >= size || json[cursor] != '[') return false;
    ++cursor;
    skip_ws(json, size, &cursor);
    int32_t count = 0;
    if (cursor < size && json[cursor] == ']') {
        *out = 0;
        return true;
    }
    while (cursor < size) {
        if (!skip_value(json, size, &cursor)) return false;
        ++count;
        skip_ws(json, size, &cursor);
        if (cursor < size && json[cursor] == ',') {
            ++cursor;
            skip_ws(json, size, &cursor);
            continue;
        }
        if (cursor < size && json[cursor] == ']') {
            *out = count;
            return true;
        }
        return false;
    }
    return false;
}

bool json_int_member(const char* json, uint32_t size, const char* key, long long* out) {
    JsonSlice value{};
    if (!json_member(json, size, key, &value) || value.size == 0) return false;
    std::string number(value.data, value.size);
    if (value.data[0] == '"') {
        if (value.size < 2 || value.data[value.size - 1] != '"') return false;
        number = std::string(value.data + 1, value.size - 2);
    }
    size_t begin = 0;
    size_t end_index = number.size();
    while (begin < end_index && static_cast<unsigned char>(number[begin]) <= 0x20) ++begin;
    while (end_index > begin && static_cast<unsigned char>(number[end_index - 1]) <= 0x20) {
        --end_index;
    }
    if (begin == end_index) return false;
    const std::string trimmed = number.substr(begin, end_index - begin);
    char* end = nullptr;
    const double parsed = std::strtod(trimmed.c_str(), &end);
    if (end == trimmed.c_str() || *end != '\0') return false;
    int32_t narrowed = 0;
    if (std::isnan(parsed)) narrowed = 0;
    else if (parsed <= static_cast<double>(std::numeric_limits<int32_t>::min())) {
        narrowed = std::numeric_limits<int32_t>::min();
    } else if (parsed >= static_cast<double>(std::numeric_limits<int32_t>::max())) {
        narrowed = std::numeric_limits<int32_t>::max();
    } else {
        narrowed = static_cast<int32_t>(parsed);
    }
    *out = narrowed;
    return true;
}

bool json_slice_is_null(const JsonSlice& slice) {
    return slice.size == 4 && std::strncmp(slice.data, "null", 4) == 0;
}

}  // namespace qol::features::core_character
