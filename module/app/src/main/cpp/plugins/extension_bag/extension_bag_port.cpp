#include "plugins/extension_bag/extension_bag_port.h"
#include "plugins/extension_bag/extension_bag_bridge.h"
#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "contracts/inventory_extension.h"
#include "contracts/sell_price.h"
#include "contracts/host_sdk.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

namespace {

const qol::contracts::QolHostSdk* g_sdk = nullptr;

// Holds the private native identity for provider-owned materialized objects. The registry remains
// authoritative for handle generation; lookups below verify it before exposing the pointer inside EB.
qol::features::extension_bag::model::ItemHandleCustody g_private_item_custody;

const qol::contracts::QolItemHandleApi* item_handle_api() {
    if (g_sdk == nullptr ||
        g_sdk->abi.size < offsetof(qol::contracts::QolHostSdk, item_handle) +
                              sizeof(qol::contracts::QolItemHandleApi)) return nullptr;
    return &g_sdk->item_handle;
}

uint32_t canonical_count_from_field(uint32_t field) {
    return virtual_bag_canonical_from_field(field, field);
}

uint32_t view_count_from_canonical(uint32_t canonical, bool extended) {
    return virtual_bag_view_from_canonical(canonical, extended);
}

uint32_t write_view_to_field(uint32_t field, uint32_t count, bool extended) {
    return virtual_bag_field_from_canonical(field, count, extended);
}

uint32_t write_canonical_to_field(uint32_t field, uint32_t canonical) {
    return write_view_to_field(field, canonical, true);
}

uint32_t count_limit(bool extended) {
    return virtual_bag_count_limit(extended);
}

const virtual_bag::CountPolicy kCountPolicy{
    &canonical_count_from_field,
    &view_count_from_canonical,
    &write_view_to_field,
    &write_canonical_to_field,
    &count_limit,
};

}

void qol::features::extension_bag::extension_bag_bind_port_sdk(
    const qol::contracts::QolHostSdk* sdk) {
    g_sdk = sdk;
    virtual_bag::set_count_policy(sdk == nullptr ? nullptr : &kCountPolicy);
}

bool extension_bag_register_item_handle(void* item) {
    const auto* api = item_handle_api();
    if (item == nullptr || api == nullptr || api->register_item == nullptr) return false;
    qol::contracts::QolItemHandle handle{};
    const bool registered = api->register_item(
        item, qol::contracts::kQolItemHandleOwnerProvider, &handle) ==
        qol::contracts::kQolItemHandleStatusOk;
    if (registered && !g_private_item_custody.bind(item, handle)) {
        if (api->invalidate_item != nullptr) {
            (void)api->invalidate_item(item, qol::contracts::kQolItemHandleOwnerProvider);
        }
        return false;
    }
    return registered && handle.slot != 0 && handle.generation != 0;
}

void extension_bag_invalidate_item_handle(void* item) {
    const auto* api = item_handle_api();
    if (item == nullptr || api == nullptr || api->invalidate_item == nullptr) return;
    qol::contracts::QolItemHandle handle{};
    uint32_t owner = 0;
    if (api->lookup_item == nullptr || api->lookup_item(item, &handle, &owner) !=
                                          qol::contracts::kQolItemHandleStatusOk ||
        owner != qol::contracts::kQolItemHandleOwnerProvider ||
        !g_private_item_custody.begin_release(handle)) return;
    const int32_t status = api->invalidate_item(
        item, qol::contracts::kQolItemHandleOwnerProvider);
    (void)g_private_item_custody.finish_release(handle,
                                                 status == qol::contracts::kQolItemHandleStatusOk);
}

