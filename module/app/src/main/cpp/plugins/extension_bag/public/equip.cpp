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

int virtual_bag_put_jewel_native(void* equip_item, void* jewel_item,
                                 VirtualBagPutJewelBackup backup) {
    if (equip_item == nullptr || jewel_item == nullptr || backup == nullptr) return 3;

    int equip_bag = -1;
    int equip_slot = -1;
    int jewel_bag = -1;
    int jewel_slot = -1;
    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        ensure_state_loaded_locked();
        if (!module_slot_of_item_locked(equip_item, &equip_bag, &equip_slot) ||
            !module_slot_of_item_locked(jewel_item, &jewel_bag, &jewel_slot) ||
            runtime_state().objects.module_objects[equip_bag][equip_slot] != equip_item ||
            runtime_state().objects.module_objects[jewel_bag][jewel_slot] != jewel_item) {
            return 3;
        }
        if (runtime_state().objects.bag.items[jewel_bag][jewel_slot].count <= 0) return 3;
    }

    const int result = backup(equip_item, jewel_item);
    if (result != 0) return result;

    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        ensure_state_loaded_locked();
        if (runtime_state().objects.module_objects[equip_bag][equip_slot] != equip_item ||
            runtime_state().objects.module_objects[jewel_bag][jewel_slot] != jewel_item) {
            VIRTBAG_LOG("native put jewel payload sync skipped after native success");
            return 0;
        }
        runtime_state().ui.pending_jewel_detail_item = equip_item;
        runtime_state().ui.pending_jewel_detail_bag = equip_bag;
        runtime_state().ui.pending_jewel_detail_slot = equip_slot;
        std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
        int payload_size = 0;
        if (!serialize_item_payload_locked(equip_item, &payload, &payload_size)) {
            VIRTBAG_LOG("native put jewel payload sync failed bag=%d slot=%d", equip_bag, equip_slot);
            return 0;
        }
        virtual_bag::Item& equip_descriptor = runtime_state().objects.bag.items[equip_bag][equip_slot];
        equip_descriptor.payload = payload;
        equip_descriptor.payload_size = payload_size;
        // descriptor.count 恒为 canonical（R-38/R-48；getter 关闭态只返回 b 视图）。
        int equip_count = 0;
        if (read_item_cumulate_count(equip_item, &equip_count)) {
            equip_descriptor.count = static_cast<int>(virtual_bag_canonical_count(equip_item, 1));
        }
        runtime_state().objects.module_object_hashes[equip_bag][equip_slot] = virtual_bag::payload_hash(equip_descriptor);
        extension_bag_mark_dirty_locked();
        persist_state_locked();
    }
    return 0;
}

// P3 真实装备（原版两装备入口的扩展对应物；拖放路径经扩展标签 drop，
// 装备按钮路径经 desc 按钮 hook 溢出）。事务顺序对齐解除侧的反向操作：
// 校验 → INVEN 定位源槽 → payload 备份 → RemoveItemDirect+重读确认
// （返回值不可信，control-plane v1.7）→ equip_bag 占位（容量由 normalize
// 派生）→ persist；失败回滚（清位 + 按备份 payload 重建入库）。
// P4.3：源物品在移除确认后入账本保管，提交/回滚均经 retire 终止保管
// （触摸窗口外真释放）；回滚重建对象经 tracked load + handover/release。
bool virtual_bag_equip_projected_item(void* character, void* item, int source_bag, int source_slot,
                                      int equip_slot) {
    if (character == nullptr || item == nullptr) return false;
    std::unique_lock<std::mutex> lock(runtime_state().mtx);
    if (!extension_bag_session_admitted_locked()) return false;
    if (!runtime_state().objects.module_view_installed || runtime_state().objects.module_view_index != source_bag) return false;
    if (runtime_state().host.game == nullptr || runtime_state().host.character_extras == nullptr || runtime_state().host.character_extras->read.find_equip_slot == nullptr) return false;
    int found = -1;
    if (!qol::contracts::succeeded(
            runtime_state().host.character_extras->read.find_equip_slot(character, item, &found))) {
        return false;
    }
    if (found < 0 || found >= kEquipSlotCount) return false;
    // 原版在源槽读空时传 equip_slot=-1（FindEquipSlot(null) 的结果）；扩展
    // 物品不在 INVEN，不能把 -1 当校验失败，以真实物品计算的装备槽为准。
    if (equip_slot >= 0 && equip_slot < kEquipSlotCount && equip_slot != found) return false;
    return equip_module_item_on_character_locked(lock, character, item, source_bag, source_slot);
}

