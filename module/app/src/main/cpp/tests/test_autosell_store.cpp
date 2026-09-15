#include "feature/autosell/autosell_store.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// host 单测：自动出售 sidecar section v2 的纯序列化
// autosell_config_to_json / autosell_config_from_json（v2 键、往返、钳制、缺省、版本回退、
// special 名字数组解析与生成、旧 specialMask 键忽略）+ 名字<->位 helper。
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
    // v2 JSON 契约键：v + enabled/rarity/enhance/socket/gemTier/gemRange/special。
    CHECK(json.find("\"v\":2") != std::string::npos);
    CHECK(json.find("\"enabled\"") != std::string::npos);
    CHECK(json.find("\"rarity\"") != std::string::npos);
    CHECK(json.find("\"enhance\"") != std::string::npos);
    CHECK(json.find("\"socket\"") != std::string::npos);
    CHECK(json.find("\"gemTier\"") != std::string::npos);
    CHECK(json.find("\"gemRange\"") != std::string::npos);
    CHECK(json.find("\"special\":[\"backpack\",\"dice\"]") != std::string::npos);
    // 旧键与历史成对开关键不得出现。
    CHECK(json.find("specialMask") == std::string::npos);
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

    // 全三种特殊类型往返。
    Config all;
    all.special_mask = autosell::kSpecialBackpack | autosell::kSpecialNormalSeal |
                       autosell::kSpecialDice;
    Config out3;
    CHECK(autosell_config_from_json(autosell_config_to_json(all).c_str(), &out3));
    CHECK(out3.special_mask == all.special_mask);
}

static void test_defaults_when_missing() {
    // 字段缺失取默认 0（关闭），enabled 默认 false，special 默认空集。
    const Config empty_obj = parse("{}");
    CHECK(config_equal(empty_obj, Config{}));
    CHECK(empty_obj.special_mask == 0u);

    // 只有 enabled 时其余保持关闭。
    const Config only_enabled = parse("{\"v\":2,\"enabled\":true}");
    CHECK(only_enabled.enabled);
    CHECK(only_enabled.rarity == 0);
    CHECK(only_enabled.gem_tier == 0);
    CHECK(only_enabled.special_mask == 0u);
}

static void test_clamp_out_of_range() {
    // 越界值按边界钳制：rarity/gemTier/gemRange 0..5、enhance 0..32、socket 0..16。
    const Config cfg = parse(
        "{\"v\":2,\"rarity\":9,\"enhance\":-5,\"socket\":20,\"gemTier\":-3,"
        "\"gemRange\":9,\"special\":[\"unknown\"]}");
    CHECK(cfg.rarity == 5);
    CHECK(cfg.enhance == 0);
    CHECK(cfg.socket == 16);
    CHECK(cfg.gem_tier == 0);
    CHECK(cfg.gem_range == 5);
    CHECK(cfg.special_mask == 0u);  // 未知名忽略

    // gemRange 负值 -> 0（关闭）。
    const Config gem_neg = parse("{\"v\":2,\"gemRange\":-4}");
    CHECK(gem_neg.gem_range == 0);

    // 正常范围内保留（含各维最大值/最小值）。
    const Config cfg2 = parse(
        "{\"v\":2,\"rarity\":5,\"enhance\":32,\"socket\":16,\"gemTier\":5,\"gemRange\":5}");
    CHECK(cfg2.rarity == 5);
    CHECK(cfg2.enhance == 32);
    CHECK(cfg2.socket == 16);
    CHECK(cfg2.gem_tier == 5);
    CHECK(cfg2.gem_range == 5);

    const Config cfg3 = parse(
        "{\"v\":2,\"rarity\":1,\"enhance\":1,\"socket\":1,\"gemTier\":1,\"gemRange\":1}");
    CHECK(cfg3.rarity == 1);
    CHECK(cfg3.enhance == 1);
    CHECK(cfg3.socket == 1);
    CHECK(cfg3.gem_tier == 1);
    CHECK(cfg3.gem_range == 1);
}

