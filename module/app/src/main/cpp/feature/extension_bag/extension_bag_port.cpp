#include "core/native/extension_bag_port.h"

#include "game_access.h"
#include "feature/extension_bag/extension_bag_context.h"
#include "feature/extension_bag/game_ui_virtbag.h"
#include "feature/extension_bag/model/virtual_bag_state.h"

#include <limits>

namespace {

thread_local unsigned int g_internal_equip_depth = 0;

}

void extension_bag_sync_projected_slot(int bag, int slot) {
    virtual_bag_sync_projected_slot(bag, slot);
}

void extension_bag_sync_projected_bag() {
    virtual_bag_sync_projected_bag();
}

bool extension_bag_module_view_installed() {
    return virtual_bag_module_view_installed();
}

bool extension_bag_enabled() {
    return virtual_bag_enabled();
}

std::string extension_bag_inventory_bags_json() {
    return virtual_bag_inventory_bags_json();
}

bool extension_bag_is_inventory_enter(uintptr_t enter) {
    return virtual_bag_is_inventory_enter(enter);
}

bool extension_bag_is_logical_bag(int bag) {
    return virtual_bag::valid_extension_logical_bag(bag);
}

void extension_bag_prepare_main_menu() {
    virtual_bag_prepare_main_menu();
}

int extension_bag_inventory_state_id() {
    return virtual_bag_inventory_state_id();
}

void extension_bag_prepare_save_slot_load() {
    virtual_bag_prepare_save_slot_load();
}

void extension_bag_for_each_logical_item(LogicalInventoryItemFn fn, void* ctx) {
    virtual_bag_for_each_logical_item(fn, ctx);
}

void* extension_bag_item_at(int bag, int slot) {
    return virtual_bag_item_at(bag, slot);
}

void* extension_bag_view_item_at(int bag, int slot) {
    return virtual_bag_view_item_at(bag, slot);
}

bool extension_bag_handle_backpack_button_equip() {
    return virtual_bag_handle_backpack_button_equip();
}

bool extension_bag_handle_original_bag_unequip(bool* out_no_space) {
    return virtual_bag_handle_original_bag_unequip(out_no_space);
}

void extension_bag_show_no_space_popup() {
    // TextData 6 = "背包已满"（与扩展袋/原版卸袋同窗）。直接解析原版弹窗
    // 函数，不依赖 extension_bag 内部（show popup 位于其匿名 namespace）。
    if (g_base == 0) return;
    const uintptr_t popup = g_base + fn_resolve("F_UI_POPUP_MSG_CREATE_OK_FROM_TEXT_DATA_VMA",
                                                F_UI_POPUP_MSG_CREATE_OK_FROM_TEXT_DATA_VMA);
    if (popup == 0) return;
    typedef void (*UiPopupFn)(int, int, int, int);
    reinterpret_cast<UiPopupFn>(popup)(6, 0, 0, 0);
}

bool extension_bag_identify_native_item(void* item, int* out_bag, int* out_slot) {
    return virtual_bag_identify_native_item(item, out_bag, out_slot);
}

void* extension_bag_find_native_item(int category) {
    return virtual_bag_find_native_item(category);
}

bool extension_bag_remove_native_item(void* item) {
    return virtual_bag_remove_native_item(item);
}

int64_t extension_bag_sell_price(void* item, int count, bool apply_variant_discount) {
    if (item == nullptr || count <= 0 || fn_item_get_sell_price == nullptr) return -1;
    const int64_t unit_price = static_cast<int64_t>(fn_item_get_sell_price(item));
    const int64_t multiplier = static_cast<int64_t>(count) *
                               (apply_variant_discount ? 7 : 10);
    const int64_t max_value = std::numeric_limits<int64_t>::max();
    if (unit_price < 0 || multiplier <= 0 || unit_price > max_value / multiplier) return -1;
    // 原版商店为单位售价×数量；详情页粉碎改出售路径再乘 70%。
    return (unit_price * multiplier) / 10;
}

bool extension_bag_equip_projected_item(void* item, int source_bag, int source_slot, int equip_slot) {
    return virtual_bag_equip_projected_item(item, source_bag, source_slot, equip_slot);
}

void extension_bag_begin_internal_equip() {
    ++g_internal_equip_depth;
}

void extension_bag_end_internal_equip() {
    if (g_internal_equip_depth > 0) --g_internal_equip_depth;
}

bool extension_bag_internal_equip_active() {
    return g_internal_equip_depth > 0;
}

bool extension_bag_consume_native_item(void* item) {
    return virtual_bag_consume_native_item(item);
}

bool extension_bag_has_empty_slots(int needed, int include_task_bag) {
    return virtual_bag_has_empty_slots(needed, include_task_bag);
}

bool extension_bag_adopt_unequipped_item(void* character, int equip_slot) {
    return virtual_bag_adopt_unequipped_item(character, equip_slot);
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