bool extension_bag_rebind_item_handle(void* item) {
    const auto* api = item_handle_api();
    if (item == nullptr || api == nullptr) return false;
    qol::contracts::QolItemHandle previous{};
    uint32_t previous_owner = 0;
    const bool had_previous = api->lookup_item != nullptr &&
        api->lookup_item(item, &previous, &previous_owner) ==
            qol::contracts::kQolItemHandleStatusOk;
    if (api->rebind_item != nullptr &&
        api->rebind_item(
            item, qol::contracts::kQolItemHandleOwnerProvider) ==
            qol::contracts::kQolItemHandleStatusOk) {
        qol::contracts::QolItemHandle rebound{};
        uint32_t owner = 0;
        if (api->lookup_item == nullptr || api->lookup_item(item, &rebound, &owner) !=
                                               qol::contracts::kQolItemHandleStatusOk ||
            owner != qol::contracts::kQolItemHandleOwnerProvider) return false;
        if (had_previous && previous_owner == qol::contracts::kQolItemHandleOwnerProvider) {
            if (g_private_item_custody.rebind(item, previous, rebound)) return true;
        }
        return g_private_item_custody.bind(item, rebound);
    }
    return extension_bag_register_item_handle(item);
}

bool extension_bag_private_item_for_handle(qol::contracts::QolItemHandle handle, void** out_item) {
    if (out_item == nullptr || handle.slot == 0 || handle.generation == 0) return false;
    *out_item = nullptr;
    if (!g_private_item_custody.lookup(handle, out_item)) return false;
    const auto* api = item_handle_api();
    qol::contracts::QolItemHandle current{};
    uint32_t owner = 0;
    if (api == nullptr || api->lookup_item == nullptr ||
        api->lookup_item(*out_item, &current, &owner) != qol::contracts::kQolItemHandleStatusOk ||
        owner != qol::contracts::kQolItemHandleOwnerProvider ||
        current.slot != handle.slot || current.generation != handle.generation) {
        (void)g_private_item_custody.begin_release(handle);
        (void)g_private_item_custody.finish_release(handle, true);
        *out_item = nullptr;
        return false;
    }
    return true;
}

bool extension_bag_sale_item_matches(qol::contracts::QolItemHandle expected, void* item,
                                     qol::contracts::QolItemHandle* out_current) {
    if (item == nullptr || out_current == nullptr || expected.slot == 0 ||
        expected.generation == 0) return false;
    void* custody_item = nullptr;
    if (!g_private_item_custody.lookup(expected, &custody_item) || custody_item != item)
        return false;
    const auto* api = item_handle_api();
    qol::contracts::QolItemHandle current{};
    uint32_t owner = 0;
    if (api == nullptr || api->lookup_item == nullptr ||
        api->lookup_item(item, &current, &owner) != qol::contracts::kQolItemHandleStatusOk ||
        owner != qol::contracts::kQolItemHandleOwnerProvider ||
        current.slot != expected.slot || current.generation != expected.generation) return false;
    *out_current = current;
    return true;
}

bool extension_bag_sale_item_begin_release(qol::contracts::QolItemHandle expected, void* item) {
    qol::contracts::QolItemHandle current{};
    return extension_bag_sale_item_matches(expected, item, &current) &&
           g_private_item_custody.begin_release(expected);
}

bool extension_bag_sale_item_finish_release(qol::contracts::QolItemHandle expected,
                                            bool committed) {
    return g_private_item_custody.finish_release(expected, committed);
}

bool extension_bag_item_handle_for_pointer(
    void* item, qol::contracts::QolItemHandle* out_handle) {
    const auto* api = item_handle_api();
    if (item == nullptr || out_handle == nullptr || api == nullptr) return false;
    uint32_t provider_id = 0;
    if (api->lookup_item(item, out_handle, &provider_id) ==
        qol::contracts::kQolItemHandleStatusOk) {
        if (provider_id == qol::contracts::kQolItemHandleOwnerProvider) {
            return g_private_item_custody.bind(item, *out_handle);
        }
        return true;
    }
    if (api->register_item(item, qol::contracts::kQolItemHandleOwnerGame, out_handle) !=
        qol::contracts::kQolItemHandleStatusOk) return false;
    return true;
}

namespace {

thread_local unsigned int g_internal_equip_depth = 0;

}

void provider_sync_projected_slot(int bag, int slot) {
    virtual_bag_sync_projected_slot(bag, slot);
}

void provider_sync_projected_bag() {
    virtual_bag_sync_projected_bag();
}

bool provider_module_view_installed() {
    return virtual_bag_module_view_installed();
}

