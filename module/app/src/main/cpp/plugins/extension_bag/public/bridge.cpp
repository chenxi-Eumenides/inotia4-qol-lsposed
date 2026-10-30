#include "plugins/extension_bag/core/ui_state.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/runtime/state_access.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"

#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "contracts/ownership.h"
#include "contracts/inventory_refresh.h"
#include "contracts/item_facts.h"
#include "plugins/extension_bag/model/equip_rules.h"
#include "plugins/extension_bag/runtime/extension_bag_port.h"
#include "plugins/extension_bag/core/move_merge.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/model/sale_transaction.h"
#include "plugins/extension_bag/model/draw_state_lock.h"
#include "plugins/extension_bag/model/main_menu_cleanup_edge.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "contracts/module_save_port.h"
#include "contracts/popup_enter_alias.h"
#include "plugins/extension_bag/core/geometry.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/plugin.h"
#include "plugins/extension_bag/model/store_sell_rules.h"
#include "plugins/extension_bag/runtime/entry_guard.h"
#include "plugins/extension_bag/runtime/bag_state_store.h"
#include "plugins/extension_bag/runtime/inventory_access.h"
#include "plugins/extension_bag/runtime/item_access.h"
#include "plugins/extension_bag/core/lifecycle.h"
#include "plugins/extension_bag/runtime/ownership_port.h"
#include "plugins/extension_bag/core/projection.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/core/observation.h"
#include "plugins/extension_bag/core/tabs.h"
#include "plugins/extension_bag/runtime/status_snapshot.h"
#include "plugins/extension_bag/transaction/transaction.h"
#include "plugins/extension_bag/core/host_adapter.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <unistd.h>


uint64_t virtual_bag_observe_item_proc_pre(void* control, uint64_t event, void* x2, void* param) {
    if (g_p5_observation_active || !p5_observation_enabled() ||
        !p5_should_begin_item_observation(event)) {
        return 0;
    }
    g_p5_observation_active = true;
    ::qol::extension_bag::ReentryLock lock;
    const uint64_t observation_token =
        log_p5_observation_locked("item-pre", control, event, x2 != nullptr, param != nullptr,
                                  nullptr, false, 0);
    if (observation_token == 0) g_p5_observation_active = false;
    return observation_token;
}

void virtual_bag_observe_item_proc_source(uint64_t observation_token, void* control, uint64_t event,
                                          void* x2, void* param, void* source_control) {
    if (observation_token == 0) return;
    ::qol::extension_bag::ReentryLock lock;
    log_p5_observation_locked("item-source", control, event, x2 != nullptr, param != nullptr,
                              source_control, false, 0, -1, -1, observation_token);
}

void virtual_bag_observe_item_proc_post(uint64_t observation_token, void* control, uint64_t event,
                                        void* x2, void* param, uint64_t result) {
    if (observation_token == 0) return;
    (void)control;
    {
        ::qol::extension_bag::ReentryLock lock;
        // The original proc may have rebuilt the control tree. Do not dereference its prior control.
        log_p5_observation_locked("item-post", nullptr, event, x2 != nullptr, param != nullptr,
                                  nullptr, true, result, -1, -1, observation_token);
    }
    g_p5_observation_active = false;
}

bool virtual_bag_is_inventory_enter(uintptr_t enter) {
    ::qol::extension_bag::ReentryLock lock;
    const uintptr_t inventory_enter = qol::extension_bag::ui_state().persistence_hooks.inventory_enter;
    return inventory_enter != 0 && enter == inventory_enter;
}

bool set_virtual_bag_enabled(bool enabled) {
    {
        ::qol::extension_bag::ReentryLock lock;
        ::qol::extension_bag::access::enabled().store(enabled);
        if (!enabled) {
            teardown_extension_bag_hosts_locked();
            ::qol::extension_bag::access::bag().mode = virtual_bag::Mode::kOriginal;
            ::qol::extension_bag::access::bag().selected = -1;
            ::qol::extension_bag::access::bag().inspected = -1;
            qol::extension_bag::ui_state().ui.extension_drag = {};
            qol::extension_bag::ui_state().ui.extension_touch_capture = false;
        }
    }
    if (!set_extension_source_protection_enabled(enabled)) {
        VIRTBAG_LOG("extension source protection update deferred enabled=%d",
                    enabled ? 1 : 0);
    }
    if (enabled) {
        ensure_lifecycle_task();
    } else {
        remove_lifecycle_task();
    }
    return true;
}

bool virtual_bag_enabled() {
    return ::qol::extension_bag::access::enabled().load();
}

bool virtual_bag_prepare_save_slot_load() {
    ::qol::extension_bag::ReentryLock lock;
    if (!module_cache_clear_preflight_locked()) {
        VIRTBAG_LOG("prepare save-slot load preflight failed");
        return false;
    }
    if (qol::extension_bag::ui_state().objects.module_view_installed) restore_module_view_locked();
    if (!clear_module_cache_locked()) {
        VIRTBAG_LOG("prepare save-slot load blocked by active module object");
        return false;
    }
    // The item pool is reset by the load; queued addresses become invalid and may be
    // reused by the new save. Free what is safe now, then drop the rest.
    drain_deferred_frees_locked();
    clear_deferred_free_queue_locked();
    ::qol::extension_bag::access::bag() = {};
    ::qol::extension_bag::access::persistence().loaded_slot = -2;
    disable_extension_tab_buttons_locked();
    qol::extension_bag::ui_state().ui.extension_tab_buttons.fill(nullptr);
    return true;
}

bool virtual_bag_prepare_main_menu() {
    ::qol::extension_bag::ReentryLock lock;
    return prepare_main_menu_locked();
}

// P3c：SlotBound 写入统一经 save.slot_data 的 request_write（安全点排空），provider 在
// 原版保存事务内捕获提交；旧 ModuleSaveCoordinator 参与者序退役，本三函数删除。
void virtual_bag_ui_register_bridge(JNIEnv* env, jclass bridge_class) {
    if (env == nullptr || bridge_class == nullptr) return;
    ::qol::extension_bag::ReentryLock lock;
    if (::qol::extension_bag::access::persistence().bridge_class != nullptr) env->DeleteGlobalRef(::qol::extension_bag::access::persistence().bridge_class);
    ::qol::extension_bag::access::persistence().bridge_class = static_cast<jclass>(env->NewGlobalRef(bridge_class));
    // P3c：旧 module_save 参与者注册已退出——保存协调唯一走 save.slot_data + core.save
    // 协调器；此处不得保留第二套参与者顺序。
}
