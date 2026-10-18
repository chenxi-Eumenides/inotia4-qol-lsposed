#include "game_ui_virtbag.h"
#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "contracts/ownership.h"
#include "contracts/inventory_refresh.h"
#include "contracts/item_facts.h"
#include "contracts/module_save_port.h"
#include "contracts/popup_enter_alias.h"
#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/model/equip_rules.h"
#include "plugins/extension_bag/extension_bag_geometry.h"
#include "plugins/extension_bag/extension_bag_internal.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/assembly/move_merge.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/model/sale_transaction.h"
#include "plugins/extension_bag/model/draw_state_lock.h"
#include "plugins/extension_bag/model/main_menu_cleanup_edge.h"
#include "plugins/extension_bag/plugin.h"
#include "plugins/extension_bag/ui/store_sell_rules.h"

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

// 商店宿主只管理 UIStore 自己的控件、投影和详情事务，不复用 UIEquip 的视图状态。

namespace {
void store_clear_desc_locked() {
    extension_bag_ui_description_set_off();
}
void store_request_item_refresh_locked() {
    store_refresh_pending = true;
    ++store_refresh_generation;
}

void store_refresh_item_after_unlock() {
    std::unique_lock<std::mutex> lock(g_virtual_bag_mtx);
    if (!store_refresh_pending) return;
    const uint64_t refresh_generation = store_refresh_generation;
    const uint64_t view_generation = store_view_generation;
    store_refresh_pending = false;
    lock.unlock();

    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->ui.refresh_store_item == nullptr) {
        VIRTBAG_LOG("store item refresh failed reason=missing-adapter");
        lock.lock();
        if (refresh_generation == store_refresh_generation) store_request_item_refresh_locked();
        return;
    }
    const QolResult refresh_result = api->ui.refresh_store_item();
    if (!qol::contracts::succeeded(refresh_result)) {
        VIRTBAG_LOG("store item refresh failed code=%d", refresh_result.code);
        lock.lock();
        if (refresh_generation == store_refresh_generation) store_request_item_refresh_locked();
        return;
    }

    lock.lock();
    if (refresh_generation != store_refresh_generation ||
        view_generation != store_view_generation) return;
    if (!store_frame_active || !g_virtual_bag_enabled.load() ||
        !extension_bag_session_admitted_locked()) {
        if (store_view_installed) store_restore_module_view_locked();
        return;
    }
    if (!store_view_installed) return;
    void* live_root = extension_bag_ui_root(qol::contracts::QolUiRootKind::kStoreItemRoot);
    if (live_root == nullptr) {
        VIRTBAG_LOG("store item refresh completed without item root; retry scheduled");
        store_request_item_refresh_locked();
        return;
    }
    store_projected_item_root = live_root;
    store_refresh_projection_locked();
}
int store_original_bag_button_index(int64_t x, int64_t y) {
    void* container = extension_bag_ui_root(qol::contracts::QolUiRootKind::kStoreBagContainer);
    if (container == nullptr) return -1;
    int count = 0;
    if (!extension_bag_ui_child_count(container, &count)) return -1;
    const int limit = count < 6 ? count : 6;
    for (int index = 0; index < limit; ++index) {
        void* control = valid_child_locked(container, index);
        if (control == nullptr) continue;
        UiRect rect{};
        if (!extension_bag_ui_rect(control, &rect)) continue;
        if (rect.w > 0 && rect.h > 0 && extension_bag_ui_hit_test(control, x, y, rect)) return index;
    }
    return -1;
}

void store_restore_for_original_event_locked(int64_t x, int64_t y) {
    if (!store_view_installed) return;
    if (store_original_bag_button_index(x, y) < 0) return;
    store_restore_module_view_locked();
    // 注意：不得 disable 扩展标签——装备页宿主切回原版袋后扩展标签仍可点回
    // 扩展视图；disable 只会清空指针（控件仍在袋容器上），draw_end 再次 install
    // 时因 store_tab_root 已清空无法删除旧控件，会导致标签重复创建。
}
}  // namespace