bool provider_enabled() {
    return virtual_bag_enabled();
}

std::string extension_bag_inventory_bags_json() {
    return virtual_bag_inventory_bags_json();
}

bool provider_is_inventory_enter(uintptr_t enter) {
    return virtual_bag_is_inventory_enter(enter);
}

bool provider_is_logical_bag(int bag) {
    return virtual_bag::valid_extension_logical_bag(bag);
}

int provider_internal_index(int logical_bag) {
    return virtual_bag::extension_internal_bag(logical_bag);
}

bool provider_prepare_main_menu() {
    return virtual_bag_prepare_main_menu();
}

int provider_inventory_state_id() {
    return virtual_bag_inventory_state_id();
}

bool provider_prepare_save_slot_load() {
    return virtual_bag_prepare_save_slot_load();
}

void provider_for_each_logical_item(qol::contracts::ExtensionBagItemFn fn, void* ctx) {
    if (fn == nullptr) return;
    struct Context {
        qol::contracts::ExtensionBagItemFn fn;
        void* ctx;
    } context{fn, ctx};
    virtual_bag_for_each_logical_item(
        [](int bag, int slot, int category, int count, qol::contracts::QolItemHandle item,
           void* raw) -> bool {
            auto* context = static_cast<Context*>(raw);
            const qol::contracts::QolInventoryExtensionItemRef ref{
                static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kProvider), bag,
                slot, category, count, item};
            return context->fn(&ref, context->ctx) != 0;
        },
        &context);
}

bool provider_logical_bag_capacity(int32_t logical_bag, int32_t* out_capacity) {
    return virtual_bag_logical_capacity(logical_bag, out_capacity);
}

int32_t provider_item_at(int bag, int slot, qol::contracts::QolItemHandle* out_item) {
    if (out_item == nullptr) return 0;
    *out_item = qol::contracts::kInvalidItemHandle;
    void* item = virtual_bag_item_at(bag, slot);
    if (item == nullptr) return 0;
    return extension_bag_register_item_handle(item) &&
                   extension_bag_item_handle_for_pointer(item, out_item)
               ? 1
               : 0;
}

int32_t provider_view_item_at(int bag, int slot, qol::contracts::QolItemHandle* out_item) {
    if (out_item == nullptr) return 0;
    *out_item = qol::contracts::kInvalidItemHandle;
    void* item = virtual_bag_view_item_at(bag, slot);
    if (item == nullptr) return 0;
    return extension_bag_register_item_handle(item) &&
                   extension_bag_item_handle_for_pointer(item, out_item)
               ? 1
               : 0;
}

bool provider_handle_backpack_button_equip() {
    return virtual_bag_handle_backpack_button_equip();
}

bool provider_handle_original_bag_unequip(bool* out_no_space, bool* out_not_empty) {
    return virtual_bag_handle_original_bag_unequip(out_no_space, out_not_empty);
}

void extension_bag_show_no_space_popup() {
    // TextData 6 = "背包已满"（与扩展袋/原版卸袋同窗）。
    if (g_sdk == nullptr || g_sdk->ui == nullptr || g_sdk->ui->show_text_popup == nullptr) return;
    (void)g_sdk->ui->show_text_popup(6);
}

void extension_bag_show_not_empty_popup() {
    // TextData 7 = "袋非空"（原版 b8084 同窗）。
    if (g_sdk == nullptr || g_sdk->ui == nullptr || g_sdk->ui->show_text_popup == nullptr) return;
    (void)g_sdk->ui->show_text_popup(7);
}

bool provider_identify_native_item(qol::contracts::QolItemHandle handle,
                                   int* out_bag, int* out_slot) {
    void* item = nullptr;
    return extension_bag_private_item_for_handle(handle, &item) &&
           virtual_bag_identify_native_item(item, out_bag, out_slot);
}

int32_t provider_find_native_item(int category, qol::contracts::QolItemHandle* out_item) {
    if (out_item == nullptr) return 0;
    *out_item = qol::contracts::kInvalidItemHandle;
    void* item = virtual_bag_find_native_item(category);
    if (item == nullptr) return 0;
    return extension_bag_register_item_handle(item) &&
                   extension_bag_item_handle_for_pointer(item, out_item)
               ? 1
               : 0;
}

