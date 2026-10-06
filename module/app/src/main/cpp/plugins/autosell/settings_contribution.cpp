#include "settings_contribution.h"

namespace qol::features::autosell {
namespace {
using namespace contracts;
constexpr SettingsItemSpec kItems[]{
    {sizeof(SettingsItemSpec), "autosell.autoSellEnabled", kSettingsOptimizationCategoryId, "autosell",
     "自动出售", "自动出售符合条件的物品。",
      SettingsItemType::kToggle, "autoSellEnabled", 1, nullptr, nullptr, nullptr},
};
}  // namespace

QolResult register_settings(const contracts::QolHostSdk* sdk) {
    const auto* catalog = contracts::settings_catalog_join_if_available(sdk);
    if (catalog == nullptr) {
        static constexpr char message[] = "settings catalog item joining unavailable; contribution skipped";
        return contracts::error(kQolErrUnsupported, message, sizeof(message) - 1);
    }
    return catalog->register_items("autosell", kSettingsOptimizationCategoryId,
                                   kItems, sizeof(kItems) / sizeof(kItems[0]));
}
}  // namespace qol::features::autosell
