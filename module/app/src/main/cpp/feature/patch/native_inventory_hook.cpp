#include "native_inventory_hook.h"
#include "inventory_find_item_poc.h"
#include "inventory_hook_stage4.h"

#include "core/native/extension_bag_port.h"
#include "feature/extension_bag/game_ui_virtbag.h"
#include "game_access.h"
#include "game_state.h"

#include <android/log.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace {

constexpr char kTag[] = "Inotia4NativeHook";
constexpr uint32_t kNativeApiVersion = 2;

std::mutex g_hook_mutex;
NativeHookFunType g_hook_func = nullptr;
NativeUnhookFunType g_unhook_func = nullptr;
std::atomic<bool> g_installing{false};
std::atomic<bool> g_installed{false};
std::atomic<bool> g_install_blocked{false};

FindItemFn g_backup_find_item = nullptr;
HaveItemFn g_backup_have_item = nullptr;
GetItemCountFn g_backup_get_item_count = nullptr;
ConsumeItemFn g_backup_consume_item = nullptr;
RemoveItemFn g_backup_remove_item = nullptr;
SaveItemFn g_backup_save_item = nullptr;
InvenMoveItemFn g_backup_move_item = nullptr;
UiEquipRefreshItemAreaFn g_backup_refresh_item_area = nullptr;
EquipItemFromInvenToSlotFn g_backup_equip_item_from_inven_to_slot = nullptr;
PutJewelFn g_backup_put_jewel = nullptr;
IsHavingEmptySlotFn g_backup_is_having_empty_slot = nullptr;
UnequipFn g_backup_unequip_item_to_inven = nullptr;
ButtonEquipExeFn g_backup_button_equip_exe = nullptr;
ButtonUnequipExeFn g_backup_button_unequip_exe = nullptr;
UiEquipOkConfirmUseItemFn g_backup_ok_confirm_use_item = nullptr;
UiEquipEquipControlEventProcFn g_backup_equip_control_event_proc = nullptr;

std::atomic<uint64_t> g_find_item_calls{0};
std::atomic<uint64_t> g_consume_item_calls{0};
std::atomic<uint64_t> g_remove_item_calls{0};

struct InstalledHook {
    void* target;
    void** backup;
    const char* name;
};

thread_local bool g_in_find_item = false;
thread_local bool g_in_have_item = false;
thread_local bool g_in_get_item_count = false;
thread_local bool g_in_is_having_empty_slot = false;
thread_local bool g_in_consume_item = false;
thread_local bool g_in_unequip_item_to_inven = false;
thread_local bool g_in_remove_item = false;
thread_local int g_refresh_depth = 0;

void log_extension_item_observation(const char* operation, void* item, bool recursive,
                                    int result_known, int result) {
    if (!extension_bag_enabled()) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "%s extension disabled recursive=%d result_known=%d result=%d",
                            operation, recursive ? 1 : 0, result_known, result);
        return;
    }
    int bag = -1;
    int slot = -1;
    const bool extension_item = extension_bag_identify_native_item(item, &bag, &slot);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "%s extension_item=%d bag=%d slot=%d recursive=%d result_known=%d result=%d",
                        operation, extension_item ? 1 : 0, bag, slot, recursive ? 1 : 0,
                        result_known, result);
}

void* find_item_wrapper(int32_t category) {
    const uint64_t call = g_find_item_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g_in_find_item || g_backup_find_item == nullptr) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "FindItem call=%llu category=%d recursive=%d backup=%p",
                            static_cast<unsigned long long>(call), category,
                            g_in_find_item ? 1 : 0,
                            reinterpret_cast<void*>(g_backup_find_item));
        void* result = inventory_find_item_original_first(
            category, g_backup_find_item, nullptr, g_in_find_item);
        log_extension_item_observation("FindItem", result, g_in_find_item, 1, result != nullptr ? 1 : 0);
        return result;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "FindItem call=%llu category=%d backup=%p",
                        static_cast<unsigned long long>(call), category,
                        reinterpret_cast<void*>(g_backup_find_item));
    void* result = inventory_find_item_original_first(
        category, g_backup_find_item,
        extension_bag_enabled() ? extension_bag_find_native_item : nullptr,
        g_in_find_item);
    log_extension_item_observation("FindItem", result, false, 1, result != nullptr ? 1 : 0);
    return result;
}

