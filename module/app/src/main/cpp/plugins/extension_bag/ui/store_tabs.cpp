#include "game_ui_virtbag.h"
#include "plugins/extension_bag/runtime/state_access.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"
#include "plugins/extension_bag/runtime/item_access.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/core/ownership.h"
#include "plugins/extension_bag/core/host_adapter.h"
#include "plugins/extension_bag/ui/host_ui.h"
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
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/model/move_merge.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/model/sale_transaction.h"
#include "plugins/extension_bag/model/draw_state_lock.h"
#include "plugins/extension_bag/model/main_menu_cleanup_edge.h"
#include "plugins/extension_bag/plugin.h"
#include "plugins/extension_bag/model/store_sell_rules.h"

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
uint64_t store_tab_item_proc(void*, uint64_t, void*, void*);
int store_tab_index(void* ctrl) {
    const int active = virtual_bag::active_extension_bags();
    const int visible = active < virtual_bag::kMaxRenderedExtensionBagTabs
                            ? active : virtual_bag::kMaxRenderedExtensionBagTabs;
    for (int index = 0; index < visible; ++index) {
        if (::qol::extension_bag::access::store().tab_buttons[index] == ctrl) return index;
    }
    return -1;
}

void store_refresh_tab_bag_items_locked() {
    for (int index = 0; index < virtual_bag::kMaxExtensionBags; ++index) {
        const uint8_t type = ::qol::extension_bag::access::bag().types[index];
        void*& item = ::qol::extension_bag::access::store().tab_bag_items[index];
        const int visible = virtual_bag::active_extension_bags() < virtual_bag::kMaxRenderedExtensionBagTabs
                                ? virtual_bag::active_extension_bags()
                                : virtual_bag::kMaxRenderedExtensionBagTabs;
        if (type == 0 || index >= visible) {
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
bool store_all_tab_buttons_installed_locked() {
    const int active = virtual_bag::active_extension_bags();
    const int visible = active < virtual_bag::kMaxRenderedExtensionBagTabs
                            ? active : virtual_bag::kMaxRenderedExtensionBagTabs;
    for (int index = 0; index < visible; ++index) {
        if (::qol::extension_bag::access::store().tab_buttons[static_cast<size_t>(index)] == nullptr) return false;
    }
    return true;
}

bool store_has_tab_buttons_locked() {
    for (void* button : ::qol::extension_bag::access::store().tab_buttons) {
        if (button != nullptr) return true;
    }
    return false;
}

void store_discard_new_tab_controls_locked(
    std::array<void*, virtual_bag::kMaxExtensionBags>& controls, int failed_index,
    void* failed_control, const char* stage) {
    VIRTBAG_LOG("store tab install failed index=%d stage=%s", failed_index, stage);
    if (failed_control != nullptr && !extension_bag_ui_delete_control(failed_control)) {
        VIRTBAG_LOG("store tab cleanup failed index=%d control=%p", failed_index,
                    failed_control);
    }
    for (int index = 0; index < virtual_bag::kMaxExtensionBags; ++index) {
        void*& control = controls[index];
        if (control == nullptr) continue;
        if (!extension_bag_ui_delete_control(control)) {
            VIRTBAG_LOG("store tab cleanup failed index=%d control=%p", index, control);
        }
        control = nullptr;
    }
}
uint64_t store_tab_item_proc(void* ctrl, uint64_t event, void*, void*) {
    if (event == 0x02) {
        ::qol::extension_bag::ReentryLock lock;
        if (!extension_bag_session_admitted_locked()) return 1;
        const int index = store_tab_index(ctrl);
        if (index >= 0 && ::qol::extension_bag::access::store().tab_generation != 0) {
            ::qol::extension_bag::access::store().pending_tab = index;
            ::qol::extension_bag::access::store().pending_tab_generation = ::qol::extension_bag::access::store().tab_generation;
        }
        return 1;
    }
    // 商店宿主不接管拖拽，避免扩展标签成为拖动源或跨袋目标。
    return 0;
}
}  // namespace

void store_disable_tab_buttons_locked() {
    ++::qol::extension_bag::access::store().tab_generation;
    ::qol::extension_bag::access::store().pending_tab = -1;
    ::qol::extension_bag::access::store().pending_tab_generation = 0;
    ::qol::extension_bag::access::store().tab_buttons.fill(nullptr);
    ::qol::extension_bag::access::store().tab_anchors = {};
    ::qol::extension_bag::access::store().tab_root = nullptr;
}

void store_delete_tab_buttons_locked() {
    // 句柄存活由锚定层证明：只删除仍能证明属于本模块的控件；失效句柄只清记录。
    const auto* components = extension_bag_ui_components();
    for (int index = 0; index < virtual_bag::kMaxExtensionBags; ++index) {
        auto& state = ::qol::extension_bag::access::store().tab_anchors[static_cast<size_t>(index)];
        void* handle = nullptr;
        uint8_t live = 0;
        if (components != nullptr && components->anchored_control_detach != nullptr) {
            (void)components->anchored_control_detach(&state, &handle, &live);
        } else {
            state = {};
        }
        if (live != 0 && handle != nullptr && !extension_bag_ui_delete_control(handle)) {
            VIRTBAG_LOG("store tab delete failed control=%p", handle);
        }
        ::qol::extension_bag::access::store().tab_buttons[static_cast<size_t>(index)] = nullptr;
    }
}
// ---- 商店页签锚定（ui_components minor 21）----
// 页签挂在 UIStore 袋容器上，游戏销毁容器子树后句柄失效；锚定层负责存活证明与重建。
struct StoreTabAnchorContext {
    int index = 0;
};
std::array<StoreTabAnchorContext, virtual_bag::kMaxExtensionBags> g_store_tab_anchor_ctx{};

QolResult store_tab_anchor_layout(void* parent, qol::contracts::UiRect* out_rect, void* user) {
    if (parent == nullptr || out_rect == nullptr || user == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "store tab layout", 16);
    }
    const auto* ctx = static_cast<const StoreTabAnchorContext*>(user);
    if (!virtual_bag::valid_index(ctx->index)) {
        return qol::contracts::error(kQolErrInvalidArgument, "store tab index", 15);
    }
    int child_count = 0;
    if (!extension_bag_ui_child_count(parent, &child_count) || child_count < 6) {
        return qol::contracts::error(kQolErrNotReady, "store bag not ready", 20);
    }
    void* bag0 = valid_child_locked(parent, 0);
    qol::contracts::UiRect bag0_rect{};
    if (bag0 == nullptr || !extension_bag_ui_rect(bag0, &bag0_rect) || bag0_rect.w <= 0) {
        return qol::contracts::error(kQolErrNotReady, "store bag0 not ready", 21);
    }
    int64_t bag0_abs_x = 0, bag0_abs_y = 0, mount_abs_x = 0, mount_abs_y = 0;
    extension_bag_control_abs_pos(bag0, &bag0_abs_x, &bag0_abs_y);
    extension_bag_control_abs_pos(parent, &mount_abs_x, &mount_abs_y);
    *out_rect = {bag0_abs_x + 68 - mount_abs_x,
        bag0_abs_y + 2 - mount_abs_y + ctx->index * 70, 57, 57};
    return qol::contracts::ok();
}

QolResult store_tab_anchor_create(void* parent, qol::contracts::UiRect rect, void* user,
                                  void** out_control) {
    if (parent == nullptr || user == nullptr || out_control == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "store tab create", 16);
    }
    *out_control = nullptr;
    const auto* ctx = static_cast<const StoreTabAnchorContext*>(user);
    if (!virtual_bag::valid_index(ctx->index)) {
        return qol::contracts::error(kQolErrInvalidArgument, "store tab index", 15);
    }
    void* control = nullptr;
    if (!extension_bag_ui_create_control(parent, &control) || control == nullptr) {
        VIRTBAG_LOG("store tab create failed index=%d", ctx->index);
        return qol::contracts::error(kQolErrNotReady, "store tab create", 16);
    }
    const auto fail = [control](const char* stage, int index) {
        (void)extension_bag_ui_delete_control(control);
        VIRTBAG_LOG("store tab install failed index=%d stage=%s", index, stage);
        return qol::contracts::error(kQolErrNotReady, "store tab setup", 15);
    };
    if (!extension_bag_ui_set_control_proc(control, &store_tab_item_proc)) {
        return fail("proc", ctx->index);
    }
    if (!extension_bag_ui_set_control_user_type(control, 2)) return fail("user-type", ctx->index);
    if (!extension_bag_ui_unuse_control_event_move(control)) return fail("event-move", ctx->index);
    if (!extension_bag_ui_set_rect(control, rect)) return fail("rect", ctx->index);
    void* data = nullptr;
    if (!extension_bag_ui_allocate_control_data(0x10, &data) || data == nullptr) {
        return fail("data-allocate", ctx->index);
    }
    std::memset(data, 0, 0x10);
    *reinterpret_cast<void**>(data) = ::qol::extension_bag::access::store().tab_bag_items[ctx->index];
    if (!extension_bag_ui_set_control_data(control, data)) return fail("data-set", ctx->index);
    *out_control = control;
    return qol::contracts::ok();
}

