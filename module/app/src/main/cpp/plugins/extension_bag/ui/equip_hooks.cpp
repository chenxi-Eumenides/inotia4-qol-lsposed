#include "plugins/extension_bag/ui/equip_hooks.h"

#include <cstring>
#include <type_traits>
#include <utility>

#include "contracts/host_sdk.h"
#include "plugins/extension_bag/core/host_adapter.h"
#include "plugins/extension_bag/core/ui_state.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/runtime/ownership_port.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"
#include "plugins/extension_bag/runtime/state.h"

// public/projection.cpp 提供：EB 自身的刷新路径（含投影 restore guard）。不放进公共头，
// 因为它只允许经本入口或 EB 内部 refresh helper 调用，避免绕过 guard。
void virtual_bag_refresh_item_area_with_gate();

namespace {
// 宿主注入的原版 PLAYER_UpdateShortcut（inventory_refresh.update_shortcut_category）。
void (*g_player_update_shortcut)(int32_t) = nullptr;
}  // namespace

namespace qol::features::extension_bag::equip_hooks {
namespace {

using ButtonExeFn = void (*)(void*);
using ConfirmUseItemFn = void (*)(void*);
using RefreshItemAreaFn = void (*)();
using EquipControlEventProcFn = uint64_t (*)(void*, uint64_t, void*, void*);

const qol::contracts::QolHostSdk* host_sdk() { return runtime_state().host.sdk; }

// ui_state() 位于 qol::extension_bag（数据面命名空间），与 features::extension_bag 不同层。
UiState& ui_state() { return ::qol::extension_bag::ui_state(); }

// EB 关闭（feature 门）时 wrapper 只做原版透传，不进入扩展投影/事务路径。
bool support_enabled() { return virtual_bag_enabled(); }

template <typename Function, typename... Args>
auto call_backup(const char* hook_id, Function backup, Args&&... args)
    -> decltype(backup(std::forward<Args>(args)...)) {
    using Return = decltype(backup(std::forward<Args>(args)...));
    if (backup == nullptr) {
        QOL_LOG_ERROR(QolDomain::kExtensionBag, "hook=%s backup missing", hook_id);
        if constexpr (std::is_void<Return>::value) {
            return;
        } else {
            return Return{};
        }
    }
    return backup(std::forward<Args>(args)...);
}

ButtonExeFn button_equip_backup() {
    return reinterpret_cast<ButtonExeFn>(ui_state().ui.backup_button_equip_exe);
}
ButtonExeFn button_unequip_backup() {
    return reinterpret_cast<ButtonExeFn>(ui_state().ui.backup_button_unequip_exe);
}
ConfirmUseItemFn ok_confirm_use_item_backup() {
    return reinterpret_cast<ConfirmUseItemFn>(ui_state().ui.backup_ok_confirm_use_item);
}
EquipControlEventProcFn equip_control_event_proc_backup() {
    return reinterpret_cast<EquipControlEventProcFn>(ui_state().ui.backup_equip_control_event_proc);
}
RefreshItemAreaFn refresh_item_area_backup() {
    return reinterpret_cast<RefreshItemAreaFn>(ui_state().ui.backup_refresh_item_area);
}

// 详情门捕获的当前物品是否为扩展物品。详情 gate 只为模块物品写入
// extension_desc_item（原版详情打开时保持 nullptr），因此非空即已捕获扩展身份；
// 锁忙时按扩展物品处理（拒绝原版），不把已捕获的扩展物品交给原版装备流程。
bool detail_is_extension_item() {
    void* item = ui_state().ui.extension_desc_item;
    if (item == nullptr) return false;
    ::qol::extension_bag::TryReentryLock lock;
    if (!lock.owns_lock()) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "equip detail identity lock busy item=%p; blocked", item);
        return true;
    }
    int bag = -1;
    int slot = -1;
    return module_slot_of_item_locked(item, &bag, &slot);
}

void equip_button_exe_wrapper(void* button) {
    const qol::contracts::InFlightGuard in_flight_guard(
        host_sdk() == nullptr ? nullptr : &host_sdk()->in_flight,
        {"hook:UIEquip_ButtonEquipExe"});
    const ButtonExeFn backup = button_equip_backup();
    if (!support_enabled()) {
        call_backup("hook:UIEquip_ButtonEquipExe", backup, button);
        return;
    }
    if (extension_handle_backpack_equip()) return;
    if (extension_handle_character_equip()) return;
    if (detail_is_extension_item()) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "equip button blocked extension detail item=%p",
                     ui_state().ui.extension_desc_item);
        return;
    }
    call_backup("hook:UIEquip_ButtonEquipExe", backup, button);
}