int extension_item_count(int32_t category) {
    struct Context { int32_t category; int count; } context{category, 0};
    extension_bag_for_each_logical_item([](int, int, int item_category, int item_count, void* raw) -> bool {
        Context* context = static_cast<Context*>(raw);
        if (item_category == context->category && item_count > 0) context->count += item_count;
        return false;
    }, &context);
    return context.count;
}

int have_item_wrapper(int32_t category) {
    return stage4_have_item(category, g_backup_have_item,
                            extension_bag_enabled() ? extension_item_count : nullptr,
                            g_in_have_item);
}

int get_item_count_wrapper(int32_t category) {
    return stage4_get_item_count(category, g_backup_get_item_count,
                                 extension_bag_enabled() ? extension_item_count : nullptr,
                                 g_in_get_item_count);
}

int is_having_empty_slot_wrapper(int32_t needed, int32_t include_task_bag) {
    return stage4_is_having_empty_slot(needed, include_task_bag,
                                        g_backup_is_having_empty_slot,
                                        extension_bag_enabled() ? extension_bag_has_empty_slots : nullptr,
                                        g_in_is_having_empty_slot);
}

int unequip_item_to_inven_wrapper(void* character, int32_t equip_slot) {
    const int result = stage4_unequip_item_to_inven(character, equip_slot,
                                                    g_backup_unequip_item_to_inven,
                                                    extension_bag_enabled()
                                                        ? extension_bag_adopt_unequipped_item
                                                        : nullptr,
                                                    g_in_unequip_item_to_inven);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "UnequipItemToInven equip_slot=%d result=%d recursive=%d",
                        equip_slot, result, g_in_unequip_item_to_inven ? 1 : 0);
    return result;
}

void consume_item_wrapper(void* item) {
    const uint64_t call = g_consume_item_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "ConsumeItem call=%llu item=%p recursive=%d backup=%p",
                        static_cast<unsigned long long>(call), item,
                        g_in_consume_item ? 1 : 0,
                        reinterpret_cast<void*>(g_backup_consume_item));
    int extension_bag = -1;
    int extension_slot = -1;
    const bool extension_item = extension_bag_enabled() &&
                                extension_bag_identify_native_item(item, &extension_bag,
                                                                   &extension_slot);
    const bool dispatched = stage4_consume_item(
        item, extension_bag_enabled() ? extension_bag_identify_native_item : nullptr,
        extension_bag_enabled() ? extension_bag_consume_native_item : nullptr,
        g_backup_consume_item, g_in_consume_item);
    if (!dispatched && extension_item) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "ConsumeItem extension consume failed bag=%d slot=%d item=%p",
                            extension_bag, extension_slot, item);
    }
    return;
}

int remove_item_wrapper(void* item) {
    const uint64_t call = g_remove_item_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "RemoveItem call=%llu item=%p recursive=%d backup=%p",
                        static_cast<unsigned long long>(call), item,
                        g_in_remove_item ? 1 : 0,
                        reinterpret_cast<void*>(g_backup_remove_item));
    return stage4_remove_item(item,
                              extension_bag_enabled() ? extension_bag_identify_native_item : nullptr,
                              extension_bag_enabled() ? extension_bag_remove_native_item : nullptr,
                              g_backup_remove_item,
                              g_in_remove_item);
}

