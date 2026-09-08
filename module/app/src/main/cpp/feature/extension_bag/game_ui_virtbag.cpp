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
#include <functional>
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
std::atomic<bool> g_main_menu_cleanup_dirty{false};
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
struct ModuleUseState {
    bool active = false;
    bool consumed = false;
    bool pending_release = false;
    void* item = nullptr;
    uint64_t generation = 0;
    std::thread::id owner{};
};
struct ModuleUseToken {
    int bag = -1;
    int slot = -1;
    void* item = nullptr;
    uint64_t generation = 0;
    std::thread::id owner{};
};
std::array<std::array<ModuleUseState, virtual_bag::kSlotCount>, virtual_bag::kBagCount>
    g_module_object_use{};
thread_local ModuleUseToken* g_active_module_use_token = nullptr;
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
void* g_pending_jewel_detail_item = nullptr;
int g_pending_jewel_detail_bag = -1;
int g_pending_jewel_detail_slot = -1;
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
int g_extension_desc_unequip_bag = -1;
std::array<PtrHook, 11> g_extension_desc_item_hooks{};
void* g_extension_desc_item = nullptr;
int g_extension_desc_item_bag = -1;
int g_extension_desc_item_slot = -1;
using ExtensionDestroyOkFn = void (*)(void*);
ExtensionDestroyOkFn g_extension_destroy_original_ok = nullptr;
ExtensionDestroyOkFn g_extension_sell_original_ok = nullptr;
ExtensionDestroyOkFn g_extension_destroy_original_cancel = nullptr;
ExtensionDestroyOkFn g_extension_sell_original_cancel = nullptr;
bool g_extension_sell_apply_variant_discount = true;
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
bool module_use_begin_locked(int bag, int slot, void* item, ModuleUseToken* out_token);
bool module_use_finish_locked(const ModuleUseToken& token, bool* out_consumed);
bool module_use_abort_locked(const ModuleUseToken& token);
bool module_slot_is_assignable_locked(int bag, int slot);
bool module_object_replacement_allowed_locked(int bag, int slot, const char* context);
bool module_object_generation_advance_locked(int bag, int slot, const char* context);
bool module_object_release_allowed_locked(int bag, int slot, void* item, const char* context,
                                          bool allow_owner, const ModuleUseToken* token);
void store_teardown_locked();
bool install_store_hooks_locked();
bool store_hooks_installed();

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
void* load_item_payload_tracked_locked(const uint8_t* payload, int payload_size,
                                       const char* context, uint32_t* out_handle);
void handover_tracked_item_locked(uint32_t handle, void* item, const char* context);
void release_tracked_item_locked(uint32_t handle, void* item, const char* context);
bool consume_extension_item_after_native_locked(int bag, int slot, void* item,
                                                int before_count, int observed_before,
                                                const char* context, void* use_token);

#include "feature/extension_bag/extension_bag_drag_session.inc"

#include "feature/extension_bag/extension_bag_runtime.inc"
}  // namespace

bool virtual_bag_identify_native_item(void* item, int* out_bag, int* out_slot) {
    if (item == nullptr || out_bag == nullptr || out_slot == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    return module_slot_of_item_locked(item, out_bag, out_slot);
}

void* virtual_bag_item_at(int bag, int slot) {
    if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    return module_item_locked(bag, slot);
}

void* virtual_bag_view_item_at(int bag, int slot) {
    // 仅当 bag 处于扩展视图（控件 index 与扩展槽 1:1 对应）时才物化兜底；
    // 原版视图下原版坐标读出的 null 就是真空槽，不得误物化成扩展物品。
    if (!g_module_view_installed || g_module_view_index != bag) return nullptr;
    if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    return module_item_locked(bag, slot);
}

void* virtual_bag_find_native_item(int category) {
    if (category <= 0) return nullptr;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < g_virtual_bag_state.capacities[bag]; ++slot) {
            const virtual_bag::Item& descriptor = g_virtual_bag_state.items[bag][slot];
            if (descriptor.category != category || descriptor.count <= 0) continue;
            void* item = module_item_locked(bag, slot);
            if (item != nullptr) return item;
            VIRTBAG_LOG("native find extension materialize failed bag=%d slot=%d category=%d",
                        bag, slot, category);
        }
    }
    return nullptr;
}

