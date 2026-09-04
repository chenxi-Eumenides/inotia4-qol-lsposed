#include "game_ui_virtbag.h"

#include "game_access.h"
#include "game_inventory.h"
#include "game_ops_common.h"
#include "feature/patch/game_patch.h"
#include "game_ptr_hook.h"
#include "game_state.h"
#include "game_symbols.h"
#include "feature/extension_bag/model/ownership_ledger.h"
#include "core/native/stack_codec.h"
#include "core/native/stack_limit_port.h"
#include "feature/extension_bag/model/virtual_bag_state.h"
#include "core/native/extension_bag_port.h"
#include "core/native/module_save_port.h"
#include "feature/extension_bag/extension_bag_context.h"
#include "feature/extension_bag/extension_bag_geometry.h"
#include "feature/extension_bag/extension_bag_logging.h"
#include "feature/ui/game_ui_kit.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace {

constexpr size_t kPopupStateSize = 0x40;
constexpr int kPopupStateCount = 27;
// 触摸事件使用 Scene_Draw 的绝对逻辑坐标；真机运行时宽度为 1408。
constexpr int64_t kCellX = 0x4c4;
constexpr int64_t kCellY = 0x86;
constexpr int64_t kCellStepY = 0x46;
// Phase one uses an independent read-only overlay. These coordinates stay in
// the same absolute logical space as the original equipment scene.
// UIEquip_CreateInvenControl absolute origin: bag button origin minus the
// bag container offset plus the item-container offset.
constexpr int64_t kGridX = 0x45c - 0x185 + 0x22; // 761
constexpr int64_t kGridY = 0x91 - 0x0c + 0x1b;   // 160
constexpr int64_t kGridCell = 0x4a;
constexpr int64_t kGridStep = 0x53;
constexpr int64_t kOriginalGridX = kGridX;
constexpr int64_t kOriginalGridY = kGridY;
constexpr size_t kInventorySlotStride = 16;
constexpr uint8_t kNoOriginalBagSelected = 6;
// Extension tab ControlObject rects are relative to the item-list root, whose
// absolute origin is (kGridX, kGridY). Their visuals keep the existing overlay
// positions so this phase changes input ownership without moving the UI.
constexpr int64_t kExtensionTabX = kCellX - kGridX;
constexpr int64_t kExtensionTabY = kCellY - kGridY;
constexpr int64_t kExtensionTabWidth = 0x70;
constexpr int64_t kExtensionTabHeight = 0x30;

using PopupEventFn = uint64_t (*)(uint64_t, uint64_t, uint64_t);
using PopupNoArgFn = void (*)();
using OriginalDrawInvenBagFn = void (*)();
using OriginalDrawInvenItemFn = void (*)();

std::mutex g_virtual_bag_mtx;
std::atomic<bool> g_inject_thread_started{false};
std::atomic<bool> g_lifecycle_thread_started{false};
std::atomic<bool> g_virtual_bag_enabled{false};
std::atomic<uint64_t> g_exit_trace_sequence{0};
jclass g_virtual_bag_bridge_class = nullptr;
uint8_t* g_state_entry = nullptr;
PopupEventFn g_orig_event = nullptr;
PopupNoArgFn g_orig_f3 = nullptr;
PopupNoArgFn g_orig_enter = nullptr;
PopupNoArgFn g_orig_save_enter = nullptr;
uint8_t* g_save_state_entry = nullptr;
bool g_save_panel_hook_installed = false;
uintptr_t g_draw_patch_addr = 0;
uintptr_t g_bag_draw_patch_addr = 0;
uintptr_t g_save_inventory_patch_addr = 0;
void* g_save_inventory_thunk = nullptr;
std::array<uintptr_t, 8> g_save_callsite_patch_addrs{};
uintptr_t g_drop_gate_patch_addr = 0;
void* g_drop_gate_thunk = nullptr;
uintptr_t g_draw_gate_patch_addr = 0;
void* g_draw_gate_thunk = nullptr;
uintptr_t g_item_draw_patch_addr = 0;
uintptr_t g_desc_open_patch_addr = 0;
void* g_desc_open_thunk = nullptr;
void* g_draw_thunk = nullptr;
void* g_bag_draw_thunk = nullptr;
void* g_item_draw_thunk = nullptr;
virtual_bag::State g_virtual_bag_state{};
int g_loaded_slot = -2;

