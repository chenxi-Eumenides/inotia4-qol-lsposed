#include "plugins/extension_bag/core/ui_state.h"
#include "game_ui_virtbag.h"
#include "plugins/extension_bag/runtime/projection_controller.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/runtime/state_access.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"
#include "plugins/extension_bag/runtime/inventory_access.h"
#include "plugins/extension_bag/runtime/item_access.h"
#include "plugins/extension_bag/runtime/session.h"
#include "plugins/extension_bag/core/host_adapter.h"
#include "plugins/extension_bag/core/lifecycle.h"
#include "plugins/extension_bag/transaction/ownership.h"
#include "plugins/extension_bag/core/projection.h"
#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "contracts/ownership.h"
#include "contracts/inventory_refresh.h"
#include "contracts/item_facts.h"
#include "contracts/module_save_port.h"
#include "contracts/popup_enter_alias.h"
#include "plugins/extension_bag/model/equip_rules.h"
#include "plugins/extension_bag/core/geometry.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/model/move_merge.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/model/sale_transaction.h"
#include "plugins/extension_bag/model/draw_state_lock.h"
#include "plugins/extension_bag/model/main_menu_cleanup_edge.h"
#include "plugins/extension_bag/plugin.h"

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

// 合成器（UIMix）宿主：只管理 UIMix 自己的页签、投影和刷新，不复用 UIEquip/商店视图
// 状态。结构参考 ui_store.cpp：
//   - PopupState ENTER/F3 回调包装（enter 装页签、f3 teardown）；
//   - Scene_Draw_POPUP_SC_MIX 内 bl GRPX_End 调用点 BL patch 挂 draw_end wrapper；
//   - 自建 5 个扩展袋页签挂到 UIMix 袋组 [g_uimix+UIMIX_SLOT_BAG_GROUP]（原版 6 袋按钮同父）；
//   - 投影安装/恢复写 UIMix 物品网格组 [g_uimix+UIMIX_SLOT_ITEM_GROUP] 的 16 个子控件。
// 宿主物理袋沿用 original_bag_locked()（UIMix 与 UIEquip 共用 [0x2f56d8] 当前袋 GOT，
// UIMix_RefreshInvenItem 反汇编确认），合法 0..4；落在任务袋 5/非法时回退 0。