void unequip_button_exe_wrapper(void* button) {
    const qol::contracts::InFlightGuard in_flight_guard(
        host_sdk() == nullptr ? nullptr : &host_sdk()->in_flight,
        {"hook:UIEquip_ButtonUnequipExe"});
    const ButtonExeFn backup = button_unequip_backup();
    if (!support_enabled()) {
        call_backup("hook:UIEquip_ButtonUnequipExe", backup, button);
        return;
    }
    if (extension_handle_original_bag_unequip()) return;
    call_backup("hook:UIEquip_ButtonUnequipExe", backup, button);
}

void ok_confirm_use_item_wrapper(void* item) {
    const qol::contracts::InFlightGuard in_flight_guard(
        host_sdk() == nullptr ? nullptr : &host_sdk()->in_flight,
        {"hook:UIEquip_OKConfirmUseItem"});
    const ConfirmUseItemFn backup = ok_confirm_use_item_backup();
    if (!support_enabled()) {
        call_backup("hook:UIEquip_OKConfirmUseItem", backup, item);
        return;
    }
    if (extension_confirm_use_item(item)) return;
    call_backup("hook:UIEquip_OKConfirmUseItem", backup, item);
}

// 入口 hook 是 EB 刷新路径的唯一入口；backup 槽同时是 inventory_refresh 的
// call_original trampoline，因此支持开启时不能直接调 backup（会跳过投影 restore/
// 容量字 guard），只有关闭态才纯透传。
void refresh_item_area_wrapper() {
    const qol::contracts::InFlightGuard in_flight_guard(
        host_sdk() == nullptr ? nullptr : &host_sdk()->in_flight,
        {"hook:UIEquip_RefreshItemArea"});
    const RefreshItemAreaFn backup = refresh_item_area_backup();
    if (!support_enabled()) {
        call_backup("hook:UIEquip_RefreshItemArea", backup);
        return;
    }
    if (backup == nullptr) {
        QOL_LOG_ERROR(QolDomain::kExtensionBag,
                      "hook=%s backup missing", "hook:UIEquip_RefreshItemArea");
        return;
    }
    virtual_bag_refresh_item_area_with_gate();
}

uint64_t equip_control_event_proc_wrapper(void* control, uint64_t event, void* x2, void* param) {
    const qol::contracts::InFlightGuard in_flight_guard(
        host_sdk() == nullptr ? nullptr : &host_sdk()->in_flight,
        {"hook:UIEquip_EquipControlEventProc"});
    const EquipControlEventProcFn backup = equip_control_event_proc_backup();
    if (!support_enabled()) {
        return call_backup("hook:UIEquip_EquipControlEventProc", backup, control, event, x2,
                           param);
    }
    if (backup == nullptr) {
        QOL_LOG_ERROR(QolDomain::kExtensionBag,
                      "hook=%s backup missing", "hook:UIEquip_EquipControlEventProc");
        return 0;
    }
    // UIEquip_EquipControlEventProc 的 0x01/0x80 会打开装备详情；当前详情不再是
    // 扩展袋卸下详情，清掉残留目标，避免卸下按钮被误分流。
    if (event == 0x01 || event == 0x80) {
        ::qol::extension_bag::ReentryLock lock;
        if (lock.owns_lock()) {
            ui_state().ui.extension_desc_unequip_bag = -1;
        }
    }
    uint64_t result = 0;
    const VirtualBagEquipControlEventResult outcome = virtual_bag_handle_equip_control_event(
        control, event, x2, param, backup, &result);
    switch (outcome) {
        case VirtualBagEquipControlEventResult::kHandled:
            return result;
        case VirtualBagEquipControlEventResult::kBlocked:
            return 0;
        case VirtualBagEquipControlEventResult::kNotExtension:
            break;
    }
    return call_backup("hook:UIEquip_EquipControlEventProc", backup, control, event, x2, param);
}

constexpr const char* kClaimOwner = "extension_bag";
constexpr const char* kClaimReason =
    "装备页 UI 入口的函数级分流：扩展袋/扩展物品接管装备、卸下、确认使用与控件事件；"
    "原版物品与关闭态一律透传 backup；业务体归属 plugins/extension_bag/ui/equip_hooks";

#define EQUIP_HOOK_CLAIM(CLAIM_ID, CLAIM_SYMBOL, HANDLER)                         \
    {CLAIM_ID, CLAIM_SYMBOL, qol::contracts::ResourceMechanism::kEntryHook,       \
     qol::contracts::ResourcePolicy::kPrivate,                                    \
     qol::contracts::ResourceRequirement::kRequired,                              \
     qol::contracts::ResourceFailure::kFailSoft, true, kClaimOwner, kClaimReason, \
     nullptr, 120, reinterpret_cast<void*>(&HANDLER)}

