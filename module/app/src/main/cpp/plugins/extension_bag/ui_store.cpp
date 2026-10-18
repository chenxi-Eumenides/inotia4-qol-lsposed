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
#include "plugins/extension_bag/extension_bag_equip_rules.h"
#include "plugins/extension_bag/extension_bag_geometry.h"
#include "plugins/extension_bag/extension_bag_internal.h"
#include "plugins/extension_bag/extension_bag_logging.h"
#include "plugins/extension_bag/move_merge.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/model/sale_transaction.h"
#include "plugins/extension_bag/model/draw_state_lock.h"
#include "plugins/extension_bag/model/main_menu_cleanup_edge.h"
#include "plugins/extension_bag/plugin.h"
#include "plugins/extension_bag/store_sell_rules.h"

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

using StoreSellItemFn = int64_t (*)(int64_t);

bool store_view_installed = false;
int store_view_bag = -1;
int store_window_original_bag = -1;
uint32_t store_original_size_word = 0;
uint32_t store_original_bag_size = 0;
void* store_projected_item_root = nullptr;
std::array<void*, virtual_bag::kBagCount> store_tab_buttons{};
std::array<void*, virtual_bag::kBagCount> store_tab_bag_items{};
void* store_tab_root = nullptr;
uint64_t store_tab_generation = 0;
int store_pending_tab = -1;
uint64_t store_pending_tab_generation = 0;
bool store_frame_active = false;
uint64_t store_view_generation = 0;
uint64_t store_refresh_generation = 0;
bool store_refresh_pending = false;

uint64_t store_tab_item_proc(void*, uint64_t, void*, void*);

void store_clear_desc_locked() {
    extension_bag_ui_description_set_off();
}

int store_tab_index(void* ctrl) {
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        if (store_tab_buttons[index] == ctrl) return index;
    }
    return -1;
}

void store_refresh_tab_bag_items_locked() {
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        const uint8_t type = g_virtual_bag_state.types[index];
        void*& item = store_tab_bag_items[index];
        if (type == 0) {
            if (item != nullptr) {
                if (defer_item_free_locked(item)) item = nullptr;
            }
            continue;
        }
        if (item != nullptr) {
            int category = -1;
            if (!read_item_category(item, &category) || category != static_cast<int>(type)) {
                if (defer_item_free_locked(item)) item = nullptr;
            }
        }
        if (item == nullptr) {
            item = extension_create_item(static_cast<int32_t>(type), 0, 0, 0);
            if (item != nullptr) {
                uint32_t flags = 0;
                const auto* count_api = item_count_api();
                if (count_api != nullptr && count_api->read_encoded_field != nullptr &&
                    count_api->write_encoded_field != nullptr &&
                    qol::contracts::succeeded(count_api->read_encoded_field(item, &flags))) {
                    (void)count_api->write_encoded_field(
                        item, ::qol::features::extension_bag::write_native_bag_object_marker(flags));
                }
            }
        }
    }
}

void store_disable_tab_buttons_locked() {
    ++store_tab_generation;
    store_pending_tab = -1;
    store_pending_tab_generation = 0;
    store_tab_buttons.fill(nullptr);
    store_tab_root = nullptr;
}

void store_delete_tab_buttons_locked() {
    for (void*& button : store_tab_buttons) {
        if (button != nullptr && !extension_bag_ui_delete_control(button)) {
            VIRTBAG_LOG("store tab delete failed control=%p", button);
        }
        button = nullptr;
    }
}

bool store_all_tab_buttons_installed_locked() {
    for (void* button : store_tab_buttons) {
        if (button == nullptr) return false;
    }
    return true;
}

bool store_has_tab_buttons_locked() {
    for (void* button : store_tab_buttons) {
        if (button != nullptr) return true;
    }
    return false;
}

void store_discard_new_tab_controls_locked(
    std::array<void*, virtual_bag::kBagCount>& controls, int failed_index,
    void* failed_control, const char* stage) {
    VIRTBAG_LOG("store tab install failed index=%d stage=%s", failed_index, stage);
    if (failed_control != nullptr && !extension_bag_ui_delete_control(failed_control)) {
        VIRTBAG_LOG("store tab cleanup failed index=%d control=%p", failed_index,
                    failed_control);
    }
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        void*& control = controls[index];
        if (control == nullptr) continue;
        if (!extension_bag_ui_delete_control(control)) {
            VIRTBAG_LOG("store tab cleanup failed index=%d control=%p", index, control);
        }
        control = nullptr;
    }
}

