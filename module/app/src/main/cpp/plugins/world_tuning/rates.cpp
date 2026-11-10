#include "rates.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace qol::features::world_tuning {
namespace {

constexpr const char* kRateKeys[] = {
    "gold", "exp", "enemy_hp", "damage_out", "damage_in",
    "drop_quantity", "drop_reroll", "potion_cooldown", "skill_cooldown",
};
constexpr const char* kRateNames[] = {
    "金币倍率", "经验倍率", "敌怪血量倍率", "伤害倍率", "承伤倍率",
    "掉落数量倍率", "掉落重抽次数", "药水冷却倍率", "技能冷却倍率",
};
constexpr const char* kRateDescriptions[] = {
    "获得的金钱缩放", "获得的经验缩放", "怪物最大生命的缩放", "对怪物造成的伤害缩放",
    "受到的伤害缩放，数值越低越耐打",
    "每次击杀的掉落结算次数", "可重掷的掉落物取同类别最优（卷轴/宝石/背包/骰子）",
    "玩家药水冷却时间缩放", "玩家技能冷却时间缩放",
};
constexpr bool kRateSupported[] = {true, true, true, true, true, true, true, true, true};
constexpr uint32_t kRateCount = static_cast<uint32_t>(RateId::kCount);

// 旧配置兼容：移速倍率已从面板与 encode_rates 中移除，但已持久化的 rates JSON 仍可能带
// move_speed 键。解析时读取并忽略该键，不能因它把整份 rates 判为非法。
constexpr const char* kLegacyMoveSpeedKey = "move_speed";
// 旧配置兼容：drop_quality 已改名为 drop_reroll，旧 config.json 仍可能带该键。
// 解析时按 drop_reroll 的别名读入。
constexpr const char* kLegacyDropQualityKey = "drop_quality";

static_assert(sizeof(kRateKeys) / sizeof(kRateKeys[0]) == kRateCount, "rate key table size");
static_assert(sizeof(kRateNames) / sizeof(kRateNames[0]) == kRateCount, "rate name table size");
static_assert(sizeof(kRateDescriptions) / sizeof(kRateDescriptions[0]) == kRateCount,
              "rate description table size");
static_assert(sizeof(kRateSupported) / sizeof(kRateSupported[0]) == kRateCount,
              "rate support table size");

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
    if (*cursor < end && **cursor == '-') {
        negative = **cursor == '-';
        ++*cursor;
    }
    if (*cursor >= end || **cursor < '0' || **cursor > '9') return false;
    int64_t value = 0;
    while (*cursor < end && **cursor >= '0' && **cursor <= '9') {
        value = value * 10 + (**cursor - '0');
        if (value > INT32_MAX) return false;
        ++*cursor;
    }
    if (*cursor == start) return false;
    *out = static_cast<int32_t>(negative ? -value : value);
    return true;
}

bool skip_string(const char** cursor, const char* end) {
    skip_space(cursor, end);
    if (*cursor >= end || **cursor != '"') return false;
    ++*cursor;
    while (*cursor < end) {
        const unsigned char ch = static_cast<unsigned char>(**cursor);
        if (ch == '"') { ++*cursor; return true; }
        if (ch < 0x20) return false;
        if (ch == '\\') {
            ++*cursor;
            if (*cursor >= end) return false;
            const char escape = **cursor;
            if (escape == 'u') {
                for (int i = 0; i < 4; ++i) {
                    ++*cursor;
                    if (*cursor >= end || !( (**cursor >= '0' && **cursor <= '9') ||
                        (**cursor >= 'a' && **cursor <= 'f') || (**cursor >= 'A' && **cursor <= 'F'))) return false;
                }
            } else if (escape != '"' && escape != '\\' && escape != '/' && escape != 'b' &&
                       escape != 'f' && escape != 'n' && escape != 'r' && escape != 't') return false;
        }
        ++*cursor;
    }
    return false;
}