bool virtual_bag_remove_native_item(void* item, void* raw_use_token) {
    if (item == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    int bag = -1;
    int slot = -1;
    if (!module_slot_of_item_locked(item, &bag, &slot) ||
        g_module_objects[bag][slot] != item) {
        return false;
    }
    ModuleUseToken* use_token = static_cast<ModuleUseToken*>(raw_use_token);
    if (!module_object_release_allowed_locked(
            bag, slot, item, "native remove", true, use_token)) {
        return false;
    }
    const virtual_bag::Item previous_item = g_virtual_bag_state.items[bag][slot];
    g_virtual_bag_state.items[bag][slot] = {};
    g_item_state_dirty = true;
    if (!free_module_object_locked(bag, slot, use_token)) {
        g_virtual_bag_state.items[bag][slot] = previous_item;
        VIRTBAG_LOG("native remove rejected object release bag=%d slot=%d", bag, slot);
        return false;
    }
    const bool persisted = persist_state_locked();
    if (g_module_view_installed && g_module_view_index == bag) {
        refresh_module_item_area_locked(bag);
    }
    VIRTBAG_LOG("native remove extension bag=%d slot=%d persisted=%d", bag, slot,
                persisted ? 1 : 0);
    return true;
}

bool virtual_bag_consume_native_item(void* item, void* raw_use_token) {
    if (item == nullptr) return false;
    void* detail_control = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        int bag = -1;
        int slot = -1;
        if (!module_slot_of_item_locked(item, &bag, &slot) || g_module_objects[bag][slot] != item) {
            return false;
        }
        ModuleUseToken* use_token = static_cast<ModuleUseToken*>(raw_use_token);
        if (!module_object_release_allowed_locked(
                bag, slot, item, "native consume", true,
                use_token)) {
            return false;
        }
        const int before_count = g_virtual_bag_state.items[bag][slot].count;
        const int observed_before = fn_get_cumulate_count != nullptr
            ? fn_get_cumulate_count(item) : before_count;
        if (!consume_extension_item_after_native_locked(
                bag, slot, item, before_count, observed_before, "native_consume", use_token)) {
            return false;
        }
        g_item_state_dirty = true;
        persist_state_locked();
        if (g_module_view_installed && g_module_view_index == bag) {
            refresh_module_item_area_locked(bag);
        }
        if (g_pending_jewel_detail_item != nullptr &&
            g_pending_jewel_detail_bag == g_module_view_index &&
            g_pending_jewel_detail_slot >= 0 &&
            g_pending_jewel_detail_slot < virtual_bag::kSlotCount &&
            g_module_view_installed && g_projected_item_root != nullptr &&
            fn_control_item_set_item != nullptr) {
            detail_control = valid_child_locked(g_projected_item_root,
                                                g_pending_jewel_detail_slot);
            if (detail_control != nullptr) {
                fn_control_item_set_item(detail_control, g_pending_jewel_detail_item);
            }
        }
        g_pending_jewel_detail_item = nullptr;
        g_pending_jewel_detail_bag = -1;
        g_pending_jewel_detail_slot = -1;
        VIRTBAG_LOG("native consume extension bag=%d slot=%d before=%d", bag, slot, before_count);
    }
    if (detail_control != nullptr && fn_ui_equip_make_desc != nullptr) {
        make_desc_equip_gate(detail_control, nullptr);
    }
    return true;
}

void* virtual_bag_current_use_token() {
    return g_active_module_use_token;
}