void store_install_tab_buttons_locked() {
    // 配置门控：扩展背包关闭时商店页签不得挂载（3 个宿主之一）。
    if (!g_virtual_bag_enabled.load() || !extension_bag_session_admitted_locked()) return;
    if (g_sdk == nullptr) return;
    void* bag_container = extension_bag_ui_root(qol::contracts::QolUiRootKind::kStoreBagContainer);
    int bag_child_count = 0;
    if (bag_container == nullptr || !extension_bag_ui_child_count(bag_container, &bag_child_count) ||
        bag_child_count < 6) return;
    if (store_tab_root == bag_container && store_all_tab_buttons_installed_locked()) return;

    if (store_tab_root != nullptr || store_has_tab_buttons_locked()) {
        store_delete_tab_buttons_locked();
        store_disable_tab_buttons_locked();
    }
    store_refresh_tab_bag_items_locked();

    int64_t bag0_abs_x = 0;
    int64_t bag0_abs_y = 0;
    void* bag0 = valid_child_locked(bag_container, 0);
    if (bag0 == nullptr) return;
    extension_bag_control_abs_pos(bag0, &bag0_abs_x, &bag0_abs_y);
    UiRect bag0_rect{};
    if (!extension_bag_ui_rect(bag0, &bag0_rect) || bag0_rect.w <= 0) return;

    int64_t mount_abs_x = 0;
    int64_t mount_abs_y = 0;
    extension_bag_control_abs_pos(bag_container, &mount_abs_x, &mount_abs_y);
    // 这里沿用原版袋列的右侧间距 68、下移 2；起点仍来自商店实时袋 0 rect。
    const int64_t tab_rel_x = bag0_abs_x + 68 - mount_abs_x;
    const int64_t tab_rel_y = bag0_abs_y + 2 - mount_abs_y;
    std::array<void*, virtual_bag::kBagCount> controls{};
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        void* control = nullptr;
        if (!extension_bag_ui_create_control(bag_container, &control)) {
            store_discard_new_tab_controls_locked(controls, index, control, "create");
            return;
        }
        if (control == nullptr) {
            store_discard_new_tab_controls_locked(controls, index, nullptr, "create-null");
            return;
        }
        controls[index] = control;
        if (!extension_bag_ui_set_control_proc(control, &store_tab_item_proc)) {
            store_discard_new_tab_controls_locked(controls, index, nullptr, "proc");
            return;
        }
        if (!extension_bag_ui_set_control_user_type(control, 2)) {
            store_discard_new_tab_controls_locked(controls, index, nullptr, "user-type");
            return;
        }
        if (!extension_bag_ui_unuse_control_event_move(control)) {
            store_discard_new_tab_controls_locked(controls, index, nullptr, "event-move");
            return;
        }
        if (!extension_bag_ui_set_rect(control, {tab_rel_x, tab_rel_y + index * 70, 57, 57})) {
            store_discard_new_tab_controls_locked(controls, index, nullptr, "rect");
            return;
        }
        void* data = nullptr;
        if (!extension_bag_ui_allocate_control_data(0x10, &data)) {
            store_discard_new_tab_controls_locked(controls, index, nullptr, "data-allocate");
            return;
        }
        if (data == nullptr) {
            store_discard_new_tab_controls_locked(controls, index, nullptr, "data-null");
            return;
        }
        *reinterpret_cast<void**>(data) = store_tab_bag_items[index];
        if (!extension_bag_ui_set_control_data(control, data)) {
            store_discard_new_tab_controls_locked(controls, index, nullptr, "data-set");
            return;
        }
    }
    store_tab_buttons = controls;
    store_tab_root = bag_container;
    ++store_tab_generation;
    VIRTBAG_LOG("store tabs installed root=%p generation=%llu", bag_container,
                static_cast<unsigned long long>(store_tab_generation));
}

uint64_t store_tab_item_proc(void* ctrl, uint64_t event, void*, void*) {
    if (event == 0x02) {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (!extension_bag_session_admitted_locked()) return 1;
        const int index = store_tab_index(ctrl);
        if (index >= 0 && store_tab_generation != 0) {
            store_pending_tab = index;
            store_pending_tab_generation = store_tab_generation;
        }
        return 1;
    }
    // 商店宿主不接管拖拽，避免扩展标签成为拖动源或跨袋目标。
    return 0;
}

