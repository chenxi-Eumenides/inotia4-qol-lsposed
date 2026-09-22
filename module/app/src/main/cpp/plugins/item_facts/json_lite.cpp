#include "json_lite.h"

#include <cstdlib>
#include <cstring>

namespace qol::features::item_facts {
namespace {

constexpr uint32_t kMaxDepth = 32;

struct Parser {
    const char* text = nullptr;
    uint32_t size = 0;
    uint32_t pos = 0;
    uint32_t depth = 0;

    void skip_ws() {
        while (pos < size) {
            const char ch = text[pos];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                ++pos;
            } else {
                break;
            }
        }
    }

    bool consume(char expected) {
        skip_ws();
        if (pos >= size || text[pos] != expected) return false;
        ++pos;
        return true;
    }

    bool parse_value(JsonValue* out) {
        if (out == nullptr || ++depth > kMaxDepth) return false;
        skip_ws();
        if (pos >= size) {
            --depth;
            return false;
        }
        const char ch = text[pos];
        bool ok = false;
        if (ch == '{') {
            ok = parse_object(out);
        } else if (ch == '[') {
            ok = parse_array(out);
        } else if (ch == '"') {
            out->type = JsonValue::Type::kString;
            ok = parse_string(&out->string);
        } else if (ch == 't' && match("true")) {
            out->type = JsonValue::Type::kBool;
            out->boolean = true;
            ok = true;
        } else if (ch == 'f' && match("false")) {
            out->type = JsonValue::Type::kBool;
            out->boolean = false;
            ok = true;
        } else if (ch == 'n' && match("null")) {
            out->type = JsonValue::Type::kNull;
            ok = true;
        } else {
            ok = parse_number(out);
        }
        --depth;
        return ok;
    }

    bool match(const char* literal) {
        const size_t length = std::strlen(literal);
        if (pos + length > size) return false;
        if (std::strncmp(text + pos, literal, length) != 0) return false;
        pos += static_cast<uint32_t>(length);
        return true;
    }

    bool parse_number(JsonValue* out) {
        const uint32_t start = pos;
        if (pos < size && (text[pos] == '-' || text[pos] == '+')) ++pos;
        bool digits = false;
        bool fractional = false;
        while (pos < size) {
            const char ch = text[pos];
            if (ch >= '0' && ch <= '9') {
                digits = true;
                ++pos;
            } else if (ch == '.' || ch == 'e' || ch == 'E' || ch == '-' || ch == '+') {
                fractional = fractional || ch == '.' || ch == 'e' || ch == 'E';
                ++pos;
            } else {
                break;
            }
        }
        if (!digits) return false;
        out->type = JsonValue::Type::kNumber;
        out->integer = std::strtoll(text + start, nullptr, 10);
        (void)fractional;  // 原始库存/商店 JSON 无小数；出现小数时按整数截断语义等价
        return true;
    }

    bool parse_string(std::string* out) {
        if (!consume('"')) return false;
        out->clear();
        while (pos < size) {
            const char ch = text[pos++];
            if (ch == '"') return true;
            if (ch == '\\' && pos < size) {
                const char escaped = text[pos++];
                switch (escaped) {
                    case 'n': out->push_back('\n'); break;
                    case 't': out->push_back('\t'); break;
                    case 'r': out->push_back('\r'); break;
                    case 'b': out->push_back('\b'); break;
                    case 'f': out->push_back('\f'); break;
                    case 'u': {
                        if (pos + 4 <= size) pos += 4;  // 原始 JSON 无转义 Unicode；丢弃码点即可
                        break;
                    }
                    default: out->push_back(escaped); break;
                }
                continue;
            }
            out->push_back(ch);
        }
        return false;
    }

    bool parse_array(JsonValue* out) {
        if (!consume('[')) return false;
        out->type = JsonValue::Type::kArray;
        skip_ws();
        if (pos < size && text[pos] == ']') {
            ++pos;
            return true;
        }
        while (true) {
            JsonValue element;
            if (!parse_value(&element)) return false;
            out->items.push_back(std::move(element));
            skip_ws();
            if (pos < size && text[pos] == ',') {
                ++pos;
                continue;
            }
            return consume(']');
        }
    }

    bool parse_object(JsonValue* out) {
        if (!consume('{')) return false;
        out->type = JsonValue::Type::kObject;
        skip_ws();
        if (pos < size && text[pos] == '}') {
            ++pos;
            return true;
        }
        while (true) {
            skip_ws();
            std::string key;
            if (!parse_string(&key)) return false;
            if (!consume(':')) return false;
            out->member_keys.push_back(std::move(key));
            out->member_values.emplace_back();
            if (!parse_value(&out->member_values.back())) return false;
            skip_ws();
            if (pos < size && text[pos] == ',') {
                ++pos;
                continue;
            }
            return consume('}');
        }
    }
};

}  // namespace

const JsonValue* JsonValue::find(const char* key) const {
    if (key == nullptr) return nullptr;
    for (size_t i = 0; i < member_keys.size(); ++i) {
        if (member_keys[i] == key) return &member_values[i];
    }
    return nullptr;
}

bool JsonValue::as_int(long long* out) const {
    if (out == nullptr || type != Type::kNumber) return false;
    *out = integer;
    return true;
}

bool JsonValue::as_bool(bool* out) const {
    if (out == nullptr || type != Type::kBool) return false;
    *out = boolean;
    return true;
}

bool json_parse(const char* text, uint32_t size, JsonValue* out) {
    if (text == nullptr || out == nullptr || size == 0) return false;
    Parser parser{text, size, 0, 0};
    parser.skip_ws();
    if (!parser.parse_value(out)) return false;
    parser.skip_ws();
    return parser.pos == size;
}

}  // namespace qol::features::item_facts