void store_refresh_projection_locked() {
    if (!store_view_installed || store_projected_item_root == nullptr ||
        !virtual_bag::valid_index(store_view_bag)) return;
    const int capacity = g_virtual_bag_state.capacities[store_view_bag];
    for (int slot = 0; slot < capacity; ++slot) {
        void* control = valid_child_locked(store_projected_item_root, slot);
        if (control == nullptr) continue;
        void* expected = module_item_locked(store_view_bag, slot);
        void* data = extension_bag_ui_data(control);
        void* current = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
        if (current != expected) (void)extension_bag_ui_set_control_item(control, expected);
    }
}
void store_install_module_view_locked(int bag) {
    if (!extension_bag_session_admitted_locked() || !virtual_bag::valid_index(bag) ||
        g_virtual_bag_state.capacities[bag] == 0 ||
        g_game == nullptr || g_write_port->ui.refresh_store_item == nullptr) return;
    const int original_bag = original_bag_locked();
    if (!virtual_bag::valid_original_transaction_bag(original_bag)) {
        VIRTBAG_LOG("store view rejected original bag=%d", original_bag);
        return;
    }
    if (store_view_installed) store_restore_module_view_locked();
    uint32_t size_word = 0;
    if (!bag_size_word_locked(original_bag, &size_word)) {
        VIRTBAG_LOG("store view rejected capacity read window=%d", original_bag);
        return;
    }
    store_clear_desc_locked();

    store_window_original_bag = original_bag;
    store_original_size_word = size_word;
    store_original_bag_size = size_word;
    constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
    if (!set_bag_size_word_locked(
            original_bag, (size_word & ~kCapacityMask) |
                              (static_cast<uint32_t>(g_virtual_bag_state.capacities[bag]) & kCapacityMask))) {
        VIRTBAG_LOG("store view rejected capacity write bag=%d window=%d", bag, original_bag);
        store_original_size_word = 0;
        store_original_bag_size = 0;
        store_window_original_bag = -1;
        return;
    }

    store_projected_item_root = extension_bag_ui_root(qol::contracts::QolUiRootKind::kStoreItemRoot);
    if (store_projected_item_root == nullptr) {
        if (!set_bag_size_word_locked(
                original_bag, (store_original_size_word & ~kCapacityMask) |
                                  (store_original_bag_size & kCapacityMask))) {
            VIRTBAG_LOG("store view rollback capacity failed window=%d", original_bag);
        }
        store_original_size_word = 0;
        store_original_bag_size = 0;
        store_window_original_bag = -1;
        store_projected_item_root = nullptr;
        store_request_item_refresh_locked();
        return;
    }
    const int capacity = g_virtual_bag_state.capacities[bag];
    for (int slot = 0; slot < capacity; ++slot) {
        if (module_item_locked(bag, slot) != nullptr) {
            ownership::borrow_for_view(&g_ownership_ledger,
                                       g_module_object_handles[bag][slot]);
        }
    }
    store_view_bag = bag;
    store_view_installed = true;
    ++store_view_generation;
    store_request_item_refresh_locked();
    VIRTBAG_LOG("store view installed bag=%d window=%d capacity=%d", bag, original_bag,
                g_virtual_bag_state.capacities[bag]);
}
bool store_module_view_installed_locked() { return store_view_installed; }

void store_restore_module_view_locked() {
    if (!store_view_installed) return;
    if (virtual_bag::valid_index(store_view_bag)) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            const uint32_t handle = g_module_object_handles[store_view_bag][slot];
            if (ownership::live_state(g_ownership_ledger, handle,
                                      ownership::State::kBorrowedForView)) {
                ownership::return_from_view(&g_ownership_ledger, handle);
            }
        }
    }
    store_clear_desc_locked();
    if (store_original_size_word != 0) {
        constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
        if (!set_bag_size_word_locked(
                store_window_original_bag, (store_original_size_word & ~kCapacityMask) |
                                                (store_original_bag_size & kCapacityMask))) {
            VIRTBAG_LOG("store restore capacity failed window=%d", store_window_original_bag);
        }
    }
    store_original_size_word = 0;
    store_original_bag_size = 0;
    store_window_original_bag = -1;
    store_projected_item_root = nullptr;
    store_view_bag = -1;
    store_view_installed = false;
    ++store_view_generation;
    store_request_item_refresh_locked();
}
uint64_t store_event_wrapper(uint64_t event, uint64_t param, uint64_t param2) {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"own:popup.store"});
    int64_t x = 0;
    int64_t y = 0;
    if ((event == 0x17 || event == 0x18) && param != 0) {
        x = *reinterpret_cast<const int64_t*>(param);
        y = *reinterpret_cast<const int64_t*>(param + 8);
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        store_restore_for_original_event_locked(x, y);
    }
    store_refresh_item_after_unlock();
    return store_original_event != nullptr ? store_original_event(event, param, param2) : 0;
}
void store_enter_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"own:popup.store"});
    if (store_original_enter != nullptr) store_original_enter();
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        store_teardown_locked();
        set_original_bag_locked(0);
        store_frame_active = true;
        store_install_tab_buttons_locked();
        store_request_item_refresh_locked();
    }
    store_refresh_item_after_unlock();
}

void store_f3_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"own:popup.store"});
    StoreSceneNoArgFn original = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        store_teardown_locked();
        store_frame_active = false;
        original = store_original_f3;
    }
    store_refresh_item_after_unlock();
    if (original != nullptr) original();
}
void store_draw_end_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"patch:P_EXT_STORE_DRAW_END"});
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (store_frame_active && g_virtual_bag_enabled.load() &&
            extension_bag_session_admitted_locked()) {
            ensure_state_loaded_locked();
            store_install_tab_buttons_locked();
            store_commit_pending_tab_locked();
            store_refresh_projection_locked();
            store_draw_tab_buttons_locked();
        } else if (store_frame_active) {
            store_delete_tab_buttons_locked();
            store_teardown_locked();
            store_frame_active = false;
        }
    }
    store_refresh_item_after_unlock();
    if (g_sdk != nullptr && g_sdk->ui != nullptr && g_sdk->ui->end_frame != nullptr) {
        (void)g_sdk->ui->end_frame();
    }
}

void store_make_desc_gate(void* ctrl, void* arg) {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"patch:P_EXT_STORE_DESC"});
    // 详情打开仍走原版 MakeDesc。W1 不再在此捕获详情身份、也不再替换出售按钮回调
    // （原全局 popup OK/Cancel 槽劫持已删除）；扩展出售改为
    // UIStore_SellItem 函数入口按 cursor item 识别，原版面板/确认/取消全部保留。
    if (ctrl == nullptr || g_game == nullptr ||
        g_write_port->ui.make_item_description == nullptr) return;
    (void)arg;
    (void)g_write_port->ui.make_item_description(ctrl);
}
void store_teardown_locked() {
    store_restore_module_view_locked();
    store_disable_tab_buttons_locked();
    for (void*& item : store_tab_bag_items) {
        if (item != nullptr) {
            if (defer_item_free_locked(item)) item = nullptr;
        }
    }
    store_frame_active = false;
}