void store_install_module_view_locked(int bag);

void store_commit_pending_tab_locked() {
    if (!extension_bag_session_admitted_locked()) {
        store_pending_tab = -1;
        store_pending_tab_generation = 0;
        return;
    }
    if (!virtual_bag::valid_index(store_pending_tab) ||
        store_pending_tab_generation != store_tab_generation) {
        store_pending_tab = -1;
        store_pending_tab_generation = 0;
        return;
    }
    const int bag = store_pending_tab;
    store_pending_tab = -1;
    store_pending_tab_generation = 0;
    if (store_view_installed && store_view_bag == bag) return;
    store_install_module_view_locked(bag);
}

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

}  // namespace

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

namespace {

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

namespace {

void store_draw_tab_buttons_locked() {
    if (!extension_bag_ui_can_draw()) return;
    for (int index = virtual_bag::kBagCount - 1; index >= 0; --index) {
        void* button = store_tab_buttons[index];
        if (button == nullptr) continue;
        int64_t x = 0;
        int64_t y = 0;
        extension_bag_control_abs_pos(button, &x, &y);
        (void)extension_bag_ui_draw_part(0xf, 0x14, static_cast<int32_t>(x - 5),
                                         static_cast<int32_t>(y - 12), 0, 1);
    }
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        void* button = store_tab_buttons[index];
        if (button == nullptr) continue;
        int64_t x = 0;
        int64_t y = 0;
        extension_bag_control_abs_pos(button, &x, &y);
        const bool selected = store_view_installed && store_view_bag == index;
        const bool equipped = g_virtual_bag_state.capacities[index] != 0;
        if (selected) {
            (void)extension_bag_ui_draw_part(0xf, 0x13, static_cast<int32_t>(x - 5),
                                             static_cast<int32_t>(y - 12), 0, 1);
            (void)extension_bag_ui_draw_part(0xf, 6, static_cast<int32_t>(x + 5),
                                             static_cast<int32_t>(y + 5), 0, 1);
        } else if (equipped) {
            (void)extension_bag_ui_draw_part(0xf, 6, static_cast<int32_t>(x + 5),
                                             static_cast<int32_t>(y + 5), 0, 0x28);
        } else {
            (void)extension_bag_ui_draw_part(0xf, 9, static_cast<int32_t>(x + 5),
                                             static_cast<int32_t>(y + 5), 0, 0x28);
        }
    }
}

}  // namespace

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

namespace {

void store_sell_popup(uint32_t text_id) {
    if (g_sdk != nullptr && g_sdk->ui != nullptr && g_sdk->ui->show_text_popup != nullptr) {
        (void)g_sdk->ui->show_text_popup(static_cast<int32_t>(text_id));
    }
}

// W1 部分出售：把模式视图数量从 view 递减 count（不调用原版 ITEMSYSTEM_Divide）。
// 写回经 item-count facade（R-47 决策 b：启用态拆段进位、关闭态只写 b 保留 a），
// 随后重序列化 payload 并把 descriptor.count 回写为 canonical（R-38/R-48）。
bool decrement_extension_item_view_locked(int bag, int slot, void* item, int count,
                                          const char* context) {
    if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount ||
        item == nullptr || count <= 0) {
        return false;
    }
    virtual_bag::Item& descriptor = g_virtual_bag_state.items[bag][slot];
    const int view = static_cast<int>(virtual_bag_effective_count(
        item, static_cast<uint32_t>(descriptor.count)));
    if (count >= view) return false;  // 整堆由调用方走删除分支
    const auto* count_api = item_count_api();
    uint32_t old_field = 0;
    if (count_api == nullptr || count_api->read_encoded_field == nullptr ||
        count_api->write_encoded_field == nullptr ||
        !qol::contracts::succeeded(count_api->read_encoded_field(item, &old_field))) return false;
    if (!virtual_bag_write_effective(item, static_cast<uint32_t>(view - count))) return false;
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    if (!serialize_item_payload_locked(item, &payload, &payload_size)) {
        (void)count_api->write_encoded_field(item, old_field);
        VIRTBAG_LOG("store sell reserialize failed context=%s bag=%d slot=%d", context, bag, slot);
        return false;
    }
    descriptor.payload = payload;
    descriptor.payload_size = payload_size;
    descriptor.count = static_cast<int>(virtual_bag_canonical_count(
        item, virtual_bag_canonical_from_field(old_field, static_cast<uint32_t>(descriptor.count))));
    g_module_object_categories[bag][slot] = descriptor.category;
    g_module_object_hashes[bag][slot] = virtual_bag::payload_hash(descriptor);
    extension_bag_mark_dirty_locked();
    return true;
}

