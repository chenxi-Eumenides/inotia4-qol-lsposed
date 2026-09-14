// host 单测：功能抽象层 game_feature（能力位 → 功能状态、monster 系列特判、名称表）。
// 链接 core/native/game_feature.cpp + game_variant.cpp + qol_log.cpp；Android 头由 stubs/ 覆盖。
#include "core/native/game_feature.h"

#include "core/native/game_variant.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

using qol::GameFeature;
using qol::FeatureState;
namespace detail = qol::game_feature_detail;

constexpr uint32_t kWh4 = qol::kCapWarehouse;
constexpr uint32_t kInline = qol::kCapWarehouseInline;
constexpr uint32_t kHidden = qol::kCapHiddenSegment;

void test_supported_by_caps() {
    // 无任何能力位：四个功能都不被能力位支持。
    CHECK(!detail::supported_by_caps(0, GameFeature::kPersonalWarehouse));
    CHECK(!detail::supported_by_caps(0, GameFeature::kWarehouseInlineInSave));
    CHECK(!detail::supported_by_caps(0, GameFeature::kWarehouseCompanionFile));
    CHECK(!detail::supported_by_caps(0, GameFeature::kItemCountUpperBound));

    // 仅 wh4：个人仓库 + 伴生文件为真，内嵌为假。
    CHECK(detail::supported_by_caps(kWh4, GameFeature::kPersonalWarehouse));
    CHECK(!detail::supported_by_caps(kWh4, GameFeature::kWarehouseInlineInSave));
    CHECK(detail::supported_by_caps(kWh4, GameFeature::kWarehouseCompanionFile));

    // 仅 inline：个人仓库 + 内嵌为真，伴生文件为假。
    CHECK(detail::supported_by_caps(kInline, GameFeature::kPersonalWarehouse));
    CHECK(detail::supported_by_caps(kInline, GameFeature::kWarehouseInlineInSave));
    CHECK(!detail::supported_by_caps(kInline, GameFeature::kWarehouseCompanionFile));

    // 两者都有：个人仓库两个子形态都为真。
    CHECK(detail::supported_by_caps(kWh4 | kInline, GameFeature::kPersonalWarehouse));
    CHECK(detail::supported_by_caps(kWh4 | kInline, GameFeature::kWarehouseInlineInSave));
    CHECK(detail::supported_by_caps(kWh4 | kInline, GameFeature::kWarehouseCompanionFile));

    // 隐藏段位不表达仓库能力；kItemCountUpperBound 不由能力位编码，恒为假。
    CHECK(!detail::supported_by_caps(kHidden, GameFeature::kPersonalWarehouse));
    CHECK(!detail::supported_by_caps(0xffffffffu, GameFeature::kItemCountUpperBound));
}

void test_state_from_unknown() {
    // 构建未知：任何功能、任何能力位都返回 kUnknown。
    CHECK(detail::state_from(0, false, true, false, GameFeature::kPersonalWarehouse) ==
          FeatureState::kUnknown);
    CHECK(detail::state_from(kWh4, false, true, true, GameFeature::kPersonalWarehouse) ==
          FeatureState::kUnknown);
    CHECK(detail::state_from(kWh4, false, true, true, GameFeature::kItemCountUpperBound) ==
          FeatureState::kUnknown);
}

void test_state_from_unsupported() {
    // 构建已知但能力位不含该功能。
    CHECK(detail::state_from(0, true, true, false, GameFeature::kPersonalWarehouse) ==
          FeatureState::kUnsupported);
    CHECK(detail::state_from(kInline, true, true, false, GameFeature::kWarehouseCompanionFile) ==
          FeatureState::kUnsupported);
    CHECK(detail::state_from(kWh4, true, true, false, GameFeature::kWarehouseInlineInSave) ==
          FeatureState::kUnsupported);
    // 非 monster 系列：物品数量上界功能不支持。
    CHECK(detail::state_from(0, true, true, false, GameFeature::kItemCountUpperBound) ==
          FeatureState::kUnsupported);
    CHECK(detail::state_from(kHidden, true, true, false, GameFeature::kItemCountUpperBound) ==
          FeatureState::kUnsupported);
}

void test_state_from_available_and_unavailable() {
    // 支持且可用。
    CHECK(detail::state_from(kWh4, true, true, false, GameFeature::kWarehouseCompanionFile) ==
          FeatureState::kAvailable);
    CHECK(detail::state_from(kInline, true, true, false, GameFeature::kWarehouseInlineInSave) ==
          FeatureState::kAvailable);
    // 支持但运行时条件不满足 → 降级 kUnavailable。
    CHECK(detail::state_from(kWh4, true, false, false, GameFeature::kWarehouseCompanionFile) ==
          FeatureState::kUnavailable);
    CHECK(detail::state_from(kInline | kWh4, true, false, false, GameFeature::kPersonalWarehouse) ==
          FeatureState::kUnavailable);
}

void test_state_from_monster_item_count() {
    // monster 系列 → 物品数量上界功能支持（即使无对应能力位）。
    CHECK(detail::state_from(0, true, true, true, GameFeature::kItemCountUpperBound) ==
          FeatureState::kAvailable);
    CHECK(detail::state_from(0, true, false, true, GameFeature::kItemCountUpperBound) ==
          FeatureState::kUnavailable);
    // monster 系列不影响仓库类功能的能力位判定。
    CHECK(detail::state_from(0, true, true, true, GameFeature::kPersonalWarehouse) ==
          FeatureState::kUnsupported);
}

void test_feature_names() {
    CHECK(std::strcmp(qol::game_feature_name(GameFeature::kPersonalWarehouse),
                      "personal_warehouse") == 0);
    CHECK(std::strcmp(qol::game_feature_name(GameFeature::kWarehouseInlineInSave),
                      "warehouse_inline_in_save") == 0);
    CHECK(std::strcmp(qol::game_feature_name(GameFeature::kWarehouseCompanionFile),
                      "warehouse_companion_file") == 0);
    CHECK(std::strcmp(qol::game_feature_name(GameFeature::kItemCountUpperBound),
                      "item_count_upper_bound") == 0);
}

void test_state_names() {
    CHECK(std::strcmp(qol::game_feature_state_name(FeatureState::kUnknown), "unknown") == 0);
    CHECK(std::strcmp(qol::game_feature_state_name(FeatureState::kUnsupported), "unsupported") == 0);
    CHECK(std::strcmp(qol::game_feature_state_name(FeatureState::kAvailable), "available") == 0);
    CHECK(std::strcmp(qol::game_feature_state_name(FeatureState::kUnavailable), "unavailable") == 0);
}

void test_runtime_state_before_variant_init() {
    // 测试进程未调用 game_variant_init()：game_variant 为未命中/未初始化 → kUnknown。
    CHECK(qol::game_feature_state(GameFeature::kPersonalWarehouse) == FeatureState::kUnknown);
}

}  // namespace

int main() {
    test_supported_by_caps();
    test_state_from_unknown();
    test_state_from_unsupported();
    test_state_from_available_and_unavailable();
    test_state_from_monster_item_count();
    test_feature_names();
    test_state_names();
    test_runtime_state_before_variant_init();
    std::printf("game_feature_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
