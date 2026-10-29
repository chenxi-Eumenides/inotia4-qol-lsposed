#include "rates.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace qol::features::world_tuning {
namespace {

constexpr const char* kRateKeys[] = {
    "gold", "exp", "player_hp", "enemy_hp", "damage_out", "damage_in", "move_speed",
};
constexpr const char* kRateNames[] = {
    "金币倍率", "经验倍率", "血量倍率", "敌怪血量倍率", "伤害倍率", "承伤倍率", "移速倍率",
};
constexpr const char* kRateDescriptions[] = {
    "获得的金钱缩放", "获得的经验缩放", "玩家最大生命的缩放", "怪物最大生命的缩放", "对怪物造成的伤害缩放",
    "受到的伤害缩放，数值越低越耐打", "移动速度的缩放",
};
constexpr bool kRateSupported[] = {false, false, false, true, true, true, false};
constexpr uint32_t kRateCount = static_cast<uint32_t>(RateId::kCount);

static_assert(sizeof(kRateKeys) / sizeof(kRateKeys[0]) == kRateCount, "rate key table size");
static_assert(sizeof(kRateNames) / sizeof(kRateNames[0]) == kRateCount, "rate name table size");
static_assert(sizeof(kRateDescriptions) / sizeof(kRateDescriptions[0]) == kRateCount,
              "rate description table size");
static_assert(sizeof(kRateSupported) / sizeof(kRateSupported[0]) == kRateCount,
              "rate support table size");

bool valid_rate(int32_t value) { return ladder_index(value) >= 0; }

void skip_space(const char** cursor, const char* end) {
    while (*cursor < end && (**cursor == ' ' || **cursor == '\t' || **cursor == '\n' ||
                             **cursor == '\r')) {
        ++*cursor;
    }
}

bool consume(const char** cursor, const char* end, char expected) {
    skip_space(cursor, end);
    if (*cursor >= end || **cursor != expected) return false;
    ++*cursor;
    return true;
}

// 读取 JSON 字符串到 out（只支持无转义的键名）。out_size 含结尾 NUL。
bool read_string(const char** cursor, const char* end, char* out, size_t out_size) {
    if (!consume(cursor, end, '"') || out_size == 0) return false;
    size_t length = 0;
    while (*cursor < end && **cursor != '"') {
        const char ch = **cursor;
        if (ch == '\\' || static_cast<unsigned char>(ch) < 0x20) return false;
        if (length + 1 >= out_size) return false;
        out[length++] = ch;
        ++*cursor;
    }
    if (*cursor >= end || **cursor != '"') return false;
    ++*cursor;
    out[length] = '\0';
    return true;
}

bool read_int(const char** cursor, const char* end, int32_t* out) {
    skip_space(cursor, end);
    const char* start = *cursor;
    bool negative = false;
    if (*cursor < end && (**cursor == '-' || **cursor == '+')) {
        negative = **cursor == '-';
        ++*cursor;
    }
    if (*cursor >= end || **cursor < '0' || **cursor > '9') return false;
    int64_t value = 0;
    while (*cursor < end && **cursor >= '0' && **cursor <= '9') {
        value = value * 10 + (**cursor - '0');
        if (value > 1000000) return false;
        ++*cursor;
    }
    if (*cursor == start) return false;
    *out = static_cast<int32_t>(negative ? -value : value);
    return true;
}

}  // namespace

bool rate_supported(RateId id) {
    const auto index = static_cast<uint32_t>(id);
    return index < kRateCount && kRateSupported[index];
}

const char* rate_key(RateId id) {
    const auto index = static_cast<uint32_t>(id);
    return index < kRateCount ? kRateKeys[index] : nullptr;
}

const char* rate_name(RateId id) {
    const auto index = static_cast<uint32_t>(id);
    return index < kRateCount ? kRateNames[index] : nullptr;
}

const char* rate_description(RateId id) {
    const auto index = static_cast<uint32_t>(id);
    return index < kRateCount ? kRateDescriptions[index] : nullptr;
}