bool skip_value(const char** cursor, const char* end, int depth) {
    if (depth > 8) return false;
    skip_space(cursor, end);
    if (*cursor >= end) return false;
    if (**cursor == '"') return skip_string(cursor, end);
    if (**cursor == '[' || **cursor == '{') {
        const bool object = **cursor == '{';
        const char close = object ? '}' : ']';
        ++*cursor;
        skip_space(cursor, end);
        if (*cursor < end && **cursor == close) { ++*cursor; return true; }
        for (;;) {
            if (object && !skip_string(cursor, end)) return false;
            if (object && !consume(cursor, end, ':')) return false;
            if (!skip_value(cursor, end, depth + 1)) return false;
            skip_space(cursor, end);
            if (*cursor < end && **cursor == ',') { ++*cursor; continue; }
            return consume(cursor, end, close);
        }
    }
    const char* start = *cursor;
    if (**cursor == '-') ++*cursor;
    if (*cursor < end && **cursor == '0') ++*cursor;
    else {
        if (*cursor >= end || **cursor < '1' || **cursor > '9') { *cursor = start; goto literal; }
        while (*cursor < end && **cursor >= '0' && **cursor <= '9') ++*cursor;
    }
    if (*cursor < end && **cursor == '.') {
        ++*cursor;
        if (*cursor >= end || **cursor < '0' || **cursor > '9') { *cursor = start; return false; }
        while (*cursor < end && **cursor >= '0' && **cursor <= '9') ++*cursor;
    }
    if (*cursor < end && (**cursor == 'e' || **cursor == 'E')) {
        ++*cursor;
        if (*cursor < end && (**cursor == '+' || **cursor == '-')) ++*cursor;
        if (*cursor >= end || **cursor < '0' || **cursor > '9') { *cursor = start; return false; }
        while (*cursor < end && **cursor >= '0' && **cursor <= '9') ++*cursor;
    }
    if (*cursor != start) return true;
literal:
    const char* literals[] = {"true", "false", "null"};
    for (const char* literal : literals) {
        const size_t length = std::strlen(literal);
        if (static_cast<size_t>(end - *cursor) >= length && std::memcmp(*cursor, literal, length) == 0) {
            *cursor += length;
            return true;
        }
    }
    return false;
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

const int32_t* rate_ladder(RateId id, bool extended, uint32_t* out_size) {
    switch (id) {
        case RateId::kDropQuantity:
            if (extended) {
                if (out_size != nullptr) *out_size = kDropQuantityLadderExtendedSize;
                return kDropQuantityLadderExtended;
            }
            if (out_size != nullptr) *out_size = kDropQuantityLadderSize;
            return kDropQuantityLadder;
        case RateId::kDropReroll:
            if (extended) {
                if (out_size != nullptr) *out_size = kDropRerollLadderExtendedSize;
                return kDropRerollLadderExtended;
            }
            if (out_size != nullptr) *out_size = kDropRerollLadderSize;
            return kDropRerollLadder;
        case RateId::kPotionCooldown:
        case RateId::kSkillCooldown:
            if (extended) {
                if (out_size != nullptr) *out_size = kCooldownLadderExtendedSize;
                return kCooldownLadderExtended;
            }
            if (out_size != nullptr) *out_size = kCooldownLadderSize;
            return kCooldownLadder;
        default:
            if (extended) {
                if (out_size != nullptr) *out_size = kRateLadderExtendedSize;
                return kRateLadderExtended;
            }
            if (out_size != nullptr) *out_size = kRateLadderSize;
            return kRateLadder;
    }
}

int32_t rate_default(RateId id) {
    switch (id) {
        case RateId::kDropQuantity: return kDropQuantityDefault;
        case RateId::kDropReroll: return kDropRerollDefault;
        default: return kRateDefault;
    }
}

bool rate_value_valid(RateId id, int32_t value) {
    switch (id) {
        case RateId::kDropQuantity: return value >= 0;
        case RateId::kDropReroll: return value >= 0;
        case RateId::kGold:
        case RateId::kExp:
        case RateId::kEnemyHp:
        case RateId::kDamageOut:
        case RateId::kDamageIn:
        case RateId::kPotionCooldown:
        case RateId::kSkillCooldown: return value >= 0;
        default: return false;
    }
}

int32_t ladder_index(RateId id, int32_t value, bool extended) {
    uint32_t size = 0;
    const int32_t* ladder = rate_ladder(id, extended, &size);
    for (uint32_t i = 0; i < size; ++i) {
        if (ladder[i] == value) return static_cast<int32_t>(i);
    }
    return -1;
}

int32_t ladder_shift(RateId id, int32_t value, int32_t delta, bool extended) {
    uint32_t size = 0;
    const int32_t* ladder = rate_ladder(id, extended, &size);
    const int32_t index = ladder_index(id, value, extended);
    if (delta == 0) return value;
    if (index < 0) {
        if (delta > 0) {
            for (uint32_t i = 0; i < size; ++i) {
                if (ladder[i] > value) return ladder[i];
            }
        } else {
            for (uint32_t i = size; i > 0; --i) {
                if (ladder[i - 1] < value) return ladder[i - 1];
            }
        }
        return value;
    }
    int64_t target = static_cast<int64_t>(index) + delta;
    if (target < 0) target = 0;
    if (target >= static_cast<int64_t>(size)) target = static_cast<int64_t>(size) - 1;
    return ladder[target];
}

bool ladder_can_shift(RateId id, int32_t value, int32_t delta, bool extended) {
    return delta != 0 && ladder_shift(id, value, delta, extended) != value;
}

std::string rate_display(RateId id, int32_t value) {
    char buffer[32]{};
    if (id != RateId::kDropQuantity && id != RateId::kDropReroll && value == 0) return "×0";
    if (id == RateId::kDropQuantity || id == RateId::kDropReroll) {
        std::snprintf(buffer, sizeof(buffer), "×%d", value);
        return std::string(buffer);
    }
    const int whole = value / 100;
    const int fraction = (value % 100) / 10;
    std::snprintf(buffer, sizeof(buffer), "×%d.%d", whole, fraction);
    return std::string(buffer);
}

Rates default_rates() {
    Rates rates{};
    for (uint32_t i = 0; i < kRateCount; ++i) {
        rates.value[i] = rate_default(static_cast<RateId>(i));
    }
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
    bool seen_legacy_move_speed = false;
    bool seen_legacy_drop_quality = false;
    bool legacy_drop_quality_valid = false;
    int32_t legacy_drop_quality = 0;
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
            const char* value_start = cursor;
            const bool integer = read_int(&cursor, end, &value);
            skip_space(&cursor, end);
            const bool complete_integer = integer && cursor < end && (*cursor == ',' || *cursor == '}');
            if (!complete_integer) {
                cursor = value_start;
                if (!skip_value(&cursor, end, 0)) return false;
            }
            uint32_t index = kRateCount;
            for (uint32_t i = 0; i < kRateCount; ++i) {
                if (std::strcmp(key, kRateKeys[i]) == 0) {
                    index = i;
                    break;
                }
            }
            if (index < kRateCount) {
                if (seen[index]) return false;
                seen[index] = true;
                if (complete_integer && rate_value_valid(static_cast<RateId>(index), value)) parsed.value[index] = value;
            } else if (std::strcmp(key, kLegacyMoveSpeedKey) == 0) {
                if (seen_legacy_move_speed) return false;
                seen_legacy_move_speed = true;
            } else if (std::strcmp(key, kLegacyDropQualityKey) == 0) {
                if (seen_legacy_drop_quality) return false;
                seen_legacy_drop_quality = true;
                if (complete_integer && rate_value_valid(RateId::kDropReroll, value)) {
                    legacy_drop_quality = value;
                    legacy_drop_quality_valid = true;
                }
            } else {
                // 未知字段：值已由 read_int 或 skip_value 消费，忽略。
            }
            skip_space(&cursor, end);
            if (cursor < end && *cursor == ',') {
                ++cursor;
                continue;
            }
            if (!consume(&cursor, end, '}')) return false;
            break;
        }
    }
    const uint32_t reroll_index = static_cast<uint32_t>(RateId::kDropReroll);
    if (seen_legacy_drop_quality && legacy_drop_quality_valid && !seen[reroll_index]) {
        parsed.value[reroll_index] = legacy_drop_quality;
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
    if (percent == 0) return 0;
    if (percent < 0) return value;
    const int64_t scaled = static_cast<int64_t>(value) * percent / 100;
    if (scaled > INT32_MAX) return INT32_MAX;
    if (scaled < 1) return 1;
    return static_cast<int32_t>(scaled);
}

