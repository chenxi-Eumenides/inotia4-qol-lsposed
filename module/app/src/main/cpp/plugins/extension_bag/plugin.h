#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>

#include "contracts/plugin.h"
#include "contracts/world_facts.h"
#include "contracts/item_facts.h"
#include "contracts/save_facts.h"

namespace qol::features::extension_bag {

const qol::contracts::PluginDescriptor& plugin_descriptor();
const qol::contracts::WorldFactsApi* world_facts();
const qol::contracts::SaveFactsApi* save_facts();

namespace detail {

enum class JsonBooleanValue : uint8_t { kInvalid, kFalse, kTrue };

struct JsonFieldValue {
    enum class Type : uint8_t { kOther, kNumber, kBoolean };
    Type type = Type::kOther;
    size_t begin = 0;
    size_t end = 0;
    bool boolean = false;
};

class JsonObjectFieldParser {
public:
    explicit JsonObjectFieldParser(std::string_view input) : input_(input) {}

    bool find(std::string_view key, JsonFieldValue* out) {
        if (out == nullptr) return false;
        JsonFieldValue found{};
        bool has_field = false;
        skip_space();
        if (!consume('{')) return false;
        skip_space();
        if (consume('}')) {
            skip_space();
            if (position_ != input_.size()) return false;
            *out = found;
            return false;
        }
        for (;;) {
            std::string field_name;
            if (!parse_string(&field_name)) return false;
            skip_space();
            if (!consume(':')) return false;
            skip_space();
            JsonFieldValue value{};
            if (!parse_value(&value, 1)) return false;
            if (field_name == key) {
                if (has_field) return false;
                found = value;
                has_field = true;
            }
            skip_space();
            if (consume('}')) break;
            if (!consume(',')) return false;
            skip_space();
        }
        skip_space();
        if (position_ != input_.size()) return false;
        *out = found;
        return has_field;
    }

private:
    static bool is_digit(char value) { return value >= '0' && value <= '9'; }

    static int hex_value(char value) {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    }

    void skip_space() {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\t' ||
                input_[position_] == '\n' || input_[position_] == '\r')) {
            ++position_;
        }
    }

    bool consume(char expected) {
        if (position_ >= input_.size() || input_[position_] != expected) return false;
        ++position_;
        return true;
    }

    bool parse_string(std::string* decoded) {
        if (!consume('"')) return false;
        if (decoded != nullptr) decoded->clear();
        while (position_ < input_.size()) {
            const unsigned char current = static_cast<unsigned char>(input_[position_++]);
            if (current == '"') return true;
            if (current < 0x20) return false;
            if (current != '\\') {
                if (decoded != nullptr) decoded->push_back(static_cast<char>(current));
                continue;
            }
            if (position_ >= input_.size()) return false;
            const char escaped = input_[position_++];
            char decoded_char = 0;
            switch (escaped) {
                case '"': decoded_char = '"'; break;
                case '\\': decoded_char = '\\'; break;
                case '/': decoded_char = '/'; break;
                case 'b': decoded_char = '\b'; break;
                case 'f': decoded_char = '\f'; break;
                case 'n': decoded_char = '\n'; break;
                case 'r': decoded_char = '\r'; break;
                case 't': decoded_char = '\t'; break;
                case 'u': {
                    if (input_.size() - position_ < 4) return false;
                    int codepoint = 0;
                    for (int i = 0; i < 4; ++i) {
                        const int digit = hex_value(input_[position_++]);
                        if (digit < 0) return false;
                        codepoint = (codepoint << 4) | digit;
                    }
                    // Only ASCII keys can match route fields; preserve other code units as
                    // a non-ASCII marker while still validating their JSON escape syntax.
                    decoded_char = codepoint <= 0x7f ? static_cast<char>(codepoint) : '\x7f';
                    break;
                }
                default: return false;
            }
            if (decoded != nullptr) decoded->push_back(decoded_char);
        }
        return false;
    }