bool store_refresh_tab_anchors_locked(const qol::contracts::QolUiComponents* components,
                                      void* parent, bool* out_created_any) {
    if (out_created_any != nullptr) *out_created_any = false;
    if (components == nullptr || components->anchored_control_ensure == nullptr) return false;
    bool all = true;
    const int active = virtual_bag::active_extension_bags();
    const int visible = active < virtual_bag::kMaxRenderedExtensionBagTabs
                            ? active : virtual_bag::kMaxRenderedExtensionBagTabs;
    for (int index = 0; index < visible; ++index) {
        auto& ctx = g_store_tab_anchor_ctx[static_cast<size_t>(index)];
        ctx.index = index;
        const qol::contracts::UiAnchoredControlDesc desc{
            static_cast<uint32_t>(qol::contracts::QolUiRootKind::kStoreBagContainer),
            &store_tab_item_proc, &store_tab_anchor_layout, &store_tab_anchor_create, &ctx};
        uint8_t created = 0;
        auto& state = ::qol::extension_bag::access::store().tab_anchors[static_cast<size_t>(index)];
        if (components->anchored_control_ensure(&state, &desc, &created).ok != kQolOk) all = false;
        ::qol::extension_bag::access::store().tab_buttons[static_cast<size_t>(index)] =
            state.mounted != 0 ? state.handle : nullptr;
        if (created != 0 && out_created_any != nullptr) *out_created_any = true;
    }
    if (out_created_any != nullptr && *out_created_any) {
        VIRTBAG_LOG("store tabs re-anchored root=%p", parent);
    }
    return all;
}