int64_t scale_money(int64_t value, int32_t percent) {
    if (value <= 0 || percent == kRateDefault) return value;
    if (percent == 0) return 0;
    if (percent < 0) return value;
    if (value > INT64_MAX / percent) return INT64_MAX;
    return value * percent / 100;
}

int64_t scale_cooldown_ticks(int64_t ticks, int32_t percent) {
    if (ticks <= 0 || percent == kRateDefault) return ticks;
    if (percent == 0) return 0;
    if (percent < 0) return ticks;
    if (ticks > INT64_MAX / percent) return INT64_MAX;
    const int64_t scaled = ticks * percent / 100;
    return scaled < 1 ? 1 : scaled;
}

int32_t scale_drop_threshold(int32_t threshold, int32_t reroll, int32_t cap) {
    if (threshold <= 0 || reroll <= 0 || reroll == 1 || cap <= 0) return threshold;
    const uint64_t multiplier = static_cast<uint64_t>(reroll);
    const uint64_t factor = multiplier * multiplier;
    if (static_cast<uint64_t>(threshold) >
        (static_cast<uint64_t>(cap) - 1u) / factor) {
        return cap;
    }
    return static_cast<int32_t>(static_cast<uint64_t>(threshold) * factor);
}

int32_t scale_attribute(int32_t raw, int32_t percent) {
    if (raw <= 0 || percent == kRateDefault) return raw;
    if (percent == 0) {
        const int64_t scaled = static_cast<int64_t>(raw) / 1000000;
        return static_cast<int32_t>(scaled < 1 ? 1 : scaled > INT32_MAX ? INT32_MAX : scaled);
    }
    if (percent < 0) return raw;
    const int64_t scaled = static_cast<int64_t>(raw) * percent / 100;
    if (scaled > INT32_MAX) return INT32_MAX;
    if (scaled < 1) return 1;
    return static_cast<int32_t>(scaled);
}

}  // namespace qol::features::world_tuning