// W1：UIStore_SellItem@0xd25f0 入口接管。原版只收总价 x0（弹窗载荷），数量由函数内部
// 自行读取并走 INVEN_RemoveItem/ITEMSYSTEM_Divide；模块在入口层用同一「载荷/单价」
// 反推数量后接管结算，原版物品一律 backup。
}  // namespace

int64_t store_sell_item_wrapper(int64_t displayed_price) {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:UIStore_SellItem"});
    if (g_backup_store_sell_item == nullptr) return 0;
    if (!g_virtual_bag_enabled.load()) return reinterpret_cast<StoreSellItemFn>(g_backup_store_sell_item)(displayed_price);

    void* item = nullptr;
    int bag = -1;
    int slot = -1;
    int view = 0;
    bool item_encoded = false;
    bool module_item = false;
    bool reject = false;
    qol::store_sell::Plan plan{};
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (store_view_installed && g_game != nullptr &&
            g_inventory_extras->read.cursor_item != nullptr &&
            g_item_facts != nullptr && g_item_facts->sell_price != nullptr) {
            void* ctrl = extension_bag_ui_root(qol::contracts::QolUiRootKind::kStoreItemRoot);
            void* cursor = nullptr;
            if (ctrl != nullptr) {
                (void)g_inventory_extras->read.cursor_item(ctrl, &cursor);
            }
            item = cursor != nullptr ? extension_bag_ui_item(cursor) : nullptr;
            if (item != nullptr && module_slot_of_item_locked(item, &bag, &slot)) {
                module_item = true;
                const bool identity_ok =
                    bag == store_view_bag && virtual_bag::valid_index(bag) && slot >= 0 &&
                    slot < virtual_bag::kSlotCount && g_module_objects[bag][slot] == item &&
                    module_object_release_allowed_locked(bag, slot, item, "store sell", false,
                                                         nullptr);
                const virtual_bag::Item& descriptor = g_virtual_bag_state.items[bag][slot];
                const bool descriptor_ok =
                    descriptor.category > 0 && descriptor.count > 0 &&
                    g_module_object_categories[bag][slot] == descriptor.category &&
                    g_module_object_hashes[bag][slot] == virtual_bag::payload_hash(descriptor);
                if (!identity_ok || !descriptor_ok) {
                    reject = true;
                } else {
                    view = static_cast<int>(virtual_bag_effective_count(
                        item, static_cast<uint32_t>(descriptor.count)));
                    item_encoded = extension_bag_ui_item_count_encoding(item) ==
                                   ::qol::contracts::QolItemCountEncoding::kEncoded;
                    int64_t unit = 0;
                    if (!extension_bag_ui_read_item_sell_price(item, &unit)) {
                        reject = true;
                    } else {
                        plan = qol::store_sell::plan(item_encoded, view, unit, displayed_price);
                        if (plan.kind == qol::store_sell::PlanKind::kReject) reject = true;
                    }
                }
            }
        }
    }
    if (!module_item) return reinterpret_cast<StoreSellItemFn>(g_backup_store_sell_item)(displayed_price);
    if (reject) {
        VIRTBAG_LOG("store sell reject reason=identity_or_plan bag=%d slot=%d", bag, slot);
        store_sell_popup(0xc);
        return 0;
    }

    const int count = plan.kind == qol::store_sell::PlanKind::kWhole ? 1 : plan.count;
    const int64_t price = extension_bag_sell_price(item, count, false);
    if (price < 0 || price != displayed_price || extension_bag_write_api() == nullptr ||
        extension_bag_write_api()->equipment.add_money == nullptr ||
        extension_bag_write_api()->equipment.minus_money == nullptr) {
        VIRTBAG_LOG("store sell reject reason=price_mismatch bag=%d slot=%d count=%d price=%lld displayed=%lld",
                    bag, slot, count, static_cast<long long>(price),
                    static_cast<long long>(displayed_price));
        store_sell_popup(0xc);
        return 0;
    }
    if (!extension_bag_ui_add_money(price)) {
        VIRTBAG_LOG("store sell credit failed bag=%d slot=%d price=%lld", bag, slot,
                    static_cast<long long>(price));
        store_sell_popup(0xc);
        return 0;
    }

    bool committed = false;
    bool whole = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        const bool current =
            virtual_bag::valid_index(bag) && slot >= 0 && slot < virtual_bag::kSlotCount &&
            g_module_objects[bag][slot] == item &&
            module_object_release_allowed_locked(bag, slot, item, "store sell", false, nullptr) &&
            (!item_encoded ||
             static_cast<int>(virtual_bag_effective_count(
                 item, static_cast<uint32_t>(g_virtual_bag_state.items[bag][slot].count))) == view);
        if (current) {
            whole = plan.kind == qol::store_sell::PlanKind::kWhole || count >= view;
            if (whole) {
                const virtual_bag::Item previous_item = g_virtual_bag_state.items[bag][slot];
                g_virtual_bag_state.items[bag][slot] = {};
                extension_bag_mark_dirty_locked();
                if (!free_module_object_locked(bag, slot)) {
                    g_virtual_bag_state.items[bag][slot] = previous_item;
                    persist_state_locked();
                    VIRTBAG_LOG("store sell rejected object release bag=%d slot=%d", bag, slot);
                } else {
                    persist_state_locked();
                    committed = true;
                }
            } else if (decrement_extension_item_view_locked(bag, slot, item, count, "store sell")) {
                persist_state_locked();
                committed = true;
            }
        }
        if (committed && store_view_installed && store_view_bag == bag) {
            // R-12：只用商店投影刷新；调用原版 UIStore_RefreshInvenItem 会把整列刷回
            // 窗口原版袋内容，破坏扩展投影。
            store_refresh_projection_locked();
        }
    }
    if (!committed) {
        const bool refunded = extension_bag_ui_minus_money(price);
        VIRTBAG_LOG("store sell remove failed bag=%d slot=%d count=%d price=%lld refunded=%d",
                    bag, slot, count, static_cast<long long>(price), refunded ? 1 : 0);
        store_sell_popup(0xc);
        return 0;
    }
    store_sell_popup(0x1f);
    VIRTBAG_LOG("store sell committed bag=%d slot=%d count=%d whole=%d view=%d price=%lld",
                bag, slot, count, whole ? 1 : 0, view, static_cast<long long>(price));
    return 0;
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