int virtual_bag_put_jewel_native(void* equip_item, void* jewel_item,
                                 VirtualBagPutJewelBackup backup) {
    if (equip_item == nullptr || jewel_item == nullptr || backup == nullptr) return 3;

    int equip_bag = -1;
    int equip_slot = -1;
    int jewel_bag = -1;
    int jewel_slot = -1;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        if (!module_slot_of_item_locked(equip_item, &equip_bag, &equip_slot) ||
            !module_slot_of_item_locked(jewel_item, &jewel_bag, &jewel_slot) ||
            g_module_objects[equip_bag][equip_slot] != equip_item ||
            g_module_objects[jewel_bag][jewel_slot] != jewel_item) {
            return 3;
        }
        if (g_virtual_bag_state.items[jewel_bag][jewel_slot].count <= 0) return 3;
    }

    const int result = backup(equip_item, jewel_item);
    if (result != 0) return result;

    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        if (g_module_objects[equip_bag][equip_slot] != equip_item ||
            g_module_objects[jewel_bag][jewel_slot] != jewel_item) {
            VIRTBAG_LOG("native put jewel payload sync skipped after native success");
            return 0;
        }
        g_pending_jewel_detail_item = equip_item;
        g_pending_jewel_detail_bag = equip_bag;
        g_pending_jewel_detail_slot = equip_slot;
        std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
        int payload_size = 0;
        if (!serialize_item_payload_locked(equip_item, &payload, &payload_size)) {
            VIRTBAG_LOG("native put jewel payload sync failed bag=%d slot=%d", equip_bag, equip_slot);
            return 0;
        }
        virtual_bag::Item& equip_descriptor = g_virtual_bag_state.items[equip_bag][equip_slot];
        equip_descriptor.payload = payload;
        equip_descriptor.payload_size = payload_size;
        equip_descriptor.count = fn_get_cumulate_count != nullptr
            ? fn_get_cumulate_count(equip_item) : equip_descriptor.count;
        g_module_object_hashes[equip_bag][equip_slot] = virtual_bag::payload_hash(equip_descriptor);
        g_item_state_dirty = true;
        persist_state_locked();
    }
    return 0;
}

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
    // 袋对象 +0x10 低 25 位是袋容量，ITEMSYSTEM_CreateItem（0x10be9c）已按
    // ITEMSTATICBASE[category]（1→4、2→8、3→12、4→16）自动写入。此后只可
    // 动 count 区（bit25..31，原版 ITEM_GetCumulateCount 同源），绝不可用
    // 模块 stack_codec::write_count（其 count 位从 bit22 起，与容量位段
    // bit0..24 重叠，会把容量破坏成巨值 → INVEN_GetBagSize 越界崩溃，真机
    // 实证）。这里把原版 count 区置 1（袋对象恒 1 份），容量区保持原值。
    {
        uint32_t flags = *reinterpret_cast<uint32_t*>(
            reinterpret_cast<uint8_t*>(item) + I_COUNT);
        constexpr uint32_t kOriginalCountBits = 0x7Fu << 25;  // bit25..31
        flags = (flags & ~kOriginalCountBits) | (1u << 25);
        *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT) = flags;
    }

    int preferred = original_bag_locked();
    if (preferred < 0 || preferred > 4) preferred = g_virtual_bag_state.original_selected;
    if (preferred < 0 || preferred > 4) preferred = 0;
    // 排除 internal_bag 自身：正在解除的袋不得作为接收方——袋对象若放回
    // 自己的行/槽，随后 unequip_bag 清袋会把刚放回的物品一起清掉
    // （真机实证：extension unequip adopt bag=N dst=N/0 后物品丢失/解除失败）。
    int order[5];
    int order_count = 0;
    for (int bag = 0; bag < 5; ++bag) {
        if (bag == internal_bag || bag == preferred) continue;
        order[order_count++] = bag;
    }
    if (preferred != internal_bag) {
        for (int i = order_count; i > 0; --i) order[i] = order[i - 1];
        order[0] = preferred;
        ++order_count;
    }
    int receiving_bag = -1;
    for (int i = 0; i < order_count && receiving_bag < 0; ++i) {
        if (fn_inven_save_item_on_empty(item, order[i])) receiving_bag = order[i];
    }
    if (receiving_bag < 0) {
        // 原版袋 0..4 全满：袋对象不回原版背包，转而收编为扩展袋物品槽的
        // 普通袋物品（与装备卸下收编 virtual_bag_adopt_unequipped_item 同
        // 模式）。收编后用户可把它拖回原版背包，或拖到空扩展标签重新装备。
        int adopt_bag = -1;
        int adopt_slot = -1;
        for (int bag = 0; bag < virtual_bag::kBagCount && adopt_bag < 0; ++bag) {
            if (bag == internal_bag) continue;  // 排除正在解除的袋（见上）
            if (g_virtual_bag_state.types[bag] == 0) continue;
            for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
                if (module_slot_is_assignable_locked(bag, slot)) {
                    adopt_bag = bag;
                    adopt_slot = slot;
                    break;
                }
            }
        }
        if (adopt_bag < 0) {
            // 原版背包与所有扩展袋物品槽都满：真无去处，释放并保持原版弹窗。
            release_tracked_item_locked(item_handle, item, "unequip no space");
            return ExtensionBagUnequipResult::kNoSpace;
        }
        std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
        int payload_size = 0;
        if (!serialize_item_payload_locked(item, &payload, &payload_size)) {
            release_tracked_item_locked(item_handle, item, "unequip adopt serialize");
            return ExtensionBagUnequipResult::kFailed;
        }
        virtual_bag::Item& descriptor = g_virtual_bag_state.items[adopt_bag][adopt_slot];
        descriptor.payload = payload;
        descriptor.payload_size = payload_size;
        descriptor.category = fn_get_bit != nullptr
            ? fn_get_bit(*reinterpret_cast<uint16_t*>(
                  reinterpret_cast<uint8_t*>(item) + I_TYPE), 15, 6)
            : 0;
        // 袋对象 count 恒为 1（本路径只处理袋物品）。不能用
        // fn_get_cumulate_count：CreateItem 后 +0x10 高位 count 区为 0，
        // 读得 0 会让收编袋显示为空槽（下次收编仍会选中它）。
        descriptor.count = 1;
        // 对象移交目标扩展槽保管（item_handle 早前已 allocate，沿用）。
        if (!module_object_generation_advance_locked(adopt_bag, adopt_slot, "unequip adopt")) {
            release_tracked_item_locked(item_handle, item, "unequip adopt slot protected");
            return ExtensionBagUnequipResult::kFailed;
        }
        g_module_objects[adopt_bag][adopt_slot] = item;
        g_module_object_categories[adopt_bag][adopt_slot] = descriptor.category;
        g_module_object_hashes[adopt_bag][adopt_slot] = virtual_bag::payload_hash(descriptor);
        g_module_object_handles[adopt_bag][adopt_slot] = item_handle;
        const uint8_t adopted_type = g_virtual_bag_state.types[internal_bag];
        virtual_bag::unequip_bag(&g_virtual_bag_state, internal_bag);
        g_item_state_dirty = true;
        if (persist_state_locked()) {
            if (g_module_view_installed && g_module_view_index == adopt_bag) {
                refresh_module_item_area_locked(adopt_bag);
            }
            VIRTBAG_LOG("extension unequip adopt bag=%d type=%d dst=%d/%d", internal_bag,
                        static_cast<int>(adopted_type), adopt_bag, adopt_slot);
            return ExtensionBagUnequipResult::kOk;
        }
        // persist 失败：回滚收编与袋装备态，对象真释放。
        if (!module_object_generation_advance_locked(adopt_bag, adopt_slot, "unequip rollback")) {
            VIRTBAG_LOG("extension unequip rollback blocked by active object bag=%d slot=%d",
                        adopt_bag, adopt_slot);
            return ExtensionBagUnequipResult::kPersistFailed;
        }
        g_module_objects[adopt_bag][adopt_slot] = nullptr;
        g_module_object_categories[adopt_bag][adopt_slot] = 0;
        g_module_object_hashes[adopt_bag][adopt_slot] = 0;
        g_module_object_handles[adopt_bag][adopt_slot] = 0;
        descriptor = virtual_bag::Item{};
        g_virtual_bag_state.types[internal_bag] = adopted_type;
        g_virtual_bag_state.isolation_now_ms = isolation_now_ms_locked();
        virtual_bag::normalize(&g_virtual_bag_state);
        release_tracked_item_locked(item_handle, item, "unequip adopt rollback");
        persist_state_locked();
        VIRTBAG_LOG("extension unequip adopt persist failed bag=%d dst=%d/%d", internal_bag,
                    adopt_bag, adopt_slot);
        return ExtensionBagUnequipResult::kPersistFailed;
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
#include "feature/extension_bag/extension_bag_store.inc"

}  // namespace

