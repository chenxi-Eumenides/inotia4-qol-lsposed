#include "plugins/extension_bag/core/ui_state.h"
#include "plugins/extension_bag/core/drag_session.h"
#include "plugins/extension_bag/runtime/inventory_access.h"
#include "plugins/extension_bag/runtime/item_access.h"
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
#include "plugins/extension_bag/core/tab_ensure_gate.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/core/game_ui_port.h"
#include "plugins/extension_bag/model/store_sell_rules.h"
#include "plugins/extension_bag/core/lifecycle.h"
#include "plugins/extension_bag/runtime/ownership_port.h"
#include "plugins/extension_bag/core/projection.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/core/tabs.h"
#include "plugins/extension_bag/transaction/transaction.h"
#include "plugins/extension_bag/core/host_adapter.h"
#include "plugins/extension_bag/runtime/entry_guard.h"
#include "plugins/extension_bag/core/native_tab_layout.h"
#include "plugins/extension_bag/usecases/move.h"
#include "plugins/extension_bag/core/drop_routing.h"
#include "plugins/extension_bag/runtime/ui_ports.h"

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

namespace {
constexpr size_t kPopupStateSize = 0x40;
constexpr int kPopupStateCount = 27;
constexpr int64_t kCellStepY = 0x46;
constexpr size_t kInventorySlotStride = 16;
// Extension tab ControlObject rects are relative to the item-list root, whose
// absolute origin is (kGridX, kGridY). Their visuals keep the existing overlay
// positions so this phase changes input ownership without moving the UI.
constexpr int64_t kExtensionTabX = kCellX - kGridX;
constexpr int64_t kExtensionTabY = kCellY - kGridY;
constexpr int64_t kExtensionTabWidth = 0x70;
constexpr int64_t kExtensionTabHeight = 0x30;
void queue_extension_tab_click_locked(int extension_bag);

bool extension_tab_control_is_current_locked(int index, void* control);
bool extension_tab_buttons_have_controls_locked();

uint64_t extension_tab_item_proc(void* ctrl, uint64_t event, void* x2, void* param) {
    qol::extension_bag::EntryGuard guard(
        "tab_proc", qol::extension_bag::ReentryPolicy::kRejectWrite);
    if (!guard.entered()) return 0;
    const uint64_t observation_token = event == 0x10
        ? 0 : virtual_bag_observe_item_proc_pre(ctrl, event, x2, param);
    const auto finish = [observation_token, ctrl, event, x2, param](uint64_t result) {
        virtual_bag_observe_item_proc_post(observation_token, ctrl, event, x2, param, result);
        return result;
    };
    if (event == 0x02) {
        uint64_t result = 1;
        {
            // 原生事件分发可能在模块已持锁的调用栈内回调本 proc；拿不到锁时
            // 不能阻塞，直接放行让原版处理，避免自死锁。
            ::qol::extension_bag::TryReentryLock lock;
            if (!lock.owns_lock()) {
                VIRTBAG_LOG("extension tab proc busy event=0x%llx",
                            static_cast<unsigned long long>(event));
                return finish(0);
            }
            if (runtime_state().enabled.load() && extension_bag_session_admitted_locked()) {
                const int index = extension_tab_index(ctrl);
                if (index >= 0) queue_extension_tab_click_locked(index);
            }
        }
        // Observation post reacquires this mutex, so finish only after leaving the scope.
        return finish(result);  // 松开确认已处理（原版袋标签点击切袋同样返回 1，b8c60）
    }
    // 0x04 = 拖动 drop 到袋控件。无论原版源还是扩展源，只要目标袋未装备且
    // item.facts 将来源分类为背包物品就走装备；其他情况走移动。装备路径
    // 返回 0，让 TouchHandle 按原版语义复位 moving。
    if (event == 0x04) {
        uint64_t result = 0;
        {
            // 同 0x02：drop 也可能在模块持锁的原生调用栈内到达。
            ::qol::extension_bag::TryReentryLock lock;
            if (!lock.owns_lock()) {
                VIRTBAG_LOG("extension tab proc busy event=0x%llx",
                            static_cast<unsigned long long>(event));
                return finish(0);
            }
            if (!runtime_state().enabled.load() || !extension_bag_session_admitted_locked()) {
                result = 1;
            } else {
                const int index = extension_tab_index(ctrl);
                if (index >= 0 && extension_tab_control_is_current_locked(index, ctrl) &&
                    qol::extension_bag::ui_state().ui.extension_drag_session.phase != virtual_bag::DragPhase::kIdle) {
                    const int logical_bag = virtual_bag::extension_logical_bag(index);
                    const bool handled = logical_bag >= 0 &&
                                         route_projected_session_to_tab_locked(lock, logical_bag);
                    if (logical_bag < 0) {
                        VIRTBAG_LOG("cross tab reject reason=target_tab_index_invalid index=%d", index);
                    }
                    result = handled ? 1 : 0;
                } else {
                    void* moving = touch_moving_item_control_locked();
                    // 原版不可堆叠装备在 drop 事件到达时可能已经清掉
                    // ITEM_CTRL_MOVING_FLAG，但事件参数仍保留真实 source control。
                    // 优先使用 TouchHandle 状态，只有状态已清空时才回退到事件源，避免
                    // 放宽普通点击/无效控件的处理范围。
                    if (moving == nullptr && param != nullptr) {
                        moving = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(param) + 8);
                    }
                    if (index >= 0 && moving != nullptr) {
                        void* data = extension_bag_ui_data(moving);
                        void* item = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
                        int module_bag = -1;
                        int module_slot = -1;
                        const bool extension_source =
                            item != nullptr && module_slot_of_item_locked(item, &module_bag, &module_slot);
                        const int category = extension_source
                            ? runtime_state().objects.bag.items[module_bag][module_slot].category
                            : original_item_category_locked(item);
                        const bool backpack_item =
                            item_type_is(item, qol::contracts::QolItemType::kBackpack);
                        if (runtime_state().objects.bag.types[index] == 0 && backpack_item) {
                            // 背包类物品进入空扩展标签 = 装备为该袋的袋对象；来源可以是
                            // 原版背包，也可以是另一个扩展袋。两者都由同一事务处理。
                            try_equip_on_extension_tab_drop_locked(lock, index, moving);
                        } else if (extension_source) {
                            // 目标袋已有背包对象，或来源是装备/其他物品：一律按普通
                            // 扩展袋间移动处理，不能把“背包槽装备”误当人物装备。
                            const bool moved = qol::features::extension_bag::usecases::move_extension_to_extension(
                                module_bag, module_slot, index, -1);
                            if (moved) {
                                persist_state_locked(false);
                                refresh_projected_module_view_after_move_locked();
                            }
                            VIRTBAG_LOG("extension tab drop extension move result=%d index=%d category=%d",
                                        moved ? 1 : 0, index, category);
                        } else {
                            int src_bag = -1;
                            int src_slot = -1;
                            qol::contracts::QolItemHandle expected_item{};
                            const bool resolved = resolve_original_drag_source(
                                moving, &src_bag, &src_slot, &expected_item);
                            const bool moved = resolved &&
                                qol::features::extension_bag::usecases::move_original_to_extension(
                                    lock, src_bag, src_slot, index, expected_item);
                            if (moved) reset_drag_state_locked(moving);
                            VIRTBAG_LOG("extension tab drop original move result=%d index=%d category=%d equipped=%d",
                                        moved ? 1 : 0, index, category,
                                        runtime_state().objects.bag.types[index] != 0 ? 1 : 0);
                        }
                    }
                }
            }
        }
        return finish(result);
    }
    // Native MoveOn queries this proc while traversing its current control tree.
    // Only claim eligibility when an item is already being dragged, so the tab is
    // never registered as a drag source on its own press; and never block on the
    // runtime mutex from that native callback stack.
    if (event == 0x10) {
        ::qol::extension_bag::TryReentryLock lock;
        if (!lock.owns_lock()) return finish(0);
        return finish(touch_moving_item_control_locked() != nullptr ? 1 : 0);
    }
    // 其余未处理事件返回 0；原版控件对未处理事件同样返回 0。
    return finish(0);
}