uint64_t isolation_now_ms_locked();
std::array<std::array<void*, virtual_bag::kSlotCount>, virtual_bag::kBagCount> g_module_objects{};
std::array<std::array<int, virtual_bag::kSlotCount>, virtual_bag::kBagCount> g_module_object_categories{};
std::array<std::array<uint32_t, virtual_bag::kSlotCount>, virtual_bag::kBagCount> g_module_object_hashes{};
std::array<void*, virtual_bag::kSlotCount> g_original_inventory{};
uint8_t g_original_current_direct = 0;
uint8_t g_original_current_got = 0;
uint32_t* g_original_bag_size_word = nullptr;
uint32_t g_original_bag_size = 0;
int g_module_window_original_bag = -1;
void* g_projected_item_root = nullptr;
bool g_module_view_installed = false;
int g_module_view_index = -1;
uint8_t g_exit_display_bag = kNoOriginalBagSelected;
bool g_inventory_frame_active = false;
bool g_item_state_dirty = false;
bool g_extension_touch_capture = false;
std::array<void*, virtual_bag::kBagCount> g_extension_tab_buttons{};
void* g_extension_tab_root = nullptr;
uint64_t g_inventory_generation = 0;
uint64_t g_extension_tab_generation = 0;
int g_pending_extension_tab = -1;
uint64_t g_pending_extension_tab_generation = 0;
PtrHook g_extension_desc_unequip_hook;
int g_extension_desc_unequip_bag = -1;
PtrHook g_extension_desc_equip_hook;
void* g_extension_desc_equip_item = nullptr;
std::array<PtrHook, 11> g_extension_desc_item_hooks{};
void* g_extension_desc_item = nullptr;
int g_extension_desc_item_bag = -1;
int g_extension_desc_item_slot = -1;
using ExtensionDestroyOkFn = void (*)();
ExtensionDestroyOkFn g_extension_destroy_original_ok = nullptr;

struct ExtensionDrag {
    bool active = false;
    uint8_t bag = 0;
    uint8_t slot = 0;
    int64_t press_x = 0;
    int64_t press_y = 0;
    int64_t current_x = 0;
    int64_t current_y = 0;
};

ExtensionDrag g_extension_drag{};
virtual_bag::ExtensionDragSession g_extension_drag_session{};

std::atomic<uint64_t> g_p5_observation_sequence{0};
std::atomic<uint64_t> g_p5_last_item_query_sample_ms{0};
std::atomic<uint64_t> g_p5_last_panel_move_sample_ms{0};
thread_local bool g_p5_observation_active = false;
void* g_tab_bag_items[virtual_bag::kBagCount] = {};

int extension_tab_index(void* ctrl);
bool module_slot_of_item_locked(void* item, int* out_bag, int* out_slot);

#include "feature/extension_bag/extension_bag_observation.inc"

void extension_tab_button_clicked(void* ctrl);

#include "feature/extension_bag/extension_bag_tabs.inc"

// 扩展标签控件事件处理：TouchHandle 转发的控件事件（0x02=click 松开确认，
// 0x04=drop 到袋）。挂袋容器后与原版袋标签同链：松开 → TouchHandle 判定。
void queue_extension_tab_click_locked(int extension_bag);
void* touch_moving_item_control_locked();
bool try_equip_on_extension_tab_drop_locked(int index, void* moving_control);
bool extension_source_should_equip_locked(int target_bag);
bool equip_extension_source_on_tab_locked(int target_bag, int source_bag, int source_slot);
bool move_extension_to_extension_locked(int src_bag, int src_slot, int dst_bag, int dst_slot);
bool persist_state_locked(bool force);
void refresh_projected_module_view_after_move_locked();
void cancel_projected_drag_session_locked(const char* reason);
bool route_projected_session_to_tab_locked(int target_bag);
int original_item_category_locked(void* item);
int drag_source_category_locked();
bool move_original_to_extension_locked(int dst_bag, void* moving_control);

#include "feature/extension_bag/extension_bag_drag_session.inc"

#include "feature/extension_bag/extension_bag_runtime.inc"
}  // namespace

