#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace qol::features::core_data {

struct JsonValue {
    enum class Type { kNull, kBoolean, kNumber, kString, kArray, kObject };
    Type type = Type::kNull;
    size_t begin = 0;
    size_t end = 0;
    std::string text;
    std::vector<JsonValue> items;
    std::vector<std::string> keys;
    std::vector<JsonValue> values;

    const JsonValue* find(const char* key) const;
    int64_t opt_int(size_t index, int64_t fallback) const;
    std::string opt_string(const char* key, const char* fallback = "") const;
};

bool parse_json(const std::string& json, JsonValue* out);
std::string quote_json(const std::string& value);
std::string stringify_json(const JsonValue& value);
int32_t decode_query_value(const char* query, uint32_t query_size, const char* key,
                           std::string* out);

}  // namespace qol::features::core_data