namespace {

uint64_t mix_tab_item_proc(void* ctrl, uint64_t event, void*, void*);
void mix_install_module_view_locked(int bag);
void mix_restore_module_view_locked();

void* mix_bag_container_locked() {
    return extension_bag_ui_root(qol::contracts::QolUiRootKind::kMixBagContainer);
}

void* mix_item_root_locked() {
    return extension_bag_ui_root(qol::contracts::QolUiRootKind::kMixItemRoot);
}

// 面板状态：0=配方菜单（原版不画背包列），1=背包/合成视图。扩展页签只在 state!=0
// 时挂载/绘制，避免在配方菜单阶段早于原版背包出现（UIMix_Draw 状态门控反汇编确认）。
int mix_state_locked() {
    if (::qol::extension_bag::access::host().craft_facts == nullptr || ::qol::extension_bag::access::host().craft_facts->read_only.panel_state == nullptr) return 0;
    int32_t state = 0;
    return qol::contracts::succeeded(::qol::extension_bag::access::host().craft_facts->read_only.panel_state(&state)) ? state : 0;
}

int mix_tab_index(void* ctrl) {
    const int active = virtual_bag::active_extension_bags();
    const int visible = active < virtual_bag::kMaxRenderedExtensionBagTabs
                            ? active : virtual_bag::kMaxRenderedExtensionBagTabs;
    for (int index = 0; index < visible; ++index) {
        if (qol::extension_bag::ui_state().mix.tab_buttons[index] == ctrl) return index;
    }
    return -1;
}

void mix_refresh_tab_bag_items_locked() {
    for (int index = 0; index < virtual_bag::kMaxExtensionBags; ++index) {
        const uint8_t type = ::qol::extension_bag::access::bag().types[index];
        void*& item = qol::extension_bag::ui_state().mix.tab_bag_items[index];
        const int active = virtual_bag::active_extension_bags();
        const int visible = active < virtual_bag::kMaxRenderedExtensionBagTabs
                                ? active : virtual_bag::kMaxRenderedExtensionBagTabs;
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

void mix_disable_tab_buttons_locked() {
    ++qol::extension_bag::ui_state().mix.tab_generation;
    qol::extension_bag::ui_state().mix.pending_tab = -1;
    qol::extension_bag::ui_state().mix.pending_tab_generation = 0;
    qol::extension_bag::ui_state().mix.tab_buttons.fill(nullptr);
    qol::extension_bag::ui_state().mix.tab_anchors = {};
    qol::extension_bag::ui_state().mix.tab_root = nullptr;
}

void mix_delete_tab_buttons_locked() {
    // 句柄存活由锚定层证明：只删除仍能证明属于本模块的控件；失效句柄只清记录。
    const auto* components = extension_bag_ui_components();
    for (int index = 0; index < virtual_bag::kMaxExtensionBags; ++index) {
        auto& state = qol::extension_bag::ui_state().mix.tab_anchors[static_cast<size_t>(index)];
        void* handle = nullptr;
        uint8_t live = 0;
        if (components != nullptr && components->anchored_control_detach != nullptr) {
            (void)components->anchored_control_detach(&state, &handle, &live);
        } else {
            state = {};
        }
        if (live != 0 && handle != nullptr && !extension_bag_ui_delete_control(handle)) {
            VIRTBAG_LOG("mix tab delete failed control=%p", handle);
        }
        qol::extension_bag::ui_state().mix.tab_buttons[static_cast<size_t>(index)] = nullptr;
    }
}

bool mix_all_tab_buttons_installed_locked() {
    const int active = virtual_bag::active_extension_bags();
    const int visible = active < virtual_bag::kMaxRenderedExtensionBagTabs
                            ? active : virtual_bag::kMaxRenderedExtensionBagTabs;
    for (int index = 0; index < visible; ++index) {
        if (qol::extension_bag::ui_state().mix.tab_buttons[static_cast<size_t>(index)] == nullptr) return false;
    }
    return true;
}

bool mix_has_tab_buttons_locked() {
    for (void* button : qol::extension_bag::ui_state().mix.tab_buttons) {
        if (button != nullptr) return true;
    }
    return false;
}

void mix_discard_new_tab_controls_locked(
    std::array<void*, virtual_bag::kMaxExtensionBags>& controls, int failed_index,
    void* failed_control, const char* stage) {
    VIRTBAG_LOG("mix tab install failed index=%d stage=%s", failed_index, stage);
    if (failed_control != nullptr && !extension_bag_ui_delete_control(failed_control)) {
        VIRTBAG_LOG("mix tab cleanup failed index=%d control=%p", failed_index,
                    failed_control);
    }
    for (int index = 0; index < virtual_bag::kMaxExtensionBags; ++index) {
        void*& control = controls[index];
        if (control == nullptr) continue;
        if (!extension_bag_ui_delete_control(control)) {
            VIRTBAG_LOG("mix tab cleanup failed index=%d control=%p", index, control);
        }
        control = nullptr;
    }
}

// ---- 合成器页签锚定（ui_components minor 21）----
// 页签挂在 UIMix 袋容器上，游戏销毁容器子树后句柄失效；锚定层负责存活证明与重建。
struct MixTabAnchorContext {
    int index = 0;
};
std::array<MixTabAnchorContext, virtual_bag::kMaxExtensionBags> g_mix_tab_anchor_ctx{};

QolResult mix_tab_anchor_layout(void* parent, qol::contracts::UiRect* out_rect, void* user) {
    if (parent == nullptr || out_rect == nullptr || user == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "mix tab layout", 14);
    }
    const auto* ctx = static_cast<const MixTabAnchorContext*>(user);
    if (!virtual_bag::valid_index(ctx->index)) {
        return qol::contracts::error(kQolErrInvalidArgument, "mix tab index", 13);
    }
    int child_count = 0;
    if (!extension_bag_ui_child_count(parent, &child_count) ||
        child_count < virtual_bag::kOriginalNativeBagCount) {
        return qol::contracts::error(kQolErrNotReady, "mix bag not ready", 18);
    }
    void* bag0 = valid_child_locked(parent, 0);
    qol::contracts::UiRect bag0_rect{};
    if (bag0 == nullptr || !extension_bag_ui_rect(bag0, &bag0_rect) || bag0_rect.w <= 0) {
        return qol::contracts::error(kQolErrNotReady, "mix bag0 not ready", 19);
    }
    int64_t bag0_abs_x = 0, bag0_abs_y = 0, mount_abs_x = 0, mount_abs_y = 0;
    extension_bag_control_abs_pos(bag0, &bag0_abs_x, &bag0_abs_y);
    extension_bag_control_abs_pos(parent, &mount_abs_x, &mount_abs_y);
    *out_rect = {bag0_abs_x + 68 - mount_abs_x,
        bag0_abs_y + 2 - mount_abs_y + ctx->index * 70, 57, 57};
    return qol::contracts::ok();
}

QolResult mix_tab_anchor_create(void* parent, qol::contracts::UiRect rect, void* user,
                                void** out_control) {
    if (parent == nullptr || user == nullptr || out_control == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "mix tab create", 14);
    }
    *out_control = nullptr;
    const auto* ctx = static_cast<const MixTabAnchorContext*>(user);
    if (!virtual_bag::valid_index(ctx->index)) {
        return qol::contracts::error(kQolErrInvalidArgument, "mix tab index", 13);
    }
    void* control = nullptr;
    if (!extension_bag_ui_create_control(parent, &control) || control == nullptr) {
        VIRTBAG_LOG("mix tab create failed index=%d", ctx->index);
        return qol::contracts::error(kQolErrNotReady, "mix tab create", 14);
    }
    const auto fail = [control](const char* stage, int index) {
        (void)extension_bag_ui_delete_control(control);
        VIRTBAG_LOG("mix tab install failed index=%d stage=%s", index, stage);
        return qol::contracts::error(kQolErrNotReady, "mix tab setup", 13);
    };
    if (!extension_bag_ui_set_control_proc(control, &mix_tab_item_proc)) {
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
    *reinterpret_cast<void**>(data) = qol::extension_bag::ui_state().mix.tab_bag_items[ctx->index];
    if (!extension_bag_ui_set_control_data(control, data)) return fail("data-set", ctx->index);
    *out_control = control;
    return qol::contracts::ok();
}

bool mix_refresh_tab_anchors_locked(const qol::contracts::QolUiComponents* components,
                                    void* parent, bool* out_created_any) {
    if (out_created_any != nullptr) *out_created_any = false;
    if (components == nullptr || components->anchored_control_ensure == nullptr) return false;
    bool all = true;
    const int active = virtual_bag::active_extension_bags();
    const int visible = active < virtual_bag::kMaxRenderedExtensionBagTabs
                            ? active : virtual_bag::kMaxRenderedExtensionBagTabs;
    for (int index = 0; index < visible; ++index) {
        auto& ctx = g_mix_tab_anchor_ctx[static_cast<size_t>(index)];
        ctx.index = index;
        const qol::contracts::UiAnchoredControlDesc desc{
            static_cast<uint32_t>(qol::contracts::QolUiRootKind::kMixBagContainer),
            &mix_tab_item_proc, &mix_tab_anchor_layout, &mix_tab_anchor_create, &ctx};
        uint8_t created = 0;
        auto& state = qol::extension_bag::ui_state().mix.tab_anchors[static_cast<size_t>(index)];
        if (components->anchored_control_ensure(&state, &desc, &created).ok != kQolOk) all = false;
        qol::extension_bag::ui_state().mix.tab_buttons[static_cast<size_t>(index)] =
            state.mounted != 0 ? state.handle : nullptr;
        if (created != 0 && out_created_any != nullptr) *out_created_any = true;
    }
    if (out_created_any != nullptr && *out_created_any) {
        VIRTBAG_LOG("mix tabs re-anchored root=%p", parent);
    }
    return all;
}

void mix_install_tab_buttons_locked() {
    // 配置门控：扩展背包关闭时合成器页签不得挂载（3 个宿主之一）。
    if (!::qol::extension_bag::access::enabled().load() || !extension_bag_session_admitted_locked()) return;
    if (::qol::extension_bag::access::host().sdk == nullptr) return;
    void* bag_container = mix_bag_container_locked();
    int bag_child_count = 0;
    if (bag_container == nullptr || !extension_bag_ui_child_count(bag_container, &bag_child_count) ||
        bag_child_count < virtual_bag::kOriginalNativeBagCount) return;
    const auto* components = extension_bag_ui_components();
    if (components == nullptr) return;
    mix_refresh_tab_bag_items_locked();
    const bool changed = !mix_all_tab_buttons_installed_locked() || qol::extension_bag::ui_state().mix.tab_root != bag_container;
    bool created_any = false;
    const bool complete = mix_refresh_tab_anchors_locked(components, bag_container, &created_any);
    if (changed || created_any) {
        ++qol::extension_bag::ui_state().mix.tab_generation;
        qol::extension_bag::ui_state().mix.tab_root = bag_container;
        VIRTBAG_LOG("mix tabs installed root=%p generation=%llu complete=%d created=%d",
                    bag_container, static_cast<unsigned long long>(qol::extension_bag::ui_state().mix.tab_generation),
                    complete ? 1 : 0, created_any ? 1 : 0);
    }
}

uint64_t mix_tab_item_proc(void* ctrl, uint64_t event, void*, void*) {
    if (event == 0x02) {
        ::qol::extension_bag::ReentryLock lock;
        if (!extension_bag_session_admitted_locked()) return 1;
        const int index = mix_tab_index(ctrl);
        if (index >= 0 && qol::extension_bag::ui_state().mix.tab_generation != 0) {
            qol::extension_bag::ui_state().mix.pending_tab = index;
            qol::extension_bag::ui_state().mix.pending_tab_generation = qol::extension_bag::ui_state().mix.tab_generation;
        }
        return 1;
    }
    // 合成器宿主不接管拖拽，避免扩展标签成为拖动源或跨袋目标。
    return 0;
}

void mix_commit_pending_tab_locked() {
    if (!extension_bag_session_admitted_locked()) {
        qol::extension_bag::ui_state().mix.pending_tab = -1;
        qol::extension_bag::ui_state().mix.pending_tab_generation = 0;
        return;
    }
    if (!virtual_bag::valid_index(qol::extension_bag::ui_state().mix.pending_tab) ||
        qol::extension_bag::ui_state().mix.pending_tab_generation != qol::extension_bag::ui_state().mix.tab_generation) {
        qol::extension_bag::ui_state().mix.pending_tab = -1;
        qol::extension_bag::ui_state().mix.pending_tab_generation = 0;
        return;
    }
    const int bag = qol::extension_bag::ui_state().mix.pending_tab;
    qol::extension_bag::ui_state().mix.pending_tab = -1;
    qol::extension_bag::ui_state().mix.pending_tab_generation = 0;
    if (qol::extension_bag::ui_state().mix.view_installed && qol::extension_bag::ui_state().mix.view_bag == bag) return;
    mix_install_module_view_locked(bag);
}

void mix_refresh_projection_locked() {
    if (!qol::extension_bag::ui_state().mix.view_installed || qol::extension_bag::ui_state().mix.projected_item_root == nullptr ||
        !virtual_bag::valid_index(qol::extension_bag::ui_state().mix.view_bag)) return;
    const int capacity = ::qol::extension_bag::access::bag().capacities[qol::extension_bag::ui_state().mix.view_bag];
    for (int slot = 0; slot < capacity; ++slot) {
        void* control = valid_child_locked(qol::extension_bag::ui_state().mix.projected_item_root, slot);
        if (control == nullptr) continue;
        void* expected = module_item_locked(qol::extension_bag::ui_state().mix.view_bag, slot);
        void* data = extension_bag_ui_data(control);
        void* current = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
        if (current != expected) (void)extension_bag_ui_set_control_item(control, expected);
    }
}

void mix_request_item_refresh_locked() {
    qol::extension_bag::ui_state().mix.refresh_pending = true;
    ++qol::extension_bag::ui_state().mix.refresh_generation;
}

void mix_refresh_item_after_unlock() {
    ::qol::extension_bag::ReentryLock lock;
    if (!qol::extension_bag::ui_state().mix.refresh_pending) return;
    const uint64_t refresh_generation = qol::extension_bag::ui_state().mix.refresh_generation;
    const uint64_t view_generation = qol::extension_bag::ui_state().mix.view_generation;
    qol::extension_bag::ui_state().mix.refresh_pending = false;
    lock.unlock();

    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->ui.refresh_mix_item == nullptr) {
        VIRTBAG_LOG("mix item refresh failed reason=missing-adapter");
        lock.lock();
        if (refresh_generation == qol::extension_bag::ui_state().mix.refresh_generation) mix_request_item_refresh_locked();
        return;
    }
    const QolResult refresh_result = api->ui.refresh_mix_item();
    if (!qol::contracts::succeeded(refresh_result)) {
        VIRTBAG_LOG("mix item refresh failed code=%d", refresh_result.code);
        lock.lock();
        if (refresh_generation == qol::extension_bag::ui_state().mix.refresh_generation) mix_request_item_refresh_locked();
        return;
    }

    lock.lock();
    if (refresh_generation != qol::extension_bag::ui_state().mix.refresh_generation ||
        view_generation != qol::extension_bag::ui_state().mix.view_generation) return;
    if (!qol::extension_bag::ui_state().mix.frame_active || !::qol::extension_bag::access::enabled().load() ||
        !extension_bag_session_admitted_locked()) {
        if (qol::extension_bag::ui_state().mix.view_installed) mix_restore_module_view_locked();
        return;
    }
    // Recipe-menu frames (state 0) are transient during a craft. Keep the EB
    // projection/capacity installed there; restoring it for one frame shows the
    // original host bag 0 before state 1 reinstalls the EB view.
    if (mix_state_locked() == 0) return;
    if (!qol::extension_bag::ui_state().mix.view_installed) return;
    void* live_root = mix_item_root_locked();
    if (live_root == nullptr) {
        VIRTBAG_LOG("mix item refresh completed without item root; retry scheduled");
        mix_request_item_refresh_locked();
        return;
    }
    qol::extension_bag::ui_state().mix.projected_item_root = live_root;
    mix_refresh_projection_locked();
}

void mix_install_module_view_locked(int bag) {
    if (!extension_bag_session_admitted_locked() || !virtual_bag::valid_index(bag) ||
        ::qol::extension_bag::access::bag().capacities[bag] == 0 ||
        ::qol::extension_bag::access::host().game == nullptr || ::qol::extension_bag::access::host().write_port->ui.refresh_mix_item == nullptr) return;
    // UIMix 与 UIEquip 共用当前袋 GOT；落在任务袋 5/非法时回退原版袋 0。
    int host = original_bag_locked();
    if (!virtual_bag::valid_original_transaction_bag(host)) host = 0;
    if (qol::extension_bag::ui_state().mix.view_installed) mix_restore_module_view_locked();
    uint32_t size_word = 0;
    if (!bag_size_word_locked(host, &size_word)) {
        VIRTBAG_LOG("mix view rejected host bag=%d", host);
        return;
    }

    qol::extension_bag::ui_state().mix.window_original_bag = host;
    qol::extension_bag::ui_state().mix.original_size_word = size_word;
    qol::extension_bag::ui_state().mix.original_bag_size = size_word;
    constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
    if (!set_bag_size_word_locked(
            host, (size_word & ~kCapacityMask) |
                      (static_cast<uint32_t>(::qol::extension_bag::access::bag().capacities[bag]) & kCapacityMask))) {
        VIRTBAG_LOG("mix view rejected capacity write bag=%d host=%d", bag, host);
        qol::extension_bag::ui_state().mix.original_size_word = 0;
        qol::extension_bag::ui_state().mix.original_bag_size = 0;
        qol::extension_bag::ui_state().mix.window_original_bag = -1;
        return;
    }
    set_original_bag_locked(host);

    qol::extension_bag::ui_state().mix.projected_item_root = mix_item_root_locked();
    if (qol::extension_bag::ui_state().mix.projected_item_root == nullptr) {
        if (!set_bag_size_word_locked(
                host, (qol::extension_bag::ui_state().mix.original_size_word & ~kCapacityMask) |
                          (qol::extension_bag::ui_state().mix.original_bag_size & kCapacityMask))) {
            VIRTBAG_LOG("mix view rollback capacity failed host=%d", host);
        }
        qol::extension_bag::ui_state().mix.original_size_word = 0;
        qol::extension_bag::ui_state().mix.original_bag_size = 0;
        qol::extension_bag::ui_state().mix.window_original_bag = -1;
        qol::extension_bag::ui_state().mix.projected_item_root = nullptr;
        mix_request_item_refresh_locked();
        return;
    }
    const int capacity = ::qol::extension_bag::access::bag().capacities[bag];
    for (int slot = 0; slot < capacity; ++slot) {
        if (module_item_locked(bag, slot) != nullptr) {
            ownership::borrow_for_view(&qol::extension_bag::ui_state().objects.ownership_ledger,
                                       qol::extension_bag::ui_state().objects.module_object_handles[bag][slot]);
        }
    }
    qol::extension_bag::ui_state().mix.view_bag = bag;
    qol::extension_bag::ui_state().mix.view_installed = true;
    ++qol::extension_bag::ui_state().mix.view_generation;
    mix_request_item_refresh_locked();
    VIRTBAG_LOG("mix view installed bag=%d window=%d capacity=%d", bag, host,
                ::qol::extension_bag::access::bag().capacities[bag]);
}

void mix_restore_module_view_locked() {
    if (!qol::extension_bag::ui_state().mix.view_installed) return;
    VIRTBAG_LOG("mix restore view bag=%d window=%d current=%d state=%d",
                qol::extension_bag::ui_state().mix.view_bag, qol::extension_bag::ui_state().mix.window_original_bag, original_bag_locked(), mix_state_locked());
    if (virtual_bag::valid_index(qol::extension_bag::ui_state().mix.view_bag)) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            const uint32_t handle = qol::extension_bag::ui_state().objects.module_object_handles[qol::extension_bag::ui_state().mix.view_bag][slot];
            if (ownership::live_state(qol::extension_bag::ui_state().objects.ownership_ledger, handle,
                                      ownership::State::kBorrowedForView)) {
                ownership::return_from_view(&qol::extension_bag::ui_state().objects.ownership_ledger, handle);
            }
        }
    }
    if (qol::extension_bag::ui_state().mix.original_size_word != 0) {
        constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
        if (!set_bag_size_word_locked(
                qol::extension_bag::ui_state().mix.window_original_bag,
                (qol::extension_bag::ui_state().mix.original_size_word & ~kCapacityMask) | (qol::extension_bag::ui_state().mix.original_bag_size & kCapacityMask))) {
            VIRTBAG_LOG("mix restore capacity failed host=%d", qol::extension_bag::ui_state().mix.window_original_bag);
        }
    }
    qol::extension_bag::ui_state().mix.original_size_word = 0;
    qol::extension_bag::ui_state().mix.original_bag_size = 0;
    qol::extension_bag::ui_state().mix.window_original_bag = -1;
    qol::extension_bag::ui_state().mix.projected_item_root = nullptr;
    qol::extension_bag::ui_state().mix.view_bag = -1;
    qol::extension_bag::ui_state().mix.view_installed = false;
    ++qol::extension_bag::ui_state().mix.view_generation;
    mix_request_item_refresh_locked();
}

