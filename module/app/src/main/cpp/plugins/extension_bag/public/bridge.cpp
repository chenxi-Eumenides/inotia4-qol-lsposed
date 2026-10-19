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

void* virtual_bag_item_at(int bag, int slot) {
    if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    ensure_state_loaded_locked();
    return module_item_locked(bag, slot);
}

void* virtual_bag_view_item_at(int bag, int slot) {
    // 仅当 bag 处于扩展视图（控件 index 与扩展槽 1:1 对应）时才物化兜底；
    // 原版视图下原版坐标读出的 null 就是真空槽，不得误物化成扩展物品。
    if (!runtime_state().objects.module_view_installed || runtime_state().objects.module_view_index != bag) return nullptr;
    if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    ensure_state_loaded_locked();
    return module_item_locked(bag, slot);
}
JNIEnv* extension_bag_current_env() {
    return current_env();
}

jclass extension_bag_bridge_class() {
    return runtime_state().persistence.bridge_class;
}

virtual_bag::State* extension_bag_state() {
    return &runtime_state().objects.bag;
}

uint64_t extension_bag_isolation_now_ms() {
    return isolation_now_ms_locked();
}
uint64_t virtual_bag_observe_item_proc_pre(void* control, uint64_t event, void* x2, void* param) {
    if (g_p5_observation_active || !p5_observation_enabled() ||
        !p5_should_begin_item_observation(event)) {
        return 0;
    }
    g_p5_observation_active = true;
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    const uint64_t observation_token =
        log_p5_observation_locked("item-pre", control, event, x2 != nullptr, param != nullptr,
                                  nullptr, false, 0);
    if (observation_token == 0) g_p5_observation_active = false;
    return observation_token;
}

void virtual_bag_observe_item_proc_source(uint64_t observation_token, void* control, uint64_t event,
                                          void* x2, void* param, void* source_control) {
    if (observation_token == 0) return;
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    log_p5_observation_locked("item-source", control, event, x2 != nullptr, param != nullptr,
                              source_control, false, 0, -1, -1, observation_token);
}

void virtual_bag_observe_item_proc_post(uint64_t observation_token, void* control, uint64_t event,
                                        void* x2, void* param, uint64_t result) {
    if (observation_token == 0) return;
    (void)control;
    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        // The original proc may have rebuilt the control tree. Do not dereference its prior control.
        log_p5_observation_locked("item-post", nullptr, event, x2 != nullptr, param != nullptr,
                                  nullptr, true, result, -1, -1, observation_token);
    }
    g_p5_observation_active = false;
}

bool virtual_bag_is_inventory_enter(uintptr_t enter) {
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    return runtime_state().persistence.inventory_enter != 0 && enter == runtime_state().persistence.inventory_enter;
}

bool virtual_bag_capture_move_item_observation(void* item, int target_bag, int target_slot,
                                               VirtualBagMoveItemObservation* out) {
    if (out == nullptr) return false;
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    *out = {};
    out->module_view_index = runtime_state().objects.module_view_index;
    out->drag_session_active = projected_drag_session_active_locked();
    out->extension_item = item != nullptr &&
                          module_slot_of_item_locked(item, &out->extension_bag,
                                                     &out->extension_slot);
    if (out->extension_item) {
        out->extension_handle =
            runtime_state().objects.module_object_handles[out->extension_bag][out->extension_slot];
        for (const ownership::State state : {ownership::State::kModuleOwned,
                                             ownership::State::kBorrowedForView,
                                             ownership::State::kInventoryOwned,
                                             ownership::State::kReleased}) {
            if (ownership::live_state(runtime_state().objects.ownership_ledger, out->extension_handle, state)) {
                out->ownership_state = static_cast<int>(state);
                break;
            }
        }
    }
    const PhysicalInventorySnapshot snapshot = capture_physical_inventory_snapshot_locked();
    out->physical_valid = snapshot.valid;
    out->physical_digest = snapshot.digest;
    out->physical_nonnull = snapshot.nonnull;
    for (size_t index = 0; index < snapshot.slots.size(); ++index) {
        void* slot_item = snapshot.slots[index];
        if (item != nullptr && slot_item == item && out->source_bag < 0) {
            out->source_bag = static_cast<int>(index / virtual_bag::kSlotCount);
            out->source_slot = static_cast<int>(index % virtual_bag::kSlotCount);
            out->source_ptr = slot_item;
        }
    }
    if (target_bag >= 0 && target_bag < 6 && target_slot >= 0 &&
        target_slot < virtual_bag::kSlotCount) {
        out->target_ptr = snapshot.slots[target_bag * virtual_bag::kSlotCount + target_slot];
    }
    return true;
}

int virtual_bag_inventory_state_id() {
    // g_state_entry is panel-local state; world lifecycle cleanup uses world_facts separately.
    // The explicit state access is equivalent to std::lock_guard<std::mutex> lock(g_virtual_bag_mtx),
    // g_state_entry == nullptr, and *reinterpret_cast<int32_t*>(g_state_entry).
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    if (runtime_state().persistence.state_entry == nullptr) return -1;
    return *reinterpret_cast<int32_t*>(runtime_state().persistence.state_entry);
}

