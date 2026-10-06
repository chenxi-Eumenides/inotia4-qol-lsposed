#include "settings_contribution.h"

namespace qol::features::core_settings {
namespace {
using namespace contracts;
constexpr SettingsCategorySpec kCategory{
    sizeof(SettingsCategorySpec), kSettingsBasicCategoryId, "core.settings", "基础设置", "常用设置。"};
constexpr SettingsCategorySpec kOptimizationCategory{
    sizeof(SettingsCategorySpec), kSettingsOptimizationCategoryId, "core.settings", "优化功能", "游戏体验优化。"};
constexpr SettingsItemSpec kItems[]{
    {sizeof(SettingsItemSpec), "core.settings.apiEnabled", kSettingsBasicCategoryId, "core.settings",
     "API 服务", "提供本机 API 访问。",
      SettingsItemType::kToggle, "apiEnabled", 1, nullptr, nullptr, nullptr},
    {sizeof(SettingsItemSpec), "core.settings.opEnabled", kSettingsBasicCategoryId, "core.settings",
      "API 服务 OP 操作", "允许通过 API 执行 OP 操作。",
      SettingsItemType::kToggle, "opEnabled", 1, nullptr, nullptr, nullptr},
    {sizeof(SettingsItemSpec), "core.settings.debugLogEnabled", kSettingsBasicCategoryId, "core.settings",
     "调试日志", "记录更多运行信息。",
      SettingsItemType::kToggle, "debugLogEnabled", 1, nullptr, nullptr, nullptr},
};
}  // namespace

QolResult register_settings(const contracts::QolHostSdk* sdk) {
    const auto* catalog = contracts::settings_catalog_join_if_available(sdk);
    if (catalog == nullptr) {
        static constexpr char message[] = "settings catalog unavailable; contribution skipped";
        return contracts::error(kQolErrUnsupported, message, sizeof(message) - 1);
    }
    return catalog->register_items("core.settings", kSettingsBasicCategoryId,
                                   kItems, sizeof(kItems) / sizeof(kItems[0]));
}

QolResult register_categories(const contracts::QolHostSdk* sdk) {
    const auto* catalog = contracts::settings_catalog_metadata_if_available(sdk);
    if (catalog == nullptr) {
        static constexpr char message[] = "settings category metadata API unavailable; fixed categories skipped";
        return contracts::error(kQolErrUnsupported, message, sizeof(message) - 1);
    }
    auto result = catalog->register_category_metadata(&kCategory);
    if (!contracts::succeeded(result)) return result;
    return catalog->register_category_metadata(&kOptimizationCategory);
}
}  // namespace qol::features::core_settings