int save_item_wrapper(void* item) {
    // INVEN_SaveItem 是所有"已创建物品放入背包"的唯一漏斗（任务奖励/事件
    // 发奖/开箱/拾取/商店/合成等 18 条调用点）。原版全袋无空位时
    // FindSaveSlot 失败返回 0 → 上层会掉地/静默消失。此处 backup 返回 0 时
    // 转扩展袋空位接管（adopt 进扩展槽），返回 1 让上层按成功处理，
    // 发奖代码零改动即支持扩展背包。
    // 商店扩展视图会把窗口原版袋容量字临时放大；买入等原版写入若以放大
    // 容量扫空槽会把物品写进超过真实容量的槽位。backup 前先恢复商店投影。
    if (extension_bag_enabled()) {
        extension_bag_store_restore_for_original_write();
    }
    const int result = g_backup_save_item(item);
    if (result != 0) return result;
    if (extension_bag_adopt_native_item(item)) return 1;
    return 0;
}

void refresh_item_area_wrapper() {
    if (g_refresh_depth > 0) {
        inventory_native_hook_call_refresh_item_area_original();
        return;
    }
    ++g_refresh_depth;
    virtual_bag_refresh_item_area_with_gate();
    --g_refresh_depth;
}

uint64_t move_item_caller_offset() {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    return g_base != 0 && caller >= g_base ? static_cast<uint64_t>(caller - g_base) : 0;
}

void log_move_item_observation(const char* phase, void* item, int count, int target_bag,
                               int target_slot, uint64_t caller_offset,
                               const VirtualBagMoveItemObservation& observation) {
    __android_log_print(
        ANDROID_LOG_INFO, kTag,
        "MoveItem %s item=%p count=%d target=%d/%d caller=libgame+0x%llx "
        "extension=%d/%d/%d handle=%u owner_state=%d view=%d session=%d source=%d/%d source_ptr=%p target_ptr=%p "
        "digest=0x%llx nonnull=%d physical_valid=%d",
        phase, item, count, target_bag, target_slot,
        static_cast<unsigned long long>(caller_offset), observation.extension_item ? 1 : 0,
        observation.extension_bag, observation.extension_slot, observation.extension_handle,
        observation.ownership_state, observation.module_view_index,
        observation.drag_session_active ? 1 : 0, observation.source_bag, observation.source_slot,
        observation.source_ptr, observation.target_ptr,
        static_cast<unsigned long long>(observation.physical_digest), observation.physical_nonnull,
        observation.physical_valid ? 1 : 0);
}

int move_item_wrapper(void* item, int count, int target_bag, int target_slot) {
    // 身份与取证数据在 g_virtual_bag_mtx 短临界区内采集，返回后立即放锁；禁止持锁
    // 调 backup，因为原版函数可能进入 RemoveItem/SaveItem/ConsumeItem hooks 再 try_lock。
    VirtualBagMoveItemObservation before{};
    const bool captured = virtual_bag_capture_move_item_observation(
        item, target_bag, target_slot, &before);
    const uint64_t caller_offset = move_item_caller_offset();
    // 仅允许装备交换 wrapper 主动借入物理槽时通过；普通投影/拖动仍必须
    // 拒绝 backup，避免扩展对象进入原版移动链。该例外不放宽其它 caller。
    if (captured && before.extension_item && !extension_bag_internal_equip_active()) {
        __android_log_print(
            ANDROID_LOG_ERROR, kTag,
            "MoveItem GUARD reject item=%p count=%d target=%d/%d caller=libgame+0x%llx "
            "extension=%d/%d handle=%u owner_state=%d view=%d session=%d source=%d/%d source_ptr=%p target_ptr=%p "
            "digest=0x%llx nonnull=%d physical_valid=%d backup=skipped",
            item, count, target_bag, target_slot,
            static_cast<unsigned long long>(caller_offset), before.extension_bag,
            before.extension_slot, before.extension_handle, before.ownership_state,
            before.module_view_index, before.drag_session_active ? 1 : 0,
            before.source_bag, before.source_slot, before.source_ptr, before.target_ptr,
            static_cast<unsigned long long>(before.physical_digest), before.physical_nonnull,
            before.physical_valid ? 1 : 0);
        return 0;
    }

    const bool observe = captured &&
                         (before.drag_session_active || before.module_view_index >= 0);
    if (observe) {
        log_move_item_observation("pre", item, count, target_bag, target_slot,
                                  caller_offset, before);
    }
    const int result = g_backup_move_item == nullptr
        ? 0 : g_backup_move_item(item, count, target_bag, target_slot);
    if (observe) {
        VirtualBagMoveItemObservation after{};
        if (virtual_bag_capture_move_item_observation(item, target_bag, target_slot, &after)) {
            log_move_item_observation("post", item, count, target_bag, target_slot,
                                      caller_offset, after);
        }
    } else {
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "MoveItem passthrough item=%p count=%d target=%d/%d "
                            "caller=libgame+0x%llx result=%d",
                            item, count, target_bag, target_slot,
                            static_cast<unsigned long long>(caller_offset), result);
    }
    return result;
}