// 扩展标签 = ControlItem（与原版袋标签同类）：挂袋容器（0x3049e0+0x50），
// SetUserType(2) + SetControlProc（自定义，0x02 松开确认切换/详情 toggle）。
// data[0] = 按袋 types 物化的真实背包物品对象（TouchHandle/详情链合法）。
// 手工父链累加（自上而下）：计算控件绝对位置。CO_RECT 为 i64 相对父值。
}  // namespace

void delete_extension_tab_buttons_locked() {
    // 句柄存活由锚定层证明：只删除仍能证明属于本模块的控件；失效句柄只清记录。
    const auto* components = extension_bag_ui_components();
    for (int index = 0; index < virtual_bag::kMaxExtensionBags; ++index) {
        auto& state = qol::extension_bag::ui_state().ui.extension_tab_anchors[static_cast<size_t>(index)];
        void* handle = nullptr;
        uint8_t live = 0;
        if (components != nullptr && components->anchored_control_detach != nullptr) {
            (void)components->anchored_control_detach(&state, &handle, &live);
        } else {
            state = {};
        }
        if (live != 0 && handle != nullptr && !extension_bag_ui_delete_control(handle)) {
            VIRTBAG_LOG("extension tab delete failed control=%p", handle);
        }
        qol::extension_bag::ui_state().ui.extension_tab_buttons[static_cast<size_t>(index)] = nullptr;
    }
}