bool virtual_bag_has_empty_slots(int needed, int include_task_bag) {
    if (needed <= 0) return true;
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    ensure_state_loaded_locked();
    int empty = 0;
    const int bag_count = include_task_bag != 0 ? virtual_bag::kBagCount : virtual_bag::kOriginalTransactionBagCount;
    for (int bag = 0; bag < bag_count; ++bag) {
        const int capacity = runtime_state().objects.bag.capacities[bag];
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
    if (character == nullptr || equip_slot < 0 || equip_slot >= kEquipSlotCount) return false;
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    if (!extension_bag_session_admitted_locked()) return false;
    if (runtime_state().host.game == nullptr || runtime_state().host.character_extras == nullptr || runtime_state().host.character_extras->read.equipped_item == nullptr) return false;
    void* item = nullptr;
    if (!qol::contracts::succeeded(
            runtime_state().host.character_extras->read.equipped_item(character, equip_slot, &item))) {
        return false;
    }
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
        if (runtime_state().objects.bag.types[bag] == 0) continue;
        const int capacity = runtime_state().objects.bag.capacities[bag];
        for (int slot = 0; slot < capacity; ++slot) {
            if (module_slot_is_assignable_locked(bag, slot)) {
                dst_bag = bag;
                dst_slot = slot;
                break;
            }
        }
    }
    if (dst_bag < 0) return false;
    virtual_bag::Item& descriptor = runtime_state().objects.bag.items[dst_bag][dst_slot];
    descriptor.payload = payload;
    descriptor.payload_size = payload_size;
    (void)read_item_category(item, &descriptor.category);
    // descriptor.count 恒为 canonical（R-38/R-48；getter 关闭态只返回 b 视图）：
    // 卸下收编的物品数量位保留完整 128a+b。
    descriptor.count = 1;
    int item_count = 0;
    if (read_item_cumulate_count(item, &item_count)) {
        descriptor.count = static_cast<int>(virtual_bag_canonical_count(item, 1));
    }
    // 装备槽对象仍被原版 UI（装备槽 desc/控件缓存）引用，不能释放给对象池；
    // 直接收编为扩展槽的物化对象（与 equip_module_item_on_character_locked
    // 对旧装备的收编同规则）。
    if (!module_object_generation_advance_locked(dst_bag, dst_slot, "unequip replacement")) {
        VIRTBAG_LOG("native unequip adopt blocked by active object bag=%d slot=%d",
                    dst_bag, dst_slot);
        return false;
    }
    runtime_state().objects.module_objects[dst_bag][dst_slot] = item;
    runtime_state().objects.module_object_categories[dst_bag][dst_slot] = descriptor.category;
    runtime_state().objects.module_object_hashes[dst_bag][dst_slot] = virtual_bag::payload_hash(descriptor);
    uint32_t adopted_handle = 0;
    if (ownership::allocate(&runtime_state().objects.ownership_ledger, &adopted_handle) == ownership::Outcome::kOk) {
        runtime_state().objects.module_object_handles[dst_bag][dst_slot] = adopted_handle;
    } else {
        runtime_state().objects.module_object_handles[dst_bag][dst_slot] = 0;
        VIRTBAG_LOG("extension unequip adopt ledger exhausted bag=%d slot=%d",
                    dst_bag, dst_slot);
    }
    if (runtime_state().host.game->character_write.set_equipped_item == nullptr ||
        !qol::contracts::succeeded(
            runtime_state().host.game->character_write.set_equipped_item(character, equip_slot, nullptr))) {
        return false;
    }
    if (!extension_bag_rebind_item_handle(item)) {
        VIRTBAG_LOG("native unequip adopt handle registration failed bag=%d slot=%d",
                    dst_bag, dst_slot);
        return false;
    }
    extension_bag_mark_dirty_locked();
    persist_state_locked();
    if (runtime_state().objects.module_view_installed && runtime_state().objects.module_view_index == dst_bag) {
        refresh_module_item_area_locked(dst_bag);
    }
    VIRTBAG_LOG("native unequip adopt equip_slot=%d bag=%d slot=%d",
                equip_slot, dst_bag, dst_slot);
    return true;
}

