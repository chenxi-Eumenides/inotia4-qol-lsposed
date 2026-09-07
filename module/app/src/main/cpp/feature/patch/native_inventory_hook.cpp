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
EquipItemFromInvenToSlotFn g_backup_equip_item_from_inven_to_slot = nullptr;
PutJewelFn g_backup_put_jewel = nullptr;
IsHavingEmptySlotFn g_backup_is_having_empty_slot = nullptr;
UnequipFn g_backup_unequip_item_to_inven = nullptr;
ButtonEquipExeFn g_backup_button_equip_exe = nullptr;
ButtonUnequipExeFn g_backup_button_unequip_exe = nullptr;

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
    stage4_consume_item(item,
                        extension_bag_enabled() ? extension_bag_identify_native_item : nullptr,
                        extension_bag_enabled() ? extension_bag_consume_native_item : nullptr,
                        g_backup_consume_item,
                        g_in_consume_item);
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
    if (extension_bag_handle_backpack_button_equip()) return;
    g_backup_button_equip_exe(button);
}

void button_unequip_exe_wrapper(void* button) {
    // 卸下按钮函数级接管：仅 desc_type=1（卸原版袋）且原版背包满时，模块
    // 把袋对象放回原版其他袋行或收编扩展空位；物品绝不消失。卸装备与其他
    // 场景一律走原函数。
    bool no_space = false;
    if (extension_bag_handle_original_bag_unequip(&no_space)) {
        if (no_space) extension_bag_show_no_space_popup();
        return;
    }
    g_backup_button_unequip_exe(button);
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
        !target_is_executable(button_unequip_exe, "UIEquip_ButtonUnequipExe")) {
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

    InstalledHook installed[11]{};
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
                      "ButtonUnequipExe")) {
        return false;
    }

    g_installed.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "hook install OK api=%u FindItem=%p/%p ConsumeItem=%p/%p RemoveItem=%p/%p EquipItemFromInvenToSlot=%p/%p",
                        kNativeApiVersion,
                        reinterpret_cast<void*>(find_item), reinterpret_cast<void*>(g_backup_find_item),
                        reinterpret_cast<void*>(consume_item), reinterpret_cast<void*>(g_backup_consume_item),
                        reinterpret_cast<void*>(remove_item), reinterpret_cast<void*>(g_backup_remove_item),
                        reinterpret_cast<void*>(equip_item_from_inven_to_slot),
                        reinterpret_cast<void*>(g_backup_equip_item_from_inven_to_slot));
    return true;
}

}  // namespace

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