const qol::contracts::ResourceClaim kEquipHookClaims[] = {
    EQUIP_HOOK_CLAIM("hook:UIEquip_ButtonEquipExe", "F_UIEQUIP_BUTTON_EQUIP_EXE_VMA",
                     equip_button_exe_wrapper),
    EQUIP_HOOK_CLAIM("hook:UIEquip_ButtonUnequipExe", "F_UIEQUIP_BUTTON_UNEQUIP_EXE_VMA",
                     unequip_button_exe_wrapper),
    EQUIP_HOOK_CLAIM("hook:UIEquip_OKConfirmUseItem", "F_UIEQUIP_OK_CONFIRM_USE_ITEM_VMA",
                     ok_confirm_use_item_wrapper),
    EQUIP_HOOK_CLAIM("hook:UIEquip_RefreshItemArea", "F_UIEQUIP_REFRESH_ITEM_AREA_VMA",
                     refresh_item_area_wrapper),
    EQUIP_HOOK_CLAIM("hook:UIEquip_EquipControlEventProc",
                     "F_UIEQUIP_EQUIP_CONTROL_EVENT_PROC_VMA",
                     equip_control_event_proc_wrapper),
};
#undef EQUIP_HOOK_CLAIM

constexpr uint32_t kEquipHookClaimCount =
    sizeof(kEquipHookClaims) / sizeof(kEquipHookClaims[0]);

void** backup_slot_for_handler(void* handler) {
#define BACKUP_SLOT(HANDLER, SLOT) \
    if (handler == reinterpret_cast<void*>(&HANDLER)) return reinterpret_cast<void**>(&SLOT)
    BACKUP_SLOT(equip_button_exe_wrapper, ui_state().ui.backup_button_equip_exe);
    BACKUP_SLOT(unequip_button_exe_wrapper, ui_state().ui.backup_button_unequip_exe);
    BACKUP_SLOT(ok_confirm_use_item_wrapper, ui_state().ui.backup_ok_confirm_use_item);
    BACKUP_SLOT(equip_control_event_proc_wrapper,
                ui_state().ui.backup_equip_control_event_proc);
    BACKUP_SLOT(refresh_item_area_wrapper, ui_state().ui.backup_refresh_item_area);
#undef BACKUP_SLOT
    return nullptr;
}

}  // namespace

const qol::contracts::ResourceClaim* equip_hooks_claims(uint32_t* out_count) {
    if (out_count != nullptr) *out_count = kEquipHookClaimCount;
    return kEquipHookClaims;
}

int32_t bind_backup(const char* resource_id, void* backup) {
    if (resource_id == nullptr || backup == nullptr) return 1;
    for (const qol::contracts::ResourceClaim& claim : kEquipHookClaims) {
        if (std::strcmp(claim.id, resource_id) != 0) continue;
        void** slot = backup_slot_for_handler(claim.handler);
        if (slot == nullptr) return 1;
        *slot = backup;
        // RefreshItemArea 的槽同时是 inventory_refresh.call_original 的 trampoline。
        QOL_LOG_INFO(QolDomain::kExtensionBag, "equip hook bound resource=%s backup=%p",
                     claim.id, backup);
        return 0;
    }
    return 1;
}

bool call_refresh_item_area_original() {
    const RefreshItemAreaFn backup = refresh_item_area_backup();
    if (backup == nullptr) {
        QOL_LOG_ERROR(QolDomain::kExtensionBag, "hook=%s backup missing",
                      "hook:UIEquip_RefreshItemArea");
        return false;
    }
    backup();
    return true;
}

void update_shortcut_noop() {
    // 兼容入口：原版 PLAYER_UpdateShortcut 需要类别参数，无参调用会使用未定义 w0。
}

void update_shortcut_category(int32_t category) {
    if (g_player_update_shortcut != nullptr && category >= 0) {
        g_player_update_shortcut(category);
    }
}

}  // namespace qol::features::extension_bag::equip_hooks

void extension_bag_bind_player_update_shortcut(void (*update_shortcut)(int32_t)) {
    g_player_update_shortcut = update_shortcut;
}

namespace qol::features::extension_bag {

const qol::contracts::InventoryRefreshApi kInventoryRefreshApi{
    &equip_hooks::call_refresh_item_area_original,
    &equip_hooks::update_shortcut_noop,
    &equip_hooks::update_shortcut_category,
};

}  // namespace qol::features::extension_bag