#include "feature/extension_bag/extension_bag_public_runtime.inc"
namespace {

const char* extension_recovery_action_name_locked() {
    const virtual_bag::RecoveryAction action =
        virtual_bag::recovery_action(g_virtual_bag_state, g_virtual_bag_state.pending);
    if (action == virtual_bag::RecoveryAction::kComplete) return "complete";
    if (action == virtual_bag::RecoveryAction::kRollback) return "rollback";
    return "none";
}

std::string extension_bag_status_json_locked() {
    return "{\"enabled\":" + std::string(g_virtual_bag_enabled.load() ? "true" : "false") +
           ",\"injected\":" + std::string(g_state_entry != nullptr ? "true" : "false") +
           ",\"save_panel_hook\":" +
           std::string(g_save_panel_hook_installed ? "true" : "false") +
           ",\"extension_tab_button\":" +
           std::string(g_extension_tab_buttons[0] != nullptr ? "true" : "false") +
           ",\"inventory_frame_active\":" +
           std::string(g_inventory_frame_active ? "true" : "false") +
           ",\"recovery_action\":\"" + extension_recovery_action_name_locked() + "\"" +
           ",\"state\":" + virtual_bag::state_json(g_virtual_bag_state, true) + "}";
}

std::string extension_bag_not_ready_error_locked() {
    if (!g_virtual_bag_enabled.load()) return op_err("extension bag disabled");
    return op_err("not in game");
}

bool extension_bag_ready_locked() {
    return g_virtual_bag_enabled.load() && game_in_world();
}

int extension_internal_bag(int logical_bag) {
    return virtual_bag::extension_internal_bag(logical_bag);
}

std::string extension_bag_view_result_json(bool ok, const char* error) {
    if (!ok) return op_err(error);
    return "{\"ok\":true,\"state\":" + extension_bag_status_json_locked() + "}";
}

enum class ExtensionBagUnequipResult {
    kOk,
    kNotEquipped,
    kNotEmpty,
    kNoSpace,
    kPersistFailed,
    kFailed,
};

// P3 袋解除（对齐原版 UIEquip_ButtonUnequipExe desc_type=1 语义）：
// 非空 → kNotEmpty（弹窗 7，b8084）；原版袋 0..4 全满 → kNoSpace（弹窗 6，
// b809c）；成功 = 按袋类型重建 count-1 背包物品入库（源袋优先、显式跳过
// 任务袋 5/ADR-006）→ 清装备态 → 切到接收袋（原版 b804c-b805c 双字节写）。
// 顺序铁律：先预检空袋再建对象（原版同序 b7f94→b7fa8），否则非空路径会把
// 已入库物品滞留 g_inven 造成复制；转移成功后仅 persist 失败需回滚
// （INVEN_RemoveItemDirect 返回值不可信 → 槽位重读确认后再真释放）。
ExtensionBagUnequipResult unequip_extension_bag_locked(int internal_bag) {
    ensure_state_loaded_locked();
    if (g_virtual_bag_state.capacities[internal_bag] == 0) {
        return ExtensionBagUnequipResult::kNotEquipped;
    }
    for (const virtual_bag::Item& item : g_virtual_bag_state.items[internal_bag]) {
        if (item.category > 0 || item.count > 0) {
            return ExtensionBagUnequipResult::kNotEmpty;
        }
    }
    if (fn_create_item == nullptr || fn_inven_save_item_on_empty == nullptr) {
        VIRTBAG_LOG("extension unequip symbols unavailable bag=%d", internal_bag);
        return ExtensionBagUnequipResult::kFailed;
    }
    void* item = fn_create_item(
        static_cast<int32_t>(g_virtual_bag_state.types[internal_bag]), 0, 0, 0);
    if (item == nullptr) {
        VIRTBAG_LOG("extension unequip create item failed bag=%d", internal_bag);
        return ExtensionBagUnequipResult::kFailed;
    }
    // P4.3：全新对象立即入账本；分配失败立即真释放（尚未暴露，安全）。
    uint32_t item_handle = 0;
    if (ownership::allocate(&g_ownership_ledger, &item_handle) != ownership::Outcome::kOk) {
        release_temporary_item_locked(item);
        VIRTBAG_LOG("extension unequip ledger exhausted bag=%d", internal_bag);
        return ExtensionBagUnequipResult::kFailed;
    }
    uint32_t count_flags =
        *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT);
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT) =
        stack_codec::write_count(count_flags, 1);

    int preferred = original_bag_locked();
    if (preferred < 0 || preferred > 4) preferred = g_virtual_bag_state.original_selected;
    if (preferred < 0 || preferred > 4) preferred = 0;
    int order[5];
    int order_count = 0;
    order[order_count++] = preferred;
    for (int bag = 0; bag < 5; ++bag) {
        if (bag != preferred) order[order_count++] = bag;
    }
    int receiving_bag = -1;
    for (int i = 0; i < order_count && receiving_bag < 0; ++i) {
        if (fn_inven_save_item_on_empty(item, order[i])) receiving_bag = order[i];
    }
    if (receiving_bag < 0) {
        // 全满：全新对象从未暴露给控件/TouchState，账本终止保管并真释放。
        release_tracked_item_locked(item_handle, item, "unequip no space");
        return ExtensionBagUnequipResult::kNoSpace;
    }

    // 入库成功后 g_inven 拥有对象，模块不得再释放；预检后 unequip_bag 不会
    // 因非空失败。UI 恢复仍由 virtual_bag_draw_end_wrapper 下一帧完成。
    const uint8_t saved_type = g_virtual_bag_state.types[internal_bag];
    virtual_bag::unequip_bag(&g_virtual_bag_state, internal_bag);
    virtual_bag::enter_original(&g_virtual_bag_state, receiving_bag);
    set_original_bag_locked(receiving_bag);
    g_item_state_dirty = true;
    if (persist_state_locked()) {
        // P4.3：persist 成功 = 提交点，此刻才移交原版库存（终态）。
        handover_tracked_item_locked(item_handle, item, "unequip handover");
        VIRTBAG_LOG("extension unequip ok bag=%d type=%d receiving=%d", internal_bag,
                    static_cast<int>(saved_type), receiving_bag);
        return ExtensionBagUnequipResult::kOk;
    }
    bool rolled_back = false;
    if (fn_remove_item_direct != nullptr) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            void* slot_item = nullptr;
            if (!inventory_slot_locked(receiving_bag, slot, &slot_item) ||
                slot_item != item) {
                continue;
            }
            fn_remove_item_direct(receiving_bag, slot);
            void* after = item;
            if (inventory_slot_locked(receiving_bag, slot, &after) && after == nullptr) {
                rolled_back = true;
            }
            break;
        }
    }
    if (rolled_back) {
        // P4.3：回滚已确认（脱离 INVEN）→ 账本终止保管 + 真释放。
        release_tracked_item_locked(item_handle, item, "unequip rollback");
        g_virtual_bag_state.types[internal_bag] = saved_type;
        g_virtual_bag_state.isolation_now_ms = isolation_now_ms_locked();
        virtual_bag::normalize(&g_virtual_bag_state);
        persist_state_locked();
        VIRTBAG_LOG("extension unequip persist failed; rolled back bag=%d", internal_bag);
        return ExtensionBagUnequipResult::kPersistFailed;
    }
    // 回滚未确认：对象去向不明，按 handover 记账（inventory-owned 终态，审计可见）。
    handover_tracked_item_locked(item_handle, item, "unequip rollback uncertain");
    VIRTBAG_LOG("extension unequip rollback failed bag=%d receiving=%d", internal_bag,
                receiving_bag);
    return ExtensionBagUnequipResult::kFailed;
}