bool virtual_bag_handle_confirm_use_item(void* item) {
    return extension_confirm_use_item(item);
}

// 商店扩展视图会把窗口原版袋容量字临时放大到扩展容量；若玩家在此视图下
// 触发原版库存写入（买入→INVEN_SaveItem），FindSaveSlot 会按放大容量找
// 空槽，可能把物品写进超过真实容量的槽位（恢复后物品丢失/错乱）。该函数
// 由 SaveItem hook wrapper 在 backup 前调用：商店投影激活时先恢复投影
// （容量字还原），让原版按真实容量入库。
void virtual_bag_store_restore_for_original_write() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (store_view_installed) {
        VIRTBAG_LOG("store save gate: restore store module view before original write");
        store_restore_module_view_locked();
    }
}

bool virtual_bag_equip_projected_item(void* character, void* item, int source_bag, int source_slot,
                                      int equip_slot) {
    if (character == nullptr || item == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    if (!g_module_view_installed || g_module_view_index != source_bag) return false;
    if (fn_find_equip_slot == nullptr) return false;
    const int found = fn_find_equip_slot(character, item);
    if (found < 0 || found >= C_EQUIP_SLOTS) return false;
    // 原版在源槽读空时传 equip_slot=-1（FindEquipSlot(null) 的结果）；扩展
    // 物品不在 INVEN，不能把 -1 当校验失败，以真实物品计算的装备槽为准。
    if (equip_slot >= 0 && equip_slot < C_EQUIP_SLOTS && equip_slot != found) return false;
    return equip_module_item_on_character_locked(character, item, source_bag, source_slot);
}

bool virtual_bag_has_empty_slots(int needed, int include_task_bag) {
    if (needed <= 0) return true;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    int empty = 0;
    const int bag_count = include_task_bag != 0 ? virtual_bag::kBagCount : virtual_bag::kOriginalTransactionBagCount;
    for (int bag = 0; bag < bag_count; ++bag) {
        const int capacity = g_virtual_bag_state.capacities[bag];
        for (int slot = 0; slot < capacity && slot < virtual_bag::kSlotCount; ++slot) {
            if (module_slot_is_assignable_locked(bag, slot)) {
                ++empty;
                if (empty >= needed) return true;
            }
        }
    }
    return false;
}

bool virtual_bag_adopt_unequipped_item(void* character, int equip_slot) {
    if (character == nullptr || equip_slot < 0 || equip_slot >= C_EQUIP_SLOTS) return false;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    void* item = fn_get_equip_item != nullptr
        ? fn_get_equip_item(character, equip_slot) : nullptr;
    if (item == nullptr) return false;
    int existing_bag = -1;
    int existing_slot = -1;
    if (module_slot_of_item_locked(item, &existing_bag, &existing_slot)) return false;
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    if (!serialize_item_payload_locked(item, &payload, &payload_size)) return false;
    int dst_bag = -1;
    int dst_slot = -1;
    for (int bag = 0; bag < virtual_bag::kBagCount && dst_bag < 0; ++bag) {
        if (g_virtual_bag_state.types[bag] == 0) continue;
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            if (module_slot_is_assignable_locked(bag, slot)) {
                dst_bag = bag;
                dst_slot = slot;
                break;
            }
        }
    }
    if (dst_bag < 0) return false;
    virtual_bag::Item& descriptor = g_virtual_bag_state.items[dst_bag][dst_slot];
    descriptor.payload = payload;
    descriptor.payload_size = payload_size;
    descriptor.category = fn_get_bit != nullptr
        ? fn_get_bit(*reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE),
                     15, 6)
        : 0;
    descriptor.count = fn_get_cumulate_count != nullptr
        ? fn_get_cumulate_count(item) : 1;
    // 装备槽对象仍被原版 UI（装备槽 desc/控件缓存）引用，不能释放给对象池；
    // 直接收编为扩展槽的物化对象（与 equip_module_item_on_character_locked
    // 对旧装备的收编同规则）。
    if (!module_object_generation_advance_locked(dst_bag, dst_slot, "unequip replacement")) {
        VIRTBAG_LOG("native unequip adopt blocked by active object bag=%d slot=%d",
                    dst_bag, dst_slot);
        return false;
    }
    g_module_objects[dst_bag][dst_slot] = item;
    g_module_object_categories[dst_bag][dst_slot] = descriptor.category;
    g_module_object_hashes[dst_bag][dst_slot] = virtual_bag::payload_hash(descriptor);
    uint32_t adopted_handle = 0;
    if (ownership::allocate(&g_ownership_ledger, &adopted_handle) == ownership::Outcome::kOk) {
        g_module_object_handles[dst_bag][dst_slot] = adopted_handle;
    } else {
        g_module_object_handles[dst_bag][dst_slot] = 0;
        VIRTBAG_LOG("extension unequip adopt ledger exhausted bag=%d slot=%d",
                    dst_bag, dst_slot);
    }
    if (fn_set_equip_item != nullptr) fn_set_equip_item(character, equip_slot, nullptr);
    g_item_state_dirty = true;
    persist_state_locked();
    if (g_module_view_installed && g_module_view_index == dst_bag) {
        refresh_module_item_area_locked(dst_bag);
    }
    VIRTBAG_LOG("native unequip adopt equip_slot=%d bag=%d slot=%d",
                equip_slot, dst_bag, dst_slot);
    return true;
}

