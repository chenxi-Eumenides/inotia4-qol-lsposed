#include "json.h"

#include <cctype>
#include <cstdlib>

namespace qol::features::core_data {
namespace {

void append_utf8(uint32_t cp, std::string* out) {
    if (cp <= 0x7f) {
        out->push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ff) {
        out->push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out->push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out->push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    bool parse(JsonValue* out) {
        if (out == nullptr || !parse_value(out, 0)) return false;
        skip_space();
        return position_ == text_.size();
    }

private:
    void skip_space() {
        while (position_ < text_.size() &&
               (text_[position_] == ' ' || text_[position_] == '\t' ||
                text_[position_] == '\n' || text_[position_] == '\r')) {
            ++position_;
        }
    }

    bool parse_string(std::string* out) {
        if (position_ >= text_.size() || text_[position_++] != '"') return false;
        out->clear();
        while (position_ < text_.size()) {
            const unsigned char ch = static_cast<unsigned char>(text_[position_++]);
            if (ch == '"') return true;
            if (ch < 0x20) return false;
            if (ch != '\\') {
                out->push_back(static_cast<char>(ch));
                continue;
            }
            if (position_ >= text_.size()) return false;
            const char escaped = text_[position_++];
            switch (escaped) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    uint32_t codepoint = 0;
                    if (!parse_hex4(&codepoint)) return false;
                    if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                        if (position_ + 2 > text_.size() || text_[position_] != '\\' ||
                            text_[position_ + 1] != 'u') return false;
                        position_ += 2;
                        uint32_t low = 0;
                        if (!parse_hex4(&low) || low < 0xdc00 || low > 0xdfff) return false;
                        codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + low - 0xdc00;
                    } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                        return false;
                    }
                    append_utf8(codepoint, out);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool parse_hex4(uint32_t* out) {
        if (position_ + 4 > text_.size()) return false;
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char ch = text_[position_++];
            value <<= 4;
            if (ch >= '0' && ch <= '9') value |= static_cast<uint32_t>(ch - '0');
            else if (ch >= 'a' && ch <= 'f') value |= static_cast<uint32_t>(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') value |= static_cast<uint32_t>(ch - 'A' + 10);
            else return false;
        }
        *out = value;
        return true;
    }

    bool parse_value(JsonValue* out, uint32_t depth) {
        if (depth > 128) return false;
        skip_space();
        if (position_ >= text_.size()) return false;
        out->begin = position_;
        const char ch = text_[position_];
        if (ch == '{') return parse_object(out, depth);
        if (ch == '[') return parse_array(out, depth);
        if (ch == '"') {
            out->type = JsonValue::Type::kString;
            if (!parse_string(&out->text)) return false;
            out->end = position_;
            return true;
        }
        if (ch == 't' && consume("true")) {
            out->type = JsonValue::Type::kBoolean;
            out->text = "true";
        } else if (ch == 'f' && consume("false")) {
            out->type = JsonValue::Type::kBoolean;
            out->text = "false";
        } else if (ch == 'n' && consume("null")) {
            out->type = JsonValue::Type::kNull;
            out->text = "null";
        } else if (parse_number(out)) {
            out->end = position_;
            return true;
        } else {
            return false;
        }
        out->end = position_;
        return true;
    }

    bool parse_object(JsonValue* out, uint32_t depth) {
        out->type = JsonValue::Type::kObject;
        ++position_;
        skip_space();
        if (position_ < text_.size() && text_[position_] == '}') {
            out->end = ++position_;
            return true;
        }
        while (position_ < text_.size()) {
            std::string key;
            if (!parse_string(&key)) return false;
            skip_space();
            if (position_ >= text_.size() || text_[position_++] != ':') return false;
            JsonValue value;
            if (!parse_value(&value, depth + 1)) return false;
            out->keys.push_back(std::move(key));
            out->values.push_back(std::move(value));
            skip_space();
            if (position_ < text_.size() && text_[position_] == '}') {
                out->end = ++position_;
                return true;
            }
            if (position_ >= text_.size() || text_[position_++] != ',') return false;
            skip_space();
        }
        return false;
    }

    bool parse_array(JsonValue* out, uint32_t depth) {
        out->type = JsonValue::Type::kArray;
        ++position_;
        skip_space();
        if (position_ < text_.size() && text_[position_] == ']') {
            out->end = ++position_;
            return true;
        }
        while (position_ < text_.size()) {
            JsonValue item;
            if (!parse_value(&item, depth + 1)) return false;
            out->items.push_back(std::move(item));
            skip_space();
            if (position_ < text_.size() && text_[position_] == ']') {
                out->end = ++position_;
                return true;
            }
            if (position_ >= text_.size() || text_[position_++] != ',') return false;
            skip_space();
        }
        return false;
    }

    bool parse_number(JsonValue* out) {
        const size_t start = position_;
        if (text_[position_] == '-') ++position_;
        if (position_ >= text_.size()) return false;
        if (text_[position_] == '0') {
            ++position_;
        } else {
            if (text_[position_] < '1' || text_[position_] > '9') return false;
            while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_])))
                ++position_;
        }
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            const size_t fraction = position_;
            while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_])))
                ++position_;
            if (fraction == position_) return false;
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-'))
                ++position_;
            const size_t exponent = position_;
            while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_])))
                ++position_;
            if (exponent == position_) return false;
        }
        out->type = JsonValue::Type::kNumber;
        out->text = text_.substr(start, position_ - start);
        return true;
    }

    bool consume(const char* word) {
        size_t at = position_;
        for (const char* cursor = word; *cursor != '\0'; ++cursor, ++at) {
            if (at >= text_.size() || text_[at] != *cursor) return false;
        }
        position_ = at;
        return true;
    }

    const std::string& text_;
    size_t position_ = 0;
};

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

}  // namespace

