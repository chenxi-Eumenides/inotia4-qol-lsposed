#include "plugins/world_tuning/rates.h"

#include <cassert>
#include <climits>
#include <cstring>
#include <string>

namespace {

using namespace qol::contracts;
using namespace qol::features::world_tuning;

void test_ladder_and_metadata() {
    assert(kRateLadderSize == 10);
    assert(kRateLadder[0] == 10 && kRateLadder[4] == 100 && kRateLadder[9] == 500);
    const int32_t expected_extended_percent[] = {
        0, 10, 30, 50, 80, 100, 120, 150, 200, 300, 500, 1000, 10000, 100000,
        1000000, 10000000, 100000000};
    assert(kRateLadderExtendedSize == 17);
    for (uint32_t i = 0; i < kRateLadderExtendedSize; ++i) {
        assert(kRateLadderExtended[i] == expected_extended_percent[i]);
    }
    assert(kRateDefault == 100);
    assert(ladder_index(RateId::kGold, 100, false) == 4);
    assert(ladder_index(RateId::kGold, 101, false) == -1);
    assert(ladder_index(RateId::kGold, 0, false) == -1);
    assert(ladder_shift(RateId::kGold, 100, -1, false) == 80);
    assert(ladder_shift(RateId::kGold, 100, 1, false) == 120);
    assert(ladder_shift(RateId::kGold, 10, -1, false) == 10);
    assert(ladder_shift(RateId::kGold, 500, 1, false) == 500);
    assert(ladder_shift(RateId::kGold, 100, 0, false) == 100);
    assert(ladder_shift(RateId::kGold, 1337, 1, false) == 1337);
    assert(ladder_shift(RateId::kGold, 1337, -1, false) == 500);
    assert(ladder_shift(RateId::kGold, 5, -1, false) == 5);
    assert(ladder_shift(RateId::kGold, 600, 1, false) == 600);
    assert(!ladder_can_shift(RateId::kGold, 1000, 1, false));
    assert(!ladder_can_shift(RateId::kGold, 1337, 1, false));
    assert(ladder_can_shift(RateId::kGold, 1337, -1, false));
    assert(!ladder_can_shift(RateId::kGold, 5, -1, false));
    assert(!ladder_can_shift(RateId::kGold, 600, 1, false));
    assert(!ladder_can_shift(RateId::kGold, 100, 0, false));
    assert(ladder_shift(RateId::kGold, 500, 1, true) == 1000);
    assert(ladder_shift(RateId::kGold, 1000, 1, true) == 10000);
    assert(ladder_shift(RateId::kGold, 10000000, 1, true) == 100000000);
    assert(!ladder_can_shift(RateId::kGold, 100000000, 1, true));
    assert(ladder_shift(RateId::kGold, 10, -1, true) == 0);
    assert(!ladder_can_shift(RateId::kGold, 0, -1, true));
    assert(ladder_shift(RateId::kGold, 0, 1, true) == 10);
    assert(rate_value_valid(RateId::kGold, 10));
    assert(rate_value_valid(RateId::kGold, 1337));
    assert(rate_value_valid(RateId::kGold, INT32_MAX));
    assert(rate_value_valid(RateId::kGold, 0));
    assert(rate_value_valid(RateId::kGold, 200000000));
    assert(!rate_value_valid(RateId::kGold, -1));
    assert(rate_value_valid(RateId::kDropQuantity, 0));
    assert(rate_value_valid(RateId::kDropQuantity, 6));
    assert(rate_value_valid(RateId::kDropQuantity, 1000));
    assert(rate_value_valid(RateId::kDropQuantity, 300));
    assert(rate_value_valid(RateId::kDropQuantity, INT32_MAX));
    assert(!rate_value_valid(RateId::kDropQuantity, -1));
    assert(rate_value_valid(RateId::kDropReroll, 0));
    assert(rate_value_valid(RateId::kDropReroll, 1));
    assert(rate_value_valid(RateId::kDropReroll, 6));
    assert(rate_value_valid(RateId::kDropReroll, 300));
    assert(rate_value_valid(RateId::kDropReroll, 0));
    for (uint32_t i = 0; i < static_cast<uint32_t>(RateId::kCount); ++i) {
        assert(rate_value_valid(static_cast<RateId>(i), 0));
        assert(!rate_value_valid(static_cast<RateId>(i), -1));
    }

    assert(std::strcmp(rate_key(RateId::kGold), "gold") == 0);
    assert(std::strcmp(rate_name(RateId::kGold), "金币倍率") == 0);
    assert(std::strcmp(rate_key(RateId::kExp), "exp") == 0);
    assert(std::strcmp(rate_key(RateId::kEnemyHp), "enemy_hp") == 0);
    assert(std::strcmp(rate_key(RateId::kDamageOut), "damage_out") == 0);
    assert(std::strcmp(rate_key(RateId::kDamageIn), "damage_in") == 0);

    assert(rate_supported(RateId::kGold));
    assert(rate_supported(RateId::kExp));
    assert(rate_supported(RateId::kEnemyHp));
    assert(rate_supported(RateId::kDamageOut));
    assert(rate_supported(RateId::kDamageIn));
    assert(rate_supported(RateId::kDropQuantity));
    assert(rate_supported(RateId::kDropReroll));
    assert(std::strcmp(rate_key(RateId::kDropQuantity), "drop_quantity") == 0);
    assert(std::strcmp(rate_key(RateId::kDropReroll), "drop_reroll") == 0);
    assert(std::strcmp(rate_key(RateId::kPotionCooldown), "potion_cooldown") == 0);
    assert(std::strcmp(rate_key(RateId::kSkillCooldown), "skill_cooldown") == 0);
    assert(rate_default(RateId::kPotionCooldown) == 100);
    assert(rate_default(RateId::kSkillCooldown) == 100);
    assert(ladder_index(RateId::kPotionCooldown, 100, false) == 4);
    assert(ladder_index(RateId::kSkillCooldown, 10, false) == 0);
    assert(ladder_shift(RateId::kPotionCooldown, 500, 1, true) == 1000);
    assert(ladder_shift(RateId::kSkillCooldown, 10, -1, true) == 0);
    assert(kDropQuantityLadderSize == 5 && kDropQuantityLadder[0] == 1 &&
           kDropQuantityLadder[4] == 5);
    assert(kDropRerollLadderSize == 5 && kDropRerollLadder[0] == 1 &&
           kDropRerollLadder[4] == 5);
    assert(ladder_shift(RateId::kDropQuantity, 1, 1, false) == 2);
    assert(ladder_shift(RateId::kDropQuantity, 0, -1, false) == 0);
    assert(ladder_shift(RateId::kDropQuantity, 5, 1, false) == 5);
    assert(ladder_shift(RateId::kDropQuantity, 5, 1, true) == 10);
    assert(ladder_shift(RateId::kDropQuantity, 1, -1, true) == 0);
    assert(!ladder_can_shift(RateId::kDropQuantity, 0, -1, true));
    assert(!ladder_can_shift(RateId::kDropQuantity, 200, 1, true));
    assert(kDropQuantityLadderExtended[0] == 0 && kDropQuantityLadderExtended[25] == 200);
    assert(kDropRerollLadderExtended[0] == 0 && kDropRerollLadderExtended[25] == 200);
    assert(kDropQuantityLadderExtendedSize == 26 && kDropRerollLadderExtendedSize == 26);
    for (uint32_t i = 6; i < kDropQuantityLadderExtendedSize; ++i) {
        assert(kDropQuantityLadderExtended[i] == static_cast<int32_t>((i - 5) * 10));
        assert(kDropRerollLadderExtended[i] == static_cast<int32_t>((i - 5) * 10));
    }
    assert(ladder_shift(RateId::kDropReroll, 1, -1, true) == 0);
    assert(ladder_shift(RateId::kDropReroll, 5, 1, true) == 10);
    assert(rate_display(RateId::kDropQuantity, 0) == "×0");
    assert(rate_display(RateId::kDropQuantity, 6) == "×6");
    assert(rate_display(RateId::kDropQuantity, INT32_MAX) == "×2147483647");
    assert(rate_display(RateId::kDropReroll, 3) == "×3");

    assert(rate_display(RateId::kGold, 10) == "×0.1");
    assert(rate_display(RateId::kGold, 100) == "×1.0");
    assert(rate_display(RateId::kGold, 150) == "×1.5");
    assert(rate_display(RateId::kGold, 500) == "×5.0");
    assert(rate_display(RateId::kGold, 0) == "×0");
    assert(rate_display(RateId::kGold, 1000) == "×10.0");
    assert(rate_display(RateId::kGold, 100000000) == "×1000000.0");
    assert(rate_display(RateId::kGold, INT32_MAX) == "×21474836.4");
}

void test_roundtrip_and_defaults() {
    Rates defaults = default_rates();
    for (uint32_t i = 0; i < static_cast<uint32_t>(RateId::kCount); ++i) {
        assert(defaults.value[i] == rate_default(static_cast<RateId>(i)));
    }
    assert(defaults.value[static_cast<uint32_t>(RateId::kDropQuantity)] == 1);
    assert(defaults.value[static_cast<uint32_t>(RateId::kDropReroll)] == 1);

    Rates rates = defaults;
    rates.value[static_cast<uint32_t>(RateId::kEnemyHp)] = 50;
    rates.value[static_cast<uint32_t>(RateId::kDamageOut)] = 150;
    rates.value[static_cast<uint32_t>(RateId::kDamageIn)] = 30;
    const std::string json = encode_rates(rates);
    assert(json.find("\"enemy_hp\":50") != std::string::npos);

    Rates decoded{};
    assert(decode_rates(json.c_str(), static_cast<uint32_t>(json.size()), &decoded));
    for (uint32_t i = 0; i < static_cast<uint32_t>(RateId::kCount); ++i) {
        assert(decoded.value[i] == rates.value[i]);
    }

    // 缺字段用默认值。
    const char* partial = "{\"enemy_hp\":200}";
    Rates parsed{};
    assert(decode_rates(partial, static_cast<uint32_t>(std::strlen(partial)), &parsed));
    assert(parsed.value[static_cast<uint32_t>(RateId::kEnemyHp)] == 200);
    assert(parsed.value[static_cast<uint32_t>(RateId::kExp)] == 100);
}

void test_decode_rejects_invalid() {
    const auto reject = [](const char* json) {
        Rates parsed{};
        return !decode_rates(json, static_cast<uint32_t>(std::strlen(json)), &parsed);
    };
    assert(reject(""));
    assert(reject("{"));
    assert(!reject("{\"unknown\":100}"));
    assert(!reject("{\"enemy_hp\":1337}"));
    assert(!reject("{\"damage_out\":100000000}"));
    assert(!reject("{\"damage_in\":2147483647}"));
    assert(!reject("{\"enemy_hp\":2147483648}"));
    assert(!reject("{\"enemy_hp\":0}"));
    assert(!reject("{\"enemy_hp\":-1}"));
    assert(!reject("{\"enemy_hp\":100.5}"));
    assert(!reject("{\"enemy_hp\":\"100\"}"));
    assert(reject("{\"enemy_hp\":100,\"enemy_hp\":50}"));
    assert(reject("{\"enemy_hp\":100}x"));
    assert(!reject("{\"enemy_hp\":-100}"));
    assert(!reject("{\"drop_quantity\":6}"));
    assert(!reject("{\"drop_reroll\":0}"));
    assert(!reject("{\"drop_reroll\":6}"));
    Rates parsed{};
    const char* partial = "{\"gold\":1000,\"enemy_hp\":0}";
    assert(decode_rates(partial, std::strlen(partial), &parsed));
    assert(parsed.value[static_cast<uint32_t>(RateId::kGold)] == 1000);
    assert(parsed.value[static_cast<uint32_t>(RateId::kEnemyHp)] == 0);
    const char* zero_drops = "{\"drop_quantity\":0,\"drop_reroll\":0}";
    assert(decode_rates(zero_drops, std::strlen(zero_drops), &parsed));
    assert(parsed.value[static_cast<uint32_t>(RateId::kDropQuantity)] == 0);
    assert(parsed.value[static_cast<uint32_t>(RateId::kDropReroll)] == 0);
}

// 旧配置兼容：移速倍率已移除，已持久化的 rates JSON 仍可能带 move_speed 键，解析须容忍。
void test_legacy_move_speed_key() {
    const auto reject = [](const char* json) {
        Rates parsed{};
        return !decode_rates(json, static_cast<uint32_t>(std::strlen(json)), &parsed);
    };

    // 旧配置中的 player_hp 与 move_speed 均作为未知/历史键忽略，其余字段正常生效。
    const char* legacy =
        "{\"gold\":150,\"exp\":100,\"player_hp\":100,\"enemy_hp\":50,\"damage_out\":150,"
        "\"damage_in\":30,\"move_speed\":120}";
    Rates parsed{};
    assert(decode_rates(legacy, static_cast<uint32_t>(std::strlen(legacy)), &parsed));
    assert(parsed.value[static_cast<uint32_t>(RateId::kGold)] == 150);
    assert(parsed.value[static_cast<uint32_t>(RateId::kEnemyHp)] == 50);
    assert(parsed.value[static_cast<uint32_t>(RateId::kDamageOut)] == 150);
    assert(parsed.value[static_cast<uint32_t>(RateId::kDamageIn)] == 30);
    assert(parsed.value[static_cast<uint32_t>(RateId::kExp)] == 100);

    // 只剩 move_speed 键的旧配置：全部字段回默认值。
    const char* legacy_only = "{\"move_speed\":100}";
    assert(decode_rates(legacy_only, static_cast<uint32_t>(std::strlen(legacy_only)), &parsed));
    for (uint32_t i = 0; i < static_cast<uint32_t>(RateId::kCount); ++i) {
        assert(parsed.value[i] == rate_default(static_cast<RateId>(i)));
    }

    // encode 不再输出 move_speed。
    const std::string json = encode_rates(default_rates());
    assert(json.find("move_speed") == std::string::npos);
    assert(json == "{\"gold\":100,\"exp\":100,\"enemy_hp\":100,"
                   "\"damage_out\":100,\"damage_in\":100,"
                   "\"drop_quantity\":1,\"drop_reroll\":1,"
                   "\"potion_cooldown\":100,\"skill_cooldown\":100}");

    // validate_config 同样容忍旧 move_speed 键。
    assert(validate_rates_config("rates", legacy, static_cast<uint32_t>(std::strlen(legacy))).ok ==
           kQolOk);

    // 历史移速键按百分比范围校验后忽略。
    assert(!reject("{\"move_speed\":101}"));
    // move_speed 重复仍拒绝；未知字段忽略。
    assert(reject("{\"move_speed\":100,\"move_speed\":50}"));
    assert(!reject("{\"move_speed\":100,\"unknown\":100}"));
}

void test_legacy_drop_quality_key() {
    // 1.0.1 持久化的 rates 带旧键 drop_quality；1.0.2 改名为 drop_reroll 后须能读入。
    const char* legacy =
        "{\"gold\":500,\"exp\":500,\"player_hp\":100,\"enemy_hp\":10,\"damage_out\":500,"
        "\"damage_in\":10,\"drop_quantity\":1,\"drop_quality\":5}";
    Rates parsed{};
    assert(decode_rates(legacy, static_cast<uint32_t>(std::strlen(legacy)), &parsed));
    assert(parsed.value[static_cast<uint32_t>(RateId::kDropQuantity)] == 1);
    assert(parsed.value[static_cast<uint32_t>(RateId::kDropReroll)] == 5);
    assert(parsed.value[static_cast<uint32_t>(RateId::kGold)] == 500);

    // 只有旧键时也按别名生效，其余字段回默认值。
    const char* legacy_only = "{\"drop_quality\":3}";
    assert(decode_rates(legacy_only, static_cast<uint32_t>(std::strlen(legacy_only)), &parsed));
    assert(parsed.value[static_cast<uint32_t>(RateId::kDropReroll)] == 3);
    assert(parsed.value[static_cast<uint32_t>(RateId::kGold)] == rate_default(RateId::kGold));

    // 显式 drop_reroll 与旧键并存时以新键为准。
    const char* both = "{\"drop_quality\":2,\"drop_reroll\":4}";
    assert(decode_rates(both, static_cast<uint32_t>(std::strlen(both)), &parsed));
    assert(parsed.value[static_cast<uint32_t>(RateId::kDropReroll)] == 4);

    // validate_config 同样接受旧键，档位与重复规则和现行字段一致。
    assert(validate_rates_config("rates", legacy, static_cast<uint32_t>(std::strlen(legacy))).ok ==
           kQolOk);
    const char* bad_zero = "{\"drop_quality\":0}";
    assert(decode_rates(bad_zero, static_cast<uint32_t>(std::strlen(bad_zero)), &parsed));
    assert(parsed.value[static_cast<uint32_t>(RateId::kDropReroll)] == 0);
    const char* bad_high = "{\"drop_quality\":6}";
    assert(decode_rates(bad_high, static_cast<uint32_t>(std::strlen(bad_high)), &parsed));
    assert(parsed.value[static_cast<uint32_t>(RateId::kDropReroll)] == 6);
    const char* duplicate = "{\"drop_quality\":2,\"drop_quality\":3}";
    assert(!decode_rates(duplicate, static_cast<uint32_t>(std::strlen(duplicate)), &parsed));

    // encode 只输出新键。
    assert(encode_rates(parsed).find("drop_quality") == std::string::npos);
}

void test_validate_config() {
    const char* good = "{\"enemy_hp\":50}";
    assert(validate_rates_config("rates", good, static_cast<uint32_t>(std::strlen(good))).ok ==
           kQolOk);
    assert(validate_rates_config("other", good, static_cast<uint32_t>(std::strlen(good))).ok ==
           kQolErrInvalidArgument);
    const char* bad = "{\"enemy_hp\":0}";
    assert(validate_rates_config("rates", bad, static_cast<uint32_t>(std::strlen(bad))).ok == kQolOk);
    const char* malformed = "{\"enemy_hp\":}";
    assert(validate_rates_config("rates", malformed, std::strlen(malformed)).ok == kQolErrInvalidArgument);
}

void test_scaling() {
    assert(scale_by_percent(100, 100) == 100);
    assert(scale_by_percent(100, 150) == 150);
    assert(scale_by_percent(100, 50) == 50);
    assert(scale_by_percent(3, 50) == 1);
    assert(scale_by_percent(1, 500) == 5);
    assert(scale_by_percent(0, 500) == 0);
    assert(scale_by_percent(-7, 500) == -7);
    assert(scale_by_percent(INT32_MAX, 500) == INT32_MAX);
    assert(scale_by_percent(10, 30) == 3);
    assert(scale_by_percent(100, 0) == 0);
    assert(scale_by_percent(3, 0) == 0);
    assert(scale_by_percent(0, 0) == 0);
    assert(scale_by_percent(100, -1) == 100);

    assert(scale_attribute(100, 100) == 100);
    assert(scale_attribute(100, 50) == 50);
    assert(scale_attribute(1, 50) == 1);
    assert(scale_attribute(0, 500) == 0);
    assert(scale_attribute(-1, 500) == -1);
    assert(scale_attribute(INT32_MAX, 500) == INT32_MAX);
    assert(scale_attribute(100, 0) == 1);
    assert(scale_attribute(1, 0) == 1);
    assert(scale_attribute(999999, 0) == 1);
    assert(scale_attribute(1000000, 0) == 1);
    assert(scale_attribute(2000000, 0) == 2);
    assert(scale_attribute(0, 0) == 0);
    assert(scale_attribute(-1, 0) == -1);

    assert(scale_money(100, 100) == 100);
    assert(scale_money(100, 150) == 150);
    assert(scale_money(3, 50) == 1);
    assert(scale_money(0, 500) == 0);
    assert(scale_money(-7, 500) == -7);
    assert(scale_money(INT64_MAX, 500) == INT64_MAX);
    assert(scale_money(100, 0) == 0);
    assert(scale_money(0, 0) == 0);
    assert(scale_money(-7, 0) == -7);
    assert(scale_money(100, -1) == 100);

    assert(scale_cooldown_ticks(165, 100) == 165);
    assert(scale_cooldown_ticks(165, 10) == 16);
    assert(scale_cooldown_ticks(165, 30) == 49);
    assert(scale_cooldown_ticks(165, 50) == 82);
    assert(scale_cooldown_ticks(165, 200) == 330);
    assert(scale_cooldown_ticks(165, 500) == 825);
    assert(scale_cooldown_ticks(1, 10) == 1);
    assert(scale_cooldown_ticks(165, 0) == 0);
    assert(scale_cooldown_ticks(0, 500) == 0);
    assert(scale_cooldown_ticks(-1, 500) == -1);
}

}  // namespace

int main() {
    test_ladder_and_metadata();
    test_roundtrip_and_defaults();
    test_decode_rejects_invalid();
    test_legacy_move_speed_key();
    test_legacy_drop_quality_key();
    test_validate_config();
    test_scaling();
    return 0;
}
