#include "plugins/extension_bag/extension_bag_port.h"
#include "plugins/extension_bag/extension_bag_bridge.h"
#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/api/port_internal.h"
#include "contracts/inventory_extension.h"
#include "contracts/sell_price.h"
#include "contracts/host_sdk.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

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
    if (qol::features::extension_bag::api::detail::g_sdk == nullptr || qol::features::extension_bag::api::detail::g_sdk->ui == nullptr || qol::features::extension_bag::api::detail::g_sdk->ui->show_text_popup == nullptr) return;
    (void)qol::features::extension_bag::api::detail::g_sdk->ui->show_text_popup(6);
}

void extension_bag_show_not_empty_popup() {
    // TextData 7 = "袋非空"（原版 b8084 同窗）。
    if (qol::features::extension_bag::api::detail::g_sdk == nullptr || qol::features::extension_bag::api::detail::g_sdk->ui == nullptr || qol::features::extension_bag::api::detail::g_sdk->ui->show_text_popup == nullptr) return;
    (void)qol::features::extension_bag::api::detail::g_sdk->ui->show_text_popup(7);
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