int equip_item_from_inven_to_slot_wrapper(void* character, int32_t bag, int32_t slot,
                                          int32_t equip_slot) {
    if (extension_bag_internal_equip_active()) {
        if (g_backup_equip_item_from_inven_to_slot == nullptr) return 0;
        return g_backup_equip_item_from_inven_to_slot(character, bag, slot, equip_slot);
    }
    const int result = stage4_equip_item(character, bag, slot, equip_slot, inventory_item_at,
                                         extension_bag_enabled()
                                             ? extension_bag_identify_native_item
                                             : nullptr,
                                         extension_bag_enabled()
                                             ? extension_bag_equip_projected_item
                                             : nullptr,
                                         g_backup_equip_item_from_inven_to_slot,
                                         extension_bag_enabled()
                                             ? extension_bag_view_item_at
                                             : nullptr);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "EquipItemFromInvenToSlot bag=%d slot=%d equip_slot=%d result=%d",
                        bag, slot, equip_slot, result);
    return result;
}

int put_jewel_wrapper(void* equip_item, void* jewel_item) {
    return stage4_put_jewel(equip_item, jewel_item,
                            extension_bag_enabled() ? extension_bag_identify_native_item : nullptr,
                            g_backup_put_jewel,
                            extension_bag_enabled() ? virtual_bag_put_jewel_native : nullptr);
}

void button_equip_exe_wrapper(void* button) {
    // 装备按钮函数级接管：详情物品为扩展槽背包物品时在原函数内联删源
    // （b7e04）之前分流，源物品销毁与袋表移交由扩展事务完成；其余一律
    // 走原函数，保持完全原版流程。
    const VirtualBagEquipButtonResult result =
        virtual_bag_handle_backpack_button_equip_result();
    if (result == VirtualBagEquipButtonResult::kHandled) return;
    if (result == VirtualBagEquipButtonResult::kBlocked) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "ButtonEquipExe extension item blocked; original backup skipped");
        return;
    }
    g_backup_button_equip_exe(button);
}

void button_unequip_exe_wrapper(void* button) {
    // 卸下按钮函数级接管：desc_type=1 卸袋（原版袋/扩展袋）时模块接管，
    // 原版背包满时把袋对象放回其他袋行或收编扩展空位；物品绝不消失。
    // 卸装备与其他场景一律走原函数。
    bool no_space = false;
    bool not_empty = false;
    if (extension_bag_handle_original_bag_unequip(&no_space, &not_empty)) {
        if (not_empty) {
            extension_bag_show_not_empty_popup();
        } else if (no_space) {
            extension_bag_show_no_space_popup();
        }
        return;
    }
    g_backup_button_unequip_exe(button);
}

void ok_confirm_use_item_wrapper(void* item) {
    if (virtual_bag_handle_confirm_use_item(item)) return;
    if (g_backup_ok_confirm_use_item == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "UIEquip_OKConfrimUseItem backup unavailable item=%p", item);
        return;
    }
    g_backup_ok_confirm_use_item(item);
}

