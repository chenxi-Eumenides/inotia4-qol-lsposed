#include "game_ui_virtbag.h"

#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "contracts/ownership.h"
#include "contracts/inventory_refresh.h"
#include "contracts/item_facts.h"
#include "plugins/extension_bag/model/equip_rules.h"
#include "plugins/extension_bag/extension_bag_port.h"
#include "plugins/extension_bag/assembly/move_merge.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/model/sale_transaction.h"
#include "plugins/extension_bag/model/draw_state_lock.h"
#include "plugins/extension_bag/model/main_menu_cleanup_edge.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "contracts/module_save_port.h"
#include "contracts/popup_enter_alias.h"
#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/extension_bag_geometry.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/plugin.h"
#include "plugins/extension_bag/ui/store_sell_rules.h"
#include "plugins/extension_bag/extension_bag_internal.h"

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

#define handle_extension_tab_click_locked handle_extension_tab_click_locked_internal
#define delete_extension_tab_buttons_locked delete_extension_tab_buttons_locked_internal
#define disable_extension_tab_buttons_locked disable_extension_tab_buttons_locked_internal
#define install_extension_tab_buttons_locked install_extension_tab_buttons_locked_internal
#define extension_tab_buttons_complete_locked extension_tab_buttons_complete_locked_internal
#define commit_pending_extension_tab_locked commit_pending_extension_tab_locked_internal
#define reset_extension_desc_unequip_hook_locked reset_extension_desc_unequip_hook_locked_internal
#define extension_tab_hit extension_tab_hit_internal

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
bool is_backpack_category(int category) {
    // 这是 State::types 使用的 BagType 判定：1..4 派生容量 4/8/12/16，
    // 只适用于已归一化的扩展袋类型。它不是原生物品类别判定；原生物品
    // 详情必须使用 category_is_extension_backpack 的 ITEMCLASSBASE +2 规则。
    return category >= 1 && category <= 4;
}

bool extension_tab_control_is_current_locked(int index, void* control);
bool extension_tab_buttons_complete_locked();
bool extension_tab_buttons_have_controls_locked();

uint64_t extension_tab_item_proc(void* ctrl, uint64_t event, void* x2, void* param) {
    const uint64_t observation_token = virtual_bag_observe_item_proc_pre(ctrl, event, x2, param);
    const auto finish = [observation_token, ctrl, event, x2, param](uint64_t result) {
        virtual_bag_observe_item_proc_post(observation_token, ctrl, event, x2, param, result);
        return result;
    };
    if (event == 0x02) {
        uint64_t result = 1;
        {
            std::unique_lock<std::mutex> lock(g_virtual_bag_mtx);
            if (g_virtual_bag_enabled.load() && extension_bag_session_admitted_locked()) {
                const int index = extension_tab_index(ctrl);
                if (index >= 0) queue_extension_tab_click_locked(index);
            }
        }
        // Observation post reacquires this mutex, so finish only after leaving the scope.
        return finish(result);  // 松开确认已处理（原版袋标签点击切袋同样返回 1，b8c60）
    }
    // 0x04 = 拖动 drop 到袋控件。无论原版源还是扩展源，只要目标袋未装备且
    // 源类别为背包物品（category 1..4）就走装备；其他情况走移动。装备路径
    // 返回 0，让 TouchHandle 按原版语义复位 moving。
    if (event == 0x04) {
        uint64_t result = 0;
        {
            std::unique_lock<std::mutex> lock(g_virtual_bag_mtx);
            if (!g_virtual_bag_enabled.load() || !extension_bag_session_admitted_locked()) {
                result = 1;
            } else {
            const int index = extension_tab_index(ctrl);
            if (index >= 0 && extension_tab_control_is_current_locked(index, ctrl) &&
                g_extension_drag_session.phase != virtual_bag::DragPhase::kIdle) {
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
                        ? g_virtual_bag_state.items[module_bag][module_slot].category
                        : original_item_category_locked(item);
                    const bool backpack_item = is_backpack_category(category);
                    if (g_virtual_bag_state.types[index] == 0 && backpack_item) {
                        // 背包类物品进入空扩展标签 = 装备为该袋的袋对象；来源可以是
                        // 原版背包，也可以是另一个扩展袋。两者都由同一事务处理。
                        try_equip_on_extension_tab_drop_locked(lock, index, moving);
                    } else if (extension_source) {
                        // 目标袋已有背包对象，或来源是装备/其他物品：一律按普通
                        // 扩展袋间移动处理，不能把“背包槽装备”误当成人物装备。
                        const bool moved = move_extension_to_extension_locked(
                            module_bag, module_slot, index, -1);
                        if (moved) {
                            persist_state_locked(false);
                            refresh_projected_module_view_after_move_locked();
                        }
                        VIRTBAG_LOG("extension tab drop extension move result=%d index=%d category=%d",
                                    moved ? 1 : 0, index, category);
                    } else {
                        const bool moved = move_original_to_extension_locked(lock, index, moving);
                        VIRTBAG_LOG("extension tab drop original move result=%d index=%d category=%d equipped=%d",
                                    moved ? 1 : 0, index, category,
                                    g_virtual_bag_state.types[index] != 0 ? 1 : 0);
                    }
                }
            }
            }
        }
        return finish(result);
    }
    // 其余事件（含 0x10 拖动发起查询：TouchHandle_MoveOn @a2fec-a304c 派发，
    // proc 返回 1 即登记 moving 控件）一律返回 0——兜底返回 1 会让标签成为
    // 拖动源（标签可被拖出背包物品的真因，且拖出物可被 drop 入库造成复制）。
    // 原版控件对未处理事件同样返回 0。
    return finish(0);
}

