#include "plugins/autosell/config_codec.h"
#include "plugins/autosell/store.h"

#include <cstdio>
#include <string>

// host 单测：sidecar v2 JSON、tier 钳制、special 旧字段兼容及新旧字段优先级。

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

using autosell::Config;

namespace {

bool config_equal(const Config& a, const Config& b) {
    return a.enabled == b.enabled && a.rarity == b.rarity && a.enhance == b.enhance &&
           a.socket == b.socket && a.gem_tier == b.gem_tier && a.gem_range == b.gem_range &&
           a.backpack_tier == b.backpack_tier && a.badge_tier == b.badge_tier &&
           a.dice_tier == b.dice_tier;
}

Config sample_config() {
    Config cfg;
    cfg.enabled = true;
    cfg.rarity = 4;
    cfg.enhance = 8;
    cfg.socket = 13;
    cfg.gem_tier = 3;
    cfg.gem_range = 4;
    cfg.backpack_tier = 3;
    cfg.badge_tier = 2;
    cfg.dice_tier = 5;
    return cfg;
}

Config parse(const std::string& json) {
    Config cfg;
    autosell_config_from_json(json.c_str(), &cfg);
    return cfg;
}

}  // namespace

static void test_round_trip_and_keys() {
    const Config cfg = sample_config();
    const std::string json = autosell_config_to_json(cfg);
    CHECK(json.find("\"v\":2") != std::string::npos);
    CHECK(json.find("\"enabled\"") != std::string::npos);
    CHECK(json.find("\"rarity\"") != std::string::npos);
    CHECK(json.find("\"enhance\"") != std::string::npos);
    CHECK(json.find("\"socket\"") != std::string::npos);
    CHECK(json.find("\"gemTier\"") != std::string::npos);
    CHECK(json.find("\"gemRange\"") != std::string::npos);
    CHECK(json.find("\"backpackTier\":3") != std::string::npos);
    CHECK(json.find("\"badgeTier\":2") != std::string::npos);
    CHECK(json.find("\"diceTier\":5") != std::string::npos);
    CHECK(json.find("\"special\":[\"backpack\",\"normalSeal\",\"dice\"]") !=
          std::string::npos);
    CHECK(json.find("specialMask") == std::string::npos);

    Config out;
    CHECK(autosell_config_from_json(json.c_str(), &out));
    CHECK(config_equal(cfg, out));

    const Config defaults;
    Config out2;
    CHECK(autosell_config_from_json(autosell_config_to_json(defaults).c_str(), &out2));
    CHECK(config_equal(defaults, out2));
}

static void test_defaults_and_clamp() {
    const Config empty = parse("{}");
    CHECK(config_equal(empty, Config{}));
    const Config only_enabled = parse("{\"v\":2,\"enabled\":true}");
    CHECK(only_enabled.enabled);
    CHECK(only_enabled.backpack_tier == 0 && only_enabled.badge_tier == 0 &&
          only_enabled.dice_tier == 0);

    const Config cfg = parse(
        "{\"v\":2,\"rarity\":9,\"enhance\":-5,\"socket\":20,\"gemTier\":-3,"
        "\"gemRange\":9,\"backpackTier\":9,\"badgeTier\":99,\"diceTier\":99}");
    CHECK(cfg.rarity == 5 && cfg.enhance == 0 && cfg.socket == 16);
    CHECK(cfg.gem_tier == 0 && cfg.gem_range == 5);
    CHECK(cfg.backpack_tier == 4 && cfg.badge_tier == 4 && cfg.dice_tier == 5);

    const Config negative_badge = parse("{\"badgeTier\":-5}");
    CHECK(negative_badge.badge_tier == 0);

    const Config lows = parse(
        "{\"rarity\":1,\"enhance\":1,\"socket\":1,\"gemTier\":1,\"gemRange\":1,"
        "\"backpackTier\":1,\"badgeTier\":1,\"diceTier\":1}");
    CHECK(lows.rarity == 1 && lows.enhance == 1 && lows.socket == 1);
    CHECK(lows.gem_tier == 1 && lows.gem_range == 1);
    CHECK(lows.backpack_tier == 1 && lows.badge_tier == 1 && lows.dice_tier == 1);
}

static void test_legacy_special_fallback_and_new_precedence() {
    const Config legacy = parse("{\"special\":[\"backpack\",\"normalSeal\",\"dice\"]}");
    CHECK(legacy.backpack_tier == 4);
    CHECK(legacy.badge_tier == 1);
    CHECK(legacy.dice_tier == 4);
    CHECK(parse("{\"special\":[\"dice\"]}").dice_tier == 4);
    CHECK(parse("{\"special\":[\"unknown\",\"dice\"]}").dice_tier == 4);
    CHECK(parse("{\"special\":[]}").backpack_tier == 0);
    CHECK(parse("{\"special\":null}").dice_tier == 0);

    const Config partial = parse(
        "{\"special\":[\"backpack\",\"dice\"],\"backpackTier\":2}");
    CHECK(partial.backpack_tier == 2);
    CHECK(partial.badge_tier == 0);
    CHECK(partial.dice_tier == 4);

    Config applied{};
    CHECK(autosell_apply_special_json("[\"dice\"]", &applied));
    CHECK(applied.backpack_tier == 0 && applied.badge_tier == 0 && applied.dice_tier == 4);
    CHECK(autosell_apply_special_json(
        "{\"special\":[\"backpack\",\"dice\"],\"backpackTier\":1,\"badgeTier\":3,"
        "\"diceTier\":5}", &applied));
    CHECK(applied.backpack_tier == 1 && applied.badge_tier == 3 && applied.dice_tier == 5);
    CHECK(autosell_apply_special_json(nullptr, &applied));
    CHECK(applied.backpack_tier == 1 && applied.badge_tier == 3 && applied.dice_tier == 5);
    CHECK(autosell_apply_special_json("", &applied));
    CHECK(!autosell_apply_special_json("invalid", &applied));
    CHECK(!autosell_apply_special_json("{}", nullptr));

    Config derived{};
    derived.badge_tier = 1;
    derived.dice_tier = 4;
    CHECK(autosell_special_to_array(derived) == "[\"normalSeal\",\"dice\"]");
}

static void test_version_and_bad_json() {
    Config out;
    CHECK(autosell_config_from_json("{\"enabled\":true,\"rarity\":1}", &out));
    CHECK(out.enabled && out.rarity == 1);
    CHECK(autosell_config_from_json("{\"v\":1,\"enabled\":true,\"rarity\":2}", &out));
    CHECK(out.enabled && out.rarity == 2);
    CHECK(autosell_config_from_json("{\"v\":0,\"enabled\":true}", &out));
    CHECK(out.enabled);
    CHECK(!autosell_config_from_json("{\"v\":3,\"enabled\":true}", &out));
    CHECK(config_equal(out, Config{}));
    CHECK(!autosell_config_from_json(nullptr, &out));
    CHECK(config_equal(out, Config{}));
    CHECK(!autosell_config_from_json("not json", &out));
    CHECK(config_equal(out, Config{}));
    CHECK(!autosell_config_from_json("{\"v\":2}", nullptr));
}

int main() {
    test_round_trip_and_keys();
    test_defaults_and_clamp();
    test_legacy_special_fallback_and_new_precedence();
    test_version_and_bad_json();
    std::printf("autosell_store_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
