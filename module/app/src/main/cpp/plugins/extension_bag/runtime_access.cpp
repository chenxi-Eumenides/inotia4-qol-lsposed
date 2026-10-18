#include "game_ui_virtbag.h"

#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "contracts/ownership.h"
#include "contracts/inventory_refresh.h"
#include "contracts/item_facts.h"
#include "plugins/extension_bag/extension_bag_equip_rules.h"
#include "plugins/extension_bag/extension_bag_port.h"
#include "plugins/extension_bag/move_merge.h"
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
#include "plugins/extension_bag/extension_bag_logging.h"
#include "plugins/extension_bag/plugin.h"
#include "plugins/extension_bag/store_sell_rules.h"
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

#define current_env current_env_internal
#define extension_remove_item_direct extension_remove_item_direct_internal
#define extension_serialize_item_payload extension_serialize_item_payload_internal
#define extension_load_item_payload extension_load_item_payload_internal
#define extension_free_item_payload extension_free_item_payload_internal
#define extension_apply_stuff extension_apply_stuff_internal
#define read_item_cumulate_count read_item_cumulate_count_internal
#define read_save_is_ok read_save_is_ok_internal
#define read_has_empty_mercenary_slot read_has_empty_mercenary_slot_internal
#define extension_description_set_off extension_description_set_off_internal
#define extension_use_item extension_use_item_internal
#define extension_make_mercenary extension_make_mercenary_internal
#define extension_refresh_bag_area extension_refresh_bag_area_internal
#define item_count_encoding item_count_encoding_internal
#define original_count_view original_count_view_internal
#define category_is_equip category_is_equip_internal
#define category_is_extension_backpack category_is_extension_backpack_internal

bool extension_save_item_on_empty(void* item, int bag) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->inventory.save_item_on_empty == nullptr) return false;
    int32_t result = 0;
    return qol::contracts::succeeded(api->inventory.save_item_on_empty(item, bag, &result)) &&
           result != 0;
}

namespace {

bool extension_equip_from_inventory(void* character, int bag, int slot, int equip_slot) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->equipment.equip_from_inventory == nullptr) return false;
    int32_t result = 0;
    return qol::contracts::succeeded(
               api->equipment.equip_from_inventory(character, bag, slot, equip_slot, &result)) &&
           result != 0;
}

bool extension_apply_stuff(void* target_item, void* material) {
    const auto* api = extension_bag_write_api();
    return api != nullptr && api->equipment.apply_stuff != nullptr &&
           qol::contracts::succeeded(api->equipment.apply_stuff(target_item, material));
}

bool extension_use_item(void* character, void* item, int arg, int* out_used) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->equipment.use_item == nullptr || out_used == nullptr) return false;
    int32_t result = 0;
    const QolResult call = api->equipment.use_item(character, item, arg, &result);
    if (!qol::contracts::succeeded(call)) return false;
    *out_used = result;
    return true;
}

bool extension_make_mercenary(void* item, void** out_mercenary) {
    const auto* api = extension_bag_write_api();
    return api != nullptr && api->equipment.make_mercenary != nullptr &&
           qol::contracts::succeeded(api->equipment.make_mercenary(item, out_mercenary));
}

bool extension_add_money(int64_t amount) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->equipment.add_money == nullptr) return false;
    int32_t result = 0;
    return qol::contracts::succeeded(api->equipment.add_money(amount, &result)) && result != 0;
}

bool extension_minus_money(int64_t amount) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->equipment.minus_money == nullptr) return false;
    int32_t result = 0;
    return qol::contracts::succeeded(api->equipment.minus_money(amount, &result)) && result != 0;
}

bool extension_move_item(void* item, int count, int bag, int slot, int* out_result) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->inventory.move_item == nullptr || out_result == nullptr) return false;
    int32_t result = 0;
    const QolResult call = api->inventory.move_item(item, count, bag, slot, &result);
    if (!qol::contracts::succeeded(call)) return false;
    *out_result = result;
    return true;
}

bool extension_remove_item_direct(int bag, int slot, int* out_result) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->inventory.remove_item_direct == nullptr || out_result == nullptr) {
        return false;
    }
    int32_t result = 0;
    const QolResult call = api->inventory.remove_item_direct(bag, slot, &result);
    if (!qol::contracts::succeeded(call)) return false;
    *out_result = result;
    return true;
}

}  // namespace