bool virtual_bag_adopt_native_item(void* item) {
    // INVEN_SaveItem（0x104528）无空位（FindSaveSlot 失败）时的扩展袋接管：
    // 把原版"新创建但无处可放"的物品收进扩展袋空位。被 hook 的 wrapper 在
    // backup 返回 0 后调用；返回 true 则 wrapper 上报成功，上层（任务奖励/
    // 事件发奖/开箱/拾取等）不会把物品掉地或静默释放。
    if (item == nullptr || !g_virtual_bag_enabled.load()) return false;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    int existing_bag = -1;
    int existing_slot = -1;
    if (module_slot_of_item_locked(item, &existing_bag, &existing_slot)) return false;
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    if (!serialize_item_payload_locked(item, &payload, &payload_size)) return false;
    int dst_bag = -1;
    int dst_slot = -1;
    for (int bag = 0; bag < virtual_bag::kBagCount && dst_bag < 0; ++bag) {
        if (g_virtual_bag_state.types[bag] == 0) continue;
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            if (module_slot_is_assignable_locked(bag, slot)) {
                dst_bag = bag;
                dst_slot = slot;
                break;
            }
        }
    }
    if (dst_bag < 0) return false;  // 扩展袋也满：如实上报失败（上层按原版语义处理）
    virtual_bag::Item& descriptor = g_virtual_bag_state.items[dst_bag][dst_slot];
    descriptor.payload = payload;
    descriptor.payload_size = payload_size;
    descriptor.category = fn_get_bit != nullptr
        ? fn_get_bit(*reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE),
                     15, 6)
        : 0;
    descriptor.count = fn_get_cumulate_count != nullptr
        ? fn_get_cumulate_count(item) : 1;
    if (descriptor.count <= 0) descriptor.count = 1;
    // 原版对象直接收编为扩展槽物化对象（SaveItem 失败对象未进库、无其他
    // 持有者；adopt 后生命周期归模块 g_module_objects 管理）。
    if (!module_object_generation_advance_locked(dst_bag, dst_slot, "native save adopt")) {
        VIRTBAG_LOG("native save adopt blocked by active object bag=%d slot=%d",
                    dst_bag, dst_slot);
        return false;
    }
    g_module_objects[dst_bag][dst_slot] = item;
    g_module_object_categories[dst_bag][dst_slot] = descriptor.category;
    g_module_object_hashes[dst_bag][dst_slot] = virtual_bag::payload_hash(descriptor);
    uint32_t adopted_handle = 0;
    if (ownership::allocate(&g_ownership_ledger, &adopted_handle) == ownership::Outcome::kOk) {
        g_module_object_handles[dst_bag][dst_slot] = adopted_handle;
    } else {
        g_module_object_handles[dst_bag][dst_slot] = 0;
        VIRTBAG_LOG("native save adopt ledger exhausted bag=%d slot=%d", dst_bag, dst_slot);
    }
    g_item_state_dirty = true;
    persist_state_locked();
    if (g_module_view_installed && g_module_view_index == dst_bag) {
        refresh_module_item_area_locked(dst_bag);
    }
    VIRTBAG_LOG("native save adopt bag=%d slot=%d item=%p category=%d count=%d", dst_bag,
                dst_slot, item, descriptor.category, descriptor.count);
    return true;
}

