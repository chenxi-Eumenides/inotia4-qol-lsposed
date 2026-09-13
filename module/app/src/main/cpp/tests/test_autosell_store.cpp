#include "feature/autosell/autosell_store.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// host 单测：自动出售 sidecar section v1 的纯序列化
// autosell_config_to_json / autosell_config_from_json（往返、钳制、版本回退、specialMask）。
// 只包含 store 头（inline 纯逻辑）+ rules 头，无 Android/游戏依赖。

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

using autosell::Config;

namespace {

bool config_equal(const Config& a, const Config& b) {
    return a.enabled == b.enabled &&
           a.rarity_enabled == b.rarity_enabled && a.rarity_threshold == b.rarity_threshold &&
           a.enhance_enabled == b.enhance_enabled && a.enhance_threshold == b.enhance_threshold &&
           a.socket_enabled == b.socket_enabled && a.socket_threshold == b.socket_threshold &&
           a.gem_tier_enabled == b.gem_tier_enabled &&
           a.gem_tier_threshold == b.gem_tier_threshold &&
           a.special_enabled == b.special_enabled && a.special_mask == b.special_mask;
}

Config sample_config() {
    Config cfg;
    cfg.enabled = true;
    cfg.rarity_enabled = true;
    cfg.rarity_threshold = 3;
    cfg.enhance_enabled = true;
    cfg.enhance_threshold = 7;
    cfg.socket_enabled = true;
    cfg.socket_threshold = 12;
    cfg.gem_tier_enabled = true;
    cfg.gem_tier_threshold = 2;
    cfg.special_mask = autosell::kSpecialBackpack | autosell::kSpecialDice;
    cfg.special_enabled = true;
    return cfg;
}

Config parse(const std::string& json) {
    Config cfg;
    autosell_config_from_json(json.c_str(), &cfg);
    return cfg;
}

}  // namespace

static void test_round_trip() {
    const Config cfg = sample_config();
    const std::string json = autosell_config_to_json(cfg);
    CHECK(json.find("\"v\":1") != std::string::npos);
    Config out;
    CHECK(autosell_config_from_json(json.c_str(), &out));
    CHECK(config_equal(cfg, out));

    // 默认配置往返恒等。
    const Config defaults;
    Config out2;
    CHECK(autosell_config_from_json(autosell_config_to_json(defaults).c_str(), &out2));
    CHECK(config_equal(defaults, out2));
}

static void test_clamp_out_of_range() {
    // 越界阈值按边界钳制。
    const Config cfg = parse(
        "{\"v\":1,\"rarityThreshold\":9,\"enhanceThreshold\":-5,"
        "\"socketThreshold\":20,\"gemTierThreshold\":-3}");
    CHECK(cfg.rarity_threshold == 4);
    CHECK(cfg.enhance_threshold == 0);
    CHECK(cfg.socket_threshold == 15);
    CHECK(cfg.gem_tier_threshold == 0);

    // 正常范围内保留。
    const Config cfg2 = parse(
        "{\"v\":1,\"rarityThreshold\":4,\"enhanceThreshold\":30,"
        "\"socketThreshold\":15,\"gemTierThreshold\":4}");
    CHECK(cfg2.rarity_threshold == 4);
    CHECK(cfg2.enhance_threshold == 30);
    CHECK(cfg2.socket_threshold == 15);
    CHECK(cfg2.gem_tier_threshold == 4);
}

static void test_version_fallback() {
    // v 缺失按 v1 解析字段。
    const Config missing = parse("{\"enabled\":true,\"rarityThreshold\":1}");
    CHECK(missing.enabled);
    CHECK(missing.rarity_threshold == 1);

    // v=1 正常。
    Config out;
    CHECK(autosell_config_from_json("{\"v\":1,\"enabled\":true}", &out));
    CHECK(out.enabled);

    // v<=1（含 0/负数）按 v1。
    CHECK(autosell_config_from_json("{\"v\":0,\"enabled\":true}", &out));
    CHECK(out.enabled);
    CHECK(autosell_config_from_json("{\"v\":-2,\"enabled\":true}", &out));
    CHECK(out.enabled);

    // 未知未来版本 v=2：回退默认且返回 false。
    out = sample_config();
    CHECK(!autosell_config_from_json("{\"v\":2,\"enabled\":true,\"rarityThreshold\":3}", &out));
    CHECK(config_equal(out, Config{}));
}

static void test_special_mask_mapping() {
    // mask=0 -> special_enabled 关闭。
    const Config zero = parse("{\"v\":1,\"specialMask\":0}");
    CHECK(!zero.special_enabled);
    CHECK(zero.special_mask == 0u);

    // mask 非 0 -> special_enabled 打开且位保留（含组合位）。
    const uint32_t mask = autosell::kSpecialBackpack | autosell::kSpecialItemBox;
    Config out;
    const std::string json = "{\"v\":1,\"specialMask\":" + std::to_string(mask) + "}";
    CHECK(autosell_config_from_json(json.c_str(), &out));
    CHECK(out.special_enabled);
    CHECK(out.special_mask == mask);

    // 往返保留组合位。
    Config cfg;
    cfg.special_mask = mask;
    cfg.special_enabled = true;
    Config rt;
    CHECK(autosell_config_from_json(autosell_config_to_json(cfg).c_str(), &rt));
    CHECK(rt.special_mask == mask);
    CHECK(rt.special_enabled);
}

static void test_bad_json_and_unknown_keys() {
    Config out = sample_config();
    CHECK(!autosell_config_from_json(nullptr, &out));
    CHECK(config_equal(out, Config{}));
    CHECK(!autosell_config_from_json("not json at all", &out));
    CHECK(config_equal(out, Config{}));
    CHECK(!autosell_config_from_json("", &out));

    // 未知键忽略，已知键仍生效。
    const Config cfg = parse("{\"v\":1,\"enabled\":true,\"futureKey\":123,\"nested\":{\"x\":1}}");
    CHECK(cfg.enabled);

    // 字段缺失取默认。
    const Config empty_obj = parse("{}");
    CHECK(config_equal(empty_obj, Config{}));

    // out 为空指针安全。
    CHECK(!autosell_config_from_json("{\"v\":1}", nullptr));
}

int main() {
    test_round_trip();
    test_clamp_out_of_range();
    test_version_fallback();
    test_special_mask_mapping();
    test_bad_json_and_unknown_keys();
    std::printf("autosell_store_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