bool extension_free_item(void* item) {
    const auto* api = extension_bag_write_api();
    return api != nullptr && api->inventory.free_item != nullptr &&
           qol::contracts::succeeded(api->inventory.free_item(item));
}

namespace {

bool extension_serialize_item(uint8_t* payload, int payload_capacity, void* item, int* out_size) {
    const auto* api = extension_bag_write_api();
    return api != nullptr && api->inventory.serialize_item != nullptr && out_size != nullptr &&
           qol::contracts::succeeded(
               api->inventory.serialize_item(payload, payload_capacity, item, out_size));
}

void* extension_load_item(const uint8_t* payload, int payload_size, int* out_consumed) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->inventory.load_item == nullptr || out_consumed == nullptr) {
        return nullptr;
    }
    void* item = nullptr;
    const QolResult result =
        api->inventory.load_item(payload, payload_size, &item, out_consumed);
    if (qol::contracts::succeeded(result)) return item;
    if (item != nullptr) (void)extension_free_item(item);
    return nullptr;
}

int extension_serialize_item_payload(uint8_t* payload, void* item) {
    int size = 0;
    return extension_serialize_item(payload, virtual_bag::kSerializedItemProbeBuffer, item, &size)
               ? size
               : 0;
}

int extension_load_item_payload(const uint8_t* payload, void** out_item, int* out_consumed) {
    if (out_item == nullptr || out_consumed == nullptr) return 0;
    *out_item = extension_load_item(payload, virtual_bag::kSerializedItemBuffer, out_consumed);
    return *out_item == nullptr ? 0 : 1;
}

void extension_free_item_payload(void* item) { (void)extension_free_item(item); }

void extension_refresh_item_area() {
    const auto* api = extension_bag_write_api();
    if (api != nullptr && api->ui.refresh_item_area != nullptr) {
        (void)api->ui.refresh_item_area();
    }
}

void extension_refresh_bag_area() {
    const auto* api = extension_bag_write_api();
    if (api != nullptr && api->ui.refresh_bag_area != nullptr) {
        (void)api->ui.refresh_bag_area();
    }
}

void extension_refresh_mix_item() {
    const auto* api = extension_bag_write_api();
    if (api != nullptr && api->ui.refresh_mix_item != nullptr) {
        (void)api->ui.refresh_mix_item();
    }
}

void extension_refresh_store_item() {
    const auto* api = extension_bag_write_api();
    if (api != nullptr && api->ui.refresh_store_item != nullptr) {
        (void)api->ui.refresh_store_item();
    }
}

void extension_update_character_equip() {
    const auto* api = extension_bag_write_api();
    if (api != nullptr && api->ui.update_character_equip != nullptr) {
        (void)api->ui.update_character_equip();
    }
}

void extension_description_set_off() {
    const auto* api = extension_bag_write_api();
    if (api != nullptr && api->ui.description_set_off != nullptr) {
        (void)api->ui.description_set_off();
    }
}

bool read_item_cumulate_count(const void* item, int* out_count) {
    qol::contracts::QolItemHandle handle{};
    if (g_item_facts == nullptr || g_item_facts->cumulate_count == nullptr ||
        item == nullptr || out_count == nullptr ||
        !extension_bag_item_handle_for_pointer(const_cast<void*>(item), &handle)) return false;
    int32_t count = 0;
    if (!qol::contracts::succeeded(
            g_item_facts->cumulate_count(handle, &count))) return false;
    *out_count = count;
    return true;
}

}  // namespace

bool read_item_is_jewel(int category) {
    if (g_item_facts == nullptr || g_item_facts->is_jewel_category == nullptr) return false;
    int32_t is_jewel = 0;
    return qol::contracts::succeeded(
               g_item_facts->is_jewel_category(category, &is_jewel)) &&
           is_jewel != 0;
}

namespace {

bool read_item_category_predicate(int category,
                                  QolResult (*predicate)(int32_t, int32_t*),
                                  bool* out_value) {
    if (g_game == nullptr || predicate == nullptr || out_value == nullptr) return false;
    int32_t value = 0;
    if (!qol::contracts::succeeded(predicate(category, &value))) return false;
    *out_value = value != 0;
    return true;
}

}

