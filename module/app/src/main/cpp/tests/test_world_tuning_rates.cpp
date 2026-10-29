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
    assert(ladder_index(100) == 4);
    assert(ladder_index(101) == -1);
    assert(ladder_index(0) == -1);
    assert(ladder_shift(100, -1) == 80);
    assert(ladder_shift(100, 1) == 120);
    assert(ladder_shift(10, -1) == 10);
    assert(ladder_shift(500, 1) == 500);
    assert(ladder_shift(100, 0) == 100);
    assert(ladder_shift(101, 1) == 101);

    assert(std::strcmp(rate_key(RateId::kGold), "gold") == 0);
    assert(std::strcmp(rate_name(RateId::kGold), "金币倍率") == 0);
    assert(std::strcmp(rate_key(RateId::kExp), "exp") == 0);
    assert(std::strcmp(rate_key(RateId::kPlayerHp), "player_hp") == 0);
    assert(std::strcmp(rate_key(RateId::kEnemyHp), "enemy_hp") == 0);
    assert(std::strcmp(rate_key(RateId::kDamageOut), "damage_out") == 0);
    assert(std::strcmp(rate_key(RateId::kDamageIn), "damage_in") == 0);
    assert(std::strcmp(rate_key(RateId::kMoveSpeed), "move_speed") == 0);

    assert(rate_supported(RateId::kGold));
    assert(rate_supported(RateId::kExp));
    assert(!rate_supported(RateId::kPlayerHp));
    assert(rate_supported(RateId::kEnemyHp));
    assert(rate_supported(RateId::kDamageOut));
    assert(rate_supported(RateId::kDamageIn));
    assert(!rate_supported(RateId::kMoveSpeed));

    assert(rate_display(10) == "×0.1");
    assert(rate_display(100) == "×1.0");
    assert(rate_display(150) == "×1.5");
    assert(rate_display(500) == "×5.0");
}

void test_roundtrip_and_defaults() {
    Rates defaults = default_rates();
    for (int32_t value : defaults.value) assert(value == 100);

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
    test_validate_config();
    test_scaling();
    return 0;
}