bool provider_remove_native_item(qol::contracts::QolItemHandle handle) {
    void* item = nullptr;
    return extension_bag_private_item_for_handle(handle, &item) &&
           virtual_bag_remove_native_item(item, virtual_bag_current_use_token());
}

int64_t provider_sell_price(qol::contracts::QolItemHandle handle, int count,
                            bool apply_variant_discount) {
    const auto* facts = qol::features::extension_bag::extension_bag_item_facts();
    if (handle.slot == 0 || handle.generation == 0 || count <= 0 || facts == nullptr ||
        facts->sell_price == nullptr) return -1;
    int64_t unit_price = 0;
    if (!qol::contracts::succeeded(facts->sell_price(handle, &unit_price))) return -1;
    // count 为调用方传入的模式视图数量（R-47 决策 b），按模式上限收敛。
    const uint32_t legal_count = static_cast<uint32_t>(count);
    int64_t final_price = 0;
    if (!qol::contracts::sell_price::calculate(unit_price, legal_count,
                                               apply_variant_discount, &final_price)) {
        return -1;
    }
    // 原版商店为单位售价×数量；详情页粉碎改出售路径再乘 70%。
    return final_price;
}

QolResult provider_remove_for_sale(int32_t logical_bag, int32_t slot,
                                   qol::contracts::QolItemHandle expected_item) {
    return virtual_bag_remove_for_sale(logical_bag, slot, expected_item);
}

bool provider_equip_projected_item(void* character, qol::contracts::QolItemHandle handle,
                                    int source_bag, int source_slot,
                                    int equip_slot) {
    void* item = nullptr;
    if (!extension_bag_private_item_for_handle(handle, &item)) return false;
    return virtual_bag_equip_projected_item(character, item, source_bag, source_slot, equip_slot);
}

void provider_begin_internal_equip() {
    ++g_internal_equip_depth;
}

void provider_end_internal_equip() {
    if (g_internal_equip_depth > 0) --g_internal_equip_depth;
}

bool provider_internal_equip_active() {
    return g_internal_equip_depth > 0;
}

bool provider_consume_native_item(qol::contracts::QolItemHandle handle) {
    void* item = nullptr;
    return extension_bag_private_item_for_handle(handle, &item) &&
           virtual_bag_consume_native_item(item, virtual_bag_current_use_token());
}

int provider_consume_category(int category, int count) {
    if (category <= 0 || count <= 0 || !provider_enabled()) return 0;
    int consumed = 0;
    while (consumed < count) {
        qol::contracts::QolItemHandle item{};
        if (provider_find_native_item(category, &item) == 0) break;
        if (!provider_consume_native_item(item)) break;
        ++consumed;
    }
    return consumed;
}

bool provider_has_empty_slots(int needed, int include_task_bag) {
    return virtual_bag_has_empty_slots(needed, include_task_bag);
}

bool provider_adopt_unequipped_item(void* character, int equip_slot) {
    return virtual_bag_adopt_unequipped_item(character, equip_slot);
}

bool provider_adopt_native_item(qol::contracts::QolItemHandle handle) {
    void* item = nullptr;
    return extension_bag_private_item_for_handle(handle, &item) &&
           virtual_bag_adopt_native_item(item);
}

bool provider_merge_native_item(qol::contracts::QolItemHandle handle) {
    void* item = nullptr;
    return extension_bag_private_item_for_handle(handle, &item) &&
           virtual_bag_merge_native_item(item);
}

void provider_store_restore_for_original_write() {
    virtual_bag_store_restore_for_original_write();
}

