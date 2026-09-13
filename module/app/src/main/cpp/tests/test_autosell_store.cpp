#include "feature/autosell/autosell_store.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// host 单测：自动出售 sidecar section v1 的纯序列化
// autosell_config_to_json / autosell_config_from_json（新 JSON 键、往返、钳制、缺省、版本回退、
// specialMask/gemRange 非负与边界）。只包含 store 头（inline 纯逻辑）+ rules 头，无 Android/游戏依赖。

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
           a.special_mask == b.special_mask;
}

Config sample_config() {
    Config cfg;
    cfg.enabled = true;
    cfg.rarity = 4;    // 1-based 档位（阈值 3）
    cfg.enhance = 8;   // 阈值 7
    cfg.socket = 13;   // 阈值 12
    cfg.gem_tier = 3;  // 阈值 2
    cfg.gem_range = 4;  // 阈值 90
    cfg.special_mask = autosell::kSpecialBackpack | autosell::kSpecialDice;
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
    // 新 JSON 契约键：v1 + enabled/rarity/enhance/socket/gemTier/gemRange/specialMask。
    CHECK(json.find("\"v\":1") != std::string::npos);
    CHECK(json.find("\"enabled\"") != std::string::npos);
    CHECK(json.find("\"rarity\"") != std::string::npos);
    CHECK(json.find("\"enhance\"") != std::string::npos);
    CHECK(json.find("\"socket\"") != std::string::npos);
    CHECK(json.find("\"gemTier\"") != std::string::npos);
    CHECK(json.find("\"gemRange\"") != std::string::npos);
    CHECK(json.find("\"specialMask\"") != std::string::npos);
    // 旧键不得出现。
    CHECK(json.find("Enabled") == std::string::npos);
    CHECK(json.find("Threshold") == std::string::npos);

    Config out;
    CHECK(autosell_config_from_json(json.c_str(), &out));
    CHECK(config_equal(cfg, out));

    // 默认配置往返恒等。
    const Config defaults;
    Config out2;
    CHECK(autosell_config_from_json(autosell_config_to_json(defaults).c_str(), &out2));
    CHECK(config_equal(defaults, out2));
}

static void test_defaults_when_missing() {
    // 字段缺失取默认 0（关闭），enabled 默认 false。
    const Config empty_obj = parse("{}");
    CHECK(config_equal(empty_obj, Config{}));
    CHECK(empty_obj.rarity == 0);
    CHECK(empty_obj.enhance == 0);
    CHECK(empty_obj.socket == 0);
    CHECK(empty_obj.gem_tier == 0);
    CHECK(empty_obj.gem_range == 0);
    CHECK(empty_obj.special_mask == 0u);

    // 只有 enabled 时其余保持关闭。
    const Config only_enabled = parse("{\"v\":1,\"enabled\":true}");
    CHECK(only_enabled.enabled);
    CHECK(only_enabled.rarity == 0);
    CHECK(only_enabled.gem_tier == 0);
}

static void test_clamp_out_of_range() {
    // 越界值按边界钳制：rarity/gemTier/gemRange 0..5、enhance 0..32、socket 0..16。
    const Config cfg = parse(
        "{\"v\":1,\"rarity\":9,\"enhance\":-5,\"socket\":20,\"gemTier\":-3,"
        "\"gemRange\":9,\"specialMask\":-1}");
    CHECK(cfg.rarity == 5);
    CHECK(cfg.enhance == 0);
    CHECK(cfg.socket == 16);
    CHECK(cfg.gem_tier == 0);
    CHECK(cfg.gem_range == 5);
    CHECK(cfg.special_mask == 0u);  // 负 mask 归 0

    // gemRange 负值 -> 0（关闭）。
    const Config gem_neg = parse("{\"v\":1,\"gemRange\":-4}");
    CHECK(gem_neg.gem_range == 0);

    // 正常范围内保留（含各维最大值/最小值）。
    const Config cfg2 = parse(
        "{\"v\":1,\"rarity\":5,\"enhance\":32,\"socket\":16,\"gemTier\":5,\"gemRange\":5}");
    CHECK(cfg2.rarity == 5);
    CHECK(cfg2.enhance == 32);
    CHECK(cfg2.socket == 16);
    CHECK(cfg2.gem_tier == 5);
    CHECK(cfg2.gem_range == 5);

    const Config cfg3 = parse(
        "{\"v\":1,\"rarity\":1,\"enhance\":1,\"socket\":1,\"gemTier\":1,\"gemRange\":1}");
    CHECK(cfg3.rarity == 1);
    CHECK(cfg3.enhance == 1);
    CHECK(cfg3.socket == 1);
    CHECK(cfg3.gem_tier == 1);
    CHECK(cfg3.gem_range == 1);
}

static void test_version_fallback() {
    // v 缺失按 v1 解析字段。
    const Config missing = parse("{\"enabled\":true,\"rarity\":1}");
    CHECK(missing.enabled);
    CHECK(missing.rarity == 1);

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
    CHECK(!autosell_config_from_json("{\"v\":2,\"enabled\":true,\"rarity\":3}", &out));
    CHECK(config_equal(out, Config{}));
}

static void test_special_mask_mapping() {
    // mask=0 -> 关闭（无独立 specialEnabled 字段）。
    const Config zero = parse("{\"v\":1,\"specialMask\":0}");
    CHECK(zero.special_mask == 0u);

    // mask 非 0 -> 位保留（含组合位）。
    const uint32_t mask = autosell::kSpecialBackpack | autosell::kSpecialItemBox;
    Config out;
    const std::string json = "{\"v\":1,\"specialMask\":" + std::to_string(mask) + "}";
    CHECK(autosell_config_from_json(json.c_str(), &out));
    CHECK(out.special_mask == mask);

    // 往返保留组合位。
    Config cfg;
    cfg.special_mask = mask;
    Config rt;
    CHECK(autosell_config_from_json(autosell_config_to_json(cfg).c_str(), &rt));
    CHECK(rt.special_mask == mask);
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

    // out 为空指针安全。
    CHECK(!autosell_config_from_json("{\"v\":1}", nullptr));
}

int main() {
    test_round_trip_and_keys();
    test_defaults_when_missing();
    test_clamp_out_of_range();
    test_version_fallback();
    test_special_mask_mapping();
    test_bad_json_and_unknown_keys();
    std::printf("autosell_store_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
