#include "settings_contribution.h"

#if defined(QOL_DEBUG_COMPONENTS) && QOL_DEBUG_COMPONENTS

#include <cstring>

#include "panels/stack.h"

namespace qol::features::core_ui {
namespace {
using namespace contracts;

constexpr const char* kCategoryId = "debug";
constexpr const char* kPanelId = "debug_components";
constexpr SettingsCategorySpec kCategory{
    sizeof(SettingsCategorySpec), kCategoryId, "core.custom_ui", "调试", "查看和测试 UI 组件。"};
const QolHostSdk* g_sdk = nullptr;

QolResult panel_available(void*, int32_t* out_available, char* reason, uint32_t capacity) {
    if (out_available == nullptr || reason == nullptr || capacity == 0) {
        constexpr char text[] = "invalid debug panel availability output";
        return error(kQolErrInvalidArgument, text, sizeof(text) - 1);
    }
    *out_available = 0;
    reason[0] = '\0';
    if (g_sdk == nullptr || g_sdk->panels.find == nullptr) {
        constexpr char text[] = "debug panel registry unavailable";
        const size_t length = sizeof(text) < capacity ? sizeof(text) : capacity;
        std::memcpy(reason, text, length - 1);
        reason[length - 1] = '\0';
        return ok();
    }
    const PanelProviderSpec* provider = nullptr;
    const QolResult result = g_sdk->panels.find(kPanelId, &provider);
    if (!succeeded(result) || provider == nullptr || provider->owner == nullptr ||
        std::strcmp(provider->owner, "core.custom_ui") != 0) {
        constexpr char text[] = "debug components panel is not registered";
        const size_t length = sizeof(text) < capacity ? sizeof(text) : capacity;
        std::memcpy(reason, text, length - 1);
        reason[length - 1] = '\0';
        return ok();
    }
    *out_available = 1;
    return ok();
}

QolResult open_debug_panel(void*) {
    if (g_sdk == nullptr) {
        constexpr char text[] = "debug panel navigation unavailable";
        return error(kQolErrNotReady, text, sizeof(text) - 1);
    }
    if (!ui_panels_open(kPanelId)) {
        constexpr char text[] = "debug panel navigation failed";
        return error(kQolErrBlocked, text, sizeof(text) - 1);
    }
    return ok();
}

const SettingsItemSpec kAction[]{
    {sizeof(SettingsItemSpec), "core.custom_ui.open_debug_components", kCategoryId,
     "core.custom_ui", "打开调试面板", "查看和测试 UI 组件。", SettingsItemType::kAction,
     nullptr, 0, &open_debug_panel, &panel_available, nullptr},
};

}  // namespace

QolResult register_settings_category(const QolHostSdk* sdk) {
    const auto* catalog = settings_catalog_metadata_if_available(sdk);
    if (catalog == nullptr) {
        constexpr char message[] = "settings category metadata API unavailable";
        return error(kQolErrUnsupported, message, sizeof(message) - 1);
    }
    return catalog->register_category_metadata(&kCategory);
}

QolResult register_settings(const QolHostSdk* sdk) {
    const auto* catalog = settings_catalog_join_if_available(sdk);
    if (catalog == nullptr) {
        constexpr char message[] = "settings catalog item joining unavailable";
        return error(kQolErrUnsupported, message, sizeof(message) - 1);
    }
    g_sdk = sdk;
    return catalog->register_items("core.custom_ui", kCategoryId, kAction,
                                   sizeof(kAction) / sizeof(kAction[0]));
}

}  // namespace qol::features::core_ui

#else

namespace qol::features::core_ui {
QolResult register_settings_category(const contracts::QolHostSdk*) {
    constexpr char message[] = "debug settings category is disabled in this build";
    return contracts::error(kQolErrUnsupported, message, sizeof(message) - 1);
}
QolResult register_settings(const contracts::QolHostSdk*) {
    constexpr char message[] = "debug settings contribution is disabled in this build";
    return contracts::error(kQolErrUnsupported, message, sizeof(message) - 1);
}
}  // namespace qol::features::core_ui

#endif