namespace {
void invalidate_extension_tab_buttons_locked() {
    cancel_projected_drag_session_locked("extension-tab-root-change");
    ++qol::extension_bag::ui_state().ui.inventory_generation;
    qol::extension_bag::ui_state().ui.pending_extension_tab = -1;
    qol::extension_bag::ui_state().ui.pending_extension_tab_generation = 0;
    qol::extension_bag::ui_state().ui.extension_tab_buttons.fill(nullptr);
    qol::extension_bag::ui_state().ui.extension_tab_anchors = {};
    qol::extension_bag::ui_state().ui.extension_tab_root = nullptr;
    qol::extension_bag::ui_state().ui.extension_tab_generation = 0;
}

// ---- 页签锚定（ui_components minor 21）----
// 页签挂在游戏袋容器上，游戏在切页/关闭面板时销毁整棵子树。句柄存活/身份校验/
// 失效重建统一交给锚定层：本文件只提供几何（layout）与建控件（create）回调。

struct EquipTabAnchorContext {
    int index = 0;
};
std::array<EquipTabAnchorContext, virtual_bag::kMaxExtensionBags> g_equip_tab_anchor_ctx{};

QolResult equip_tab_anchor_layout(void* parent, qol::contracts::UiRect* out_rect, void* user) {
    if (parent == nullptr || out_rect == nullptr || user == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "tab layout", 10);
    }
    const auto* ctx = static_cast<const EquipTabAnchorContext*>(user);
    if (!virtual_bag::valid_index(ctx->index)) {
        return qol::contracts::error(kQolErrInvalidArgument, "tab index", 9);
    }
    const auto* game = runtime_state().host.game;
    const bool c18_grid = game != nullptr && game->target.has_capability != nullptr &&
        game->target.has_capability(
            qol::contracts::kQolTargetCapabilityC18NativeBagLayoutCandidate) != 0;
    if (c18_grid) {
        return c18_native_tab_anchor_layout_locked(parent, ctx->index, out_rect);
    }
    int child_count = 0;
    if (!extension_bag_ui_child_count(parent, &child_count) || child_count < 6) {
        return qol::contracts::error(kQolErrNotReady, "bag container not ready", 24);
    }
    void* bag0 = valid_child_locked(parent, 0);
    qol::contracts::UiRect bag0_rect{};
    if (bag0 == nullptr || !extension_bag_ui_rect(bag0, &bag0_rect) || bag0_rect.w <= 0) {
        return qol::contracts::error(kQolErrNotReady, "bag0 not ready", 15);
    }
    int64_t bag0_abs_x = 0, bag0_abs_y = 0, mount_abs_x = 0, mount_abs_y = 0;
    extension_bag_control_abs_pos(bag0, &bag0_abs_x, &bag0_abs_y);
    extension_bag_control_abs_pos(parent, &mount_abs_x, &mount_abs_y);
    *out_rect = {bag0_abs_x + 68 - mount_abs_x,
        bag0_abs_y + 2 - mount_abs_y + ctx->index * 70, 57, 57};
    return qol::contracts::ok();
}

QolResult equip_tab_anchor_create(void* parent, qol::contracts::UiRect rect, void* user,
                                  void** out_control) {
    if (parent == nullptr || user == nullptr || out_control == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "tab create", 10);
    }
    *out_control = nullptr;
    const auto* ctx = static_cast<const EquipTabAnchorContext*>(user);
    if (!virtual_bag::valid_index(ctx->index)) {
        return qol::contracts::error(kQolErrInvalidArgument, "tab index", 9);
    }
    // ControlItem_Create 语义组装：Add(parent, 0, 0, type=3, proc) + SetControlProc + SetUserType(2)。
    // 第 5 参 = proc，经 CreateControlInfo 写入 +0x90（CO_PROC，TouchHandle 统一事件分发）。
    void* ctrl = nullptr;
    if (!extension_bag_ui_create_control(parent, &ctrl) || ctrl == nullptr) {
        VIRTBAG_LOG("extension tab create failed index=%d", ctx->index);
        return qol::contracts::error(kQolErrNotReady, "tab create", 10);
    }
    const auto fail = [ctrl](const char* reason, int index) {
        (void)extension_bag_ui_delete_control(ctrl);
        VIRTBAG_LOG("extension tab setup failed reason=%s index=%d control=%p", reason, index, ctrl);
        return qol::contracts::error(kQolErrNotReady, "tab setup", 9);
    };
    if (!extension_bag_ui_set_control_proc(ctrl, &extension_tab_item_proc)) {
        return fail("proc", ctx->index);
    }
    if (!extension_bag_ui_set_control_user_type(ctrl, 2)) return fail("user-type", ctx->index);
    if (!extension_bag_ui_unuse_control_event_move(ctrl)) return fail("event-move", ctx->index);
    if (!extension_bag_ui_use_control_event_on(ctrl)) return fail("event-on", ctx->index);
    if (!extension_bag_ui_set_rect(ctrl, rect)) return fail("rect", ctx->index);
    // data：16B（+0x0=item、+0x8..0xb=标志），装箱背包物品对象
    void* data = nullptr;
    if (!extension_bag_ui_allocate_control_data(0x10, &data) || data == nullptr) {
        return fail("data-allocate", ctx->index);
    }
    std::memset(data, 0, 0x10);
    *reinterpret_cast<void**>(data) = qol::extension_bag::ui_state().ui.tab_bag_items[ctx->index];
    if (!extension_bag_ui_set_control_data(ctrl, data)) return fail("data-set", ctx->index);
    *out_control = ctrl;
    return qol::contracts::ok();
}

