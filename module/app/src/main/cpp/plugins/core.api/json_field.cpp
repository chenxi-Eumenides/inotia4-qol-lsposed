#include "json_field.h"

#include <cctype>

namespace qol::features::core_api {
namespace {

void skip_ws(const std::string& text, size_t* cursor) {
    while (*cursor < text.size() &&
           std::isspace(static_cast<unsigned char>(text[*cursor])) != 0) {
        ++*cursor;
    }
}

bool parse_string(const std::string& text, size_t* cursor, std::string* out) {
    if (*cursor >= text.size() || text[*cursor] != '"') return false;
    ++*cursor;
    out->clear();
    while (*cursor < text.size()) {
        const char ch = text[*cursor];
        if (ch == '"') {
            ++*cursor;
            return true;
        }
        if (ch == '\\') {
            ++*cursor;
            if (*cursor >= text.size()) return false;
            switch (text[*cursor]) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u':
                    // id/action 契约上为 ASCII；\u 序列按 4 位十六进制字面量跳过，不解释码点。
                    if (*cursor + 4 >= text.size()) return false;
                    out->append(text, *cursor + 1, 4);
                    *cursor += 4;
                    break;
                default: return false;
            }
            ++*cursor;
            continue;
        }
        if (static_cast<unsigned char>(ch) < 0x20) return false;  // 未转义控制字符非法
        out->push_back(ch);
        ++*cursor;
    }
    return false;
}

bool skip_value(const std::string& text, size_t* cursor);

bool skip_object(const std::string& text, size_t* cursor) {
    ++*cursor;  // '{'
    skip_ws(text, cursor);
    if (*cursor < text.size() && text[*cursor] == '}') {
        ++*cursor;
        return true;
    }
    while (true) {
        skip_ws(text, cursor);
        std::string key;
        if (!parse_string(text, cursor, &key)) return false;
        skip_ws(text, cursor);
        if (*cursor >= text.size() || text[*cursor] != ':') return false;
        ++*cursor;
        skip_ws(text, cursor);
        if (!skip_value(text, cursor)) return false;
        skip_ws(text, cursor);
        if (*cursor >= text.size()) return false;
        if (text[*cursor] == ',') {
            ++*cursor;
            continue;
        }
        if (text[*cursor] == '}') {
            ++*cursor;
            return true;
        }
        return false;
    }
}

bool skip_array(const std::string& text, size_t* cursor) {
    ++*cursor;  // '['
    skip_ws(text, cursor);
    if (*cursor < text.size() && text[*cursor] == ']') {
        ++*cursor;
        return true;
    }
    while (true) {
        skip_ws(text, cursor);
        if (!skip_value(text, cursor)) return false;
        skip_ws(text, cursor);
        if (*cursor >= text.size()) return false;
        if (text[*cursor] == ',') {
            ++*cursor;
            continue;
        }
        if (text[*cursor] == ']') {
            ++*cursor;
            return true;
        }
        return false;
    }
}

bool skip_value(const std::string& text, size_t* cursor) {
    skip_ws(text, cursor);
    if (*cursor >= text.size()) return false;
    const char ch = text[*cursor];
    if (ch == '"') {
        std::string ignored;
        return parse_string(text, cursor, &ignored);
    }
    if (ch == '{') return skip_object(text, cursor);
    if (ch == '[') return skip_array(text, cursor);
    // number/true/false/null：按标量记号跳过；空记号视为非法。
    const size_t start = *cursor;
    while (*cursor < text.size()) {
        const char c = text[*cursor];
        if (c == ',' || c == '}' || c == ']' ||
            std::isspace(static_cast<unsigned char>(c)) != 0) {
            break;
        }
        ++*cursor;
    }
    return *cursor > start;
}

}  // namespace

bool extract_string_fields(const uint8_t* body, uint32_t body_len, const char* key_a,
                           std::string* out_a, bool* found_a, const char* key_b,
                           std::string* out_b, bool* found_b) {
    if (body == nullptr || key_a == nullptr || key_b == nullptr || out_a == nullptr ||
        out_b == nullptr || found_a == nullptr || found_b == nullptr) {
        return false;
    }
    const std::string text(reinterpret_cast<const char*>(body), body_len);
    size_t cursor = 0;
    skip_ws(text, &cursor);
    if (cursor >= text.size() || text[cursor] != '{') return false;
    ++cursor;
    skip_ws(text, &cursor);
    if (cursor < text.size() && text[cursor] == '}') {
        ++cursor;
    } else {
        while (true) {
            skip_ws(text, &cursor);
            std::string key;
            if (!parse_string(text, &cursor, &key)) return false;
            skip_ws(text, &cursor);
            if (cursor >= text.size() || text[cursor] != ':') return false;
            ++cursor;
            skip_ws(text, &cursor);
            const bool is_a = key == key_a;
            const bool is_b = key == key_b;
            if (is_a || is_b) {
                std::string value;
                if (cursor < text.size() && text[cursor] == '"' &&
                    parse_string(text, &cursor, &value)) {
                    // 重复键后者覆盖前者，与 JSONObject 语义一致。
                    if (is_a) {
                        *out_a = value;
                        *found_a = true;
                    }
                    if (is_b) {
                        *out_b = value;
                        *found_b = true;
                    }
                } else if (!skip_value(text, &cursor)) {
                    return false;
                }
            } else if (!skip_value(text, &cursor)) {
                return false;
            }
            skip_ws(text, &cursor);
            if (cursor >= text.size()) return false;
            if (text[cursor] == ',') {
                ++cursor;
                continue;
            }
            if (text[cursor] == '}') {
                ++cursor;
                break;
            }
            return false;
        }
    }
    skip_ws(text, &cursor);
    return cursor == text.size();  // 尾部还有非空白内容不是合法 JSON 对象
}

}  // namespace qol::features::core_api