int32_t ladder_index(int32_t value) {
    for (uint32_t i = 0; i < kRateLadderSize; ++i) {
        if (kRateLadder[i] == value) return static_cast<int32_t>(i);
    }
    return -1;
}

int32_t ladder_shift(int32_t value, int32_t delta) {
    const int32_t index = ladder_index(value);
    if (index < 0 || delta == 0) return value;
    int64_t target = static_cast<int64_t>(index) + delta;
    if (target < 0) target = 0;
    if (target >= static_cast<int64_t>(kRateLadderSize)) target = kRateLadderSize - 1;
    return kRateLadder[target];
}

std::string rate_display(int32_t value) {
    char buffer[16]{};
    const int whole = value / 100;
    const int fraction = (value % 100) / 10;
    std::snprintf(buffer, sizeof(buffer), "×%d.%d", whole, fraction);
    return std::string(buffer);
}

Rates default_rates() {
    Rates rates{};
    for (uint32_t i = 0; i < kRateCount; ++i) rates.value[i] = kRateDefault;
    return rates;
}

std::string encode_rates(const Rates& rates) {
    std::string json = "{";
    for (uint32_t i = 0; i < kRateCount; ++i) {
        if (i != 0) json += ",";
        char item[64]{};
        std::snprintf(item, sizeof(item), "\"%s\":%d", kRateKeys[i], rates.value[i]);
        json += item;
    }
    json += "}";
    return json;
}

bool decode_rates(const char* json, uint32_t size, Rates* out) {
    if (json == nullptr || out == nullptr) return false;
    Rates parsed = default_rates();
    bool seen[kRateCount]{};
    const char* cursor = json;
    const char* end = json + size;
    if (!consume(&cursor, end, '{')) return false;
    skip_space(&cursor, end);
    if (cursor < end && *cursor == '}') {
        ++cursor;
    } else {
        for (;;) {
            char key[32]{};
            if (!read_string(&cursor, end, key, sizeof(key))) return false;
            if (!consume(&cursor, end, ':')) return false;
            int32_t value = 0;
            if (!read_int(&cursor, end, &value) || !valid_rate(value)) return false;
            uint32_t index = kRateCount;
            for (uint32_t i = 0; i < kRateCount; ++i) {
                if (std::strcmp(key, kRateKeys[i]) == 0) {
                    index = i;
                    break;
                }
            }
            if (index == kRateCount || seen[index]) return false;
            seen[index] = true;
            parsed.value[index] = value;
            skip_space(&cursor, end);
            if (cursor < end && *cursor == ',') {
                ++cursor;
                continue;
            }
            if (!consume(&cursor, end, '}')) return false;
            break;
        }
    }
    skip_space(&cursor, end);
    if (cursor != end) return false;
    *out = parsed;
    return true;
}

QolResult validate_rates_config(const char* key, const char* json, uint32_t size) {
    if (key == nullptr || std::strcmp(key, "rates") != 0) {
        return contracts::error(kQolErrInvalidArgument, "unsupported config key", 22);
    }
    Rates parsed{};
    if (!decode_rates(json, size, &parsed)) {
        return contracts::error(kQolErrInvalidArgument, "invalid rates payload", 21);
    }
    return contracts::ok();
}

int32_t scale_by_percent(int32_t value, int32_t percent) {
    if (value <= 0 || percent == kRateDefault) return value;
    const int64_t scaled = static_cast<int64_t>(value) * percent / 100;
    if (scaled > INT32_MAX) return INT32_MAX;
    if (scaled < 1) return 1;
    return static_cast<int32_t>(scaled);
}

int32_t scale_attribute(int32_t raw, int32_t percent) {
    if (raw <= 0 || percent == kRateDefault) return raw;
    const int64_t scaled = static_cast<int64_t>(raw) * percent / 100;
    if (scaled > INT32_MAX) return INT32_MAX;
    if (scaled < 1) return 1;
    return static_cast<int32_t>(scaled);
}

}  // namespace qol::features::world_tuning
