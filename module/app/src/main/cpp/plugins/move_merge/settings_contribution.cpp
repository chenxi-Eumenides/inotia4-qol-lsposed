#include "settings_contribution.h"

namespace qol::features::move_merge {
namespace {
using namespace contracts;

constexpr SettingsItemSpec kItems[]{
    {sizeof(SettingsItemSpec), "move_merge.moveMergeEnabled", kSettingsOptimizationCategoryId,
     "move_merge", "拖拽合并", "拖拽同类物品时自动合并。",
     SettingsItemType::kToggle, "moveMergeEnabled", 1, nullptr, nullptr, nullptr},
};
}  // namespace

QolResult register_settings(const contracts::QolHostSdk* sdk) {
    const auto* catalog = contracts::settings_catalog_join_if_available(sdk);
    if (catalog == nullptr) {
        static constexpr char message[] = "settings catalog item joining unavailable; contribution skipped";
        return contracts::error(kQolErrUnsupported, message, sizeof(message) - 1);
    }
    return catalog->register_items("move_merge", kSettingsOptimizationCategoryId,
                                   kItems, sizeof(kItems) / sizeof(kItems[0]));
}
}  // namespace qol::features::move_merge