// 扩展标签 = ControlItem（与原版袋标签同类）：挂袋容器（0x3049e0+0x50），
// SetUserType(2) + SetControlProc（自定义，0x02 松开确认切换/详情 toggle）。
// data[0] = 按袋 types 物化的真实背包物品对象（TouchHandle/详情链合法）。
// 手工父链累加（自上而下）：计算控件绝对位置。CO_RECT 为 i64 相对父值。
void delete_extension_tab_buttons_locked() {
    for (void*& button : g_extension_tab_buttons) {
        if (button == nullptr) continue;
        if (extension_bag_ui_delete_control(button)) {
            button = nullptr;
        } else {
            VIRTBAG_LOG("extension tab delete failed control=%p", button);
        }
    }
}

void invalidate_extension_tab_buttons_locked() {
    cancel_projected_drag_session_locked("extension-tab-root-change");
    ++g_inventory_generation;
    g_pending_extension_tab = -1;
    g_pending_extension_tab_generation = 0;
    g_extension_tab_buttons.fill(nullptr);
    g_extension_tab_root = nullptr;
    g_extension_tab_generation = 0;
}

void install_extension_tab_buttons_locked() {
    // 配置门控：扩展背包关闭时不得挂载页签（3 个宿主之一）。启用/禁用均以
    // g_virtual_bag_enabled 为准；调用方不再各自判断。
    if (!g_virtual_bag_enabled.load() || !extension_bag_session_admitted_locked()) return;
    void* bag_container = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer);
    if (bag_container == nullptr) return;
    // 容器就位守卫：面板打开初期 0x50 槽位指向占位容器（无原版袋标签子控件，
    // rect 未定位）。此时挂载会让标签落在占位容器上（绝对 x≈296 出位）。
    // 等原版 6 袋标签就位（child count ≥ 6）后再挂载；未就位直接跳过，
    // 由 draw 每帧检测容器就位后重试。
    int bag_child_count = 0;
    if (!extension_bag_ui_child_count(bag_container, &bag_child_count) || bag_child_count < 6) return;
    // 动态计算：打开面板后，原版袋 0 按钮的绝对位置 =
    // 袋容器绝对（父链累加，袋 0 相对容器为 (0,0)）。缓存至面板重建。
    // 挂载层 = 袋容器（与原版 6 袋标签同父，跟随容器动画，天然不漂移）。
    int64_t bag0_abs_x = 0, bag0_abs_y = 0;
    extension_bag_control_abs_pos(bag_container, &bag0_abs_x, &bag0_abs_y);
    void* mount_parent = bag_container;
    int64_t mount_abs_x = 0, mount_abs_y = 0;
    extension_bag_control_abs_pos(mount_parent, &mount_abs_x, &mount_abs_y);
    // 标签相对挂载层 = 原版袋列右侧（袋 0 绝对 + (68,2)，减挂载层绝对）
    const int64_t tab_rel_x = bag0_abs_x + 68 - mount_abs_x;
    const int64_t tab_rel_y = bag0_abs_y + 2 - mount_abs_y;
    if (g_extension_tab_generation == g_inventory_generation &&
        g_extension_tab_root == mount_parent && extension_tab_buttons_complete_locked()) {
        return;
    }
    // A replaced root owns the old controls; invalidate them without dereferencing stale handles.
    if (g_extension_tab_root != nullptr && g_extension_tab_root != mount_parent) {
        invalidate_extension_tab_buttons_locked();
    } else if (g_extension_tab_root == mount_parent || extension_tab_buttons_have_controls_locked()) {
        delete_extension_tab_buttons_locked();
        if (extension_tab_buttons_have_controls_locked()) {
            VIRTBAG_LOG("extension tab install aborted reason=previous_controls_not_deleted root=%p",
                        mount_parent);
            return;
        }
        g_extension_tab_root = nullptr;
        g_extension_tab_generation = 0;
    }
    refresh_tab_bag_items_locked();
    std::array<void*, virtual_bag::kBagCount> created{};
    auto discard_created = [&]() {
        bool all_deleted = true;
        for (int index = 0; index < virtual_bag::kBagCount; ++index) {
            void*& control = created[static_cast<size_t>(index)];
            if (control == nullptr) continue;
            if (!extension_bag_ui_delete_control(control)) {
                all_deleted = false;
                g_extension_tab_buttons[static_cast<size_t>(index)] = control;
                VIRTBAG_LOG("extension tab cleanup failed index=%d control=%p", index, control);
            }
            control = nullptr;
        }
        if (!all_deleted) {
            // Keep the failed live controls tracked so a same-root retry can delete them.
            g_extension_tab_root = mount_parent;
            g_extension_tab_generation = 0;
        }
        return all_deleted;
    };
    bool complete = true;
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        // ControlItem_Create 语义组装：Add(parent, 0, 0, type=3, proc) + SetControlProc + SetUserType(2)。
        // 第 5 参 = proc，经 CreateControlInfo 写入 +0x90（CO_PROC，TouchHandle 统一事件分发）。
        // 传 nullptr 会让 +0x90 为空 → TouchHandle 命中后无法分发到 +0x98 control proc（点击失效）。
        void* ctrl = nullptr;
        if (!extension_bag_ui_create_control(mount_parent, &ctrl) ||
            !extension_bag_ui_set_control_proc(ctrl, &extension_tab_item_proc) ||
            !extension_bag_ui_set_control_user_type(ctrl, 2) ||
            !extension_bag_ui_unuse_control_event_move(ctrl)) {
            if (ctrl != nullptr && !extension_bag_ui_delete_control(ctrl)) {
                VIRTBAG_LOG("extension tab cleanup failed index=%d control=%p", index, ctrl);
                created[static_cast<size_t>(index)] = ctrl;
                complete = false;
                break;
            }
            ctrl = nullptr;
        }
        if (ctrl == nullptr) {
            VIRTBAG_LOG("extension tab create failed index=%d", index);
            complete = false;
            break;
        }
        created[static_cast<size_t>(index)] = ctrl;
        // rect：与原版袋标签同尺寸 57x57（loc6/loc9 底框 47x47 居中填充成背景框）。
        // 相对场景根（不随切袋移动），由动态计算的原版袋列绝对位置换算。
        const int64_t row_y = tab_rel_y + index * 70;
        if (!extension_bag_ui_set_rect(ctrl, {tab_rel_x, row_y, 57, 57})) {
            VIRTBAG_LOG("extension tab setup failed reason=set_rect index=%d control=%p", index, ctrl);
            complete = false;
            break;
        }
        // data：16B（+0x0=item、+0x8..0xb=标志），装箱背包物品对象
        void* data = nullptr;
        if (!extension_bag_ui_allocate_control_data(0x10, &data) || data == nullptr) {
            VIRTBAG_LOG("extension tab setup failed reason=allocate_control_data index=%d control=%p",
                        index, ctrl);
            complete = false;
            break;
        }
        std::memset(data, 0, 0x10);
        *reinterpret_cast<void**>(data) = g_tab_bag_items[index];
        if (!extension_bag_ui_set_control_data(ctrl, data)) {
            VIRTBAG_LOG("extension tab setup failed reason=set_control_data index=%d control=%p",
                        index, ctrl);
            complete = false;
            break;
        }
    }
    if (extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer) != mount_parent) {
        // The game replaced this tree while controls were being created; its handles are stale.
        invalidate_extension_tab_buttons_locked();
        VIRTBAG_LOG("extension tab install aborted reason=root_replaced during_create root=%p",
                    mount_parent);
        return;
    }
    if (!complete) {
        (void)discard_created();
        return;
    }
    g_extension_tab_buttons = created;
    g_extension_tab_root = mount_parent;
    g_extension_tab_generation = g_inventory_generation;
    VIRTBAG_LOG("extension tabs installed generation=%llu root=%p",
                static_cast<unsigned long long>(g_inventory_generation), bag_container);
}