int mix_original_bag_button_index_locked(int64_t x, int64_t y) {
    void* container = mix_bag_container_locked();
    if (container == nullptr) return -1;
    int count = 0;
    if (!extension_bag_ui_child_count(container, &count)) return -1;
    const int limit = count < virtual_bag::kOriginalNativeBagCount
                          ? count
                          : virtual_bag::kOriginalNativeBagCount;
    for (int index = 0; index < limit; ++index) {
        void* control = valid_child_locked(container, index);
        if (control == nullptr) continue;
        qol::contracts::UiRect rect{};
        if (!extension_bag_ui_rect(control, &rect)) continue;
        if (rect.w > 0 && rect.h > 0 && extension_bag_ui_hit_test(control, x, y, rect)) return index;
    }
    return -1;
}

}  // namespace

uint32_t mix_event_wrapper(uint64_t event, uint64_t param, uint64_t param2) {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        ::qol::extension_bag::access::host().sdk == nullptr ? nullptr : &::qol::extension_bag::access::host().sdk->in_flight,
        {"own:popup.mix"});
    int original_bag_target = -1;
    if ((event == 0x17 || event == 0x18) && param != 0) {
        const int64_t x = *reinterpret_cast<const int64_t*>(param);
        const int64_t y = *reinterpret_cast<const int64_t*>(param + 8);
        ::qol::extension_bag::ReentryLock lock;
        if (qol::extension_bag::ui_state().mix.view_installed) {
            original_bag_target = mix_original_bag_button_index_locked(x, y);
        }
        if (original_bag_target >= 0) {
            qol::extension_bag::ui_state().mix.pending_tab = -1;
            qol::extension_bag::ui_state().mix.pending_tab_generation = 0;
        }
    }
    const uint32_t result = qol::extension_bag::ui_state().ui.mix_original_event != nullptr
                                ? static_cast<uint32_t>(
                                      qol::extension_bag::ui_state().ui.mix_original_event(event, param, param2))
                                : 0;
    if (original_bag_target >= 0) {
        ::qol::extension_bag::ReentryLock lock;
        if (qol::extension_bag::ui_state().mix.view_installed) {
            // The original event may return before it writes the shared current
            // bag. Pin the clicked original bag before restoring, otherwise the
            // first frame after restore draws host bag 0.
            set_original_bag_locked(original_bag_target);
            mix_restore_module_view_locked();
        }
    }
    return result;
}

