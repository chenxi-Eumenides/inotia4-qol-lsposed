#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "contracts/result.h"

namespace qol::features::extension_bag::bag_count_setting {

struct Value {
    int32_t count = 5;
    bool enabled = false;
};

enum class ConfigRead : uint8_t { kFound, kNotFound, kFailed };

inline bool parse_integer(const char* text, uint32_t size, int32_t minimum,
                          int32_t maximum, int32_t* out) {
    if (text == nullptr || out == nullptr || size == 0 || size >= 32) return false;
    uint32_t begin = 0, end = size;
    while (begin < end && (text[begin] == ' ' || text[begin] == '\n' ||
                           text[begin] == '\r' || text[begin] == '\t')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\n' ||
                           text[end - 1] == '\r' || text[end - 1] == '\t')) --end;
    if (begin == end) return false;
    uint32_t cursor = begin;
    if (text[cursor] == '-') ++cursor;
    const uint32_t digits = cursor;
    while (cursor < end && text[cursor] >= '0' && text[cursor] <= '9') ++cursor;
    if (digits == cursor || cursor != end ||
        (cursor - digits > 1 && text[digits] == '0')) return false;
    char raw[32]{};
    std::memcpy(raw, text + begin, end - begin);
    char* parsed_end = nullptr;
    const long value = std::strtol(raw, &parsed_end, 10);
    if (parsed_end == raw || *parsed_end != '\0' || value < minimum || value > maximum)
        return false;
    *out = static_cast<int32_t>(value);
    return true;
}

inline Value from_count(int32_t count) {
    return count == 0 ? Value{} : Value{count, true};
}

inline Value resolve_startup(ConfigRead config_state, const char* config_json,
                             uint32_t config_size, bool choice_read_ok,
                             const char* choice, uint32_t choice_size) {
    int32_t count = 0;
    if (config_state == ConfigRead::kFound) {
        if (parse_integer(config_json, config_size, 0, 128, &count)) return from_count(count);
        return {};
    }
    if (config_state == ConfigRead::kFailed || !choice_read_ok) return {};
    if (config_state == ConfigRead::kNotFound &&
        parse_integer(choice, choice_size, 5, 20, &count)) return from_count(count);
    return {};
}

inline QolResult validate_config(const char* key, const char* json, uint32_t size) {
    static constexpr char kKeyError[] = "unknown extension_bag config key";
    static constexpr char kValueError[] = "bagCount must be an integer from 0 to 128";
    if (key == nullptr || std::strcmp(key, "bagCount") != 0)
        return contracts::error(kQolErrInvalidArgument, kKeyError,
                                static_cast<uint32_t>(sizeof(kKeyError) - 1));
    int32_t ignored = 0;
    if (!parse_integer(json, size, 0, 128, &ignored))
        return contracts::error(kQolErrInvalidArgument, kValueError,
                                static_cast<uint32_t>(sizeof(kValueError) - 1));
    return contracts::ok();
}

template <typename Apply, typename Persist>
auto apply_and_persist_nonblocking(int32_t count, Apply&& apply, Persist&& persist) {
    auto result = apply(count);
    (void)persist(count);
    return result;
}

}  // namespace qol::features::extension_bag::bag_count_setting