uint64_t equip_control_event_proc_wrapper(void* control, uint64_t event, void* x2, void* param) {
    uint64_t original_result = 0;
    const VirtualBagEquipControlEventResult result =
        virtual_bag_handle_equip_control_event(
            control, event, x2, param,
            g_backup_equip_control_event_proc, &original_result);
    if (result == VirtualBagEquipControlEventResult::kHandled) return original_result;
    if (result == VirtualBagEquipControlEventResult::kBlocked) return 0;
    return g_backup_equip_control_event_proc == nullptr
        ? 0 : g_backup_equip_control_event_proc(control, event, x2, param);
}

bool target_is_executable(uintptr_t target, const char* name) {
    if (target == 0 || !game_memory_accessible(reinterpret_cast<void*>(target), 4, 'x')) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "%s target invalid: %p", name,
                            reinterpret_cast<void*>(target));
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "%s target validated: %p", name,
                        reinterpret_cast<void*>(target));
    return true;
}

bool rollback_installed_hooks(const InstalledHook* hooks, std::size_t count) {
    bool rolled_back = true;
    for (std::size_t index = count; index > 0; --index) {
        const InstalledHook& hook = hooks[index - 1];
        const int result = g_unhook_func == nullptr || hook.target == nullptr
            ? -1 : g_unhook_func(hook.target);
        if (result == 0) {
            if (hook.backup != nullptr) *hook.backup = nullptr;
            __android_log_print(ANDROID_LOG_INFO, kTag,
                                "hook rollback OK name=%s", hook.name);
        } else {
            rolled_back = false;
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "hook rollback failed name=%s result=%d", hook.name, result);
        }
    }
    if (!rolled_back) {
        g_install_blocked.store(true, std::memory_order_release);
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "hook rollback incomplete; retry blocked");
    }
    return rolled_back;
}