bool item_is_equip(const void* item) {
    qol::contracts::QolItemHandle handle{};
    if (g_item_facts == nullptr || g_item_facts->is_real_equip == nullptr || item == nullptr ||
        !extension_bag_item_handle_for_pointer(const_cast<void*>(item), &handle)) {
        return false;
    }
    int32_t value = 0;
    return qol::contracts::succeeded(g_item_facts->is_real_equip(handle, &value)) &&
           value != 0;
}

bool read_item_is_enchant_scroll(int category) {
    bool value = false;
    return read_item_category_predicate(
        category, g_game == nullptr ? nullptr : g_game->read_only.item.is_enchant_scroll, &value) &&
        value;
}

bool read_item_is_mercenary_seal(int category) {
    bool value = false;
    return read_item_category_predicate(
        category, g_game == nullptr ? nullptr : g_game->read_only.item.is_mercenary_seal, &value) &&
        value;
}

bool read_item_is_usable(int category) {
    bool value = false;
    return read_item_category_predicate(
        category, g_game == nullptr ? nullptr : g_game->read_only.item.is_usable, &value) &&
        value;
}

bool read_world_state_id(int* out_state) {
    if (out_state == nullptr) return false;
    const auto* facts = qol::features::extension_bag::world_facts();
    if (facts == nullptr || facts->state == nullptr) return false;
    int32_t state = -1;
    if (!qol::contracts::succeeded(facts->state(&state))) return false;
    *out_state = state;
    return true;
}

bool read_world_in_world() {
    int state = -1;
    return read_world_state_id(&state) && state == 5;
}

namespace {

bool read_item_sell_price(const void* item, int64_t* out_price) {
    qol::contracts::QolItemHandle handle{};
    if (g_item_facts == nullptr || g_item_facts->sell_price == nullptr ||
        item == nullptr || out_price == nullptr ||
        !extension_bag_item_handle_for_pointer(const_cast<void*>(item), &handle)) return false;
    return qol::contracts::succeeded(g_item_facts->sell_price(handle, out_price));
}

bool read_item_encoded_field(const void* item, uint32_t* out_field) {
    const auto* api = item_count_api();
    if (api == nullptr || api->read_encoded_field == nullptr || item == nullptr ||
        out_field == nullptr) return false;
    return qol::contracts::succeeded(api->read_encoded_field(item, out_field));
}

qol::contracts::QolItemCountEncoding item_count_encoding(const void* item) {
    const auto* api = item_count_api();
    if (api == nullptr || api->is_encoded == nullptr || item == nullptr) {
        return qol::contracts::QolItemCountEncoding::kUnknown;
    }
    qol::contracts::QolItemCountEncoding encoding =
        qol::contracts::QolItemCountEncoding::kUnknown;
    if (!qol::contracts::succeeded(api->is_encoded(item, &encoding))) {
        return qol::contracts::QolItemCountEncoding::kUnknown;
    }
    return encoding;
}

bool read_char_can_equip(const void* character, const void* item) {
    if (g_game == nullptr || g_character_extras == nullptr || g_character_extras->read.can_equip == nullptr) return false;
    int32_t allowed = 0;
    return qol::contracts::succeeded(
               g_character_extras->read.can_equip(character, item, &allowed)) &&
           allowed != 0;
}

bool read_char_find_equip_slot(const void* character, const void* item, int* out_slot) {
    if (g_game == nullptr || g_character_extras == nullptr || g_character_extras->read.find_equip_slot == nullptr ||
        out_slot == nullptr) return false;
    int32_t slot = -1;
    if (!qol::contracts::succeeded(
            g_character_extras->read.find_equip_slot(character, item, &slot))) return false;
    *out_slot = slot;
    return true;
}

void* read_char_equipped_item(const void* character, int slot) {
    if (g_game == nullptr || g_character_extras == nullptr || g_character_extras->read.equipped_item == nullptr) return nullptr;
    void* item = nullptr;
    return qol::contracts::succeeded(
               g_character_extras->read.equipped_item(character, slot, &item))
               ? item
               : nullptr;
}

bool read_save_is_ok() {
    const auto* facts = qol::features::extension_bag::save_facts();
    if (facts == nullptr || facts->is_ok == nullptr) return false;
    int32_t value = 0;
    return qol::contracts::succeeded(facts->is_ok(&value)) && value != 0;
}

bool read_has_empty_mercenary_slot() {
    if (g_game == nullptr || g_character_extras == nullptr || g_character_extras->read.has_empty_mercenary_slot == nullptr) {
        return false;
    }
    int32_t value = 0;
    return qol::contracts::succeeded(
               g_character_extras->read.has_empty_mercenary_slot(&value)) && value != 0;
}

bool read_inventory_bag_size(int bag, int* out_size) {
    if (g_game == nullptr || g_inventory_extras == nullptr || g_inventory_extras->read.bag_size == nullptr ||
        out_size == nullptr) return false;
    int32_t value = 0;
    if (!qol::contracts::succeeded(g_inventory_extras->read.bag_size(bag, &value))) return false;
    *out_size = value;
    return true;
}

bool read_inventory_is_empty_bag(int bag) {
    if (g_game == nullptr || g_inventory_extras == nullptr || g_inventory_extras->read.is_empty_bag == nullptr) return false;
    int32_t value = 0;
    return qol::contracts::succeeded(
               g_inventory_extras->read.is_empty_bag(bag, &value)) && value != 0;
}

bool read_inventory_find_save_slot(const void* item, int* out_slot) {
    if (g_game == nullptr || g_inventory_extras == nullptr || g_inventory_extras->read.find_save_slot == nullptr ||
        item == nullptr || out_slot == nullptr) return false;
    int32_t value = -1;
    if (!qol::contracts::succeeded(
            g_inventory_extras->read.find_save_slot(item, &value))) return false;
    *out_slot = value;
    return true;
}

int read_current_save_slot() {
    const auto* facts = qol::features::extension_bag::save_facts();
    if (facts == nullptr || facts->current_slot == nullptr) return -1;
    int32_t slot = -1;
    return qol::contracts::succeeded(facts->current_slot(&slot)) ? slot : -1;
}


}  // namespace