int32_t provider_move_item(int32_t from_bag, int32_t from_slot, int32_t to_bag,
                           int32_t to_slot, char* out, uint32_t capacity,
                           uint32_t* out_len) {
    if (out_len != nullptr) *out_len = 0;
    if (out == nullptr || out_len == nullptr) return 0;
    const std::string body = extension_bag_api_move_item_impl(
        from_bag, from_slot, to_bag, to_slot);
    const uint32_t copied = static_cast<uint32_t>(std::min<std::size_t>(body.size(), capacity));
    if (copied != 0) std::memcpy(out, body.data(), copied);
    *out_len = static_cast<uint32_t>(body.size());
    // core.item ignores successful payloads but retains the original error JSON on failure.
    return body.compare(0, 11, "{\"ok\":false") == 0 ? 0 : 1;
}

// 扩展插件内部使用的稳定入口（不再经宿主 capability 包装）。
bool extension_bag_enabled() { return virtual_bag_enabled(); }

int64_t extension_bag_sell_price(void* item, int count, bool apply_variant_discount) {
    qol::contracts::QolItemHandle handle{};
    if (!extension_bag_item_handle_for_pointer(item, &handle)) return -1;
    return provider_sell_price(handle, count, apply_variant_discount);
}

bool extension_bag_has_empty_slots(int needed, int include_task_bag) {
    return provider_has_empty_slots(needed, include_task_bag);
}

void extension_bag_begin_internal_equip() { provider_begin_internal_equip(); }
void extension_bag_end_internal_equip() { provider_end_internal_equip(); }

// ===========================================================================
// capability provider —— ExtensionBagApi v3.1 接口表
// ===========================================================================
//
// 契约限制（contracts/extension_bag.h）：POD + 固定宽度 C 类型 + 函数指针；
// 全部成员仅游戏主线程调用。成员逐条转发既有稳定 port：优先 extension_bag_*（本
// 文件内的 port 包装），无对应包装者直连 virtual_bag_*。仅 3 处需 POD 适配：
//   · handle_backpack_button_equip_result / handle_equip_control_event：port 枚举
//     → contract 枚举（数值一一对应，kNotExtension=0/kHandled=1/kBlocked=2）；
//   · capture_move_item_observation：port 观测结构 → contract 观测结构（字段逐一拷贝）。
// int32_t/int 在目标 ABI 同型，其余成员函数指针类型完全一致，无需适配。