void mix_enter_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        ::qol::extension_bag::access::host().sdk == nullptr ? nullptr : &::qol::extension_bag::access::host().sdk->in_flight,
        {"own:popup.mix"});
    if (qol::extension_bag::ui_state().ui.mix_original_enter != nullptr) qol::extension_bag::ui_state().ui.mix_original_enter();
    {
        ::qol::extension_bag::ReentryLock lock;
        mix_teardown_locked();
        // Enter 时面板处于 state 0（配方菜单，原版尚未画背包列）；页签等 state!=0 的
        // draw_end 再挂载，避免早于原版背包出现。
        qol::extension_bag::ui_state().mix.frame_active = true;
    }
    mix_refresh_item_after_unlock();
}

void mix_f3_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        ::qol::extension_bag::access::host().sdk == nullptr ? nullptr : &::qol::extension_bag::access::host().sdk->in_flight,
        {"own:popup.mix"});
    MixSceneNoArgFn original = nullptr;
    {
        ::qol::extension_bag::ReentryLock lock;
        mix_teardown_locked();
        qol::extension_bag::ui_state().mix.frame_active = false;
        original = qol::extension_bag::ui_state().ui.mix_original_f3;
    }
    mix_refresh_item_after_unlock();
    if (original != nullptr) original();
}