// 每帧刷新：解析活袋容器、证明/重建 5 个页签，并把句柄同步到运行视图数组。
bool refresh_extension_tab_anchors_locked(const qol::contracts::QolUiComponents* components,
                                          void* parent, bool* out_created_any) {
    if (out_created_any != nullptr) *out_created_any = false;
    if (components == nullptr || components->anchored_control_ensure == nullptr) return false;
    bool all = true;
    const int active = virtual_bag::active_extension_bags();
    const int visible = active < virtual_bag::kMaxRenderedExtensionBagTabs
                            ? active : virtual_bag::kMaxRenderedExtensionBagTabs;
    if (active > virtual_bag::kMaxRenderedExtensionBagTabs) {
        VIRTBAG_LOG("tab layout pending multi-column count=%d", active);
    }
    for (int index = 0; index < visible; ++index) {
        auto& ctx = g_equip_tab_anchor_ctx[static_cast<size_t>(index)];
        ctx.index = index;
        const qol::contracts::UiAnchoredControlDesc desc{
            static_cast<uint32_t>(qol::contracts::QolUiRootKind::kEquipBagContainer),
            &extension_tab_item_proc, &equip_tab_anchor_layout, &equip_tab_anchor_create, &ctx};
        uint8_t created = 0;
        auto& state = qol::extension_bag::ui_state().ui.extension_tab_anchors[static_cast<size_t>(index)];
        const QolResult ensured = components->anchored_control_ensure(&state, &desc, &created);
        if (ensured.ok != kQolOk) {
            all = false;
            // NotReady 是面板打开初期的正常态（占位容器/布局未就位），不刷日志。
            if (ensured.ok != kQolErrNotReady) {
                VIRTBAG_LOG("extension tab anchor ensure failed index=%d code=%d root=%p", index,
                            ensured.ok, parent);
            }
        }
        qol::extension_bag::ui_state().ui.extension_tab_buttons[static_cast<size_t>(index)] =
            state.mounted != 0 ? state.handle : nullptr;
        if (created != 0) {
            if (out_created_any != nullptr) *out_created_any = true;
            VIRTBAG_LOG("extension tab re-anchored index=%d root=%p", index, parent);
        }
    }
    return all;
}

}  // namespace

void install_extension_tab_buttons_locked() {
    // 配置门控：扩展背包关闭时不得挂载页签（3 个宿主之一）。启用/禁用均以
    // g_virtual_bag_enabled 为准；调用方不再各自判断。
    if (!runtime_state().enabled.load() || !extension_bag_session_admitted_locked()) return;
    void* bag_container = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer);
    if (bag_container == nullptr) return;
    // 容器就位守卫：面板打开初期 0x50 槽位指向占位容器（无原版袋标签子控件，
    // rect 未定位）。等原版 6 袋标签就位后再挂载；未就位由 draw 每帧重试。
    int bag_child_count = 0;
    if (!extension_bag_ui_child_count(bag_container, &bag_child_count) || bag_child_count < 6) return;
    // 每帧完整 ensure 的成本主要在控件遍历/校验；root、子控件数和 inventory
    // generation 未变时每 10 帧做一次存活校验即可，其余帧只保留绘制路径。
    const uint64_t generation = qol::extension_bag::ui_state().ui.inventory_generation;
    const int active = virtual_bag::active_extension_bags();
    const int visible = active < virtual_bag::kMaxRenderedExtensionBagTabs
                            ? active : virtual_bag::kMaxRenderedExtensionBagTabs;
    static qol::features::extension_bag::TabEnsureGate s_gate;
    if (qol::features::extension_bag::tab_ensure_can_skip(
            s_gate, bag_container, bag_child_count, generation,
            qol::extension_bag::ui_state().ui.extension_tab_buttons.data(), visible)) {
        return;
    }
    const auto* components = extension_bag_ui_components();
    if (components == nullptr) return;
    refresh_tab_bag_items_locked();
    const bool changed = qol::extension_bag::ui_state().ui.extension_tab_root != bag_container ||
        qol::extension_bag::ui_state().ui.extension_tab_generation != qol::extension_bag::ui_state().ui.inventory_generation;
    bool created_any = false;
    const bool complete = refresh_extension_tab_anchors_locked(components, bag_container,
                                                               &created_any);
    qol::extension_bag::ui_state().ui.extension_tab_root = bag_container;
    qol::extension_bag::ui_state().ui.extension_tab_generation = qol::extension_bag::ui_state().ui.inventory_generation;
    if (changed || created_any) {
        VIRTBAG_LOG("extension tabs installed generation=%llu root=%p complete=%d created=%d",
                    static_cast<unsigned long long>(qol::extension_bag::ui_state().ui.inventory_generation),
                    bag_container, complete ? 1 : 0, created_any ? 1 : 0);
    }
}

void disable_extension_tab_buttons_locked() {
    void* live_root = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer);
    if (qol::extension_bag::ui_state().ui.extension_tab_root != nullptr && live_root == qol::extension_bag::ui_state().ui.extension_tab_root) {
        delete_extension_tab_buttons_locked();
    } else if (qol::extension_bag::ui_state().ui.extension_tab_root != nullptr) {
        VIRTBAG_LOG("extension tab references invalidated reason=root_not_live saved=%p live=%p",
                    qol::extension_bag::ui_state().ui.extension_tab_root, live_root);
    }
    // The game owns controls; stale-root references are invalidated, never deleted.
    invalidate_extension_tab_buttons_locked();
}

