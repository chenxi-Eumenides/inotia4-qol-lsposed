#include "settings_contribution.h"

namespace qol::features::extension_bag {
namespace {
using namespace contracts;
constexpr SettingsItemSpec kItems[]{
    {sizeof(SettingsItemSpec), "extension_bag.extensionBagEnabled", kSettingsOptimizationCategoryId, "extension_bag",
      "扩展背包", "增加五个扩展背包。",
      SettingsItemType::kToggle, "extensionBagEnabled", 1, nullptr, nullptr, nullptr},
};
}  // namespace

QolResult register_settings(const contracts::QolHostSdk* sdk) {
    const auto* catalog = contracts::settings_catalog_join_if_available(sdk);
    if (catalog == nullptr) {
        static constexpr char message[] = "settings catalog unavailable; contribution skipped";
        return contracts::error(kQolErrUnsupported, message, sizeof(message) - 1);
    }
    return catalog->register_items("extension_bag", kSettingsOptimizationCategoryId,
                                   kItems, sizeof(kItems) / sizeof(kItems[0]));
}
}  // namespace qol::features::extension_bag