bool set_virtual_bag_enabled(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        runtime_state().enabled.store(enabled);
        if (!enabled) {
            teardown_extension_bag_hosts_locked();
            runtime_state().objects.bag.mode = virtual_bag::Mode::kOriginal;
            runtime_state().objects.bag.selected = -1;
            runtime_state().objects.bag.inspected = -1;
            runtime_state().ui.extension_drag = {};
            runtime_state().ui.extension_touch_capture = false;
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
    return runtime_state().enabled.load();
}

bool virtual_bag_prepare_save_slot_load() {
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    if (!module_cache_clear_preflight_locked()) {
        VIRTBAG_LOG("prepare save-slot load preflight failed");
        return false;
    }
    if (runtime_state().objects.module_view_installed) restore_module_view_locked();
    if (!clear_module_cache_locked()) {
        VIRTBAG_LOG("prepare save-slot load blocked by active module object");
        return false;
    }
    runtime_state().objects.bag = {};
    runtime_state().persistence.loaded_slot = -2;
    disable_extension_tab_buttons_locked();
    runtime_state().ui.extension_tab_buttons.fill(nullptr);
    return true;
}

bool virtual_bag_prepare_main_menu() {
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    return prepare_main_menu_locked();
}

// 模块级保存 prepare 中扩展背包自己的校验部分。模块级协调在
// Host SDK 存档参与者的 prepare 阶段，各功能只负责自己数据的
// 校验与收敛；本函数即扩展背包在 prepare 中的唯一校验/清理落点。调用方必须已持有
// g_virtual_bag_mtx。
// 1) 归一化 types → capacities（派生容量的唯一来源）；
// 2) 剔除派生容量之外的槽位数据：超容数据不参与 sidecar 序列化、不落盘；
// 3) 容量外槽若仍持有物化对象或账本句柄（写入路径全部收紧后不可达），拒绝保存并记
//    ERROR，避免半持久化与悬挂引用。
bool virtual_bag_module_save_validate_locked() {
    virtual_bag::normalize(&runtime_state().objects.bag);
    int discarded = 0;
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        const int capacity = runtime_state().objects.bag.capacities[bag];
        for (int slot = capacity; slot < virtual_bag::kSlotCount; ++slot) {
            const virtual_bag::Item& item = runtime_state().objects.bag.items[bag][slot];
            if (item.category <= 0 && item.count <= 0) continue;
            if (runtime_state().objects.module_objects[bag][slot] != nullptr ||
                runtime_state().objects.module_object_handles[bag][slot] != 0) {
                QOL_LOG_ERROR(QolDomain::kExtensionBag,
                              "save validation reject: over-capacity slot holds live object bag=%d slot=%d",
                              bag, slot);
                return false;
            }
            ++discarded;
        }
    }
    if (discarded > 0) {
        virtual_bag::discard_over_capacity(&runtime_state().objects.bag);
        extension_bag_mark_dirty_locked();
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "save validation discarded %d over-capacity item(s)", discarded);
    }
    return true;
}

// P3c：SlotBound 写入统一经 save.slot_data 的 request_write（安全点排空），provider 在
// 原版保存事务内捕获提交；旧 ModuleSaveCoordinator 参与者序退役，本三函数删除。
// 扫描入口的准入拒绝见 virtual_bag_for_each_logical_item 与各写操作入口。
void virtual_bag_ui_register_bridge(JNIEnv* env, jclass bridge_class) {
    if (env == nullptr || bridge_class == nullptr) return;
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    if (runtime_state().persistence.bridge_class != nullptr) env->DeleteGlobalRef(runtime_state().persistence.bridge_class);
    runtime_state().persistence.bridge_class = static_cast<jclass>(env->NewGlobalRef(bridge_class));
    // P3c：旧 module_save 参与者注册已退出——保存协调唯一走 save.slot_data + core.save
    // 协调器；此处不得保留第二套参与者顺序。
}

