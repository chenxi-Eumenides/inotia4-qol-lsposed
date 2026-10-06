#include "settings_contribution.h"

#include <array>
#include <cstring>

namespace qol::features::save_backup {
namespace {
using namespace contracts;

struct ActionContext {
    QolPanelFindFn find = nullptr;
    QolCapabilityRequireFn require = nullptr;
};
// 宿主机制函数常驻；可撤销的 provider/capability 表每次现取。
ActionContext g_action;

QolResult fail(int32_t code, const char* message) {
    thread_local std::array<char, kSettingsDescriptionCapacity> text{};
    const auto length = strnlen(message, text.size() - 1);
    std::memcpy(text.data(), message, length);
    text[length] = '\0';
    return error(code, text.data(), static_cast<uint32_t>(length));
}

QolResult available(void* user, int32_t* out_available, char* reason, uint32_t capacity) {
    if (out_available == nullptr || reason == nullptr || capacity == 0 || user == nullptr) {
        return fail(kQolErrInvalidArgument, "存档管理器可用性输出无效");
    }
    *out_available = 0;
    reason[0] = '\0';
    const auto& context = *static_cast<const ActionContext*>(user);
    if (context.find == nullptr) return fail(kQolErrNotReady, "面板查询接口不可用");
    const PanelProviderSpec* provider = nullptr;
    const auto result = context.find("save_backup", &provider);
    if (!succeeded(result)) return result;
    if (provider == nullptr) return fail(kQolErrNotReady, "存档管理器面板未登记");
    const char* blocked = provider->block_reason == nullptr ? nullptr : provider->block_reason();
    if (blocked != nullptr && blocked[0] != '\0') {
        const auto length = std::strlen(blocked);
        if (length >= capacity) return fail(kQolErrInvalidArgument, "面板拒绝原因缓冲区不足");
        std::memcpy(reason, blocked, length + 1);
    } else {
        *out_available = 1;
    }
    return ok();
}

QolResult open(void* user) {
    std::array<char, kSettingsDescriptionCapacity> reason{};
    int32_t is_available = 0;
    auto result = available(user, &is_available, reason.data(), reason.size());
    if (!succeeded(result)) return result;
    if (is_available == 0) return fail(kQolErrBlocked, reason.data());
    const auto& context = *static_cast<const ActionContext*>(user);
    if (context.require == nullptr) return fail(kQolErrNotReady, "ui_panels 能力查询接口不可用");
    const void* iface = nullptr;
    result = context.require(kUiPanelsCapabilityId,
                             {kUiPanelsVersionMajor, kUiPanelsVersionMinor}, &iface);
    if (!succeeded(result)) return result;
    if (iface == nullptr) return fail(kQolErrNotReady, "ui_panels 能力不可用");
    const auto* panels = static_cast<const UiPanelsApi*>(iface);
    if (panels->open == nullptr) return fail(kQolErrUnsupported, "ui_panels 不支持打开面板");
    // 必须经共享导航栈，保留存档页嵌套 push 与返回兜底。
    if (panels->open("save_backup")) return ok();
    result = available(user, &is_available, reason.data(), reason.size());
    if (!succeeded(result)) return result;
    return fail(kQolErrBlocked, is_available == 0 ? reason.data() : "存档管理器打开失败：面板尚未就绪或导航被拒绝");
}

constexpr SettingsCategorySpec kCategory{
    sizeof(SettingsCategorySpec), kSettingsBasicCategoryId, "core.settings", "基础设置", "常用设置。"};
const SettingsItemSpec kItems[]{
    {sizeof(SettingsItemSpec), "save_backup.open_manager", kSettingsBasicCategoryId, "save_backup",
     "存档备份导出", "管理存档备份。",
      SettingsItemType::kAction, nullptr, 0, &open, &available, &g_action},
};
}  // namespace

QolResult register_settings(const contracts::QolHostSdk* sdk) {
    const auto* catalog = contracts::settings_catalog_if_available(sdk);
    if (catalog == nullptr) return fail(kQolErrUnsupported, "settings catalog unavailable; contribution skipped");
    const auto* join = contracts::settings_catalog_join_if_available(sdk);
    if (join == nullptr) return fail(kQolErrUnsupported, "settings catalog item joining unavailable");
    const auto result = join->register_items("save_backup", kSettingsBasicCategoryId,
                                              kItems, sizeof(kItems) / sizeof(kItems[0]));
    if (contracts::succeeded(result)) g_action = {sdk->panels.find, sdk->capabilities.require};
    return result;
}
}  // namespace qol::features::save_backup