namespace {
// ---- 入库落位单一 owner（R-53）----
// SaveItem 漏斗的两条入口（backup 前合并 / backup 失败后收编）共用同一落位驱动：
// 先并入扩展袋既有同类堆（adopt_merge_into 判据），否则（仅 allow_new_slot）新建
// 扩展空槽。owner 内部取 g_virtual_bag_mtx，只做副作用（对象写回/descriptor/hash/
// persist/refresh/free），不调用被 Hook 原版（仅 defer_item_free_locked/
// fn_itempool_free），满足 R-44。original-first 由调用方 wrapper 保证：本 owner
// 不触碰原版 backup。
enum class VirtualBagPlaceOutcome : uint8_t {
    kNotApplicable,  // null/禁用/门控不符/已在模块槽：调用方保持原版
    kMerged,         // 并入既有扩展同类堆
    kPlacedNewSlot,  // 新建扩展槽（仅 allow_new_slot）
    kNoSpace,        // 无合并目标且无空槽
    kRejected,       // serialize/generation 拒绝（失败路径已回滚）
};

struct VirtualBagPlaceRequest {
    void* item = nullptr;
    bool allow_new_slot = false;  // merge=false, adopt=true
    bool require_world = false;   // merge=true, adopt=false
};

VirtualBagPlaceOutcome virtual_bag_place_native_item(const VirtualBagPlaceRequest& req) {
    if (req.item == nullptr || !runtime_state().enabled.load()) {
        return VirtualBagPlaceOutcome::kNotApplicable;
    }
    if (req.require_world && !read_world_in_world()) {
        return VirtualBagPlaceOutcome::kNotApplicable;
    }
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    if (!extension_bag_session_admitted_locked()) return VirtualBagPlaceOutcome::kNotApplicable;
    int holder_bag = -1;
    int holder_slot = -1;
    if (module_slot_of_item_locked(req.item, &holder_bag, &holder_slot)) {
        return VirtualBagPlaceOutcome::kNotApplicable;
    }
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    if (!serialize_item_payload_locked(req.item, &payload, &payload_size)) {
        return VirtualBagPlaceOutcome::kRejected;
    }
    int category = 0;
    (void)read_item_category(req.item, &category);
    // descriptor.count 恒为 canonical（R-38/R-48）：getter 关闭态只返回 b 视图，
    // 必须按 S2 解码保存完整 128a+b，否则 sidecar 读档会把 a 段对齐抹掉。
    int canonical_count = static_cast<int>(virtual_bag_canonical_count(req.item, 1));
    if (canonical_count <= 0) canonical_count = 1;
    const bool limit_enabled = virtual_bag_extended_mode(req.item);
    virtual_bag::Item source{};
    source.category = category;
    source.count = canonical_count;
    source.payload = payload;
    source.payload_size = payload_size;

    const virtual_bag::PlacePlan plan = virtual_bag::place_plan(
        runtime_state().objects.bag.items, runtime_state().objects.bag.types,
        runtime_state().objects.bag.capacities, source, req.allow_new_slot, limit_enabled,
        virtual_bag_category_uses_stack_count, &module_slot_is_assignable_locked);

    if (plan.outcome == virtual_bag::PlaceOutcome::kMerged) {
        const int bag = plan.bag;
        const int slot = plan.slot;
        const virtual_bag::Item& merged = plan.merged;
        // 物化对象同步：已有堆对象（若已物化）数量位按模式视图写回，保持
        // 对象 +0x10 与 descriptor canonical 一致（非 count-encoded 在
        // adopt_merge_into 的 mergeable_items 门控内已拒绝，R-46）。
        void* materialized = runtime_state().objects.module_objects[bag][slot];
        if (materialized != nullptr) {
            const uint32_t merged_view = virtual_bag_view_from_canonical(
                static_cast<uint32_t>(merged.count), virtual_bag_extended_mode(materialized));
            (void)virtual_bag_write_effective(materialized, merged_view);
        }
        runtime_state().objects.bag.items[bag][slot] = merged;
        runtime_state().objects.module_object_hashes[bag][slot] = virtual_bag::payload_hash(merged);
        extension_bag_mark_dirty_locked();
        persist_state_locked();
        if (runtime_state().objects.module_view_installed && runtime_state().objects.module_view_index == bag) {
            refresh_module_item_area_locked(bag);
        }
        // 收编对象未进库、无其他持有者（调用方前置门控），并入后释放回池。
        if (!defer_item_free_locked(req.item)) {
            if (runtime_state().host.game != nullptr && runtime_state().host.write_port->inventory.free_item != nullptr) {
                (void)runtime_state().host.write_port->inventory.free_item(req.item);
            }
        }
        VIRTBAG_LOG("native save place merge bag=%d slot=%d category=%d count=%d->%d item=%p",
                    bag, slot, category, canonical_count, merged.count, req.item);
        return VirtualBagPlaceOutcome::kMerged;
    }
    if (plan.outcome == virtual_bag::PlaceOutcome::kPlacedNewSlot) {
        const int bag = plan.bag;
        const int slot = plan.slot;
        virtual_bag::Item& descriptor = runtime_state().objects.bag.items[bag][slot];
        const virtual_bag::Item previous_descriptor = descriptor;
        descriptor = virtual_bag::apply_new_slot_descriptor(previous_descriptor, source, true);
        // 原版对象直接收编为扩展槽物化对象（SaveItem 失败对象未进库、无其他
        // 持有者；落位后生命周期归模块 g_module_objects 管理）。generation 校验
        // 失败必须回滚 descriptor，避免 descriptor 与 g_module_objects 分叉。
        if (!module_object_generation_advance_locked(bag, slot, "native save place")) {
            descriptor =
                virtual_bag::apply_new_slot_descriptor(previous_descriptor, source, false);
            VIRTBAG_LOG("native save place blocked by active object bag=%d slot=%d", bag, slot);
            return VirtualBagPlaceOutcome::kRejected;
        }
        if (!extension_bag_rebind_item_handle(req.item)) {
            descriptor =
                virtual_bag::apply_new_slot_descriptor(previous_descriptor, source, false);
            VIRTBAG_LOG("native save place handle registration failed bag=%d slot=%d", bag, slot);
            return VirtualBagPlaceOutcome::kRejected;
        }
        runtime_state().objects.module_objects[bag][slot] = req.item;
        runtime_state().objects.module_object_categories[bag][slot] = descriptor.category;
        runtime_state().objects.module_object_hashes[bag][slot] = virtual_bag::payload_hash(descriptor);
        uint32_t adopted_handle = 0;
        if (ownership::allocate(&runtime_state().objects.ownership_ledger, &adopted_handle) ==
            ownership::Outcome::kOk) {
            runtime_state().objects.module_object_handles[bag][slot] = adopted_handle;
        } else {
            runtime_state().objects.module_object_handles[bag][slot] = 0;
            VIRTBAG_LOG("native save place ledger exhausted bag=%d slot=%d", bag, slot);
        }
        extension_bag_mark_dirty_locked();
        persist_state_locked();
        if (runtime_state().objects.module_view_installed && runtime_state().objects.module_view_index == bag) {
            refresh_module_item_area_locked(bag);
        }
        VIRTBAG_LOG("native save place new bag=%d slot=%d item=%p category=%d count=%d",
                    bag, slot, req.item, descriptor.category, descriptor.count);
        return VirtualBagPlaceOutcome::kPlacedNewSlot;
    }
    return plan.outcome == virtual_bag::PlaceOutcome::kRejected
               ? VirtualBagPlaceOutcome::kRejected
               : VirtualBagPlaceOutcome::kNoSpace;
}
}  // namespace