namespace qol::features::extension_bag {
namespace {

qol::contracts::ExtensionBagEquipButtonResult api_handle_backpack_button_equip_result() {
    return static_cast<qol::contracts::ExtensionBagEquipButtonResult>(
        static_cast<uint8_t>(virtual_bag_handle_backpack_button_equip_result()));
}

qol::contracts::ExtensionBagEquipEventResult api_handle_equip_control_event(
    void* control, uint64_t event, void* x2, void* param,
    qol::contracts::ExtensionBagEquipEventBackup backup, uint64_t* out_result) {
    return static_cast<qol::contracts::ExtensionBagEquipEventResult>(
        static_cast<uint8_t>(virtual_bag_handle_equip_control_event(
            control, event, x2, param, reinterpret_cast<VirtualBagEquipControlEventBackup>(backup),
            out_result)));
}

bool api_capture_move_item_observation(qol::contracts::QolItemHandle handle,
                                       int32_t target_bag, int32_t target_slot,
                                       qol::contracts::ExtensionBagMoveObservation* out) {
    if (out == nullptr) return false;
    void* item = nullptr;
    if (!extension_bag_private_item_for_handle(handle, &item)) return false;
    VirtualBagMoveItemObservation observation{};
    if (!virtual_bag_capture_move_item_observation(item, target_bag, target_slot, &observation)) {
        return false;
    }
    out->extension_item = observation.extension_item;
    out->extension_bag = observation.extension_bag;
    out->extension_slot = observation.extension_slot;
    out->extension_handle = observation.extension_handle;
    out->ownership_state = observation.ownership_state;
    out->module_view_index = observation.module_view_index;
    out->drag_session_active = observation.drag_session_active;
    out->source_bag = observation.source_bag;
    out->source_slot = observation.source_slot;
    out->source_item = qol::contracts::kInvalidItemHandle;
    out->target_item = qol::contracts::kInvalidItemHandle;
    if (observation.source_ptr != nullptr &&
        !extension_bag_item_handle_for_pointer(observation.source_ptr, &out->source_item)) {
        return false;
    }
    if (observation.target_ptr != nullptr &&
        !extension_bag_item_handle_for_pointer(observation.target_ptr, &out->target_item)) {
        return false;
    }
    out->physical_digest = observation.physical_digest;
    out->physical_nonnull = observation.physical_nonnull;
    out->physical_valid = observation.physical_valid;
    return true;
}

bool api_handle_confirm_use_item(qol::contracts::QolItemHandle handle) {
    void* item = nullptr;
    return extension_bag_private_item_for_handle(handle, &item) &&
           virtual_bag_handle_confirm_use_item(item);
}

thread_local qol::contracts::ExtensionBagPutJewelBackup g_put_jewel_backup = nullptr;
thread_local qol::contracts::QolItemHandle g_put_jewel_equip_handle{};
thread_local qol::contracts::QolItemHandle g_put_jewel_material_handle{};

int put_jewel_backup_handle_bridge(void*, void*) {
    return g_put_jewel_backup == nullptr
               ? 3
               : g_put_jewel_backup(g_put_jewel_equip_handle, g_put_jewel_material_handle);
}

int32_t api_put_jewel_native(qol::contracts::QolItemHandle equip_handle,
                             qol::contracts::QolItemHandle jewel_handle,
                             qol::contracts::ExtensionBagPutJewelBackup backup) {
    void* equip_item = nullptr;
    void* jewel_item = nullptr;
    if (!extension_bag_private_item_for_handle(equip_handle, &equip_item) ||
        !extension_bag_private_item_for_handle(jewel_handle, &jewel_item)) return 3;
    auto* previous_backup = g_put_jewel_backup;
    const auto previous_equip = g_put_jewel_equip_handle;
    const auto previous_jewel = g_put_jewel_material_handle;
    g_put_jewel_backup = backup;
    g_put_jewel_equip_handle = equip_handle;
    g_put_jewel_material_handle = jewel_handle;
    const int result = virtual_bag_put_jewel_native(
        equip_item, jewel_item, &put_jewel_backup_handle_bridge);
    g_put_jewel_backup = previous_backup;
    g_put_jewel_equip_handle = previous_equip;
    g_put_jewel_material_handle = previous_jewel;
    return result;
}

QolResult api_put_jewel(int32_t role, int32_t logical_bag, int32_t slot, int32_t equip_slot) {
    return extension_bag_api_put_jewel_typed(role, logical_bag, slot, equip_slot);
}

const qol::contracts::ExtensionBagApi kExtensionBagApi{
    // 1 状态/索引
    &provider_enabled,
    &provider_module_view_installed,
    &provider_is_inventory_enter,
    &provider_is_logical_bag,
    &provider_internal_index,
    &provider_prepare_main_menu,
    &provider_prepare_save_slot_load,
    &provider_inventory_state_id,
    &provider_for_each_logical_item,
    // 2 对象反查
    &provider_item_at,
    &provider_view_item_at,
    &provider_identify_native_item,
    &provider_find_native_item,
    &provider_remove_native_item,
    // 3 投影
    &provider_sync_projected_slot,
    &provider_sync_projected_bag,
    &provider_store_restore_for_original_write,
    // 4 收编/消耗
    &provider_consume_native_item,
    &provider_consume_category,
    &provider_has_empty_slots,
    &provider_adopt_unequipped_item,
    &provider_adopt_native_item,
    &provider_merge_native_item,
    &provider_sell_price,
    &provider_equip_projected_item,
    // 5 交互接管
    &api_handle_backpack_button_equip_result,
    &api_handle_confirm_use_item,
    &virtual_bag_handle_use_mercenary_seal,
    &api_handle_equip_control_event,
    &provider_handle_original_bag_unequip,
    &api_put_jewel_native,
    // 6 观测/拖放
    &api_capture_move_item_observation,
    // 7 内部标记
    &provider_begin_internal_equip,
    &provider_end_internal_equip,
    &provider_internal_equip_active,
    &virtual_bag_native_call_active,
    &virtual_bag_refresh_item_area_with_gate,
    // 8 移动写面
    &provider_move_item,
    // 9 typed inventory view support
     &provider_logical_bag_capacity,
     // 10 typed jewel transaction
     &api_put_jewel,
     // v3.1 sale removal operation
     &provider_remove_for_sale,
};

}  // namespace

const qol::contracts::ExtensionBagApi* extension_bag_api_table() {
    return &kExtensionBagApi;
}

// ===========================================================================
// ITM-13：中立物品域扩展 provider —— 扩展袋语义唯一的对外实现点
// ===========================================================================
// core.item / game 物品读路径只经此中立表感知扩展物品；此表不含 ExtensionBag 名称、逻辑袋
// 硬编码或业务实现细节（袋号换算在此内部完成）。全部成员仅游戏主线程调用；持
// g_virtual_bag_mtx 时不得重入本表的加锁成员。
namespace {

QolResult neutral_error(int32_t code, const char* message) {
    return qol::contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

int32_t neutral_owns_bag(int32_t bag) {
    return virtual_bag::valid_extension_logical_bag(bag) ? 1 : 0;
}

QolResult neutral_item_ref_at(int32_t bag, int32_t slot,
                              qol::contracts::QolInventoryExtensionItemRef* out) {
    if (out == nullptr) return neutral_error(kQolErrInvalidArgument, "invalid argument");
    *out = {};
    struct Ctx {
        int32_t bag;
        int32_t slot;
         qol::contracts::QolInventoryExtensionItemRef* out;
        bool found;
    } ctx{bag, slot, out, false};
    virtual_bag_for_each_logical_item(
        [](int32_t item_bag, int32_t item_slot, int32_t category, int32_t count,
           qol::contracts::QolItemHandle item_handle,
           void* raw) -> bool {
            auto* p = static_cast<Ctx*>(raw);
            if (item_bag != p->bag || item_slot != p->slot) return false;
             *p->out = {static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kProvider),
                        item_bag, item_slot, category, count, item_handle};
            p->found = true;
            return true;
        },
        &ctx);
    if (!ctx.found) return neutral_error(kQolErrNotFound, "not found");
    const int internal_bag = virtual_bag::extension_internal_bag(bag);
    if (internal_bag < 0) return neutral_error(kQolErrNotFound, "not found");
    void* item = virtual_bag_item_at(internal_bag, slot);
    if (item == nullptr || !extension_bag_register_item_handle(item) ||
        !extension_bag_item_handle_for_pointer(item, &out->item)) {
        return neutral_error(kQolErrNotFound, "not found");
    }
    return qol::contracts::ok();
}

QolResult neutral_for_each_item(qol::contracts::QolInventoryExtensionItemFn fn, void* ctx) {
    if (fn == nullptr) return neutral_error(kQolErrInvalidArgument, "invalid argument");
    struct Bridge {
        qol::contracts::QolInventoryExtensionItemFn fn;
        void* ctx;
        bool stopped;
    } bridge{fn, ctx, false};
    virtual_bag_for_each_logical_item(
        [](int32_t bag, int32_t slot, int32_t category, int32_t count,
           qol::contracts::QolItemHandle item_handle, void* raw) -> bool {
            auto* p = static_cast<Bridge*>(raw);
            // Enumeration stays allocation-free; consumers request a materialized handle by slot.
            const qol::contracts::QolInventoryExtensionItemRef ref{
                static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kProvider), bag, slot,
                category, count, item_handle};
            p->stopped = p->fn(&ref, p->ctx) != 0;
            return p->stopped;
        },
        &bridge);
    return qol::contracts::ok();
}

QolResult neutral_bag_capacity(int32_t bag, int32_t* out_capacity) {
    if (out_capacity == nullptr) return neutral_error(kQolErrInvalidArgument, "invalid argument");
    *out_capacity = 0;
    if (!virtual_bag_logical_capacity(bag, out_capacity)) {
        return neutral_error(kQolErrNotFound, "not found");
    }
    return qol::contracts::ok();
}

QolResult neutral_inventory_fragment(char* out, uint32_t capacity, uint32_t* out_size) {
    if (out == nullptr || out_size == nullptr) {
        return neutral_error(kQolErrInvalidArgument, "invalid argument");
    }
    *out_size = 0;
    // R-47b 真值表与袋号/capacity/slot_count 组装唯一实现点在扩展袋序列化内。
    const std::string fragment = extension_bag_inventory_bags_json();
    if (fragment.empty()) return qol::contracts::ok();
    if (fragment.size() >= capacity) return neutral_error(kQolErrBlocked, "buffer too small");
    std::memcpy(out, fragment.data(), fragment.size());
    out[fragment.size()] = '\0';
    *out_size = static_cast<uint32_t>(fragment.size());
    return qol::contracts::ok();
}

int32_t neutral_move_item(int32_t from_bag, int32_t from_slot, int32_t to_bag, int32_t to_slot,
                          char* out, uint32_t capacity, uint32_t* out_size) {
    return provider_move_item(from_bag, from_slot, to_bag, to_slot, out, capacity, out_size);
}

int32_t neutral_use_item(int32_t bag, int32_t slot, char* out, uint32_t capacity,
                         uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (out == nullptr || out_size == nullptr) return 0;
    const std::string body = extension_bag_api_use_item_impl(bag, slot);
    const uint32_t copied = static_cast<uint32_t>(std::min<std::size_t>(body.size(), capacity));
    if (copied != 0) std::memcpy(out, body.data(), copied);
    *out_size = static_cast<uint32_t>(body.size());
    return 1;
}

QolResult neutral_sell_price(qol::contracts::QolItemHandle item, int32_t count,
                             int32_t apply_discount,
                             int64_t* out_price) {
    if (out_price == nullptr) return neutral_error(kQolErrInvalidArgument, "invalid argument");
    *out_price = 0;
    const int64_t price = provider_sell_price(item, count, apply_discount != 0);
    if (price < 0) return neutral_error(kQolErrInvalidArgument, "invalid sell price");
    *out_price = price;
    return qol::contracts::ok();
}

QolResult neutral_sync_projected_slot(int32_t bag, int32_t slot) {
    provider_sync_projected_slot(bag, slot);
    return qol::contracts::ok();
}

QolResult neutral_sync_projected_bag() {
    provider_sync_projected_bag();
    return qol::contracts::ok();
}

int32_t neutral_module_view_installed() {
    return provider_module_view_installed() ? 1 : 0;
}

int32_t neutral_identify_native_item(qol::contracts::QolItemHandle item,
                                     int32_t* out_bag, int32_t* out_slot) {
    if (out_bag == nullptr || out_slot == nullptr) return 0;
    int internal_bag = -1;
    int slot = -1;
    if (!provider_identify_native_item(item, &internal_bag, &slot)) return 0;
    if (internal_bag < 0 || internal_bag >= virtual_bag::kBagCount) return 0;
    // provider 内部完成袋号换算：对外只输出公共逻辑袋号。
    *out_bag = virtual_bag::kExtensionLogicalBagFirst + internal_bag;
    *out_slot = slot;
    return 1;
}

int32_t neutral_remove_native_item(qol::contracts::QolItemHandle item) {
    return provider_remove_native_item(item) ? 1 : 0;
}

QolResult neutral_put_jewel(int32_t role, int32_t bag, int32_t slot, int32_t equip_slot) {
    return extension_bag_api_put_jewel_typed(role, bag, slot, equip_slot);
}

const qol::contracts::QolInventoryExtensionProvider kInventoryExtensionProvider{
    &neutral_owns_bag,
    &neutral_item_ref_at,
    &neutral_for_each_item,
    &neutral_bag_capacity,
    &neutral_inventory_fragment,
    &neutral_move_item,
    &neutral_sell_price,
    &neutral_sync_projected_slot,
    &neutral_sync_projected_bag,
    &neutral_module_view_installed,
    &neutral_identify_native_item,
    &neutral_remove_native_item,
    &neutral_put_jewel,
    &neutral_use_item,
    &provider_remove_for_sale,
};

}  // namespace

const qol::contracts::QolInventoryExtensionProvider* inventory_extension_provider_table() {
    return &kInventoryExtensionProvider;
}

}  // namespace qol::features::extension_bag