void disable_extension_tab_buttons_locked() {
    void* live_root = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer);
    if (g_extension_tab_root != nullptr && live_root == g_extension_tab_root) {
        delete_extension_tab_buttons_locked();
    } else if (g_extension_tab_root != nullptr) {
        VIRTBAG_LOG("extension tab references invalidated reason=root_not_live saved=%p live=%p",
                    g_extension_tab_root, live_root);
    }
    // The game owns controls; stale-root references are invalidated, never deleted.
    invalidate_extension_tab_buttons_locked();
}
bool extension_tab_control_is_current_locked(int index, void* control);
void handle_extension_tab_click_impl_locked(int extension_bag,
                                            std::unique_lock<std::mutex>* unlockable_lock);
bool extension_tab_buttons_complete_locked() {
    for (void* button : g_extension_tab_buttons) {
        if (button == nullptr) return false;
    }
    return true;
}

bool extension_tab_buttons_have_controls_locked() {
    for (void* button : g_extension_tab_buttons) {
        if (button != nullptr) return true;
    }
    return false;
}

void reset_extension_desc_unequip_hook_locked() {
    // 卸下按钮由函数级 ButtonUnequipExe hook 承担，
    // 仅清 desc 袋记录。
    g_extension_desc_unequip_bag = -1;
}