void show_extension_bag_not_empty_popup() {
    if (g_base == 0) return;
    const uintptr_t popup =
        g_base + fn_resolve("F_UI_POPUP_MSG_CREATE_OK_FROM_TEXT_DATA_VMA",
                            F_UI_POPUP_MSG_CREATE_OK_FROM_TEXT_DATA_VMA);
    if (popup == 0) return;
    reinterpret_cast<UiPopupMsgCreateOkFromTextDataFn>(popup)(7, 0, 0, 0);
}

void show_extension_bag_no_space_popup() {
    if (g_base == 0) return;
    const uintptr_t popup =
        g_base + fn_resolve("F_UI_POPUP_MSG_CREATE_OK_FROM_TEXT_DATA_VMA",
                            F_UI_POPUP_MSG_CREATE_OK_FROM_TEXT_DATA_VMA);
    if (popup == 0) return;
    reinterpret_cast<UiPopupMsgCreateOkFromTextDataFn>(popup)(6, 0, 0, 0);
}

// P3 真实装备（原版两装备入口的扩展对应物；拖放路径经扩展标签 drop，
// 装备按钮路径经 desc 按钮 hook 溢出）。事务顺序对齐解除侧的反向操作：
// 校验 → INVEN 定位源槽 → payload 备份 → RemoveItemDirect+重读确认
// （返回值不可信，control-plane v1.7）→ equip_bag 占位（容量由 normalize
// 派生）→ persist；失败回滚（清位 + 按备份 payload 重建入库）。
// P4.3：源物品在移除确认后入账本保管，提交/回滚均经 retire 终止保管
// （触摸窗口外真释放）；回滚重建对象经 tracked load + handover/release。
#include "feature/extension_bag/extension_bag_equip.inc"
}  // namespace

#include "feature/extension_bag/extension_bag_api_impl.inc"