namespace {
bool extension_tab_control_is_current_locked(int index, void* control);
void handle_extension_tab_click_impl_locked(int extension_bag,
                                            ::qol::extension_bag::ReentryLock* unlockable_lock);
}  // namespace

bool extension_tab_buttons_complete_locked() {
    const int active = virtual_bag::active_extension_bags();
    const int visible = active < virtual_bag::kMaxRenderedExtensionBagTabs
                            ? active : virtual_bag::kMaxRenderedExtensionBagTabs;
    for (int index = 0; index < visible; ++index) {
        if (qol::extension_bag::ui_state().ui.extension_tab_buttons[static_cast<size_t>(index)] == nullptr) return false;
    }
    return true;
}

namespace {
bool extension_tab_buttons_have_controls_locked() {
    for (void* button : qol::extension_bag::ui_state().ui.extension_tab_buttons) {
        if (button != nullptr) return true;
    }
    return false;
}
}  // namespace

void reset_extension_desc_unequip_hook_locked() {
    // 卸下按钮由函数级 ButtonUnequipExe hook 承担，
    // 仅清 desc 袋记录。
    qol::extension_bag::ui_state().ui.extension_desc_unequip_bag = -1;
}

namespace {
void install_extension_desc_unequip_hook_locked(int extension_bag) {
    reset_extension_desc_unequip_hook_locked();
    if (!virtual_bag::valid_index(extension_bag)) return;
    // 扩展袋 desc 打开时记录袋位，供卸下入口识别路径。
    qol::extension_bag::ui_state().ui.extension_desc_unequip_bag = extension_bag;
    VIRTBAG_LOG("extension desc unequip armed bag=%d", extension_bag);
}

// P3：扩展袋切换音效，与原版袋按钮同款（UIEquip_InvenBagControlEventProc b8c34：Play(0x11)）。
void play_extension_switch_sound() {
    if (runtime_state().host.game == nullptr || runtime_state().host.write_port->ui.play_switch_sound == nullptr) return;
    (void)runtime_state().host.write_port->ui.play_switch_sound();
}
}  // namespace

void handle_extension_tab_click_locked(int extension_bag) {
    handle_extension_tab_click_impl_locked(extension_bag, nullptr);
}