bool virtual_bag_merge_native_item(void* item) {
    // INVEN_SaveItem backup 前的扩展同类堆合并（VM-39 拾取并入扩展堆）：
    // original-first——仅当扩展袋已有可合并堆（allow_new_slot=false）时并入并
    // 返回 true；找不到则返回 false 让原版按真实容量入库。薄包装 owner。
    const VirtualBagPlaceRequest req{item, /*allow_new_slot=*/false, /*require_world=*/true};
    return virtual_bag_place_native_item(req) == VirtualBagPlaceOutcome::kMerged;
}

bool virtual_bag_adopt_native_item(void* item) {
    // INVEN_SaveItem（0x104528）无空位（FindSaveSlot 失败）时的扩展袋接管：
    // 先并入既有同类堆，否则新建扩展空槽（allow_new_slot=true）。返回 true 则
    // wrapper 上报成功，上层（任务奖励/事件发奖/开箱/拾取等）不会把物品掉地或
    // 静默释放。薄包装 owner。
    const VirtualBagPlaceRequest req{item, /*allow_new_slot=*/true, /*require_world=*/false};
    const VirtualBagPlaceOutcome outcome = virtual_bag_place_native_item(req);
    return outcome == VirtualBagPlaceOutcome::kMerged ||
           outcome == VirtualBagPlaceOutcome::kPlacedNewSlot;
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
//   扩展也满 → 已识别扩展源时阻断，原版源才交还原函数显示原版错误。
// 非扩展物品仍交还原函数，保持原版流程。
// 定义在匿名 namespace 之外（port 跨 TU 调用）；依赖的本 TU 匿名 namespace
// 函数（category_is_extension_backpack 等）在 TU 内可见。
VirtualBagEquipButtonResult virtual_bag_handle_backpack_button_equip_result() {
    if (!runtime_state().enabled.load() || !read_world_in_world()) {
        return VirtualBagEquipButtonResult::kNotExtension;
    }
    std::unique_lock<std::mutex> lock(runtime_state().mtx);
    ensure_state_loaded_locked();
    void* item = runtime_state().ui.extension_desc_item;
    // g_extension_desc_item 只在扩展物品详情被捕获；原版背包物品详情点装备
    // 时（原版 ButtonEquipExe 函数级 hook 覆盖所有详情），用原版 UIDesc_GetData
    // 读取当前 desc 物品（原版 ButtonEquipExe b7c2c 同源）。
    if (item == nullptr && runtime_state().host.game != nullptr && runtime_state().host.inventory_extras != nullptr &&
        runtime_state().host.inventory_extras->read.detail_item != nullptr) {
        (void)runtime_state().host.inventory_extras->read.detail_item(&item);
    }
    if (item == nullptr) return VirtualBagEquipButtonResult::kNotExtension;
    int src_bag = -1;
    int src_slot = -1;
    const bool from_extension = module_slot_of_item_locked(item, &src_bag, &src_slot);
    int category = -1;
    const bool category_read = read_item_category(item, &category);
    const bool jewel = category_read && read_item_is_jewel(category);
    if (jewel) {
        return from_extension ? VirtualBagEquipButtonResult::kBlocked
                              : VirtualBagEquipButtonResult::kNotExtension;
    }
    if (!category_is_extension_backpack(category)) {
        // 扩展身份已确认后，角色装备未能完整完成时不运行原版删源路径。
        // 扩展槽装备物品 → 装备到角色（原详情按钮职责，函数 hook 化；
        // 装备按钮 proc 恒为 ButtonEquipExe，见 native 第 10 hook）。
        const ::qol::contracts::QolItemCountEncoding item_encoding = item_count_encoding(item);
        if (from_extension && item_encoding == ::qol::contracts::QolItemCountEncoding::kUnknown) {
            VIRTBAG_LOG("extension equip button blocked: category unavailable bag=%d slot=%d",
                        src_bag, src_slot);
            return VirtualBagEquipButtonResult::kBlocked;
        }
        if (from_extension && item_encoding == ::qol::contracts::QolItemCountEncoding::kNotEncoded) {
            if (!module_object_replacement_allowed_locked(src_bag, src_slot,
                                                          "character equip button")) {
                VIRTBAG_LOG("extension character equip button blocked bag=%d slot=%d",
                            src_bag, src_slot);
                return VirtualBagEquipButtonResult::kBlocked;
            }
            void* character = extension_menu_character();
            if (character != nullptr &&
                equip_module_item_on_character_locked(lock, character, item, src_bag, src_slot)) {
                extension_description_set_off();
                clear_original_desc_locked();
                finish_extension_equip_ui_locked(src_bag, nullptr);
                return VirtualBagEquipButtonResult::kHandled;
            }
            VIRTBAG_LOG("extension character equip button failed bag=%d slot=%d", src_bag,
                        src_slot);
            return VirtualBagEquipButtonResult::kBlocked;
        }
        return from_extension ? VirtualBagEquipButtonResult::kBlocked
                              : VirtualBagEquipButtonResult::kNotExtension;
    }

    int found = -1;
    for (int bag = 1; bag <= 4; ++bag) {
        int32_t bag_size = -1;
        if (runtime_state().host.game != nullptr && runtime_state().host.inventory_extras != nullptr &&
            runtime_state().host.inventory_extras->read.bag_size != nullptr &&
            qol::contracts::succeeded(runtime_state().host.inventory_extras->read.bag_size(bag, &bag_size)) &&
            bag_size == 0) {
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
        if (runtime_state().host.game == nullptr || runtime_state().host.write_port == nullptr ||
            runtime_state().host.write_port->ui.set_original_bag_item == nullptr) {
            VIRTBAG_LOG("extension backpack fn-equip bag table missing");
            return VirtualBagEquipButtonResult::kBlocked;
        }
        const ModuleUseState source_use = runtime_state().objects.module_object_use[src_bag][src_slot];
        if (!module_object_generation_advance_locked(src_bag, src_slot,
                                                     "unequip to original")) {
            VIRTBAG_LOG("extension unequip to original blocked by active object bag=%d slot=%d",
                        src_bag, src_slot);
            return VirtualBagEquipButtonResult::kBlocked;
        }
        // 袋表接收对象成功后才允许清理扩展源及交接 ownership。
        const QolResult bag_write = runtime_state().host.write_port->ui.set_original_bag_item(found, item);
        if (!qol::contracts::succeeded(bag_write)) {
            runtime_state().objects.module_object_use[src_bag][src_slot] = source_use;
            VIRTBAG_LOG_ERROR("ERROR extension backpack equip original bag write failed bag=%d src=%d/%d item=%p code=%d; source retained",
                              found, src_bag, src_slot, item, bag_write.code);
            return VirtualBagEquipButtonResult::kBlocked;
        }
        virtual_bag::Item& descriptor = runtime_state().objects.bag.items[src_bag][src_slot];
        descriptor = virtual_bag::Item{};
        runtime_state().objects.module_objects[src_bag][src_slot] = nullptr;
        runtime_state().objects.module_object_categories[src_bag][src_slot] = 0;
        runtime_state().objects.module_object_hashes[src_bag][src_slot] = 0;
        const uint32_t handle = runtime_state().objects.module_object_handles[src_bag][src_slot];
        runtime_state().objects.module_object_handles[src_bag][src_slot] = 0;
        if (handle != 0) {
            // 对象可能仍处于扩展视图借用态（控件引用），先归还再移交，
            // 否则 handover 会因 live borrow 被拒而推迟，账本与袋表状态分裂。
            if (ownership::live_state(runtime_state().objects.ownership_ledger, handle,
                                      ownership::State::kBorrowedForView)) {
                ownership::return_from_view(&runtime_state().objects.ownership_ledger, handle);
            }
            handover_tracked_item_locked(handle, item, "backpack fn-equip original");
        }
        extension_bag_mark_dirty_locked();
        persist_state_locked();
        extension_description_set_off();
        virtual_bag_call_original_refresh_item_area_with_guard();
        extension_refresh_bag_area();
        clear_original_desc_locked();
        if (runtime_state().objects.module_view_installed && runtime_state().objects.module_view_index == src_bag) {
            refresh_module_item_area_locked(src_bag);
        }
        VIRTBAG_LOG("extension backpack fn-equip original bag=%d src=%d/%d item=%p",
                    found, src_bag, src_slot, item);
        return VirtualBagEquipButtonResult::kHandled;
    }

    // 原版袋全满 → 扩展接管。equip_extension_bag_item_locked 同时支持扩展槽
    // 源与"原版 INVEN 源"（内部先扫原版库存定位物品再移除，源物品真实销毁）。
    const int target = first_empty_extension_bag_locked();
    if (target < 0) {
        return from_extension ? VirtualBagEquipButtonResult::kBlocked
                              : VirtualBagEquipButtonResult::kNotExtension;
    }
    const ExtensionBagEquipResult result = equip_extension_bag_item_locked(lock, target, item);
    if (result != ExtensionBagEquipResult::kOk) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "extension backpack equip transaction failed result=%d target=%d source_extension=%d",
                         static_cast<int>(result), target, from_extension ? 1 : 0);
        return VirtualBagEquipButtonResult::kBlocked;
    }
    extension_description_set_off();
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
//     unequip_extension_bag_locked（原扩展袋卸下按钮的职责，
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
    if (!runtime_state().enabled.load() || !read_world_in_world()) return false;
    std::unique_lock<std::mutex> lock(runtime_state().mtx);
    ensure_state_loaded_locked();
    // 仅 desc_type=1（袋详情）卸下走接管；卸装备（desc_type=0）交原版（其
    // 收编由 CHAR_UnequipItemToInven 第 9 hook 覆盖）。
    int32_t desc_type = 0;
    if (runtime_state().host.game == nullptr || runtime_state().host.write_port->ui.desc_type == nullptr ||
        !qol::contracts::succeeded(runtime_state().host.write_port->ui.desc_type(&desc_type)) ||
        desc_type != 1) return false;
    // 扩展视图激活：desc 必为扩展袋（原版袋 desc 只能在原版视图触发，二次
    // 点击扩展标签打开）。识别 g_extension_desc_unequip_bag 走扩展解除。
    if (runtime_state().objects.bag.mode == virtual_bag::Mode::kModule) {
        const int ext_bag = runtime_state().ui.extension_desc_unequip_bag;
        const bool owns_desc = virtual_bag::valid_index(ext_bag) &&
                               runtime_state().objects.bag.selected == ext_bag &&
                               runtime_state().objects.bag.info_bag == ext_bag;
        if (owns_desc) {
            const ExtensionBagUnequipResult result = unequip_extension_bag_locked(lock, ext_bag);
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
            // 已确认详情来自扩展袋时，失败也必须阻断原版 backup，避免原版卸下
            // 一个并不在物理袋表中的扩展对象。
            return true;
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
        int32_t current_bag = -1;
        if (runtime_state().host.write_port->ui.current_bag != nullptr &&
            qol::contracts::succeeded(
                runtime_state().host.write_port->ui.current_bag(&current_bag)) &&
            current_bag < kNoOriginalBagSelected) {
            bag_index = current_bag;
        }
    }
    if (bag_index < 0 || bag_index >= 6) return false;  // 无有效袋位 → 交原版
    void* item = nullptr;
    if (runtime_state().host.write_port->ui.original_bag_item == nullptr ||
        !qol::contracts::succeeded(
            runtime_state().host.write_port->ui.original_bag_item(bag_index, &item))) return false;
    if (item == nullptr) return false;  // 该袋位无袋对象（空位详情等）→ 交原版
    int32_t empty_bag = 0;
    if (runtime_state().host.inventory_extras == nullptr || runtime_state().host.inventory_extras->read.is_empty_bag == nullptr ||
        !qol::contracts::succeeded(
            runtime_state().host.inventory_extras->read.is_empty_bag(bag_index, &empty_bag)) ||
        empty_bag == 0) {
        // 袋非空：模块接管弹"袋非空"（袋保持安全），不交原版。
        if (out_not_empty != nullptr) *out_not_empty = true;
        return true;
    }
    // 放回原版背包（排除自身袋行，防再次落入容量归零行）。
    int receiving_bag = -1;
    for (int bag = 0; bag < 5; ++bag) {
        if (bag == bag_index) continue;
        if (extension_save_item_on_empty(item, bag)) {
            receiving_bag = bag;
            break;
        }
    }
    if (receiving_bag >= 0) {
        (void)runtime_state().host.write_port->ui.set_original_bag_item(bag_index, nullptr);
        extension_bag_mark_dirty_locked();
        persist_state_locked();
        extension_description_set_off();
        virtual_bag_call_original_refresh_item_area_with_guard();
        extension_refresh_bag_area();
        clear_original_desc_locked();
        VIRTBAG_LOG("original bag unequip->original bag=%d dst=%d item=%p", bag_index,
                    receiving_bag, item);
        return true;
    }
    // 原版背包满：收编扩展袋空位（serialize 后对象移交扩展槽，袋表清空）。
    int adopt_bag = -1;
    int adopt_slot = -1;
    for (int bag = 0; bag < virtual_bag::kBagCount && adopt_bag < 0; ++bag) {
        if (runtime_state().objects.bag.types[bag] == 0) continue;
        const int capacity = runtime_state().objects.bag.capacities[bag];
        for (int slot = 0; slot < capacity; ++slot) {
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
    virtual_bag::Item& descriptor = runtime_state().objects.bag.items[adopt_bag][adopt_slot];
    descriptor.payload = payload;
    descriptor.payload_size = payload_size;
    (void)read_item_category(item, &descriptor.category);
    descriptor.count = 1;
    uint32_t adopted_handle = 0;
    if (ownership::allocate(&runtime_state().objects.ownership_ledger, &adopted_handle) == ownership::Outcome::kOk) {
        runtime_state().objects.module_object_handles[adopt_bag][adopt_slot] = adopted_handle;
    } else {
        runtime_state().objects.module_object_handles[adopt_bag][adopt_slot] = 0;
        VIRTBAG_LOG("original bag unequip ledger exhausted bag=%d", bag_index);
    }
    if (!module_object_generation_advance_locked(
            adopt_bag, adopt_slot, "original bag unequip adopt")) {
        VIRTBAG_LOG("original bag unequip adopt blocked by active object bag=%d slot=%d",
                    adopt_bag, adopt_slot);
        return false;
    }
    if (!extension_bag_rebind_item_handle(item)) {
        descriptor = virtual_bag::Item{};
        if (adopted_handle != 0) ownership::release(&runtime_state().objects.ownership_ledger, adopted_handle);
        runtime_state().objects.module_object_handles[adopt_bag][adopt_slot] = 0;
        VIRTBAG_LOG("original bag unequip handle registration failed bag=%d slot=%d",
                    adopt_bag, adopt_slot);
        return false;
    }
    runtime_state().objects.module_objects[adopt_bag][adopt_slot] = item;
    runtime_state().objects.module_object_categories[adopt_bag][adopt_slot] = descriptor.category;
    runtime_state().objects.module_object_hashes[adopt_bag][adopt_slot] = virtual_bag::payload_hash(descriptor);
    (void)runtime_state().host.write_port->ui.set_original_bag_item(bag_index, nullptr);
    extension_bag_mark_dirty_locked();
    persist_state_locked();
    extension_description_set_off();
    clear_original_desc_locked();
    if (runtime_state().objects.module_view_installed && runtime_state().objects.module_view_index == adopt_bag) {
        refresh_module_item_area_locked(adopt_bag);
    } else {
        virtual_bag_call_original_refresh_item_area_with_guard();
    }
    extension_refresh_bag_area();
    VIRTBAG_LOG("original bag unequip->extension bag=%d dst=%d/%d item=%p", bag_index,
                adopt_bag, adopt_slot, item);
    return true;
}