bool install_locked() {
    if (g_installed.load(std::memory_order_acquire)) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "hook install skipped: already installed");
        return true;
    }
    if (g_install_blocked.load(std::memory_order_acquire)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "hook install blocked after rollback failure");
        return false;
    }
    if (g_hook_func == nullptr || !bridge_ready()) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "hook install deferred: hook_func=%p base=%p",
                            reinterpret_cast<void*>(g_hook_func),
                            reinterpret_cast<void*>(g_base));
        return false;
    }

    const uintptr_t find_item = g_base + fn_resolve("F_FIND_ITEM_VMA", F_FIND_ITEM_VMA);
    const uintptr_t have_item = g_base + fn_resolve("F_INVEN_HAVE_ITEM_VMA", F_INVEN_HAVE_ITEM_VMA);
    const uintptr_t get_item_count = g_base + fn_resolve("F_INVEN_GET_ITEM_COUNT_VMA", F_INVEN_GET_ITEM_COUNT_VMA);
    const uintptr_t consume_item = g_base + fn_resolve("F_CONSUME_ITEM_VMA", F_CONSUME_ITEM_VMA);
    const uintptr_t remove_item = g_base + fn_resolve("F_REMOVE_ITEM_VMA", F_REMOVE_ITEM_VMA);
    const uintptr_t equip_item_from_inven_to_slot =
        g_base + fn_resolve("F_EQUIP_ITEM_FROM_INVEN_TO_SLOT_VMA", F_EQUIP_ITEM_FROM_INVEN_TO_SLOT_VMA);
    const uintptr_t put_jewel = g_base + fn_resolve("F_PUT_JEWEL_VMA", F_PUT_JEWEL_VMA);
    const uintptr_t is_having_empty_slot = g_base + fn_resolve(
        "F_INVEN_IS_HAVING_EMPTY_SLOT_VMA", F_INVEN_IS_HAVING_EMPTY_SLOT_VMA);
    const uintptr_t unequip_item_to_inven = g_base + fn_resolve(
        "F_UNEQUIP_VMA", F_UNEQUIP_VMA);
    const uintptr_t button_equip_exe = g_base + fn_resolve(
        "F_UIEQUIP_BUTTON_EQUIP_EXE_VMA", F_UIEQUIP_BUTTON_EQUIP_EXE_VMA);
    const uintptr_t button_unequip_exe = g_base + fn_resolve(
        "F_UIEQUIP_BUTTON_UNEQUIP_EXE_VMA", F_UIEQUIP_BUTTON_UNEQUIP_EXE_VMA);
    const uintptr_t ok_confirm_use_item = g_base + fn_resolve(
        "F_UIEQUIP_OK_CONFIRM_USE_ITEM_VMA", F_UIEQUIP_OK_CONFIRM_USE_ITEM_VMA);
    const uintptr_t save_item = g_base + fn_resolve(
        "F_INVEN_SAVE_ITEM_VMA", F_INVEN_SAVE_ITEM_VMA);
    const uintptr_t move_item = g_base + fn_resolve(
        "F_INVEN_MOVE_ITEM_VMA", F_INVEN_MOVE_ITEM_VMA);
    const uintptr_t equip_control_event_proc = g_base + fn_resolve(
        "F_UIEQUIP_EQUIP_CONTROL_EVENT_PROC_VMA", F_UIEQUIP_EQUIP_CONTROL_EVENT_PROC_VMA);
    const uintptr_t refresh_item_area = g_base + fn_resolve(
        "F_UIEQUIP_REFRESH_ITEM_AREA_VMA", F_UIEQUIP_REFRESH_ITEM_AREA_VMA);
    if (!target_is_executable(find_item, "INVEN_FindItem") ||
        !target_is_executable(have_item, "INVEN_HaveItem") ||
        !target_is_executable(get_item_count, "INVEN_GetItemCount") ||
        !target_is_executable(consume_item, "INVEN_ConsumeItem") ||
        !target_is_executable(remove_item, "INVEN_RemoveItem") ||
        !target_is_executable(equip_item_from_inven_to_slot, "CHAR_EquipItemFromInvenToSlot") ||
        !target_is_executable(put_jewel, "ITEMSYSTEM_PutJewel") ||
        !target_is_executable(is_having_empty_slot, "INVEN_IsHavingEmptySlot") ||
        !target_is_executable(unequip_item_to_inven, "CHAR_UnequipItemToInven") ||
        !target_is_executable(button_equip_exe, "UIEquip_ButtonEquipExe") ||
        !target_is_executable(button_unequip_exe, "UIEquip_ButtonUnequipExe") ||
        !target_is_executable(ok_confirm_use_item, "UIEquip_OKConfrimUseItem") ||
        !target_is_executable(save_item, "INVEN_SaveItem") ||
        !target_is_executable(move_item, "INVEN_MoveItem") ||
        !target_is_executable(equip_control_event_proc, "UIEquip_EquipControlEventProc") ||
        !target_is_executable(refresh_item_area, "UIEquip_RefreshItemArea")) {
        return false;
    }

    g_backup_find_item = nullptr;
    g_backup_have_item = nullptr;
    g_backup_get_item_count = nullptr;
    g_backup_consume_item = nullptr;
    g_backup_remove_item = nullptr;
    g_backup_equip_item_from_inven_to_slot = nullptr;
    g_backup_put_jewel = nullptr;
    g_backup_is_having_empty_slot = nullptr;
    g_backup_unequip_item_to_inven = nullptr;
    g_backup_button_equip_exe = nullptr;
    g_backup_button_unequip_exe = nullptr;
    g_backup_ok_confirm_use_item = nullptr;
    g_backup_save_item = nullptr;
    g_backup_move_item = nullptr;
    g_backup_refresh_item_area = nullptr;
    g_backup_equip_control_event_proc = nullptr;

    InstalledHook installed[16]{};
    std::size_t installed_count = 0;
    const auto install_hook = [&](void* target, void* replacement, void** backup,
                                  const char* name) -> bool {
        const int result = g_hook_func(target, replacement, backup);
        if (result == 0) {
            installed[installed_count++] = {target, backup, name};
        }
        if (result != 0 || backup == nullptr || *backup == nullptr) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "%s hook failed result=%d backup=%p; rolling back %zu hooks",
                                name, result, backup == nullptr ? nullptr : *backup,
                                installed_count);
            rollback_installed_hooks(installed, installed_count);
            return false;
        }
        return true;
    };

    if (!install_hook(reinterpret_cast<void*>(find_item),
                      reinterpret_cast<void*>(find_item_wrapper),
                      reinterpret_cast<void**>(&g_backup_find_item), "FindItem") ||
        !install_hook(reinterpret_cast<void*>(have_item),
                      reinterpret_cast<void*>(have_item_wrapper),
                      reinterpret_cast<void**>(&g_backup_have_item), "HaveItem") ||
        !install_hook(reinterpret_cast<void*>(get_item_count),
                      reinterpret_cast<void*>(get_item_count_wrapper),
                      reinterpret_cast<void**>(&g_backup_get_item_count), "GetItemCount") ||
        !install_hook(reinterpret_cast<void*>(consume_item),
                      reinterpret_cast<void*>(consume_item_wrapper),
                      reinterpret_cast<void**>(&g_backup_consume_item), "ConsumeItem") ||
        !install_hook(reinterpret_cast<void*>(remove_item),
                      reinterpret_cast<void*>(remove_item_wrapper),
                      reinterpret_cast<void**>(&g_backup_remove_item), "RemoveItem") ||
        !install_hook(reinterpret_cast<void*>(equip_item_from_inven_to_slot),
                      reinterpret_cast<void*>(equip_item_from_inven_to_slot_wrapper),
                      reinterpret_cast<void**>(&g_backup_equip_item_from_inven_to_slot),
                      "EquipItemFromInvenToSlot") ||
        !install_hook(reinterpret_cast<void*>(put_jewel),
                      reinterpret_cast<void*>(put_jewel_wrapper),
                      reinterpret_cast<void**>(&g_backup_put_jewel), "PutJewel") ||
        !install_hook(reinterpret_cast<void*>(is_having_empty_slot),
                      reinterpret_cast<void*>(is_having_empty_slot_wrapper),
                      reinterpret_cast<void**>(&g_backup_is_having_empty_slot),
                      "IsHavingEmptySlot") ||
        !install_hook(reinterpret_cast<void*>(unequip_item_to_inven),
                      reinterpret_cast<void*>(unequip_item_to_inven_wrapper),
                      reinterpret_cast<void**>(&g_backup_unequip_item_to_inven),
                      "UnequipItemToInven") ||
        !install_hook(reinterpret_cast<void*>(button_equip_exe),
                      reinterpret_cast<void*>(button_equip_exe_wrapper),
                      reinterpret_cast<void**>(&g_backup_button_equip_exe),
                      "ButtonEquipExe") ||
        !install_hook(reinterpret_cast<void*>(button_unequip_exe),
                      reinterpret_cast<void*>(button_unequip_exe_wrapper),
                      reinterpret_cast<void**>(&g_backup_button_unequip_exe),
                      "ButtonUnequipExe") ||
        !install_hook(reinterpret_cast<void*>(ok_confirm_use_item),
                      reinterpret_cast<void*>(ok_confirm_use_item_wrapper),
                      reinterpret_cast<void**>(&g_backup_ok_confirm_use_item),
                      "UIEquip_OKConfrimUseItem") ||
        !install_hook(reinterpret_cast<void*>(save_item),
                      reinterpret_cast<void*>(save_item_wrapper),
                      reinterpret_cast<void**>(&g_backup_save_item),
                      "SaveItem") ||
        !install_hook(reinterpret_cast<void*>(move_item),
                      reinterpret_cast<void*>(move_item_wrapper),
                      reinterpret_cast<void**>(&g_backup_move_item),
                      "MoveItem") ||
        !install_hook(reinterpret_cast<void*>(equip_control_event_proc),
                      reinterpret_cast<void*>(equip_control_event_proc_wrapper),
                      reinterpret_cast<void**>(&g_backup_equip_control_event_proc),
                      "UIEquip_EquipControlEventProc") ||
        !install_hook(reinterpret_cast<void*>(refresh_item_area),
                      reinterpret_cast<void*>(refresh_item_area_wrapper),
                      reinterpret_cast<void**>(&g_backup_refresh_item_area),
                      "UIEquip_RefreshItemArea")) {
        return false;
    }

    g_installed.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "hook install OK api=%u count=16 FindItem=%p/%p ConsumeItem=%p/%p RemoveItem=%p/%p OKConfirmUseItem=%p/%p EquipItemFromInvenToSlot=%p/%p MoveItem=%p/%p EquipControlEventProc=%p/%p RefreshItemArea=%p/%p",
                        kNativeApiVersion,
                        reinterpret_cast<void*>(find_item), reinterpret_cast<void*>(g_backup_find_item),
                        reinterpret_cast<void*>(consume_item), reinterpret_cast<void*>(g_backup_consume_item),
                        reinterpret_cast<void*>(remove_item), reinterpret_cast<void*>(g_backup_remove_item),
                        reinterpret_cast<void*>(ok_confirm_use_item), reinterpret_cast<void*>(g_backup_ok_confirm_use_item),
                        reinterpret_cast<void*>(equip_item_from_inven_to_slot),
                        reinterpret_cast<void*>(g_backup_equip_item_from_inven_to_slot),
                        reinterpret_cast<void*>(move_item),
                        reinterpret_cast<void*>(g_backup_move_item),
                        reinterpret_cast<void*>(equip_control_event_proc),
                        reinterpret_cast<void*>(g_backup_equip_control_event_proc),
                        reinterpret_cast<void*>(refresh_item_area),
                        reinterpret_cast<void*>(g_backup_refresh_item_area));
    return true;
}

}  // namespace