static void test_version_handling() {
    // v 缺失按 2 解析字段。
    const Config missing = parse("{\"enabled\":true,\"rarity\":1,\"special\":[\"dice\"]}");
    CHECK(missing.enabled);
    CHECK(missing.rarity == 1);
    CHECK(missing.special_mask == autosell::kSpecialDice);

    Config out;
    // v=2 正常。
    CHECK(autosell_config_from_json("{\"v\":2,\"enabled\":true}", &out));
    CHECK(out.enabled);

    // v<=2（含 1/0/负数，即旧存档）不整段回退：照常读取同名键。
    CHECK(autosell_config_from_json("{\"v\":1,\"enabled\":true,\"rarity\":2}", &out));
    CHECK(out.enabled);
    CHECK(out.rarity == 2);
    CHECK(autosell_config_from_json("{\"v\":0,\"enabled\":true}", &out));
    CHECK(out.enabled);
    CHECK(autosell_config_from_json("{\"v\":-2,\"enabled\":true}", &out));
    CHECK(out.enabled);

    // 未知未来版本 v=3：回退默认且返回 false。
    out = sample_config();
    CHECK(!autosell_config_from_json("{\"v\":3,\"enabled\":true,\"rarity\":3}", &out));
    CHECK(config_equal(out, Config{}));
}

static void test_legacy_special_mask_ignored() {
    // v1 的 specialMask 键不再读取（不做迁移）：任意值均被忽略 -> 空集。
    const Config legacy = parse("{\"v\":1,\"enabled\":true,\"rarity\":2,\"specialMask\":7}");
    CHECK(legacy.enabled);
    CHECK(legacy.rarity == 2);
    CHECK(legacy.special_mask == 0u);

    // specialMask 与 special 并存时只认 special。
    const Config both = parse("{\"v\":2,\"specialMask\":3,\"special\":[\"dice\"]}");
    CHECK(both.special_mask == autosell::kSpecialDice);
}

static void test_special_array_parsing() {
    // 单名 -> 位。
    CHECK(parse("{\"special\":[\"backpack\"]}").special_mask == autosell::kSpecialBackpack);
    CHECK(parse("{\"special\":[\"normalSeal\"]}").special_mask == autosell::kSpecialNormalSeal);
    CHECK(parse("{\"special\":[\"dice\"]}").special_mask == autosell::kSpecialDice);

    // 多选组合。
    CHECK(parse("{\"special\":[\"backpack\",\"normalSeal\",\"dice\"]}").special_mask ==
          (autosell::kSpecialBackpack | autosell::kSpecialNormalSeal | autosell::kSpecialDice));
    CHECK(parse("{\"special\":[\"dice\",\"backpack\"]}").special_mask ==
          (autosell::kSpecialBackpack | autosell::kSpecialDice));

    // 空数组 / 缺失 / null -> 空集。
    CHECK(parse("{\"special\":[]}").special_mask == 0u);
    CHECK(parse("{\"special\":null}").special_mask == 0u);

    // 容错空白。
    CHECK(parse("{ \"special\" : [ \"backpack\" ,  \"dice\" ] }").special_mask ==
          (autosell::kSpecialBackpack | autosell::kSpecialDice));

    // 未知名忽略，知名保留。
    CHECK(parse("{\"special\":[\"heroSeal\",\"dice\",\"\"]}").special_mask ==
          autosell::kSpecialDice);

    // 非数组值 / 坏值 -> 空集（其余键照常）。
    const Config bad = parse("{\"v\":2,\"enabled\":true,\"special\":7}");
    CHECK(bad.enabled);
    CHECK(bad.special_mask == 0u);
    CHECK(parse("{\"special\":\"dice\"}").special_mask == 0u);
    CHECK(parse("{\"special\":[7,\"dice\"]}").special_mask == autosell::kSpecialDice);

    // 数组内元素含空格容错。
    CHECK(parse("{\"special\":[\"dice \",\" normalSeal\"]}").special_mask ==
          (autosell::kSpecialNormalSeal | autosell::kSpecialDice));
}