namespace {

enum class StoreSaveSlotLookup : uint8_t { kFound, kNotFound, kError };

StoreSaveSlotLookup store_find_original_slot_with_real_capacity(void* item,
                                                                 int32_t* out_slot) {
    if (item == nullptr || out_slot == nullptr || g_game == nullptr ||
        g_inventory_extras == nullptr || g_inventory_extras->read.find_save_slot == nullptr) {
        VIRTBAG_LOG("store buy gate original lookup unavailable item=%p", item);
        return StoreSaveSlotLookup::kError;
    }

    std::unique_lock<std::mutex> lock(g_virtual_bag_mtx);
    const bool projected = store_view_installed;
    const int original_bag = store_window_original_bag;
    const uint64_t view_generation = store_view_generation;
    uint32_t projected_size_word = 0;
    if (projected) {
        if (!virtual_bag::valid_original_transaction_bag(original_bag) ||
            !bag_size_word_locked(original_bag, &projected_size_word)) {
            VIRTBAG_LOG("store buy gate cannot read projected capacity window=%d", original_bag);
            return StoreSaveSlotLookup::kError;
        }
        if (!set_bag_size_word_locked(original_bag, store_original_size_word)) {
            VIRTBAG_LOG("store buy gate cannot restore original capacity window=%d", original_bag);
            return StoreSaveSlotLookup::kError;
        }
    }

    lock.unlock();
    int32_t slot = -1;
    const QolResult result = g_inventory_extras->read.find_save_slot(item, &slot);
    lock.lock();

    if (projected) {
        if (store_view_generation != view_generation || !store_view_installed ||
            store_window_original_bag != original_bag) {
            VIRTBAG_LOG("store buy gate view changed during original lookup generation=%llu->%llu",
                        static_cast<unsigned long long>(view_generation),
                        static_cast<unsigned long long>(store_view_generation));
            return StoreSaveSlotLookup::kError;
        }
        if (!set_bag_size_word_locked(original_bag, projected_size_word)) {
            VIRTBAG_LOG("store buy gate failed to restore projected capacity window=%d", original_bag);
            return StoreSaveSlotLookup::kError;
        }
    }

    if (qol::contracts::succeeded(result)) {
        if (slot < 0 || slot > UINT8_MAX) {
            VIRTBAG_LOG("store buy gate received invalid original slot=%d", slot);
            return StoreSaveSlotLookup::kError;
        }
        *out_slot = slot;
        return StoreSaveSlotLookup::kFound;
    }
    if (result.code == kQolErrNotFound) return StoreSaveSlotLookup::kNotFound;
    VIRTBAG_LOG("store buy gate original lookup failed code=%d", result.code);
    return StoreSaveSlotLookup::kError;
}

bool store_extension_can_accept_item_locked(void* item) {
    if (item == nullptr || !extension_bag_session_admitted_locked()) return false;
    ensure_state_loaded_locked();

    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    int category = 0;
    if (!serialize_item_payload_locked(item, &payload, &payload_size) ||
        !read_item_category(item, &category)) {
        VIRTBAG_LOG("store buy gate extension preflight rejected item=%p reason=source-read", item);
        return false;
    }
    int canonical_count = static_cast<int>(virtual_bag_canonical_count(item, 1));
    if (canonical_count <= 0) canonical_count = 1;
    virtual_bag::Item source{};
    source.category = category;
    source.count = canonical_count;
    source.payload = payload;
    source.payload_size = payload_size;

    const virtual_bag::PlacePlan plan = virtual_bag::place_plan(
        g_virtual_bag_state.items, g_virtual_bag_state.types,
        g_virtual_bag_state.capacities, source, /*allow_new_slot=*/true,
        virtual_bag_extended_mode(item), virtual_bag_category_uses_stack_count,
        &module_slot_is_assignable_locked);
    return plan.outcome == virtual_bag::PlaceOutcome::kMerged ||
           plan.outcome == virtual_bag::PlaceOutcome::kPlacedNewSlot;
}

// 买入预检 gate：替代 UIStore_BuyItem 内两处 bl INVEN_FindSaveSlot。
// 原版查询期间临时恢复真实袋容量，并以查询成功状态而非槽编码值判断找到；
// 原版无槽时，扩展袋预检复用 SaveItem adopt 的合并/新建落位判据，允许两种
// CopyAsNewUID/普通货物分支进入最终 SaveItem。原版 UI 查询始终在锁外执行。
}  // namespace