const JsonValue* JsonValue::find(const char* key) const {
    if (type != Type::kObject || key == nullptr) return nullptr;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == key) return &values[i];
    }
    return nullptr;
}

int64_t JsonValue::opt_int(size_t index, int64_t fallback) const {
    if (type != Type::kArray || index >= items.size()) return fallback;
    const JsonValue& value = items[index];
    if (value.type != Type::kNumber) return fallback;
    char* end = nullptr;
    const long long result = std::strtoll(value.text.c_str(), &end, 10);
    return end != value.text.c_str() && end != nullptr && *end == '\0' ? result : fallback;
}

std::string JsonValue::opt_string(const char* key, const char* fallback) const {
    const JsonValue* value = find(key);
    if (value == nullptr || value->type == Type::kNull) return fallback;
    if (value->type == Type::kString || value->type == Type::kNumber ||
        value->type == Type::kBoolean) return value->text;
    return fallback;
}

bool parse_json(const std::string& json, JsonValue* out) {
    return Parser(json).parse(out);
}

std::string quote_json(const std::string& value) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (ch < 0x20) {
                    result += "\\u00";
                    result.push_back(kHex[ch >> 4]);
                    result.push_back(kHex[ch & 0x0f]);
                } else {
                    result.push_back(static_cast<char>(ch));
                }
        }
    }
    result.push_back('"');
    return result;
}

std::string stringify_json(const JsonValue& value) {
    switch (value.type) {
        case JsonValue::Type::kNull: return "null";
        case JsonValue::Type::kBoolean:
        case JsonValue::Type::kNumber: return value.text;
        case JsonValue::Type::kString: return quote_json(value.text);
        case JsonValue::Type::kArray: {
            std::string result = "[";
            for (size_t i = 0; i < value.items.size(); ++i) {
                if (i != 0) result.push_back(',');
                result += stringify_json(value.items[i]);
            }
            result.push_back(']');
            return result;
        }
        case JsonValue::Type::kObject: {
            std::string result = "{";
            for (size_t i = 0; i < value.values.size(); ++i) {
                if (i != 0) result.push_back(',');
                result += quote_json(value.keys[i]);
                result.push_back(':');
                result += stringify_json(value.values[i]);
            }
            result.push_back('}');
            return result;
        }
    }
    return "null";
}

int32_t decode_query_value(const char* query, uint32_t query_size, const char* key,
                           std::string* out) {
    if (query == nullptr || key == nullptr || out == nullptr) return 0;
    const std::string wanted(key);
    uint32_t begin = 0;
    while (begin < query_size) {
        uint32_t end = begin;
        while (end < query_size && query[end] != '&') ++end;
        uint32_t equal = begin;
        while (equal < end && query[equal] != '=') ++equal;
        if (std::string(query + begin, equal - begin) == wanted) {
            out->clear();
            for (uint32_t i = equal < end ? equal + 1 : end; i < end; ++i) {
                if (query[i] == '+') {
                    out->push_back(' ');
                } else if (query[i] == '%' && i + 2 < end) {
                    const int hi = hex_value(query[i + 1]);
                    const int lo = hex_value(query[i + 2]);
                    if (hi < 0 || lo < 0) return -1;
                    out->push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                } else {
                    out->push_back(query[i]);
                }
            }
            return 1;
        }
        begin = end + 1;
    }
    return 0;
}

}  // namespace qol::features::core_data