void virtual_bag_for_each_logical_item(LogicalInventoryItemFn fn, void* ctx) {
    if (fn == nullptr || !runtime_state().enabled.load() || !read_world_in_world()) return;
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    ensure_state_loaded_locked();
    // P3c 准入：session 未加载/epoch 未登记/降级阻断时明确拒绝扫描（不产出任何物品，
    // 不再惰性 IO 兜底空状态）。
    if (!extension_bag_session_admitted_locked()) {
        VIRTBAG_LOG("logical item scan rejected: session not admitted");
        return;
    }
    // 输出模式视图数量（R-47 决策 b）：启用态 canonical、关闭态 b。占位判定
    // 仍按 descriptor canonical（结构性判断与模式无关）。
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        const int capacity = runtime_state().objects.bag.capacities[bag];
        for (int slot = 0; slot < capacity; ++slot) {
            const virtual_bag::Item& item = runtime_state().objects.bag.items[bag][slot];
            if (item.category <= 0 || item.count <= 0) continue;
            qol::contracts::QolItemHandle item_handle = qol::contracts::kInvalidItemHandle;
            void* native_item = runtime_state().objects.module_objects[bag][slot];
            if (native_item != nullptr && extension_bag_register_item_handle(native_item)) {
                (void)extension_bag_item_handle_for_pointer(native_item, &item_handle);
            }
            if (fn(virtual_bag::kExtensionLogicalBagFirst + bag, slot, item.category,
                   static_cast<int>(virtual_bag_effective_count(
                        runtime_state().objects.module_objects[bag][slot], static_cast<uint32_t>(item.count))),
                   item_handle, ctx)) {
                return;
            }
        }
    }
}

bool virtual_bag_logical_capacity(int32_t logical_bag, int32_t* out_capacity) {
    if (out_capacity == nullptr || !virtual_bag::valid_extension_logical_bag(logical_bag) ||
        !runtime_state().enabled.load() || !read_world_in_world()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    if (!extension_bag_session_admitted_locked()) return false;
    const int internal_bag = virtual_bag::extension_internal_bag(logical_bag);
    if (!virtual_bag::valid_index(internal_bag)) return false;
    *out_capacity = runtime_state().objects.bag.capacities[internal_bag];
    return true;
}

std::string virtual_bag_inventory_bags_json() {
    if (!runtime_state().enabled.load() || !read_world_in_world()) return "";
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    ensure_state_loaded_locked();
    std::string out;
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        const int capacity = runtime_state().objects.bag.capacities[bag];
        int filled = 0;
        std::string items;
        // 只遍历派生容量内的槽位：容量外不是有效槽（R-05），不参与 items 输出
        // 与 slot_count 统计。
        for (int slot = 0; slot < capacity && slot < virtual_bag::kSlotCount; ++slot) {
            const virtual_bag::Item& item = runtime_state().objects.bag.items[bag][slot];
            if (item.category <= 0 || item.count <= 0) continue;
            if (filled > 0) items += ",";
            // count 输出模式视图（R-47 决策 b）：启用态 canonical、关闭态 b。
            items += "{\"slot\":" + std::to_string(slot) +
                     ",\"category\":" + std::to_string(item.category) +
                     ",\"count\":" +
                     std::to_string(static_cast<int>(virtual_bag_effective_count(runtime_state().objects.module_objects[bag][slot], static_cast<uint32_t>(item.count)))) +
                     "}";
            ++filled;
        }
        out += ",{\"bag\":" + std::to_string(bag + 6) +
               ",\"items\":[" + items + "]" +
               ",\"capacity\":" + std::to_string(capacity) +
               ",\"slot_count\":" + std::to_string(filled) + "}";
    }
    return out;
}

std::string data_virtual_bag_test_equip(int index, int bag_type) {
    bool updated = false;
    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        if (!extension_bag_session_admitted_locked()) return op_err("slot data not ready");
        updated = virtual_bag::set_test_equipped(&runtime_state().objects.bag, index, bag_type);
        if (updated) extension_bag_mark_dirty_locked();
    }
    if (!updated) return op_err("bad virtual bag index or bag type");
    return op_ok();
}

std::string data_virtual_bag_test_item(int index, int slot, int category, int count) {
    bool updated = false;
    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        if (!extension_bag_session_admitted_locked()) return op_err("slot data not ready");
        if (!virtual_bag::valid_index(index) || slot < 0 || slot >= virtual_bag::kSlotCount ||
            category <= 0 || count <= 0) {
            return op_err("bad virtual bag item");
        }
        // Production items always carry a serialized payload. Without it the
        // projection cannot materialize a native object, so debug injection
        // would update status but leave the EB grid empty.
        void* native_item = extension_create_item(category, 0, 0, count);
        if (native_item == nullptr) return op_err("create item failed");
        std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
        int payload_size = 0;
        const auto* write_api = extension_bag_write_api();
        const bool serialized = write_api != nullptr && write_api->inventory.serialize_item != nullptr &&
                                qol::contracts::succeeded(write_api->inventory.serialize_item(
                                    payload.data(), static_cast<int32_t>(payload.size()),
                                    native_item, &payload_size)) &&
                                payload_size > 0;
        (void)extension_free_item(native_item);
        if (!serialized) return op_err("serialize item failed");
        updated = virtual_bag::set_item(&runtime_state().objects.bag, index, slot, category, count);
        if (updated) {
            virtual_bag::Item& descriptor = runtime_state().objects.bag.items[index][slot];
            descriptor.payload = payload;
            descriptor.payload_size = payload_size;
            extension_bag_mark_dirty_locked();
        }
    }
    if (!updated) return op_err("bad virtual bag item");
    return op_ok();
}