void install_extension_desc_unequip_hook_locked(int extension_bag) {
    reset_extension_desc_unequip_hook_locked();
    if (!virtual_bag::valid_index(extension_bag)) return;
    // 扩展袋 desc 打开时记录袋位，供函数级 ButtonUnequipExe hook
    // （virtual_bag_handle_original_bag_unequip）识别扩展袋解除路径。
    g_extension_desc_unequip_bag = extension_bag;
    VIRTBAG_LOG("extension desc unequip armed bag=%d", extension_bag);
}

// P3：扩展袋切换音效，与原版袋按钮同款（UIEquip_InvenBagControlEventProc b8c34：Play(0x11)）。
void play_extension_switch_sound() {
    if (g_game == nullptr || g_write_port->ui.play_switch_sound == nullptr) return;
    (void)g_write_port->ui.play_switch_sound();
}

void handle_extension_tab_click_locked(int extension_bag) {
    handle_extension_tab_click_impl_locked(extension_bag, nullptr);
}

void handle_pending_extension_tab_click_locked(std::unique_lock<std::mutex>& lock,
                                               int extension_bag) {
    handle_extension_tab_click_impl_locked(extension_bag, &lock);
}

void handle_extension_tab_click_impl_locked(int extension_bag,
                                            std::unique_lock<std::mutex>* unlockable_lock) {
    if (!virtual_bag::valid_index(extension_bag) ||
        !extension_bag_session_admitted_locked()) return;
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kOriginal) {
        const int original_bag = original_bag_locked();
        if (original_bag == virtual_bag::kOriginalTaskBag) {
            constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
            void* panel_root = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipItemRoot);
            const int target_capacity = g_virtual_bag_state.capacities[extension_bag];
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
            g_original_current_direct = static_cast<uint8_t>(projection_host);
            g_original_current_got = static_cast<uint8_t>(projection_host);
            if (!install_module_view_locked(extension_bag)) {
                set_original_bag_locked(original_bag);
                g_original_current_direct = static_cast<uint8_t>(original_bag);
                g_original_current_got = static_cast<uint8_t>(original_bag);
                VIRTBAG_LOG(
                    "extension tab reject reason=projection_host_unavailable source_original_bag=%d target=%d host=%d",
                    original_bag, extension_bag, projection_host);
                return;
            }
            if (virtual_bag::click(&g_virtual_bag_state, extension_bag) !=
                virtual_bag::ClickResult::kSelected) {
                restore_module_view_locked();
                set_original_bag_locked(original_bag);
                g_original_current_direct = static_cast<uint8_t>(original_bag);
                g_original_current_got = static_cast<uint8_t>(original_bag);
                VIRTBAG_LOG(
                    "extension tab reject reason=projection_host_unavailable source_original_bag=%d target=%d host=%d",
                    original_bag, extension_bag, projection_host);
                return;
            }
            persist_state_locked();
            VIRTBAG_LOG("extension tab selected bag=%d original_bag=%d projection_host=%d",
                        extension_bag, original_bag, projection_host);
            return;
        }
        if (virtual_bag::valid_original_transaction_bag(original_bag)) {
            g_original_current_direct = static_cast<uint8_t>(original_bag);
            g_original_current_got = static_cast<uint8_t>(original_bag);
            if (virtual_bag::click(&g_virtual_bag_state, extension_bag) !=
                virtual_bag::ClickResult::kIgnored) {
                install_module_view_locked(extension_bag);
                persist_state_locked();
                VIRTBAG_LOG("extension tab selected bag=%d original_bag=%d", extension_bag,
                            original_bag);
            }
        } else {
            VIRTBAG_LOG("extension tab reject original window reason=invalid_transaction_domain bag=%d",
                        original_bag);
        }
        return;
    }
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule) {
        if (g_virtual_bag_state.selected != extension_bag) {
            if (virtual_bag::click(&g_virtual_bag_state, extension_bag) !=
                virtual_bag::ClickResult::kIgnored) {
                install_module_view_locked(extension_bag);
                play_extension_switch_sound();
                VIRTBAG_LOG("extension tab switched bag=%d", extension_bag);
            }
            return;
        }
        // 二次点击 = 原版袋信息面板（原版语义：desc_type=1 + UIEquip_MakeDesc）。
        // MakeDesc 读控件物品（data[0]=袋物品对象）生成详情，SetDescMenu 按
        // desc_type=1 加解除按钮；关闭详情经 UIDesc_SetOff。
        if (virtual_bag::click(&g_virtual_bag_state, extension_bag) ==
            virtual_bag::ClickResult::kInspected) {
            play_extension_switch_sound();
            void* ctrl = g_extension_tab_buttons[extension_bag];
            if (ctrl != nullptr && g_game != nullptr &&
                g_write_port->ui.make_item_description != nullptr &&
                g_write_port->ui.set_current_bag != nullptr &&
                g_write_port->ui.set_desc_type != nullptr) {
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
                const uint8_t bag_type = g_virtual_bag_state.types[extension_bag];
                const int bag_capacity = g_virtual_bag_state.capacities[extension_bag];
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
                        g_write_port->ui.set_current_bag(1));
                    const bool desc_set = bag_set && qol::contracts::succeeded(
                        g_write_port->ui.set_desc_type(1));
                    const bool made = desc_set && qol::contracts::succeeded(
                        g_write_port->ui.make_item_description(ctrl));
                    const bool bag_restored = saved_bag < 0 || qol::contracts::succeeded(
                        g_write_port->ui.set_current_bag(saved_bag));
                    const bool desc_restored = saved_desc_type < 0 || qol::contracts::succeeded(
                        g_write_port->ui.set_desc_type(saved_desc_type));
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
                    const uint64_t generation = g_inventory_generation;
                    void* const root = g_extension_tab_root;
                    unlockable_lock->unlock();
                    const bool bag_set = qol::contracts::succeeded(
                        g_write_port->ui.set_current_bag(1));
                    const bool desc_set = bag_set && qol::contracts::succeeded(
                        g_write_port->ui.set_desc_type(1));
                    const bool made = desc_set && qol::contracts::succeeded(
                        g_write_port->ui.make_item_description(ctrl));
                    const bool bag_restored = saved_bag < 0 || qol::contracts::succeeded(
                        g_write_port->ui.set_current_bag(saved_bag));
                    const bool desc_restored = saved_desc_type < 0 || qol::contracts::succeeded(
                        g_write_port->ui.set_desc_type(saved_desc_type));
                    unlockable_lock->lock();
                    bool still_current = g_virtual_bag_enabled.load() &&
                        extension_bag_session_admitted_locked() &&
                        g_inventory_generation == generation &&
                        g_extension_tab_root == root &&
                        g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
                        g_virtual_bag_state.selected == extension_bag &&
                        g_virtual_bag_state.types[extension_bag] == bag_type &&
                        g_virtual_bag_state.capacities[extension_bag] == bag_capacity &&
                        g_tab_bag_items[extension_bag] == described_item &&
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
                            if (g_write_port->ui.description_set_off != nullptr) {
                                unlockable_lock->unlock();
                                closed = qol::contracts::succeeded(
                                    g_write_port->ui.description_set_off());
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
    if (!g_virtual_bag_enabled.load() || !extension_bag_session_admitted_locked() ||
        !virtual_bag::valid_index(extension_bag) ||
        !extension_tab_control_is_current_locked(
            extension_bag, g_extension_tab_buttons[static_cast<size_t>(extension_bag)]) ||
        g_extension_tab_generation != g_inventory_generation) {
        return;
    }
    if (g_pending_extension_tab == extension_bag &&
        g_pending_extension_tab_generation == g_inventory_generation) {
        return;
    }
    g_pending_extension_tab = extension_bag;
    g_pending_extension_tab_generation = g_inventory_generation;
    VIRTBAG_LOG("extension tab pending bag=%d generation=%llu", extension_bag,
                static_cast<unsigned long long>(g_inventory_generation));
}

void extension_tab_button_clicked(void* ctrl) {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    const int extension_bag = extension_tab_index(ctrl);
    queue_extension_tab_click_locked(extension_bag);
}

bool extension_tab_control_is_current_locked(int index, void* control) {
    if (!g_virtual_bag_enabled.load() || !extension_bag_session_admitted_locked() ||
        !virtual_bag::valid_index(index) || control == nullptr ||
        g_extension_tab_root == nullptr || g_extension_tab_buttons[index] != control ||
        g_extension_tab_generation != g_inventory_generation ||
        extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer) !=
            g_extension_tab_root) {
        return false;
    }
    int child_count = 0;
    if (!extension_bag_ui_child_count(g_extension_tab_root, &child_count)) return false;
    for (int child = 0; child < child_count; ++child) {
        if (extension_bag_ui_child_at(g_extension_tab_root, child) == control) {
            return valid_child_locked(g_extension_tab_root, child) == control;
        }
    }
    return false;
}
void commit_pending_extension_tab_locked(std::unique_lock<std::mutex>& lock) {
    if (!virtual_bag::valid_index(g_pending_extension_tab) ||
        !g_virtual_bag_enabled.load() || !extension_bag_session_admitted_locked() ||
        g_pending_extension_tab_generation != g_inventory_generation ||
        g_extension_tab_generation != g_inventory_generation ||
        !extension_tab_control_is_current_locked(
            g_pending_extension_tab,
            g_extension_tab_buttons[static_cast<size_t>(g_pending_extension_tab)])) {
        g_pending_extension_tab = -1;
        g_pending_extension_tab_generation = 0;
        return;
    }
    const int extension_bag = g_pending_extension_tab;
    g_pending_extension_tab = -1;
    g_pending_extension_tab_generation = 0;
    handle_pending_extension_tab_click_locked(lock, extension_bag);
}

bool extension_tab_hit(int index, int64_t x, int64_t y) {
    if (!virtual_bag::valid_index(index)) return false;
    if (g_virtual_bag_state.capacities[index] == 0) {
        // 未装备袋只有在当前拖动物品满足装备规则时可命中；否则仍保持
        // G-12 的不可命中门禁，避免普通物品误落到未装备标签。
        const int category = drag_source_category_locked();
        if (!is_backpack_category(category)) return false;
    }
    // 标签位置是动态换算的（tab_rel + 袋容器绝对），用控件真实绝对 rect 判断，
    // 不用旧硬编码 kCellX/kCellY（标签左移后二者偏移 36px 致命中失效）。
    void* ctrl = g_extension_tab_buttons[index];
    if (ctrl == nullptr) return false;
    UiRect rect{};
    return extension_bag_ui_rect(ctrl, &rect) && rect.w > 0 && rect.h > 0 &&
           extension_bag_ui_hit_test(ctrl, x, y, rect);
}
}  // namespace