// ---- Path A'：装备按钮函数级接管（Stage4 第 10 hook 的扩展侧实现）----
// UIEquip_ButtonEquipExe（0xb7c18）被函数级 hook 后，wrapper 先进这里判定。
// 只接管"详情物品来自扩展槽的背包物品"：原版 BagEquipExe 的删源是内联
// 指令（b7e04 str xzr 写 INVEN[袋][槽]），扩展物品不在 INVEN，走原函数会
// 删空槽导致源不销毁 + 袋对象被袋表与 g_module_objects 双持有（点击崩
// 溃）。因此：
//   原版有空袋 → 扩展出库 + 袋表写入（b7dbc 同款：bag_table[bag]=item，
//                袋对象移交原版袋表）+ 刷新，完全不进原函数；
//   原版全满 → 扩展袋接管（equip_extension_bag_item_locked）；
//   扩展也满 → 交还原函数（原版自己弹 6 号"背包已满"）。
// 其他物品（原版物品、装备、宝石）一律交还原函数，保持原版流程。
// 定义在匿名 namespace 之外（port 跨 TU 调用）；依赖的本 TU 匿名 namespace
// 函数（category_is_extension_backpack 等）在 TU 内可见。
VirtualBagEquipButtonResult virtual_bag_handle_backpack_button_equip_result() {
    if (!g_virtual_bag_enabled.load() || !game_in_world()) {
        return VirtualBagEquipButtonResult::kNotExtension;
    }
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    void* item = g_extension_desc_item;
    // g_extension_desc_item 只在扩展物品详情被捕获；原版背包物品详情点装备
    // 时（原版 ButtonEquipExe 函数级 hook 覆盖所有详情），用原版 UIDesc_GetData
    // 读取当前 desc 物品（原版 ButtonEquipExe b7c2c 同源）。
    if (item == nullptr && fn_ui_desc_get_data != nullptr) {
        item = fn_ui_desc_get_data();
    }
    if (item == nullptr) return VirtualBagEquipButtonResult::kNotExtension;
    int src_bag = -1;
    int src_slot = -1;
    const bool from_extension = module_slot_of_item_locked(item, &src_bag, &src_slot);
    const uint16_t flags =
        *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
    const int category = fn_get_bit != nullptr ? fn_get_bit(flags, 15, 6) : -1;
    const bool jewel =
        category >= 0 && fn_is_jewel != nullptr && fn_is_jewel(category) != 0;
    if (jewel) return VirtualBagEquipButtonResult::kNotExtension;
    if (!category_is_extension_backpack(category)) {
        // 扩展槽装备物品 → 装备到角色（原详情按钮 PtrHook 职责，函数 hook 化；
        // 装备按钮 proc 恒为 ButtonEquipExe，见 native 第 10 hook）。
        if (from_extension && item_is_equip(item)) {
            if (!module_object_replacement_allowed_locked(src_bag, src_slot,
                                                          "character equip button")) {
                VIRTBAG_LOG("extension character equip button blocked bag=%d slot=%d",
                            src_bag, src_slot);
                return VirtualBagEquipButtonResult::kBlocked;
            }
            void* character = extension_menu_character();
            if (character != nullptr &&
                equip_module_item_on_character_locked(character, item, src_bag, src_slot)) {
                if (fn_ui_desc_set_off != nullptr) fn_ui_desc_set_off();
                clear_original_desc_locked();
                finish_extension_equip_ui_locked(src_bag, nullptr);
                return VirtualBagEquipButtonResult::kHandled;
            }
            VIRTBAG_LOG("extension character equip button failed bag=%d slot=%d", src_bag,
                        src_slot);
        }
        return VirtualBagEquipButtonResult::kNotExtension;  // 其余（原版源装备/其他）交还原函数
    }

    int found = -1;
    for (int bag = 1; bag <= 4; ++bag) {
        if (fn_get_bag_size != nullptr && fn_get_bag_size(bag) == 0) {
            found = bag;
            break;
        }
    }
    if (found >= 0) {
        // 原版背包源物品 + 原版有空袋：源在原版 INVEN，直接交还原版流程装袋
        // （原版自己写袋表 + 删源 + 刷新），模块不介入。
        if (!from_extension) return VirtualBagEquipButtonResult::kNotExtension;
        if (!module_object_replacement_allowed_locked(src_bag, src_slot,
                                                      "backpack equip to original")) {
            VIRTBAG_LOG("extension backpack equip to original blocked bag=%d slot=%d",
                        src_bag, src_slot);
            return VirtualBagEquipButtonResult::kBlocked;
        }
        if (g_base == 0) return VirtualBagEquipButtonResult::kNotExtension;
        void** bag_table = *reinterpret_cast<void***>(g_base + G_BAG_TABLE_VMA);
        if (bag_table == nullptr) {
            VIRTBAG_LOG("extension backpack fn-equip bag table missing");
            return VirtualBagEquipButtonResult::kBlocked;
        }
        if (!module_object_generation_advance_locked(src_bag, src_slot,
                                                     "unequip to original")) {
            VIRTBAG_LOG("extension unequip to original blocked by active object bag=%d slot=%d",
                        src_bag, src_slot);
            return VirtualBagEquipButtonResult::kBlocked;
        }
        bag_table[found] = item;
        virtual_bag::Item& descriptor = g_virtual_bag_state.items[src_bag][src_slot];
        descriptor = virtual_bag::Item{};
        g_module_objects[src_bag][src_slot] = nullptr;
        g_module_object_categories[src_bag][src_slot] = 0;
        g_module_object_hashes[src_bag][src_slot] = 0;
        const uint32_t handle = g_module_object_handles[src_bag][src_slot];
        g_module_object_handles[src_bag][src_slot] = 0;
        if (handle != 0) {
            // 对象可能仍处于扩展视图借用态（控件引用），先归还再移交，
            // 否则 handover 会因 live borrow 被拒而推迟，账本与袋表状态分裂。
            if (ownership::live_state(g_ownership_ledger, handle,
                                      ownership::State::kBorrowedForView)) {
                ownership::return_from_view(&g_ownership_ledger, handle);
            }
            handover_tracked_item_locked(handle, item, "backpack fn-equip original");
        }
        g_item_state_dirty = true;
        persist_state_locked();
        if (fn_ui_desc_set_off != nullptr) fn_ui_desc_set_off();
        if (fn_ui_equip_refresh_item_area != nullptr) fn_ui_equip_refresh_item_area();
        if (fn_ui_equip_refresh_bag_area != nullptr) fn_ui_equip_refresh_bag_area();
        clear_original_desc_locked();
        if (g_module_view_installed && g_module_view_index == src_bag) {
            refresh_module_item_area_locked(src_bag);
        }
        VIRTBAG_LOG("extension backpack fn-equip original bag=%d src=%d/%d item=%p",
                    found, src_bag, src_slot, item);
        return VirtualBagEquipButtonResult::kHandled;
    }

    // 原版袋全满 → 扩展接管。equip_extension_bag_item_locked 同时支持扩展槽
    // 源与"原版 INVEN 源"（内部先扫原版库存定位物品再移除，源物品真实销毁）。
    const int target = first_empty_extension_bag_locked();
    if (target < 0) return VirtualBagEquipButtonResult::kNotExtension;
    const ExtensionBagEquipResult result = equip_extension_bag_item_locked(target, item);
    if (result != ExtensionBagEquipResult::kOk) return VirtualBagEquipButtonResult::kNotExtension;
    if (fn_ui_desc_set_off != nullptr) fn_ui_desc_set_off();
    clear_original_desc_locked();
    finish_extension_equip_ui_locked(target, nullptr);
    VIRTBAG_LOG("extension backpack fn-equip extension target=%d src=%d/%d", target,
                src_bag, src_slot);
    return VirtualBagEquipButtonResult::kHandled;
}