int raw_direct_bag_locked() {
    if (g_game == nullptr || g_write_port->ui.current_bag == nullptr) return -1;
    int32_t bag = -1;
    return qol::contracts::succeeded(g_write_port->ui.current_bag(&bag)) ? bag : -1;
}

int raw_got_bag_locked() {
    return raw_direct_bag_locked();
}

int raw_desc_type_locked() {
    if (g_game == nullptr || g_write_port->ui.desc_type == nullptr) return -1;
    int32_t type = -1;
    return qol::contracts::succeeded(g_write_port->ui.desc_type(&type)) ? type : -1;
}

namespace {
JNIEnv* current_env() {
    JavaVM* jvm = g_jvm();
    if (jvm == nullptr) return nullptr;
    JNIEnv* env = nullptr;
    if (jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    return jvm->AttachCurrentThread(&env, nullptr) == JNI_OK ? env : nullptr;
}
::qol::contracts::QolItemCountEncoding category_is_equip(int category) {
    if (g_game == nullptr || g_game->read_only.item.count_encoding == nullptr || category <= 0) {
        return ::qol::contracts::QolItemCountEncoding::kUnknown;
    }
    ::qol::contracts::QolItemCountEncoding encoding =
        ::qol::contracts::QolItemCountEncoding::kUnknown;
    return qol::contracts::succeeded(
               g_game->read_only.item.count_encoding(category, &encoding))
        ? encoding
        : ::qol::contracts::QolItemCountEncoding::kUnknown;
}

// 原版 ButtonEquipExe 的背包身份判定（反汇编 b7c74）：ITEMCLASSBASE 记录
// +2 == 0x1f 即背包类。category 只是 ITEMDATABASE 索引，不能承担装备语义；
// +2 是记录字段偏移（非 VMA），表基址/步长由宿主读取面提供。
// G_ITEMCLASS_DATA/SIZE GOT 槽。
bool category_is_extension_backpack(int category) {
    if (g_game == nullptr || g_game->read_only.item.is_backpack_category == nullptr ||
        category <= 0) return false;
    int32_t value = 0;
    return qol::contracts::succeeded(
               g_game->read_only.item.is_backpack_category(category, &value)) && value != 0;
}
uint32_t original_count_view(uint32_t field) {
    return virtual_bag_view_from_canonical(virtual_bag_canonical_from_field(field, 0), false);
}
}  // namespace

#undef current_env
#undef isolation_now_ms_locked
#undef extension_apply_stuff
#undef read_item_cumulate_count
#undef read_save_is_ok
#undef read_has_empty_mercenary_slot
#undef extension_description_set_off
#undef extension_use_item
#undef extension_make_mercenary
#undef extension_refresh_bag_area
#undef item_count_encoding
#undef original_count_view
#undef category_is_equip
#undef category_is_extension_backpack
#undef handle_extension_tab_click_locked
#undef extension_remove_item_direct
#undef extension_serialize_item_payload
#undef extension_load_item_payload
#undef extension_free_item_payload

bool extension_remove_item_direct(int bag, int slot, int* out_result) {
    return extension_remove_item_direct_internal(bag, slot, out_result);
}
int extension_serialize_item_payload(uint8_t* payload, void* item) {
    return extension_serialize_item_payload_internal(payload, item);
}
int extension_load_item_payload(const uint8_t* payload, void** out_item, int* out_consumed) {
    return extension_load_item_payload_internal(payload, out_item, out_consumed);
}
void extension_free_item_payload(void* item) { extension_free_item_payload_internal(item); }

JNIEnv* current_env() { return current_env_internal(); }
bool extension_apply_stuff(void* target_item, void* material) {
    return extension_apply_stuff_internal(target_item, material);
}
bool read_item_cumulate_count(const void* item, int* out_count) {
    return read_item_cumulate_count_internal(item, out_count);
}
bool read_save_is_ok() { return read_save_is_ok_internal(); }
bool read_has_empty_mercenary_slot() { return read_has_empty_mercenary_slot_internal(); }
void extension_description_set_off() { extension_description_set_off_internal(); }
bool extension_use_item(void* character, void* item, int arg, int* out_used) {
    return extension_use_item_internal(character, item, arg, out_used);
}
bool extension_make_mercenary(void* item, void** out_mercenary) {
    return extension_make_mercenary_internal(item, out_mercenary);
}
void extension_refresh_bag_area() { extension_refresh_bag_area_internal(); }
qol::contracts::QolItemCountEncoding item_count_encoding(const void* item) {
    return item_count_encoding_internal(item);
}
uint32_t original_count_view(uint32_t field) { return original_count_view_internal(field); }
qol::contracts::QolItemCountEncoding category_is_equip(int category) {
    return category_is_equip_internal(category);
}
bool category_is_extension_backpack(int category) {
    return category_is_extension_backpack_internal(category);
}
uint64_t extension_bag_ui_isolation_now_ms_locked() {
    return isolation_now_ms_locked();
}
bool extension_bag_ui_equip_from_inventory(void* character, int bag, int slot, int equip_slot) {
    return extension_equip_from_inventory(character, bag, slot, equip_slot);
}
bool extension_bag_ui_add_money(int64_t amount) { return extension_add_money(amount); }
bool extension_bag_ui_minus_money(int64_t amount) { return extension_minus_money(amount); }
void extension_bag_ui_update_character_equip() { extension_update_character_equip(); }
void extension_bag_ui_description_set_off() { extension_description_set_off(); }
bool extension_bag_ui_read_item_cumulate_count(const void* item, int* out_count) {
    return read_item_cumulate_count(item, out_count);
}
bool extension_bag_ui_read_item_sell_price(const void* item, int64_t* out_price) {
    return read_item_sell_price(item, out_price);
}
bool extension_bag_ui_read_item_encoded_field(const void* item, uint32_t* out_field) {
    return read_item_encoded_field(item, out_field);
}
qol::contracts::QolItemCountEncoding extension_bag_ui_item_count_encoding(const void* item) {
    return item_count_encoding(item);
}
bool extension_bag_ui_read_char_can_equip(const void* character, const void* item) {
    return read_char_can_equip(character, item);
}
bool extension_bag_ui_read_char_find_equip_slot(const void* character, const void* item,
                                                int* out_slot) {
    return read_char_find_equip_slot(character, item, out_slot);
}
void* extension_bag_ui_read_char_equipped_item(const void* character, int slot) {
    return read_char_equipped_item(character, slot);
}
bool extension_bag_ui_use_item(void* character, void* item, int arg, int* out_used) {
    return extension_use_item(character, item, arg, out_used);
}
bool extension_bag_ui_make_mercenary(void* item, void** out_mercenary) {
    return extension_make_mercenary(item, out_mercenary);
}
bool extension_bag_ui_read_save_is_ok() { return read_save_is_ok(); }
bool extension_bag_ui_read_has_empty_mercenary_slot() {
    return read_has_empty_mercenary_slot();
}