#undef handle_extension_tab_click_locked
#undef delete_extension_tab_buttons_locked
#undef disable_extension_tab_buttons_locked
#undef install_extension_tab_buttons_locked
#undef extension_tab_buttons_complete_locked
#undef commit_pending_extension_tab_locked
#undef reset_extension_desc_unequip_hook_locked
#undef extension_tab_hit

void handle_extension_tab_click_locked(int extension_bag) {
    handle_extension_tab_click_locked_internal(extension_bag);
}
void extension_bag_ui_play_switch_sound() { play_extension_switch_sound(); }
void extension_bag_ui_handle_tab_click_locked(int extension_bag) {
    handle_extension_tab_click_locked(extension_bag);
}

bool extension_tab_hit(int index, int64_t x, int64_t y) {
    return extension_tab_hit_internal(index, x, y);
}
bool extension_bag_runtime_is_backpack_category(int category) {
    return is_backpack_category(category);
}
void delete_extension_tab_buttons_locked() {
    delete_extension_tab_buttons_locked_internal();
}
void disable_extension_tab_buttons_locked() {
    disable_extension_tab_buttons_locked_internal();
}
void install_extension_tab_buttons_locked() {
    install_extension_tab_buttons_locked_internal();
}
bool extension_tab_buttons_complete_locked() {
    return extension_tab_buttons_complete_locked_internal();
}
void reset_extension_desc_unequip_hook_locked() {
    reset_extension_desc_unequip_hook_locked_internal();
}
void commit_pending_extension_tab_locked(std::unique_lock<std::mutex>& lock) {
    commit_pending_extension_tab_locked_internal(lock);
}
