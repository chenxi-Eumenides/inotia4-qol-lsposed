#include "charinfo_watch.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../../contracts/host_sdk.h"
#include "../../contracts/panels.h"
#include "../../contracts/popup_enter_alias.h"
#include "../../contracts/ui.h"
#include "feature.h"
#include "overview.h"

namespace qol::features::character_panel {
namespace {

// 原版人物属性面板的 PopupState 定位符号（game/access/symbols.cpp 登记的导出名）。
constexpr const char* kCharacterInfoPopupSymbol = "F_PANEL_CHARACTER_INFO_ENTER";
// 触摸事件：按下记录起点、抬起判定、移动取消待判定。
constexpr uint64_t kEventPress = 0x17;
constexpr uint64_t kEventRelease = 0x18;
constexpr uint64_t kEventMove = 0x19;
constexpr const char* kOpenRejectedMessage = "character overview open rejected";

using PopupEnterFn = void (*)();
using PopupEventFn = uint32_t (*)(uint64_t, uint64_t, uint64_t);

contracts::QolPopupStateEntry g_entry{};
std::atomic<PopupEnterFn> g_original_enter{nullptr};
std::atomic<PopupEventFn> g_original_event{nullptr};
std::atomic<bool> g_pending_open{false};
// 按下点（内容区局部判定在抬起时用同一矩形复核）；事件回调均在游戏主线程。
bool g_press_valid = false;
int64_t g_press_x = 0;
int64_t g_press_y = 0;

void log_warn(const char* message) {
    const contracts::QolHostSdk* sdk = host_sdk();
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    (void)sdk->log.write(1, 1, "character_panel", message,
                         static_cast<uint32_t>(std::strlen(message)));
}

// 契约尾部门禁：旧宿主未填 character_info_content_rect 时不得读未初始化槽。
bool content_rect_available(const contracts::QolUiAdapter* ui) {
    if (ui == nullptr || ui->major != contracts::kUiAdapterMajor ||
        ui->minor < contracts::kUiAdapterMinor) {
        return false;
    }
    constexpr size_t kRequiredSize =
        offsetof(contracts::QolUiAdapter, character_info_content_rect) +
        sizeof(((contracts::QolUiAdapter*)nullptr)->character_info_content_rect);
    return ui->size >= kRequiredSize && ui->touch_coordinates != nullptr &&
           ui->character_info_content_rect != nullptr;
}

// 取触摸坐标与内容区矩形；任一失败都视为不可判定（不改原事件语义）。
bool read_tap(uint64_t param, int64_t* x, int64_t* y, contracts::UiRect* rect) {
    const contracts::QolHostSdk* sdk = host_sdk();
    const contracts::QolUiAdapter* ui = sdk == nullptr ? nullptr : sdk->ui;
    if (param == 0 || x == nullptr || y == nullptr || rect == nullptr ||
        !content_rect_available(ui)) {
        return false;
    }
    if (!contracts::succeeded(ui->touch_coordinates(param, x, y))) return false;
    return contracts::succeeded(ui->character_info_content_rect(rect));
}

bool open_overview() {
    const contracts::QolHostSdk* sdk = host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return false;
    const void* iface = nullptr;
    if (!contracts::succeeded(sdk->capabilities.require(
            contracts::kUiPanelsCapabilityId,
            {static_cast<uint16_t>(contracts::kUiPanelsVersionMajor),
             static_cast<uint16_t>(contracts::kUiPanelsVersionMinor)},
            &iface)) ||
        iface == nullptr) {
        return false;
    }
    const auto* panels = static_cast<const contracts::UiPanelsApi*>(iface);
    return panels->open != nullptr && panels->open(kOverviewPanelId);
}

PopupEnterFn backup_enter(const contracts::QolPopupStateEntry& entry) {
    PopupEnterFn value = nullptr;
    std::memcpy(&value, entry.backup + kPopupStateEntryEnterOffset, sizeof(value));
    return value;
}

PopupEventFn backup_event(const contracts::QolPopupStateEntry& entry) {
    PopupEventFn value = nullptr;
    std::memcpy(&value, entry.backup + kPopupStateEntryEventOffset, sizeof(value));
    return value;
}

}  // namespace

const contracts::OwnershipOps kCharacterInfoPopupOps{&character_info_popup_acquire,
                                                    &character_info_popup_release};

int32_t character_info_popup_acquire(const char* id) {
    (void)id;
    if (g_entry.slot != nullptr) return 0;  // 幂等（资源重试）
    const contracts::QolHostSdk* sdk = host_sdk();
    if (sdk == nullptr || sdk->hooks.popup_resolve == nullptr ||
        sdk->hooks.popup_acquire == nullptr) {
        return 1;
    }
    contracts::QolPopupStateEntry entry{};
    if (!contracts::succeeded(sdk->hooks.popup_resolve(kCharacterInfoPopupSymbol, &entry))) {
        return 1;
    }
    // 机制层要求 enter 非空（作为该条目的 owner 身份）；enter 只透传原函数。
    // process/f3/f4 传 nullptr，宿主保留原函数，原版面板行为不变。
    const contracts::QolPopupStateHooks hooks{&character_info_enter_wrapper, nullptr, nullptr,
                                              nullptr, &character_info_event_wrapper};
    if (!contracts::succeeded(sdk->hooks.popup_acquire(&entry, &hooks))) return 1;
    g_entry = entry;
    g_original_enter.store(backup_enter(entry), std::memory_order_release);
    g_original_event.store(backup_event(entry), std::memory_order_release);
    return 0;
}

void character_info_popup_release(const char* id) {
    (void)id;
    if (g_entry.slot == nullptr) return;
    const contracts::QolHostSdk* sdk = host_sdk();
    if (sdk == nullptr || sdk->hooks.popup_restore == nullptr) return;
    // 还原用副本：失败时保留唯一重试 token（与 extension_bag 同口径）。
    contracts::QolPopupStateEntry restore_entry = g_entry;
    if (!contracts::succeeded(sdk->hooks.popup_restore(&restore_entry))) {
        log_warn("character info popup restore failed");
        return;
    }
    g_entry = {};
    g_original_enter.store(nullptr, std::memory_order_release);
    g_original_event.store(nullptr, std::memory_order_release);
}

void character_info_enter_wrapper() {
    const PopupEnterFn original = g_original_enter.load(std::memory_order_acquire);
    if (original != nullptr) original();
}

uint32_t character_info_event_wrapper(uint64_t event, uint64_t param, uint64_t param2) {
    // 按下点必须与抬起点落在同一内容区：按下不在内容区时，抬起命中也不触发。
    if (event == kEventPress) {
        contracts::UiRect rect{};
        g_press_valid = read_tap(param, &g_press_x, &g_press_y, &rect) &&
                        character_info_tap_inside(rect, g_press_x, g_press_y);
    } else if (event == kEventRelease) {
        if (g_press_valid && overview_enabled()) {
            int64_t release_x = 0;
            int64_t release_y = 0;
            contracts::UiRect rect{};
            if (read_tap(param, &release_x, &release_y, &rect) &&
                character_info_tap_inside(rect, g_press_x, g_press_y) &&
                character_info_tap_inside(rect, release_x, release_y)) {
                g_pending_open.store(true, std::memory_order_release);
            }
        }
        g_press_valid = false;
    } else if (event == kEventMove && g_press_valid) {
        // 只在移出内容区时取消：触屏轻点常伴随小幅 move，无条件清除会让正常点击失效。
        int64_t move_x = 0;
        int64_t move_y = 0;
        contracts::UiRect rect{};
        g_press_valid = read_tap(param, &move_x, &move_y, &rect) &&
                        character_info_tap_inside(rect, move_x, move_y);
    }
    const PopupEventFn original = g_original_event.load(std::memory_order_acquire);
    if (original != nullptr) return original(event, param, param2);
    return 0;
}

int32_t character_info_open_tick(int64_t frame, void* ctx) {
    (void)frame;
    (void)ctx;
    if (g_pending_open.exchange(false, std::memory_order_acq_rel) && overview_enabled() &&
        !open_overview()) {
        log_warn(kOpenRejectedMessage);
    }
    // 常驻任务：返回 0 会被 scheduler 注销；只在 deactivate 里显式 remove。
    return 1;
}

void character_info_reset() {
    g_pending_open.store(false, std::memory_order_release);
    g_press_valid = false;
}

void character_info_test_reset() {
    if (g_entry.slot != nullptr) (void)character_info_popup_release(nullptr);
    // 无论 popup_restore 结果如何都清掉本地状态：测试隔离优先，stub 宿主不持有真实条目。
    g_entry = {};
    g_original_enter.store(nullptr, std::memory_order_release);
    g_original_event.store(nullptr, std::memory_order_release);
    character_info_reset();
}

}  // namespace qol::features::character_panel