static void test_special_name_bit_helpers() {
    // 位 -> 名。
    CHECK(std::strcmp(autosell_special_name(autosell::kSpecialBackpack), "backpack") == 0);
    CHECK(std::strcmp(autosell_special_name(autosell::kSpecialNormalSeal), "normalSeal") == 0);
    CHECK(std::strcmp(autosell_special_name(autosell::kSpecialDice), "dice") == 0);
    CHECK(autosell_special_name(0) == nullptr);
    CHECK(autosell_special_name(1u << 7) == nullptr);

    // 名 -> 位；未知名/空/nullptr -> 0。
    CHECK(autosell_special_bit("backpack") == autosell::kSpecialBackpack);
    CHECK(autosell_special_bit("normalSeal") == autosell::kSpecialNormalSeal);
    CHECK(autosell_special_bit("dice") == autosell::kSpecialDice);
    CHECK(autosell_special_bit("Dice") == 0u);  // 大小写敏感
    CHECK(autosell_special_bit("") == 0u);
    CHECK(autosell_special_bit(nullptr) == 0u);

    // 数组文本解析（JNI 入参口径）。
    CHECK(autosell_special_parse_array("[\"dice\"]") == autosell::kSpecialDice);
    CHECK(autosell_special_parse_array("[]") == 0u);
    CHECK(autosell_special_parse_array(nullptr) == 0u);
    CHECK(autosell_special_parse_array("not a json") == 0u);
    CHECK(autosell_special_parse_array("[ \"backpack\" ]") == autosell::kSpecialBackpack);
    // 未闭合容错：取已有内容。
    CHECK(autosell_special_parse_array("[\"dice\"") == autosell::kSpecialDice);

    // 位掩码 -> 数组文本；顺序固定 backpack -> normalSeal -> dice。
    CHECK(autosell_special_to_array(0) == "[]");
    CHECK(autosell_special_to_array(autosell::kSpecialDice) == "[\"dice\"]");
    CHECK(autosell_special_to_array(autosell::kSpecialDice | autosell::kSpecialBackpack) ==
          "[\"backpack\",\"dice\"]");
    CHECK(autosell_special_to_array(autosell::kSpecialNormalSeal | autosell::kSpecialDice |
                                    autosell::kSpecialBackpack) ==
          "[\"backpack\",\"normalSeal\",\"dice\"]");
    // 未定义位不输出，往返只保留已知名字。
    CHECK(autosell_special_to_array((1u << 5) | autosell::kSpecialDice) == "[\"dice\"]");
    CHECK(autosell_special_parse_array(
              autosell_special_to_array((1u << 5) | autosell::kSpecialDice).c_str()) ==
          autosell::kSpecialDice);
}

static void test_bad_json_and_unknown_keys() {
    Config out = sample_config();
    CHECK(!autosell_config_from_json(nullptr, &out));
    CHECK(config_equal(out, Config{}));
    CHECK(!autosell_config_from_json("not json at all", &out));
    CHECK(config_equal(out, Config{}));
    CHECK(!autosell_config_from_json("", &out));

    // 未知键忽略，已知键仍生效。
    const Config cfg = parse("{\"v\":2,\"enabled\":true,\"futureKey\":123,\"nested\":{\"x\":1}}");
    CHECK(cfg.enabled);

    // out 为空指针安全。
    CHECK(!autosell_config_from_json("{\"v\":2}", nullptr));
}

int main() {
    test_round_trip_and_keys();
    test_defaults_when_missing();
    test_clamp_out_of_range();
    test_version_handling();
    test_legacy_special_mask_ignored();
    test_special_array_parsing();
    test_special_name_bit_helpers();
    test_bad_json_and_unknown_keys();
    std::printf("autosell_store_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