    bool parse_number(JsonFieldValue* out) {
        const size_t begin = position_;
        (void)consume('-');
        if (position_ >= input_.size()) return false;
        if (input_[position_] == '0') {
            ++position_;
        } else {
            if (input_[position_] < '1' || input_[position_] > '9') return false;
            while (position_ < input_.size() && is_digit(input_[position_])) ++position_;
        }
        if (consume('.')) {
            const size_t fraction_begin = position_;
            while (position_ < input_.size() && is_digit(input_[position_])) ++position_;
            if (position_ == fraction_begin) return false;
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            const size_t exponent_begin = position_;
            while (position_ < input_.size() && is_digit(input_[position_])) ++position_;
            if (position_ == exponent_begin) return false;
        }
        out->type = JsonFieldValue::Type::kNumber;
        out->begin = begin;
        out->end = position_;
        return true;
    }

    bool parse_literal(std::string_view literal, JsonFieldValue* out, bool boolean) {
        if (input_.substr(position_, literal.size()) != literal) return false;
        position_ += literal.size();
        if (boolean) {
            out->type = JsonFieldValue::Type::kBoolean;
            out->boolean = literal == "true";
        }
        return true;
    }

    bool parse_object(uint32_t depth) {
        if (depth > 64 || !consume('{')) return false;
        skip_space();
        if (consume('}')) return true;
        for (;;) {
            if (!parse_string(nullptr)) return false;
            skip_space();
            if (!consume(':')) return false;
            skip_space();
            JsonFieldValue ignored{};
            if (!parse_value(&ignored, depth)) return false;
            skip_space();
            if (consume('}')) return true;
            if (!consume(',')) return false;
            skip_space();
        }
    }

    bool parse_array(uint32_t depth) {
        if (depth > 64 || !consume('[')) return false;
        skip_space();
        if (consume(']')) return true;
        for (;;) {
            JsonFieldValue ignored{};
            if (!parse_value(&ignored, depth)) return false;
            skip_space();
            if (consume(']')) return true;
            if (!consume(',')) return false;
            skip_space();
        }
    }

    bool parse_value(JsonFieldValue* out, uint32_t depth) {
        if (depth > 64 || position_ >= input_.size()) return false;
        const char current = input_[position_];
        if (current == '"') return parse_string(nullptr);
        if (current == '{') return parse_object(depth + 1);
        if (current == '[') return parse_array(depth + 1);
        if (current == 't') return parse_literal("true", out, true);
        if (current == 'f') return parse_literal("false", out, true);
        if (current == 'n') return parse_literal("null", out, false);
        return parse_number(out);
    }

    std::string_view input_;
    size_t position_ = 0;
};

inline bool parse_json_int32_field(std::string_view json, std::string_view key,
                                   int32_t* out) {
    if (out == nullptr) return false;
    JsonFieldValue field{};
    JsonObjectFieldParser parser(json);
    if (!parser.find(key, &field) || field.type != JsonFieldValue::Type::kNumber ||
        field.begin >= field.end) {
        return false;
    }
    const std::string token(json.substr(field.begin, field.end - field.begin));
    if (token.find_first_of(".eE") != std::string::npos) return false;
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(token.c_str(), &end, 10);
    if (errno == ERANGE || end == token.c_str() || end == nullptr || *end != '\0' ||
        value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    *out = static_cast<int32_t>(value);
    return true;
}

inline JsonBooleanValue parse_json_top_level_boolean(std::string_view json,
                                                      std::string_view key) {
    JsonFieldValue field{};
    JsonObjectFieldParser parser(json);
    if (!parser.find(key, &field) || field.type != JsonFieldValue::Type::kBoolean) {
        return JsonBooleanValue::kInvalid;
    }
    return field.boolean ? JsonBooleanValue::kTrue : JsonBooleanValue::kFalse;
}

}  // namespace detail

}  // namespace qol::features::extension_bag
