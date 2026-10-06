#include "settings_contribution.h"

namespace qol::features::simple_mode {
namespace {
using namespace contracts;
constexpr SettingsItemSpec kItems[]{
    {sizeof(SettingsItemSpec), "simple_mode.simpleModeEnabled", kSettingsOptimizationCategoryId, "simple_mode",
      "简单模式", "降低怪物生命并增强玩家伤害与承伤。",
     SettingsItemType::kToggle, "simpleModeEnabled", 1, nullptr, nullptr, nullptr},
};
}  // namespace

QolResult register_settings(const contracts::QolHostSdk* sdk) {
    const auto* catalog = contracts::settings_catalog_join_if_available(sdk);
    if (catalog == nullptr) {
        static constexpr char message[] = "settings catalog item joining unavailable; contribution skipped";
        return contracts::error(kQolErrUnsupported, message, sizeof(message) - 1);
    }
    return catalog->register_items("simple_mode", kSettingsOptimizationCategoryId,
                                   kItems, sizeof(kItems) / sizeof(kItems[0]));
}
}  // namespace qol::features::simple_mode
