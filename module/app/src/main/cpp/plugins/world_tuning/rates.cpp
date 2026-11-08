#include "rates.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace qol::features::world_tuning {
namespace {

constexpr const char* kRateKeys[] = {
    "gold", "exp", "player_hp", "enemy_hp", "damage_out", "damage_in",
    "drop_quantity", "drop_reroll",
};
constexpr const char* kRateNames[] = {
    "金币倍率", "经验倍率", "血量倍率", "敌怪血量倍率", "伤害倍率", "承伤倍率",
    "掉落数量倍率", "掉落重抽次数",
};
constexpr const char* kRateDescriptions[] = {
    "获得的金钱缩放", "获得的经验缩放", "玩家最大生命的缩放", "怪物最大生命的缩放", "对怪物造成的伤害缩放",
    "受到的伤害缩放，数值越低越耐打",
    "每次击杀的掉落结算次数", "可重掷的掉落物取同类别最优（卷轴/宝石/背包/骰子）",
};
constexpr bool kRateSupported[] = {true, true, false, true, true, true, true, true};
constexpr uint32_t kRateCount = static_cast<uint32_t>(RateId::kCount);

// 旧配置兼容：移速倍率已从面板与 encode_rates 中移除，但已持久化的 rates JSON 仍可能带
// move_speed 键。解析时读取并忽略该键，不能因它把整份 rates 判为非法。
constexpr const char* kLegacyMoveSpeedKey = "move_speed";
// 旧配置兼容：drop_quality 已改名为 drop_reroll（档位同为 1..5），覆盖升级时旧 config.json
// 仍带旧键。解析时按 drop_reroll 的别名读入，避免整份 rates 被判非法。
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

const int32_t* rate_ladder(RateId id, uint32_t* out_size) {
    switch (id) {
        case RateId::kDropQuantity:
            if (out_size != nullptr) *out_size = kDropQuantityLadderSize;
            return kDropQuantityLadder;
        case RateId::kDropReroll:
            if (out_size != nullptr) *out_size = kDropRerollLadderSize;
            return kDropRerollLadder;
        default:
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
        case RateId::kDropQuantity: return value >= 0 && value <= 5;
        case RateId::kDropReroll: return value >= 1 && value <= 5;
        case RateId::kGold:
        case RateId::kExp:
        case RateId::kPlayerHp:
        case RateId::kEnemyHp:
        case RateId::kDamageOut:
        case RateId::kDamageIn: return value >= 1;
        default: return false;
    }
}

int32_t ladder_index(RateId id, int32_t value) {
    uint32_t size = 0;
    const int32_t* ladder = rate_ladder(id, &size);
    for (uint32_t i = 0; i < size; ++i) {
        if (ladder[i] == value) return static_cast<int32_t>(i);
    }
    return -1;
}

int32_t ladder_shift(RateId id, int32_t value, int32_t delta) {
    uint32_t size = 0;
    const int32_t* ladder = rate_ladder(id, &size);
    const int32_t index = ladder_index(id, value);
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

bool ladder_can_shift(RateId id, int32_t value, int32_t delta) {
    return delta != 0 && ladder_shift(id, value, delta) != value;
}

std::string rate_display(RateId id, int32_t value) {
    char buffer[32]{};
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
            if (!read_int(&cursor, end, &value)) return false;
            uint32_t index = kRateCount;
            for (uint32_t i = 0; i < kRateCount; ++i) {
                if (std::strcmp(key, kRateKeys[i]) == 0) {
                    index = i;
                    break;
                }
            }
            if (index < kRateCount) {
                if (!rate_value_valid(static_cast<RateId>(index), value)) return false;
                if (seen[index]) return false;
                seen[index] = true;
                parsed.value[index] = value;
            } else if (std::strcmp(key, kLegacyMoveSpeedKey) == 0) {
                // 历史移速值按百分比范围校验后忽略，不写入 rates。
                if (!rate_value_valid(RateId::kGold, value)) return false;
                if (seen_legacy_move_speed) return false;
                seen_legacy_move_speed = true;
            } else if (std::strcmp(key, kLegacyDropQualityKey) == 0) {
                // 旧配置的 drop_quality 键：作为 drop_reroll 别名读入；显式 drop_reroll 优先。
                if (!rate_value_valid(RateId::kDropReroll, value)) return false;
                if (seen_legacy_drop_quality) return false;
                seen_legacy_drop_quality = true;
                legacy_drop_quality = value;
            } else {
                return false;
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
    if (seen_legacy_drop_quality && !seen[reroll_index]) {
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
    const int64_t scaled = static_cast<int64_t>(value) * percent / 100;
    if (scaled > INT32_MAX) return INT32_MAX;
    if (scaled < 1) return 1;
    return static_cast<int32_t>(scaled);
}

int64_t scale_money(int64_t value, int32_t percent) {
    if (value <= 0 || percent == kRateDefault) return value;
    if (value > INT64_MAX / percent) return INT64_MAX;
    return value * percent / 100;
}

int32_t scale_attribute(int32_t raw, int32_t percent) {
    if (raw <= 0 || percent == kRateDefault) return raw;
    const int64_t scaled = static_cast<int64_t>(raw) * percent / 100;
    if (scaled > INT32_MAX) return INT32_MAX;
    if (scaled < 1) return 1;
    return static_cast<int32_t>(scaled);
}

}  // namespace qol::features::world_tuning