void store_install_tab_buttons_locked() {
    // 配置门控：扩展背包关闭时商店页签不得挂载（3 个宿主之一）。
    if (!::qol::extension_bag::access::enabled().load() || !extension_bag_session_admitted_locked()) return;
    if (::qol::extension_bag::access::host().sdk == nullptr) return;
    void* bag_container = extension_bag_ui_root(qol::contracts::QolUiRootKind::kStoreBagContainer);
    int bag_child_count = 0;
    if (bag_container == nullptr || !extension_bag_ui_child_count(bag_container, &bag_child_count) ||
        bag_child_count < 6) return;
    const auto* components = extension_bag_ui_components();
    if (components == nullptr) return;
    store_refresh_tab_bag_items_locked();
    const bool changed = !store_all_tab_buttons_installed_locked() ||
        ::qol::extension_bag::access::store().tab_root != bag_container;
    bool created_any = false;
    const bool complete = store_refresh_tab_anchors_locked(components, bag_container, &created_any);
    if (changed || created_any) {
        ++::qol::extension_bag::access::store().tab_generation;
        ::qol::extension_bag::access::store().tab_root = bag_container;
        VIRTBAG_LOG("store tabs installed root=%p generation=%llu complete=%d created=%d",
                    bag_container,
                    static_cast<unsigned long long>(::qol::extension_bag::access::store().tab_generation),
                    complete ? 1 : 0, created_any ? 1 : 0);
    }
}
void store_commit_pending_tab_locked() {
    if (!extension_bag_session_admitted_locked()) {
        ::qol::extension_bag::access::store().pending_tab = -1;
        ::qol::extension_bag::access::store().pending_tab_generation = 0;
        return;
    }
    if (!virtual_bag::valid_index(::qol::extension_bag::access::store().pending_tab) ||
        ::qol::extension_bag::access::store().pending_tab_generation != ::qol::extension_bag::access::store().tab_generation) {
        ::qol::extension_bag::access::store().pending_tab = -1;
        ::qol::extension_bag::access::store().pending_tab_generation = 0;
        return;
    }
    const int bag = ::qol::extension_bag::access::store().pending_tab;
    ::qol::extension_bag::access::store().pending_tab = -1;
    ::qol::extension_bag::access::store().pending_tab_generation = 0;
    if (::qol::extension_bag::access::store().view_installed && ::qol::extension_bag::access::store().view_bag == bag) return;
    store_install_module_view_locked(bag);
}
void store_draw_tab_buttons_locked() {
    if (!extension_bag_ui_can_draw()) return;
    // 每帧 ensure：游戏切页/关闭面板会销毁页签子树，锚定层负责证明并重建。
    store_install_tab_buttons_locked();
    const int active = virtual_bag::active_extension_bags();
    const int visible = active < virtual_bag::kMaxRenderedExtensionBagTabs
                            ? active : virtual_bag::kMaxRenderedExtensionBagTabs;
    for (int index = visible - 1; index >= 0; --index) {
        void* button = ::qol::extension_bag::access::store().tab_buttons[index];
        if (button == nullptr) continue;
        int64_t x = 0;
        int64_t y = 0;
        extension_bag_control_abs_pos(button, &x, &y);
        (void)extension_bag_ui_draw_part_alpha(0xf, 0x14, static_cast<int32_t>(x - 5),
                                               static_cast<int32_t>(y - 12), 0, 1, 0);
    }
    for (int index = 0; index < visible; ++index) {
        void* button = ::qol::extension_bag::access::store().tab_buttons[index];
        if (button == nullptr) continue;
        int64_t x = 0;
        int64_t y = 0;
        extension_bag_control_abs_pos(button, &x, &y);
        const bool selected = ::qol::extension_bag::access::store().view_installed && ::qol::extension_bag::access::store().view_bag == index;
        const bool equipped = ::qol::extension_bag::access::bag().capacities[index] != 0;
        if (selected) {
            (void)extension_bag_ui_draw_part_alpha(0xf, 0x13, static_cast<int32_t>(x - 5),
                                                   static_cast<int32_t>(y - 12), 0, 1, 0);
            (void)extension_bag_ui_draw_part_alpha(0xf, 6, static_cast<int32_t>(x + 5),
                                                   static_cast<int32_t>(y + 5), 0, 1, 0);
        } else if (equipped) {
            (void)extension_bag_ui_draw_part_alpha(0xf, 6, static_cast<int32_t>(x + 5),
                                                   static_cast<int32_t>(y + 5), 0, 1, 0x28);
        } else {
            (void)extension_bag_ui_draw_part_alpha(0xf, 9, static_cast<int32_t>(x + 5),
                                                   static_cast<int32_t>(y + 5), 0, 1, 0x28);
        }
    }
}
// 商店袋列入口 hook：与背包页 virtual_bag_draw_original_bag_wrapper 同法，在
// UIStore_DrawInvenBagGroup 真正读取当前袋之前写成 6，画完恢复 → 原版袋不再高亮。
void store_draw_inven_bag_group_entry() {
    using DrawFn = void (*)();
    const auto backup = reinterpret_cast<DrawFn>(
        ::qol::extension_bag::access::ui().backup_store_draw_inven_bag_group);
    if (backup == nullptr) return;
    const ::qol::contracts::InFlightGuard in_flight_guard(
        ::qol::extension_bag::access::host().sdk == nullptr ? nullptr : &::qol::extension_bag::access::host().sdk->in_flight,
        {"hook:UIStore_DrawInvenBagGroup"});
    ::qol::extension_bag::ReentryLock lock;
    int32_t saved_current = 0;
    bool masked = false;
    if (::qol::extension_bag::access::store().view_installed &&
        ::qol::extension_bag::access::host().write_port->ui.current_bag != nullptr &&
        ::qol::extension_bag::access::host().write_port->ui.set_current_bag != nullptr &&
        qol::contracts::succeeded(::qol::extension_bag::access::host().write_port->ui.current_bag(&saved_current)) &&
        saved_current != kNoOriginalBagSelected) {
        (void)::qol::extension_bag::access::host().write_port->ui.set_current_bag(kNoOriginalBagSelected);
        masked = true;
    }
    backup();
    if (masked) (void)::qol::extension_bag::access::host().write_port->ui.set_current_bag(saved_current);
}
// R-57 原版袋列高亮遮蔽（商店）：进入扩展视图时，画原版 UIStore_DrawInvenBagGroup
// 前把当前袋 GOT 临时置 kNoOriginalBagSelected(6)，画完恢复 → 原版 6 袋不再高亮
// （与扩展页 virtual_bag_draw_original_bag_wrapper 同法）。扩展页签高亮由 draw_end
// 的 store_draw_tab_buttons_locked 在袋列绘制之后补画。
void store_draw_inven_bag_group_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        ::qol::extension_bag::access::host().sdk == nullptr ? nullptr : &::qol::extension_bag::access::host().sdk->in_flight,
        {"patch:P_EXT_STORE_BAG_DRAW"});
    if (::qol::extension_bag::access::host().game == nullptr ||
        ::qol::extension_bag::access::host().write_port->ui.draw_original_store_bag_group == nullptr) return;
    // The entry hook owns masking. The runtime has no business mutex: the
    // entry hook runs while this wrapper calls the function and uses the same
    // non-recursive mutex.
    (void)::qol::extension_bag::access::host().write_port->ui.draw_original_store_bag_group();
}
