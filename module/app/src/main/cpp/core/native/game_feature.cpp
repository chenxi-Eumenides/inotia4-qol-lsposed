// game_feature.cpp —— game_variant 之上的功能抽象层实现。
//
// 依赖方向：core 层，仅依赖 core/native/game_variant.h 与 STL；不依赖 game_access.h。
#include "core/native/game_feature.h"

#include "core/native/game_variant.h"

#include <atomic>

namespace qol {
namespace {

std::atomic<FeatureUsabilityFn> g_usability_fn{nullptr};

bool usability_ok(GameFeature f) {
    FeatureUsabilityFn fn = g_usability_fn.load(std::memory_order_acquire);
    return fn == nullptr ? true : fn(f);
}

}  // namespace

namespace game_feature_detail {

bool supported_by_caps(uint32_t caps, GameFeature f) {
    switch (f) {
        case GameFeature::kPersonalWarehouse:
            return (caps & (kCapWarehouse | kCapWarehouseInline)) != 0;
        case GameFeature::kWarehouseInlineInSave:
            return (caps & kCapWarehouseInline) != 0;
        case GameFeature::kWarehouseCompanionFile:
            return (caps & kCapWarehouse) != 0;
        case GameFeature::kItemCountUpperBound:
            // 能力位未编码「monster 自写包装器」；由 state_from 的 series_is_monster 判定。
            return false;
    }
    return false;
}

FeatureState state_from(uint32_t caps, bool variant_known, bool usability_ok,
                        bool series_is_monster, GameFeature f) {
    if (!variant_known) return FeatureState::kUnknown;
    const bool supported = (f == GameFeature::kItemCountUpperBound)
                               ? series_is_monster
                               : supported_by_caps(caps, f);
    if (!supported) return FeatureState::kUnsupported;
    if (!usability_ok) return FeatureState::kUnavailable;
    return FeatureState::kAvailable;
}

}  // namespace game_feature_detail

const char* game_feature_name(GameFeature f) {
    switch (f) {
        case GameFeature::kPersonalWarehouse: return "personal_warehouse";
        case GameFeature::kWarehouseInlineInSave: return "warehouse_inline_in_save";
        case GameFeature::kWarehouseCompanionFile: return "warehouse_companion_file";
        case GameFeature::kItemCountUpperBound: return "item_count_upper_bound";
    }
    return "unknown";
}

const char* game_feature_state_name(FeatureState s) {
    switch (s) {
        case FeatureState::kUnknown: return "unknown";
        case FeatureState::kUnsupported: return "unsupported";
        case FeatureState::kAvailable: return "available";
        case FeatureState::kUnavailable: return "unavailable";
    }
    return "unknown";
}

FeatureState game_feature_state(GameFeature f) {
    const GameVariant& variant = game_variant();
    const bool series_is_monster = variant.series == GameSeries::kMonster;
    return game_feature_detail::state_from(variant.capabilities, variant.known,
                                           usability_ok(f), series_is_monster, f);
}

void game_feature_set_usability_fn(FeatureUsabilityFn fn) {
    g_usability_fn.store(fn, std::memory_order_release);
}

}  // namespace qol