namespace {

void mix_draw_tab_buttons_locked() {
    if (!extension_bag_ui_can_draw()) return;
    // 每帧 ensure：游戏切页/关闭面板会销毁页签子树，锚定层负责证明并重建。
    mix_install_tab_buttons_locked();
    const int active = virtual_bag::active_extension_bags();
    const int visible = active < virtual_bag::kMaxRenderedExtensionBagTabs
                            ? active : virtual_bag::kMaxRenderedExtensionBagTabs;
    for (int index = visible - 1; index >= 0; --index) {
        void* button = qol::extension_bag::ui_state().mix.tab_buttons[index];
        if (button == nullptr) continue;
        int64_t x = 0;
        int64_t y = 0;
        extension_bag_control_abs_pos(button, &x, &y);
        (void)extension_bag_ui_draw_part_alpha(0xf, 0x14, static_cast<int32_t>(x - 5),
                                                 static_cast<int32_t>(y - 12), 0, 1, 0);
    }
    for (int index = 0; index < visible; ++index) {
        void* button = qol::extension_bag::ui_state().mix.tab_buttons[index];
        if (button == nullptr) continue;
        int64_t x = 0;
        int64_t y = 0;
        extension_bag_control_abs_pos(button, &x, &y);
        const bool selected = qol::extension_bag::ui_state().mix.view_installed && qol::extension_bag::ui_state().mix.view_bag == index;
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

}  // namespace

void mix_draw_end_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        ::qol::extension_bag::access::host().sdk == nullptr ? nullptr : &::qol::extension_bag::access::host().sdk->in_flight,
        {"patch:P_EXT_MIX_DRAW_END"});
    {
        ::qol::extension_bag::ReentryLock lock;
        if (qol::extension_bag::ui_state().mix.frame_active) {
            const int mix_state = mix_state_locked();
            static int last_mix_state = -2;
            if (last_mix_state != mix_state) {
                last_mix_state = mix_state;
                VIRTBAG_LOG("mix state edge state=%d installed=%d bag=%d window=%d current=%d",
                            mix_state, qol::extension_bag::ui_state().mix.view_installed ? 1 : 0, qol::extension_bag::ui_state().mix.view_bag,
                            qol::extension_bag::ui_state().mix.window_original_bag, original_bag_locked());
            }
            const bool interactive = ::qol::extension_bag::access::enabled().load() &&
                                     extension_bag_session_admitted_locked() &&
                                     mix_state != 0;
            if (interactive) {
                // 原版袋/配方切换会改写当前袋。必须先处理原版切换并清掉
                // 尚未提交的 EB 页签，否则 commit 会在同一帧重新装回旧 EB 视图。
                if (qol::extension_bag::ui_state().mix.view_installed && original_bag_locked() != qol::extension_bag::ui_state().mix.window_original_bag) {
                    qol::extension_bag::ui_state().mix.pending_tab = -1;
                    qol::extension_bag::ui_state().mix.pending_tab_generation = 0;
                    mix_restore_module_view_locked();
                }
                mix_install_tab_buttons_locked();
                mix_commit_pending_tab_locked();
                mix_refresh_projection_locked();
                mix_draw_tab_buttons_locked();
            } else {
                // state 0（配方菜单）：只隐藏页签，不卸载投影。合成确认会短暂
                // 回到 state 0，若在此 restore 会在下一帧 state 1 前闪出原版袋。
                // 仅扩展禁用或面板关闭（mix_f3）时恢复投影。
                if (!::qol::extension_bag::access::enabled().load()) {
                    mix_restore_module_view_locked();
                }
                if (qol::extension_bag::ui_state().mix.tab_buttons[0] != nullptr) {
                    mix_delete_tab_buttons_locked();
                    mix_disable_tab_buttons_locked();
                }
            }
        }
    }
    mix_refresh_item_after_unlock();
    if (::qol::extension_bag::access::host().sdk != nullptr && ::qol::extension_bag::access::host().sdk->ui != nullptr && ::qol::extension_bag::access::host().sdk->ui->end_frame != nullptr) {
        (void)::qol::extension_bag::access::host().sdk->ui->end_frame();
    }
}

