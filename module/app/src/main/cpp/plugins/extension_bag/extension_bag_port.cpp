#include "plugins/extension_bag/extension_bag_port.h"
#include "plugins/extension_bag/extension_bag_bridge.h"
#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "contracts/sell_price.h"
#include "contracts/host_sdk.h"

namespace {

const qol::contracts::QolHostSdk* g_sdk = nullptr;

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

void provider_for_each_logical_item(LogicalInventoryItemFn fn, void* ctx) {
    virtual_bag_for_each_logical_item(fn, ctx);
}

void* provider_item_at(int bag, int slot) {
    return virtual_bag_item_at(bag, slot);
}

void* provider_view_item_at(int bag, int slot) {
    return virtual_bag_view_item_at(bag, slot);
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

bool provider_identify_native_item(void* item, int* out_bag, int* out_slot) {
    return virtual_bag_identify_native_item(item, out_bag, out_slot);
}

void* provider_find_native_item(int category) {
    return virtual_bag_find_native_item(category);
}

bool provider_remove_native_item(void* item) {
    return virtual_bag_remove_native_item(item, virtual_bag_current_use_token());
}

int64_t provider_sell_price(void* item, int count, bool apply_variant_discount) {
    const auto* game = qol::features::extension_bag::extension_bag_game_host();
    if (item == nullptr || count <= 0 || game == nullptr ||
        game->read_only.item.sell_price == nullptr) return -1;
    int64_t unit_price = 0;
    if (!qol::contracts::succeeded(game->read_only.item.sell_price(item, &unit_price))) return -1;
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

bool provider_equip_projected_item(void* character, void* item, int source_bag, int source_slot,
                                    int equip_slot) {
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

bool provider_consume_native_item(void* item) {
    return virtual_bag_consume_native_item(item, virtual_bag_current_use_token());
}

int provider_consume_category(int category, int count) {
    if (category <= 0 || count <= 0 || !provider_enabled()) return 0;
    int consumed = 0;
    while (consumed < count) {
        void* item = provider_find_native_item(category);
        if (item == nullptr) break;
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

bool provider_adopt_native_item(void* item) {
    return virtual_bag_adopt_native_item(item);
}

bool provider_merge_native_item(void* item) {
    return virtual_bag_merge_native_item(item);
}

void provider_store_restore_for_original_write() {
    virtual_bag_store_restore_for_original_write();
}

std::string extension_bag_use_item(int bag, int slot) {
    return extension_bag_api_use_item_impl(bag, slot);
}

std::string extension_bag_move_item(int from_bag, int from_slot, int to_bag, int to_slot) {
    return extension_bag_api_move_item_impl(from_bag, from_slot, to_bag, to_slot);
}

std::string extension_bag_put_jewel(int role, int bag, int slot, int equip_slot) {
    return extension_bag_api_put_jewel_impl(role, bag, slot, equip_slot);
}

// ===========================================================================
// D2a-3c：capability provider —— ExtensionBagApi v1.0 接口表
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

bool api_capture_move_item_observation(void* item, int32_t target_bag, int32_t target_slot,
                                       qol::contracts::ExtensionBagMoveObservation* out) {
    if (out == nullptr) return false;
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
    out->source_ptr = observation.source_ptr;
    out->target_ptr = observation.target_ptr;
    out->physical_digest = observation.physical_digest;
    out->physical_nonnull = observation.physical_nonnull;
    out->physical_valid = observation.physical_valid;
    return true;
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
    &virtual_bag_handle_confirm_use_item,
    &virtual_bag_handle_use_mercenary_seal,
    &api_handle_equip_control_event,
    &provider_handle_original_bag_unequip,
    &virtual_bag_put_jewel_native,
    // 6 观测/拖放
    &api_capture_move_item_observation,
    // 7 内部标记
    &provider_begin_internal_equip,
    &provider_end_internal_equip,
    &provider_internal_equip_active,
    &virtual_bag_native_call_active,
    &virtual_bag_refresh_item_area_with_gate,
};

}  // namespace

const qol::contracts::ExtensionBagApi* extension_bag_api_table() {
    return &kExtensionBagApi;
}

}  // namespace qol::features::extension_bag
