#include "plugins/world_tuning/rates.h"

#include <cassert>
#include <cstring>
#include <string>

namespace {

using namespace qol::contracts;
using namespace qol::features::world_tuning;

void test_ladder_and_metadata() {
    assert(kRateLadderSize == 10);
    assert(kRateLadder[0] == 10 && kRateLadder[4] == 100 && kRateLadder[9] == 500);
    assert(kRateDefault == 100);
    assert(ladder_index(RateId::kGold, 100) == 4);
    assert(ladder_index(RateId::kGold, 101) == -1);
    assert(ladder_index(RateId::kGold, 0) == -1);
    assert(ladder_shift(RateId::kGold, 100, -1) == 80);
    assert(ladder_shift(RateId::kGold, 100, 1) == 120);
    assert(ladder_shift(RateId::kGold, 10, -1) == 10);
    assert(ladder_shift(RateId::kGold, 500, 1) == 500);
    assert(ladder_shift(RateId::kGold, 100, 0) == 100);
    assert(ladder_shift(RateId::kGold, 101, 1) == 101);

    assert(std::strcmp(rate_key(RateId::kGold), "gold") == 0);
    assert(std::strcmp(rate_name(RateId::kGold), "金币倍率") == 0);
    assert(std::strcmp(rate_key(RateId::kExp), "exp") == 0);
    assert(std::strcmp(rate_key(RateId::kPlayerHp), "player_hp") == 0);
    assert(std::strcmp(rate_key(RateId::kEnemyHp), "enemy_hp") == 0);
    assert(std::strcmp(rate_key(RateId::kDamageOut), "damage_out") == 0);
    assert(std::strcmp(rate_key(RateId::kDamageIn), "damage_in") == 0);

    assert(rate_supported(RateId::kGold));
    assert(rate_supported(RateId::kExp));
    assert(!rate_supported(RateId::kPlayerHp));
    assert(rate_supported(RateId::kEnemyHp));
    assert(rate_supported(RateId::kDamageOut));
    assert(rate_supported(RateId::kDamageIn));
    assert(rate_supported(RateId::kDropQuantity));
    assert(rate_supported(RateId::kDropReroll));
    assert(std::strcmp(rate_key(RateId::kDropQuantity), "drop_quantity") == 0);
    assert(std::strcmp(rate_key(RateId::kDropReroll), "drop_reroll") == 0);
    assert(kDropQuantityLadderSize == 6 && kDropQuantityLadder[0] == 0 &&
           kDropQuantityLadder[5] == 5);
    assert(kDropRerollLadderSize == 5 && kDropRerollLadder[0] == 1 &&
           kDropRerollLadder[4] == 5);
    assert(ladder_shift(RateId::kDropQuantity, 1, 1) == 2);
    assert(ladder_shift(RateId::kDropQuantity, 0, -1) == 0);
    assert(ladder_shift(RateId::kDropQuantity, 5, 1) == 5);
    assert(ladder_shift(RateId::kDropReroll, 1, -1) == 1);
    assert(ladder_shift(RateId::kDropReroll, 5, 1) == 5);
    assert(rate_display(RateId::kDropQuantity, 0) == "×0");
    assert(rate_display(RateId::kDropReroll, 3) == "×3");

    assert(rate_display(RateId::kGold, 10) == "×0.1");
    assert(rate_display(RateId::kGold, 100) == "×1.0");
    assert(rate_display(RateId::kGold, 150) == "×1.5");
    assert(rate_display(RateId::kGold, 500) == "×5.0");
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
    assert(reject("{\"unknown\":100}"));
    assert(reject("{\"enemy_hp\":101}"));
    assert(reject("{\"enemy_hp\":0}"));
    assert(reject("{\"enemy_hp\":100.5}"));
    assert(reject("{\"enemy_hp\":\"100\"}"));
    assert(reject("{\"enemy_hp\":100,\"enemy_hp\":50}"));
    assert(reject("{\"enemy_hp\":100}x"));
    assert(reject("{\"enemy_hp\":-100}"));
    assert(reject("{\"enemy_hp\":999999999}"));
    assert(reject("{\"drop_quantity\":6}"));
    assert(reject("{\"drop_reroll\":0}"));
    assert(reject("{\"drop_reroll\":6}"));
}

// 旧配置兼容：移速倍率已移除，已持久化的 rates JSON 仍可能带 move_speed 键，解析须容忍。
void test_legacy_move_speed_key() {
    const auto reject = [](const char* json) {
        Rates parsed{};
        return !decode_rates(json, static_cast<uint32_t>(std::strlen(json)), &parsed);
    };

    // 旧版完整 7 字段 JSON：move_speed 读取后忽略，其余字段正常生效。
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
    assert(json == "{\"gold\":100,\"exp\":100,\"player_hp\":100,\"enemy_hp\":100,"
                   "\"damage_out\":100,\"damage_in\":100,"
                   "\"drop_quantity\":1,\"drop_reroll\":1}");

    // validate_config 同样容忍旧 move_speed 键。
    assert(validate_rates_config("rates", legacy, static_cast<uint32_t>(std::strlen(legacy))).ok ==
           kQolOk);

    // 值规则与现行字段一致：非档位取值仍拒绝。
    assert(reject("{\"move_speed\":101}"));
    // move_speed 重复仍拒绝；move_speed 与未知键并存时未知键仍拒绝。
    assert(reject("{\"move_speed\":100,\"move_speed\":50}"));
    assert(reject("{\"move_speed\":100,\"unknown\":100}"));
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
    assert(!decode_rates(bad_zero, static_cast<uint32_t>(std::strlen(bad_zero)), &parsed));
    const char* bad_high = "{\"drop_quality\":6}";
    assert(!decode_rates(bad_high, static_cast<uint32_t>(std::strlen(bad_high)), &parsed));
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
    const char* bad = "{\"enemy_hp\":55}";
    assert(validate_rates_config("rates", bad, static_cast<uint32_t>(std::strlen(bad))).ok ==
           kQolErrInvalidArgument);
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

    assert(scale_attribute(100, 100) == 100);
    assert(scale_attribute(100, 50) == 50);
    assert(scale_attribute(1, 50) == 1);
    assert(scale_attribute(0, 500) == 0);
    assert(scale_attribute(-1, 500) == -1);
    assert(scale_attribute(INT32_MAX, 500) == INT32_MAX);

    assert(scale_money(100, 100) == 100);
    assert(scale_money(100, 150) == 150);
    assert(scale_money(3, 50) == 1);
    assert(scale_money(0, 500) == 0);
    assert(scale_money(-7, 500) == -7);
    assert(scale_money(INT64_MAX, 500) == INT64_MAX);
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