bool virtual_bag_handle_backpack_button_equip() {
    return virtual_bag_handle_backpack_button_equip_result() ==
           VirtualBagEquipButtonResult::kHandled;
}

// ---- Path A'2：卸袋按钮函数级接管（Stage4 第 11 hook 的扩展侧实现）----
// UIEquip_ButtonUnequipExe desc_type=1 覆盖两类袋解除：
//   · 扩展袋（mode=kModule，desc 由扩展标签二次点击打开）：走扩展解除
//     unequip_extension_bag_locked（原扩展袋卸下按钮 PtrHook 的职责，
//     现已函数 hook 化；g_extension_desc_unequip_bag 记录袋位）。
//   · 原版袋（原版视图）：纯原版下有个边角缺陷——先 INVEN_FindSaveSlot 找
//     空槽、后清袋表[N]；FindSaveSlot 在清袋表前运行会把"即将腾空的自身
//     袋行 N"当成可用位选中，SaveItem 却在清袋表之后执行，袋对象落进容量
//     已归零的行 N → 不可见=物品消失（原版背包满时必现）。接管语义：
//       袋内容非空 → 弹"袋非空"（袋保持）；
//       原版其他袋有空位 → 放回原版（排除自身行 N）+ 清袋表[N]；
//       原版满 → 收编扩展袋空位；
//       扩展也满 → 弹"背包已满"，袋保持装备态（物品绝不消失）。
bool virtual_bag_handle_original_bag_unequip(bool* out_no_space, bool* out_not_empty) {
    if (out_no_space != nullptr) *out_no_space = false;
    if (out_not_empty != nullptr) *out_not_empty = false;
    if (!g_virtual_bag_enabled.load() || !game_in_world()) return false;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    if (g_base == 0) return false;
    // 仅 desc_type=1（袋详情）卸下走接管；卸装备（desc_type=0）交原版（其
    // 收编由 CHAR_UnequipItemToInven 第 9 hook 覆盖）。
    const uint8_t desc_type = *reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_DESC_TYPE_VMA);
    if (desc_type != 1) return false;
    // 扩展视图激活：desc 必为扩展袋（原版袋 desc 只能在原版视图触发，二次
    // 点击扩展标签打开）。识别 g_extension_desc_unequip_bag 走扩展解除。
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule) {
        const int ext_bag = g_extension_desc_unequip_bag;
        const bool owns_desc = virtual_bag::valid_index(ext_bag) &&
                               g_virtual_bag_state.selected == ext_bag &&
                               g_virtual_bag_state.info_bag == ext_bag;
        if (owns_desc) {
            const ExtensionBagUnequipResult result = unequip_extension_bag_locked(ext_bag);
            VIRTBAG_LOG("extension desc unequip bag=%d result=%d", ext_bag,
                        static_cast<int>(result));
            if (result == ExtensionBagUnequipResult::kNotEmpty && out_not_empty != nullptr) {
                *out_not_empty = true;
                return true;
            }
            if (result == ExtensionBagUnequipResult::kNoSpace && out_no_space != nullptr) {
                *out_no_space = true;
                return true;
            }
            return result == ExtensionBagUnequipResult::kOk;
        }
        // 扩展态但 desc 袋记录失效：保守拦截（不交原版误卸原版袋）。
        VIRTBAG_LOG("extension desc unequip stale rejected ext_bag=%d", ext_bag);
        return true;
    }
    // 被卸袋位=当前袋：desc_type=1 恒为二次点击当前袋标签产生（原版
    // InvenBagControlEventProc N==current）。不能用 UIDesc_GetData/扫袋表
    // 反查——原版卸袋按钮回调拿的是袋标签控件 GetItem（data[0]），desc 面板
    // 数据在卸袋详情不可靠（真机实证：扩展也满时袋仍被解除+物品消失）。
    int bag_index = -1;
    if (original_bag_locked() < kNoOriginalBagSelected) {
        bag_index = original_bag_locked();
    } else {
        uint8_t** current_bag =
            reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
        if (current_bag != nullptr && *current_bag != nullptr &&
            **current_bag < kNoOriginalBagSelected) {
            bag_index = **current_bag;
        }
    }
    if (bag_index < 0 || bag_index >= 6) return false;  // 无有效袋位 → 交原版
    void** bag_table = *reinterpret_cast<void***>(g_base + G_BAG_TABLE_VMA);
    if (bag_table == nullptr) return false;
    void* item = bag_table[bag_index];
    if (item == nullptr) return false;  // 该袋位无袋对象（空位详情等）→ 交原版
    if (fn_is_empty_bag != nullptr && !fn_is_empty_bag(bag_index)) {
        // 袋非空：模块接管弹"袋非空"（袋保持安全），不交原版。
        if (out_not_empty != nullptr) *out_not_empty = true;
        return true;
    }
    // 放回原版背包（排除自身袋行，防再次落入容量归零行）。
    int receiving_bag = -1;
    for (int bag = 0; bag < 5; ++bag) {
        if (bag == bag_index) continue;
        if (fn_inven_save_item_on_empty != nullptr && fn_inven_save_item_on_empty(item, bag)) {
            receiving_bag = bag;
            break;
        }
    }
    if (receiving_bag >= 0) {
        bag_table[bag_index] = nullptr;
        g_item_state_dirty = true;
        persist_state_locked();
        if (fn_ui_desc_set_off != nullptr) fn_ui_desc_set_off();
        if (fn_ui_equip_refresh_item_area != nullptr) fn_ui_equip_refresh_item_area();
        if (fn_ui_equip_refresh_bag_area != nullptr) fn_ui_equip_refresh_bag_area();
        clear_original_desc_locked();
        VIRTBAG_LOG("original bag unequip->original bag=%d dst=%d item=%p", bag_index,
                    receiving_bag, item);
        return true;
    }
    // 原版背包满：收编扩展袋空位（serialize 后对象移交扩展槽，袋表清空）。
    int adopt_bag = -1;
    int adopt_slot = -1;
    for (int bag = 0; bag < virtual_bag::kBagCount && adopt_bag < 0; ++bag) {
        if (g_virtual_bag_state.types[bag] == 0) continue;
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            if (module_slot_is_assignable_locked(bag, slot)) {
                adopt_bag = bag;
                adopt_slot = slot;
                break;
            }
        }
    }
    if (adopt_bag < 0) {
        // 扩展也满：置标志由 wrapper 解锁后弹"背包已满"，袋保持装备态，
        // 物品不消失（此处绝不弹窗：锁内零 UI）。
        if (out_no_space != nullptr) *out_no_space = true;
        VIRTBAG_LOG("original bag unequip reject no space bag=%d item=%p", bag_index, item);
        return true;
    }
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    if (!serialize_item_payload_locked(item, &payload, &payload_size)) {
        VIRTBAG_LOG("original bag unequip serialize failed bag=%d", bag_index);
        return false;  // 序列化失败极罕见：交原版，宁可原版语义也不凭空丢
    }
    virtual_bag::Item& descriptor = g_virtual_bag_state.items[adopt_bag][adopt_slot];
    descriptor.payload = payload;
    descriptor.payload_size = payload_size;
    descriptor.category = fn_get_bit != nullptr
        ? fn_get_bit(*reinterpret_cast<uint16_t*>(
              reinterpret_cast<uint8_t*>(item) + I_TYPE), 15, 6)
        : 0;
    descriptor.count = 1;
    uint32_t adopted_handle = 0;
    if (ownership::allocate(&g_ownership_ledger, &adopted_handle) == ownership::Outcome::kOk) {
        g_module_object_handles[adopt_bag][adopt_slot] = adopted_handle;
    } else {
        g_module_object_handles[adopt_bag][adopt_slot] = 0;
        VIRTBAG_LOG("original bag unequip ledger exhausted bag=%d", bag_index);
    }
    if (!module_object_generation_advance_locked(
            adopt_bag, adopt_slot, "original bag unequip adopt")) {
        VIRTBAG_LOG("original bag unequip adopt blocked by active object bag=%d slot=%d",
                    adopt_bag, adopt_slot);
        return false;
    }
    g_module_objects[adopt_bag][adopt_slot] = item;
    g_module_object_categories[adopt_bag][adopt_slot] = descriptor.category;
    g_module_object_hashes[adopt_bag][adopt_slot] = virtual_bag::payload_hash(descriptor);
    bag_table[bag_index] = nullptr;
    g_item_state_dirty = true;
    persist_state_locked();
    if (fn_ui_desc_set_off != nullptr) fn_ui_desc_set_off();
    clear_original_desc_locked();
    if (g_module_view_installed && g_module_view_index == adopt_bag) {
        refresh_module_item_area_locked(adopt_bag);
    } else if (fn_ui_equip_refresh_item_area != nullptr) {
        fn_ui_equip_refresh_item_area();
    }
    if (fn_ui_equip_refresh_bag_area != nullptr) fn_ui_equip_refresh_bag_area();
    VIRTBAG_LOG("original bag unequip->extension bag=%d dst=%d/%d item=%p", bag_index,
                adopt_bag, adopt_slot, item);
    return true;
}

#include "feature/extension_bag/extension_bag_api_impl.inc"
