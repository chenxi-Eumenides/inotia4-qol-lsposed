#include "settings_contribution.h"

namespace qol::features::stack_limit {
namespace {
using namespace contracts;
constexpr SettingsItemSpec kItems[]{
    {sizeof(SettingsItemSpec), "stack_limit.stackLimitIncrease", kSettingsOptimizationCategoryId, "stack_limit",
      "堆叠上限", "将可堆叠物品上限提高到 999。",
     SettingsItemType::kToggle, "stackLimitIncrease", 1, nullptr, nullptr, nullptr},
};
}  // namespace

QolResult register_settings(const contracts::QolHostSdk* sdk) {
    const auto* catalog = contracts::settings_catalog_join_if_available(sdk);
    if (catalog == nullptr) {
        static constexpr char message[] = "settings catalog item joining unavailable; contribution skipped";
        return contracts::error(kQolErrUnsupported, message, sizeof(message) - 1);
    }
    return catalog->register_items("stack_limit", kSettingsOptimizationCategoryId,
                                   kItems, sizeof(kItems) / sizeof(kItems[0]));
}
}  // namespace qol::features::stack_limit