void inventory_native_hook_call_refresh_item_area_original() {
    const UiEquipRefreshItemAreaFn original = g_backup_refresh_item_area != nullptr
        ? g_backup_refresh_item_area : fn_ui_equip_refresh_item_area;
    if (original != nullptr) original();
}

void inventory_native_hook_on_api(const NativeAPIEntries* entries) {
    if (entries == nullptr || entries->hook_func == nullptr || entries->unhook_func == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "native_init received invalid API entries");
        return;
    }
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    if (entries->version < kNativeApiVersion) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "native API version unsupported: %u", entries->version);
        return;
    }
    g_hook_func = entries->hook_func;
    g_unhook_func = entries->unhook_func;
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "native_init called api_version=%u hook_func=%p",
                        entries->version, reinterpret_cast<void*>(g_hook_func));
    install_locked();
}

void inventory_native_hook_on_module_loaded(const char* name, void*) {
    if (name == nullptr || std::strstr(name, "libgame.so") == nullptr) return;
    __android_log_print(ANDROID_LOG_INFO, kTag, "module loaded: %s", name);
    inventory_native_hook_install_if_ready();
}

void inventory_native_hook_install_if_ready() {
    if (g_installing.exchange(true, std::memory_order_acq_rel)) return;
    {
        std::lock_guard<std::mutex> lock(g_hook_mutex);
        install_locked();
    }
    g_installing.store(false, std::memory_order_release);
}

extern "C" [[gnu::visibility("default")]] [[gnu::used]]
NativeOnModuleLoaded native_init(const NativeAPIEntries* entries) {
    inventory_native_hook_on_api(entries);
    return inventory_native_hook_on_module_loaded;
}
