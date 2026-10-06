#include "settings_contribution.h"

namespace qol::features::custom_recipe {
namespace {
using namespace contracts;
constexpr SettingsItemSpec kItems[]{
    {sizeof(SettingsItemSpec), "custom_recipe.customRecipeEnabled", kSettingsOptimizationCategoryId, "custom_recipe",
      "自定义合成", "启用自定义配方。",
     SettingsItemType::kToggle, "customRecipeEnabled", 1, nullptr, nullptr, nullptr},
};
}  // namespace

QolResult register_settings(const contracts::QolHostSdk* sdk) {
    const auto* catalog = contracts::settings_catalog_join_if_available(sdk);
    if (catalog == nullptr) {
        static constexpr char message[] = "settings catalog item joining unavailable; contribution skipped";
        return contracts::error(kQolErrUnsupported, message, sizeof(message) - 1);
    }
    return catalog->register_items("custom_recipe", kSettingsOptimizationCategoryId,
                                   kItems, sizeof(kItems) / sizeof(kItems[0]));
}
}  // namespace qol::features::custom_recipe