uint8_t store_buy_find_slot_gate(void* item, void* out_slot) {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"patch:P_EXT_STORE_BUY_GATE_1"});
    int32_t slot = -1;
    const StoreSaveSlotLookup lookup = store_find_original_slot_with_real_capacity(item, &slot);
    if (lookup == StoreSaveSlotLookup::kFound) {
        if (out_slot != nullptr) {
            const uint8_t encoded_slot = static_cast<uint8_t>(slot);
            std::memcpy(out_slot, &encoded_slot, sizeof(encoded_slot));
        }
        return 1;
    }
    if (lookup == StoreSaveSlotLookup::kError) return 0;
    if (!g_virtual_bag_enabled.load()) return 0;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    return store_extension_can_accept_item_locked(item) ? 1 : 0;
}


// R-57 原版袋列高亮遮蔽（商店）：进入扩展视图时，画原版 UIStore_DrawInvenBagGroup
// 前把当前袋 GOT 临时置 kNoOriginalBagSelected(6)，画完恢复 → 原版 6 袋不再高亮
// （与扩展页 virtual_bag_draw_original_bag_wrapper 同法）。扩展页签高亮由 draw_end
// 的 store_draw_tab_buttons_locked 在袋列绘制之后补画。
void store_draw_inven_bag_group_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"patch:P_EXT_STORE_BAG_DRAW"});
    if (g_game == nullptr ||
        g_write_port->ui.draw_original_bag == nullptr ||
        g_write_port->ui.current_bag == nullptr ||
        g_write_port->ui.set_current_bag == nullptr) return;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    int32_t saved_current = 0;
    bool masked = false;
    if (store_view_installed &&
        qol::contracts::succeeded(g_write_port->ui.current_bag(&saved_current)) &&
        saved_current != kNoOriginalBagSelected) {
        (void)g_write_port->ui.set_current_bag(kNoOriginalBagSelected);
        masked = true;
    }
    (void)g_write_port->ui.draw_original_bag();
    if (masked) (void)g_write_port->ui.set_current_bag(saved_current);
}