namespace {
void handle_pending_extension_tab_click_locked(::qol::extension_bag::ReentryLock& lock,
                                               int extension_bag) {
    handle_extension_tab_click_impl_locked(extension_bag, &lock);
}

void handle_extension_tab_click_impl_locked(int extension_bag,
                                            ::qol::extension_bag::ReentryLock* unlockable_lock) {
    if (!virtual_bag::valid_index(extension_bag) ||
        !extension_bag_session_admitted_locked()) return;
    if (runtime_state().objects.bag.mode == virtual_bag::Mode::kOriginal) {
        const int original_bag = original_bag_locked();
        if (original_bag == virtual_bag::kOriginalTaskBag) {
            constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
            void* panel_root = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipItemRoot);
            const int target_capacity = runtime_state().objects.bag.capacities[extension_bag];
            const bool target_ready = target_capacity > 0 && panel_root != nullptr;
            int projection_host = -1;
            if (target_ready) {
                for (int candidate = 0;
                     candidate < virtual_bag::kOriginalTransactionBagCount; ++candidate) {
                    uint32_t size_word = 0;
                    if (!bag_size_word_locked(candidate, &size_word) ||
                        (size_word & kCapacityMask) == 0) continue;
                    bool controls_ready = true;
                    for (int slot = 0; slot < target_capacity; ++slot) {
                        if (valid_child_locked(panel_root, slot) == nullptr) {
                            controls_ready = false;
                            break;
                        }
                    }
                    if (controls_ready) {
                        projection_host = candidate;
                        break;
                    }
                }
            }
            if (projection_host < 0) {
                VIRTBAG_LOG(
                    "extension tab reject reason=projection_host_unavailable source_original_bag=%d target=%d",
                    original_bag, extension_bag);
                return;
            }
            set_original_bag_locked(projection_host);
            qol::extension_bag::ui_state().objects.original_current_direct = static_cast<uint8_t>(projection_host);
            qol::extension_bag::ui_state().objects.original_current_got = static_cast<uint8_t>(projection_host);
            if (!install_module_view_locked(extension_bag)) {
                set_original_bag_locked(original_bag);
                qol::extension_bag::ui_state().objects.original_current_direct = static_cast<uint8_t>(original_bag);
                qol::extension_bag::ui_state().objects.original_current_got = static_cast<uint8_t>(original_bag);
                VIRTBAG_LOG(
                    "extension tab reject reason=projection_host_unavailable source_original_bag=%d target=%d host=%d",
                    original_bag, extension_bag, projection_host);
                return;
            }
            if (virtual_bag::click(&runtime_state().objects.bag, extension_bag) !=
                virtual_bag::ClickResult::kSelected) {
                restore_module_view_locked();
                set_original_bag_locked(original_bag);
                qol::extension_bag::ui_state().objects.original_current_direct = static_cast<uint8_t>(original_bag);
                qol::extension_bag::ui_state().objects.original_current_got = static_cast<uint8_t>(original_bag);
                VIRTBAG_LOG(
                    "extension tab reject reason=projection_host_unavailable source_original_bag=%d target=%d host=%d",
                    original_bag, extension_bag, projection_host);
                return;
            }
            persist_state_locked();
            extension_bag_mark_dirty_locked();
            VIRTBAG_LOG("extension tab selected bag=%d original_bag=%d projection_host=%d",
                        extension_bag, original_bag, projection_host);
            return;
        }
        if (virtual_bag::valid_original_transaction_bag(original_bag)) {
            qol::extension_bag::ui_state().objects.original_current_direct = static_cast<uint8_t>(original_bag);
            qol::extension_bag::ui_state().objects.original_current_got = static_cast<uint8_t>(original_bag);
            if (virtual_bag::click(&runtime_state().objects.bag, extension_bag) !=
                virtual_bag::ClickResult::kIgnored) {
                install_module_view_locked(extension_bag);
                persist_state_locked();
                extension_bag_mark_dirty_locked();
                VIRTBAG_LOG("extension tab selected bag=%d original_bag=%d", extension_bag,
                            original_bag);
            }
        } else {
            VIRTBAG_LOG("extension tab reject original window reason=invalid_transaction_domain bag=%d",
                        original_bag);
        }
        return;
    }
    if (runtime_state().objects.bag.mode == virtual_bag::Mode::kModule) {
        if (runtime_state().objects.bag.selected != extension_bag) {
            if (virtual_bag::click(&runtime_state().objects.bag, extension_bag) !=
                virtual_bag::ClickResult::kIgnored) {
                install_module_view_locked(extension_bag);
                extension_bag_mark_dirty_locked();
                play_extension_switch_sound();
                VIRTBAG_LOG("extension tab switched bag=%d", extension_bag);
            }
            return;
        }
        // 二次点击 = 原版袋信息面板（原版语义：desc_type=1 + UIEquip_MakeDesc）。
        // MakeDesc 读控件物品（data[0]=袋物品对象）生成详情，SetDescMenu 按
        // desc_type=1 加解除按钮；关闭详情经 UIDesc_SetOff。
        if (virtual_bag::click(&runtime_state().objects.bag, extension_bag) ==
            virtual_bag::ClickResult::kInspected) {
            play_extension_switch_sound();
            void* ctrl = qol::extension_bag::ui_state().ui.extension_tab_buttons[extension_bag];
            if (ctrl != nullptr && runtime_state().host.game != nullptr &&
                runtime_state().host.write_port->ui.make_item_description != nullptr &&
                runtime_state().host.write_port->ui.set_current_bag != nullptr &&
                runtime_state().host.write_port->ui.set_desc_type != nullptr) {
                // 门禁掩码（P3）：UIEquip_SetDescMenu desc_type=1 分支经 GOT 槽读
                // 当前原版袋索引，0（初始袋）/5（任务袋）短路不生成解除按钮
                // （b8570-b8578）。扩展袋与该全局无关——进入前所在原版袋决定
                // 按钮有无。MakeDesc 同步尾跳 SetDescMenu（b89c0），临时把
                // direct+GOT 双字节掩码为 1 使按钮恒生成，返回后立即恢复快照
                // （成对写约定同 set_original_bag_locked；按钮在 panel+0x68，
                // 不随恢复消失，点击回调已被模块 hook 接管）。Pending 路径在这些
                // UI 回调期间释放运行时锁，并在返回后复核视图身份。
                const int saved_bag = raw_direct_bag_locked();
                const int saved_desc_type = raw_desc_type_locked();
                void* const tab_data = extension_bag_ui_data(ctrl);
                void* const described_item = tab_data != nullptr
                    ? *reinterpret_cast<void**>(tab_data) : nullptr;
                const uint8_t bag_type = runtime_state().objects.bag.types[extension_bag];
                const int bag_capacity = runtime_state().objects.bag.capacities[extension_bag];
                if (tab_data == nullptr || described_item == nullptr) {
                    QOL_LOG_ERROR(QolDomain::kExtensionBag,
                                  "extension bag description rejected missing control item bag=%d control=%p",
                                  extension_bag, ctrl);
                } else if (saved_bag < 0 || saved_desc_type < 0) {
                    QOL_LOG_ERROR(QolDomain::kExtensionBag,
                                  "extension bag description rejected state snapshot bag=%d current_bag=%d desc_type=%d",
                                  extension_bag, saved_bag, saved_desc_type);
                } else if (unlockable_lock == nullptr) {
                    const bool bag_set = qol::contracts::succeeded(
                        runtime_state().host.write_port->ui.set_current_bag(1));
                    const bool desc_set = bag_set && qol::contracts::succeeded(
                        runtime_state().host.write_port->ui.set_desc_type(1));
                    const bool made = desc_set && qol::contracts::succeeded(
                        runtime_state().host.write_port->ui.make_item_description(ctrl));
                    const bool bag_restored = saved_bag < 0 || qol::contracts::succeeded(
                        runtime_state().host.write_port->ui.set_current_bag(saved_bag));
                    const bool desc_restored = saved_desc_type < 0 || qol::contracts::succeeded(
                        runtime_state().host.write_port->ui.set_desc_type(saved_desc_type));
                    if (!bag_set || !desc_set || !made || !bag_restored || !desc_restored) {
                        QOL_LOG_ERROR(QolDomain::kExtensionBag,
                                      "extension bag description UI operation failed bag=%d bag_set=%d desc_set=%d made=%d bag_restored=%d desc_restored=%d",
                                      extension_bag, bag_set ? 1 : 0, desc_set ? 1 : 0,
                                      made ? 1 : 0, bag_restored ? 1 : 0,
                                      desc_restored ? 1 : 0);
                    } else {
                        install_extension_desc_unequip_hook_locked(extension_bag);
                    }
                } else {
                    const uint64_t generation = qol::extension_bag::ui_state().ui.inventory_generation;
                    void* const root = qol::extension_bag::ui_state().ui.extension_tab_root;
                    unlockable_lock->unlock();
                    // Original callback operation: g_write_port->ui.make_item_description(ctrl).
                    const bool bag_set = qol::contracts::succeeded(
                        runtime_state().host.write_port->ui.set_current_bag(1));
                    const bool desc_set = bag_set && qol::contracts::succeeded(
                        runtime_state().host.write_port->ui.set_desc_type(1));
                    const bool made = desc_set && qol::contracts::succeeded(
                        runtime_state().host.write_port->ui.make_item_description(ctrl));
                    const bool bag_restored = saved_bag < 0 || qol::contracts::succeeded(
                        runtime_state().host.write_port->ui.set_current_bag(saved_bag));
                    const bool desc_restored = saved_desc_type < 0 || qol::contracts::succeeded(
                        runtime_state().host.write_port->ui.set_desc_type(saved_desc_type));
                    unlockable_lock->lock();
                    bool still_current = runtime_state().enabled.load() &&
                        extension_bag_session_admitted_locked() &&
                        // Former generation predicate: g_inventory_generation == generation.
                        qol::extension_bag::ui_state().ui.inventory_generation == generation &&
                        qol::extension_bag::ui_state().ui.extension_tab_root == root &&
                        runtime_state().objects.bag.mode == virtual_bag::Mode::kModule &&
                        runtime_state().objects.bag.selected == extension_bag &&
                        runtime_state().objects.bag.types[extension_bag] == bag_type &&
                        runtime_state().objects.bag.capacities[extension_bag] == bag_capacity &&
                        qol::extension_bag::ui_state().ui.tab_bag_items[extension_bag] == described_item &&
                        extension_tab_control_is_current_locked(extension_bag, ctrl);
                    if (still_current) {
                        void* const current_data = extension_bag_ui_data(ctrl);
                        still_current = current_data != nullptr &&
                            *reinterpret_cast<void**>(current_data) == described_item;
                    }
                    if (!bag_set || !desc_set || !made || !bag_restored || !desc_restored) {
                        QOL_LOG_ERROR(QolDomain::kExtensionBag,
                                      "extension bag description UI operation failed bag=%d bag_set=%d desc_set=%d made=%d bag_restored=%d desc_restored=%d",
                                      extension_bag, bag_set ? 1 : 0, desc_set ? 1 : 0,
                                      made ? 1 : 0, bag_restored ? 1 : 0,
                                      desc_restored ? 1 : 0);
                    }
                    const bool description_ready = bag_set && desc_set && made &&
                                                   bag_restored && desc_restored;
                    if (!still_current || !description_ready) {
                        if (!still_current) {
                            QOL_LOG_WARN(QolDomain::kExtensionBag,
                                         "extension bag description discarded after view changed bag=%d generation=%llu",
                                         extension_bag,
                                         static_cast<unsigned long long>(generation));
                        }
                        if (made) {
                            bool closed = false;
                            if (runtime_state().host.write_port->ui.description_set_off != nullptr) {
                                unlockable_lock->unlock();
                                closed = qol::contracts::succeeded(
                                    runtime_state().host.write_port->ui.description_set_off());
                                unlockable_lock->lock();
                            }
                            if (!closed) {
                                QOL_LOG_ERROR(QolDomain::kExtensionBag,
                                              "extension bag description close failed bag=%d",
                                              extension_bag);
                            }
                        }
                    } else {
                        install_extension_desc_unequip_hook_locked(extension_bag);
                    }
                }
            } else {
                QOL_LOG_ERROR(QolDomain::kExtensionBag,
                              "extension bag description rejected UI adapter unavailable bag=%d control=%p",
                              extension_bag, ctrl);
            }
            VIRTBAG_LOG("extension bag desc opened bag=%d", extension_bag);
        }
    }
}