void mix_teardown_locked() {
    mix_restore_module_view_locked();
    mix_disable_tab_buttons_locked();
    for (void*& item : qol::extension_bag::ui_state().mix.tab_bag_items) {
        if (item != nullptr) {
            if (defer_item_free_locked(item)) item = nullptr;
        }
    }
    qol::extension_bag::ui_state().mix.frame_active = false;
}


// R-57 原版袋列高亮遮蔽：进入扩展视图时，画原版 UIMix_DrawInvenBagGroup 前把当前袋
// GOT 临时置为动态原生袋数，画完恢复 → 原版袋不再高亮（与扩展页
// virtual_bag_draw_original_bag_wrapper 同法）。扩展页签高亮由 draw_end 的
// mix_draw_tab_buttons_locked 在袋列绘制之后补画。
void mix_draw_inven_bag_group_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        ::qol::extension_bag::access::host().sdk == nullptr ? nullptr : &::qol::extension_bag::access::host().sdk->in_flight,
        {"patch:P_EXT_MIX_BAG_DRAW"});
    if (::qol::extension_bag::access::host().game == nullptr ||
        ::qol::extension_bag::access::host().write_port->ui.draw_original_mix_bag_group == nullptr ||
        ::qol::extension_bag::access::host().write_port->ui.current_bag == nullptr ||
        ::qol::extension_bag::access::host().write_port->ui.set_current_bag == nullptr) return;
    ::qol::extension_bag::ReentryLock lock;
    int32_t saved_current = 0;
    bool masked = false;
    if (qol::extension_bag::ui_state().mix.view_installed &&
        qol::contracts::succeeded(::qol::extension_bag::access::host().write_port->ui.current_bag(&saved_current)) &&
        saved_current != native_bag_count_locked()) {
        (void)::qol::extension_bag::access::host().write_port->ui.set_current_bag(native_bag_count_locked());
        masked = true;
    }
    (void)::qol::extension_bag::access::host().write_port->ui.draw_original_mix_bag_group();
    if (masked) (void)::qol::extension_bag::access::host().write_port->ui.set_current_bag(saved_current);
}