void queue_extension_tab_click_locked(int extension_bag) {
    // The enabled gate is the explicit runtime_state().enabled form of g_virtual_bag_enabled.load().
    if (!runtime_state().enabled.load() || !extension_bag_session_admitted_locked() ||
        !virtual_bag::valid_index(extension_bag) ||
        !extension_tab_control_is_current_locked(
            extension_bag, qol::extension_bag::ui_state().ui.extension_tab_buttons[static_cast<size_t>(extension_bag)]) ||
        qol::extension_bag::ui_state().ui.extension_tab_generation != qol::extension_bag::ui_state().ui.inventory_generation) {
        return;
    }
    if (qol::extension_bag::ui_state().ui.pending_extension_tab == extension_bag &&
        qol::extension_bag::ui_state().ui.pending_extension_tab_generation == qol::extension_bag::ui_state().ui.inventory_generation) {
        return;
    }
    qol::extension_bag::ui_state().ui.pending_extension_tab = extension_bag;
    qol::extension_bag::ui_state().ui.pending_extension_tab_generation = qol::extension_bag::ui_state().ui.inventory_generation;
    VIRTBAG_LOG("extension tab pending bag=%d generation=%llu", extension_bag,
                static_cast<unsigned long long>(qol::extension_bag::ui_state().ui.inventory_generation));
}

void extension_tab_button_clicked(void* ctrl) {
    ::qol::extension_bag::ReentryLock lock;
    const int extension_bag = extension_tab_index(ctrl);
    queue_extension_tab_click_locked(extension_bag);
}

bool extension_tab_control_is_current_locked(int index, void* control) {
    if (!runtime_state().enabled.load() || !extension_bag_session_admitted_locked() ||
        !virtual_bag::valid_index(index) || control == nullptr ||
        qol::extension_bag::ui_state().ui.extension_tab_root == nullptr ||
        qol::extension_bag::ui_state().ui.extension_tab_anchors[index].mounted == 0 ||
        qol::extension_bag::ui_state().ui.extension_tab_anchors[index].handle != control ||
        qol::extension_bag::ui_state().ui.extension_tab_buttons[index] != control ||
        qol::extension_bag::ui_state().ui.extension_tab_generation != qol::extension_bag::ui_state().ui.inventory_generation ||
        extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer) !=
            qol::extension_bag::ui_state().ui.extension_tab_root) {
        return false;
    }
    int child_count = 0;
    if (!extension_bag_ui_child_count(qol::extension_bag::ui_state().ui.extension_tab_root, &child_count)) return false;
    for (int child = 0; child < child_count; ++child) {
        if (extension_bag_ui_child_at(qol::extension_bag::ui_state().ui.extension_tab_root, child) == control) {
            return valid_child_locked(qol::extension_bag::ui_state().ui.extension_tab_root, child) == control;
        }
    }
    return false;
}
}  // namespace

void commit_pending_extension_tab_locked(::qol::extension_bag::ReentryLock& lock) {
    if (!virtual_bag::valid_index(qol::extension_bag::ui_state().ui.pending_extension_tab) ||
        !runtime_state().enabled.load() || !extension_bag_session_admitted_locked() ||
        qol::extension_bag::ui_state().ui.pending_extension_tab_generation != qol::extension_bag::ui_state().ui.inventory_generation ||
        qol::extension_bag::ui_state().ui.extension_tab_generation != qol::extension_bag::ui_state().ui.inventory_generation ||
        !extension_tab_control_is_current_locked(
            qol::extension_bag::ui_state().ui.pending_extension_tab,
            qol::extension_bag::ui_state().ui.extension_tab_buttons[static_cast<size_t>(qol::extension_bag::ui_state().ui.pending_extension_tab)])) {
        qol::extension_bag::ui_state().ui.pending_extension_tab = -1;
        qol::extension_bag::ui_state().ui.pending_extension_tab_generation = 0;
        return;
    }
    const int extension_bag = qol::extension_bag::ui_state().ui.pending_extension_tab;
    qol::extension_bag::ui_state().ui.pending_extension_tab = -1;
    qol::extension_bag::ui_state().ui.pending_extension_tab_generation = 0;
    handle_pending_extension_tab_click_locked(lock, extension_bag);
}

bool extension_tab_hit(int index, int64_t x, int64_t y) {
    if (!virtual_bag::valid_index(index)) return false;
    if (runtime_state().objects.bag.capacities[index] == 0) {
        // 未装备袋只有在当前拖动物品满足装备规则时可命中；否则仍保持
        // G-12 的不可命中门禁，避免普通物品误落到未装备标签。
        if (!item_type_is(drag_source_item_locked(),
                          qol::contracts::QolItemType::kBackpack)) return false;
    }
    // 标签位置是动态换算的（tab_rel + 袋容器绝对），用控件真实绝对 rect 判断，
    // 不用旧硬编码 kCellX/kCellY（标签左移后二者偏移 36px 致命中失效）。
    void* ctrl = qol::extension_bag::ui_state().ui.extension_tab_buttons[index];
    if (ctrl == nullptr) return false;
    qol::contracts::UiRect rect{};
    return extension_bag_ui_rect(ctrl, &rect) && rect.w > 0 && rect.h > 0 &&
           extension_bag_ui_hit_test(ctrl, x, y, rect);
}

void extension_bag_ui_play_switch_sound() { play_extension_switch_sound(); }
void extension_bag_ui_handle_tab_click_locked(int extension_bag) {
    handle_extension_tab_click_locked(extension_bag);
}

bool extension_c18_native_bag_hit(int64_t x, int64_t y, int* out_native_bag) {
    return c18_native_bag_hit_locked(x, y, out_native_bag);
}
void prepare_c18_native_bag_layout_locked() {
    prepare_c18_native_bag_layout_locked_impl();
}
