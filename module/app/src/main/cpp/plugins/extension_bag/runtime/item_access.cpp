#include "plugins/extension_bag/runtime/item_access.h"

#include "plugins/extension_bag/runtime/extension_bag_port.h"
#include "plugins/extension_bag/runtime/inventory_access.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/runtime/state.h"

#include <cstdint>

bool read_item_category(const void* item, int* out_category) {
    qol::contracts::QolItemHandle handle{};
    if (runtime_state().host.item_facts == nullptr || runtime_state().host.item_facts->category == nullptr ||
        item == nullptr || out_category == nullptr ||
        !extension_bag_item_handle_for_pointer(const_cast<void*>(item), &handle)) return false;
    int32_t category = 0;
    if (!qol::contracts::succeeded(
            runtime_state().host.item_facts->category(handle, &category))) return false;
    *out_category = category;
    return true;
}

bool read_item_type(const void* item, qol::contracts::QolItemType* out_type) {
    qol::contracts::QolItemHandle handle{};
    const auto* facts = runtime_state().host.item_facts;
    if (facts == nullptr || facts->item_type == nullptr || item == nullptr || out_type == nullptr ||
        !extension_bag_item_handle_for_pointer(const_cast<void*>(item), &handle)) {
        return false;
    }
    qol::contracts::QolItemType type = qol::contracts::QolItemType::kUnknown;
    if (!qol::contracts::succeeded(facts->item_type(handle, &type))) return false;
    *out_type = type;
    return true;
}

int original_item_category_locked(void* item) {
    if (item == nullptr) return 0;
    int category = 0;
    return read_item_category(item, &category) ? category : 0;
}

void* extension_create_item(int category, int arg1, int arg2, int arg3) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->inventory.create_item == nullptr) return nullptr;
    void* item = nullptr;
    return qol::contracts::succeeded(
               api->inventory.create_item(category, arg1, arg2, arg3, &item))
               ? item
               : nullptr;
}

const qol::contracts::QolItemCountApi* item_count_api() {
    return runtime_state().host.game == nullptr ? nullptr : &runtime_state().host.game->item_count;
}

bool mark_native_bag_object(void* item) {
    const auto* api = item_count_api();
    if (api == nullptr || api->read_encoded_field == nullptr ||
        api->write_encoded_field == nullptr || item == nullptr) return false;
    uint32_t field = 0;
    if (!qol::contracts::succeeded(api->read_encoded_field(item, &field))) return false;
    field = qol::features::extension_bag::write_native_bag_object_marker(field);
    return qol::contracts::succeeded(api->write_encoded_field(item, field));
}

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

bool extension_open_item_box(int category) {
    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->equipment.open_item_box == nullptr) return false;
    int32_t result = 0;
    return qol::contracts::succeeded(api->equipment.open_item_box(category, &result)) &&
           result != 0;
}

bool extension_consume_item(void* item) {
    const auto* api = extension_bag_write_api();
    return api != nullptr && api->equipment.consume_item != nullptr &&
           qol::contracts::succeeded(api->equipment.consume_item(item));
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

bool extension_free_item(void* item) {
    const auto* api = extension_bag_write_api();
    return api != nullptr && api->inventory.free_item != nullptr &&
           qol::contracts::succeeded(api->inventory.free_item(item));
}

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



namespace {

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

}  // namespace

void extension_description_set_off() {
    const auto* api = extension_bag_write_api();
    if (api != nullptr && api->ui.description_set_off != nullptr) {
        (void)api->ui.description_set_off();
    }
}

bool read_item_cumulate_count(const void* item, int* out_count) {
    qol::contracts::QolItemHandle handle{};
    if (runtime_state().host.item_facts == nullptr || runtime_state().host.item_facts->cumulate_count == nullptr ||
        item == nullptr || out_count == nullptr ||
        !extension_bag_item_handle_for_pointer(const_cast<void*>(item), &handle)) return false;
    int32_t count = 0;
    if (!qol::contracts::succeeded(
            runtime_state().host.item_facts->cumulate_count(handle, &count))) return false;
    *out_count = count;
    return true;
}

bool item_type_is(const void* item, qol::contracts::QolItemType expected) {
    qol::contracts::QolItemType type = qol::contracts::QolItemType::kUnknown;
    return read_item_type(item, &type) && type == expected;
}

bool read_item_is_usable(int category) {
    bool value = false;
    const auto* game = runtime_state().host.game;
    if (game == nullptr || game->read_only.item.is_usable == nullptr) return false;
    int32_t raw_value = 0;
    if (!qol::contracts::succeeded(game->read_only.item.is_usable(category, &raw_value))) return false;
    value = raw_value != 0;
    return value;
}



namespace {

bool read_item_sell_price(const void* item, int64_t* out_price) {
    qol::contracts::QolItemHandle handle{};
    if (runtime_state().host.item_facts == nullptr || runtime_state().host.item_facts->sell_price == nullptr ||
        item == nullptr || out_price == nullptr ||
        !extension_bag_item_handle_for_pointer(const_cast<void*>(item), &handle)) return false;
    return qol::contracts::succeeded(runtime_state().host.item_facts->sell_price(handle, out_price));
}

bool read_item_encoded_field(const void* item, uint32_t* out_field) {
    const auto* api = item_count_api();
    if (api == nullptr || api->read_encoded_field == nullptr || item == nullptr ||
        out_field == nullptr) return false;
    return qol::contracts::succeeded(api->read_encoded_field(item, out_field));
}

}  // namespace

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

namespace {

bool read_char_can_equip(const void* character, const void* item) {
    if (runtime_state().host.game == nullptr || runtime_state().host.character_extras == nullptr || runtime_state().host.character_extras->read.can_equip == nullptr) return false;
    int32_t allowed = 0;
    return qol::contracts::succeeded(
               runtime_state().host.character_extras->read.can_equip(character, item, &allowed)) &&
           allowed != 0;
}

bool read_char_find_equip_slot(const void* character, const void* item, int* out_slot) {
    if (runtime_state().host.game == nullptr || runtime_state().host.character_extras == nullptr || runtime_state().host.character_extras->read.find_equip_slot == nullptr ||
        out_slot == nullptr) return false;
    int32_t slot = -1;
    if (!qol::contracts::succeeded(
            runtime_state().host.character_extras->read.find_equip_slot(character, item, &slot))) return false;
    *out_slot = slot;
    return true;
}

void* read_char_equipped_item(const void* character, int slot) {
    if (runtime_state().host.game == nullptr || runtime_state().host.character_extras == nullptr || runtime_state().host.character_extras->read.equipped_item == nullptr) return nullptr;
    void* item = nullptr;
    return qol::contracts::succeeded(
               runtime_state().host.character_extras->read.equipped_item(character, slot, &item))
                ? item
                : nullptr;
}

}  // namespace

bool read_save_is_ok() {
    const auto* facts = extension_bag_save_facts();
    if (facts == nullptr || facts->is_ok == nullptr) return false;
    int32_t value = 0;
    return qol::contracts::succeeded(facts->is_ok(&value)) && value != 0;
}

bool read_has_empty_mercenary_slot() {
    if (runtime_state().host.game == nullptr || runtime_state().host.character_extras == nullptr || runtime_state().host.character_extras->read.has_empty_mercenary_slot == nullptr) {
        return false;
    }
    int32_t value = 0;
    return qol::contracts::succeeded(
               runtime_state().host.character_extras->read.has_empty_mercenary_slot(&value)) && value != 0;
}
JNIEnv* current_env() {
    JavaVM* jvm = g_jvm();
    if (jvm == nullptr) return nullptr;
    JNIEnv* env = nullptr;
    if (jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
#if defined(__ANDROID__)
    return jvm->AttachCurrentThread(&env, nullptr) == JNI_OK ? env : nullptr;
#else
    return jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) == JNI_OK ? env : nullptr;
#endif
}
::qol::contracts::QolItemCountEncoding category_is_equip(int category) {
    if (runtime_state().host.game == nullptr || runtime_state().host.game->read_only.item.count_encoding == nullptr || category <= 0) {
        return ::qol::contracts::QolItemCountEncoding::kUnknown;
    }
    ::qol::contracts::QolItemCountEncoding encoding =
        ::qol::contracts::QolItemCountEncoding::kUnknown;
    return qol::contracts::succeeded(
               runtime_state().host.game->read_only.item.count_encoding(category, &encoding))
        ? encoding
        : ::qol::contracts::QolItemCountEncoding::kUnknown;
}

uint32_t original_count_view(uint32_t field) {
    return virtual_bag_view_from_canonical(virtual_bag_canonical_from_field(field, 0), false);
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

::qol::contracts::QolItemCountEncoding virtual_bag_category_uses_stack_count(int category) {
    return category_is_equip(category);
}

uint32_t virtual_bag_effective_count(const void* item, uint32_t fallback_canonical) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (item != nullptr) {
        if (api != nullptr && api->effective != nullptr) {
            uint32_t count = 0;
            const QolResult result = api->effective(item, &count);
            if (qol::contracts::succeeded(result)) {
                if (count != 0 || fallback_canonical == 0) return count;
                return virtual_bag_view_from_canonical(fallback_canonical, true);
            }
        }
        uint32_t field = 0;
        if (api != nullptr && api->read_encoded_field != nullptr &&
            qol::contracts::succeeded(api->read_encoded_field(item, &field))) {
            const uint32_t view = original_count_view(field);
            if (view != 0) return view;
        }
    }
    return virtual_bag_view_from_canonical(fallback_canonical, true);
}

uint32_t virtual_bag_canonical_from_field(uint32_t field, uint32_t fallback_canonical) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (api != nullptr && api->canonical_from_field != nullptr) {
        uint32_t canonical = fallback_canonical;
        if (qol::contracts::succeeded(api->canonical_from_field(field, &canonical))) {
            return canonical;
        }
    }
    return fallback_canonical;
}

uint32_t virtual_bag_field_from_canonical(uint32_t field, uint32_t canonical, bool extended) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (api != nullptr && api->field_from_canonical != nullptr) {
        uint32_t updated = field;
        if (qol::contracts::succeeded(
                api->field_from_canonical(field, canonical, extended ? 1 : 0, &updated))) {
            return updated;
        }
    }
    return field;
}

uint32_t virtual_bag_view_from_canonical(uint32_t canonical, bool extended) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (api != nullptr && api->view_from_canonical != nullptr) {
        uint32_t view = canonical;
        if (qol::contracts::succeeded(
                api->view_from_canonical(canonical, extended ? 1 : 0, &view))) {
            return view;
        }
    }
    return canonical;
}

uint32_t virtual_bag_count_limit(bool extended) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (api != nullptr && api->view_limit != nullptr) {
        uint32_t limit = 0;
        if (qol::contracts::succeeded(api->view_limit(extended ? 1 : 0, &limit))) return limit;
    }
    return 0;
}

uint32_t virtual_bag_canonical_count(const void* item, uint32_t fallback_canonical) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (api != nullptr && api->canonical != nullptr && item != nullptr) {
        uint32_t count = 0;
        const QolResult result = api->canonical(item, &count);
        if (qol::contracts::succeeded(result)) return count;
        if (result.code == kQolErrUnsupported) return fallback_canonical;
    }
    if (item == nullptr) return fallback_canonical;
    uint32_t field = 0;
    if (api == nullptr || api->read_encoded_field == nullptr ||
        !qol::contracts::succeeded(api->read_encoded_field(item, &field))) {
        return fallback_canonical;
    }
    return virtual_bag_canonical_from_field(field, fallback_canonical);
}

bool virtual_bag_extended_mode(const void* item) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (item == nullptr || api == nullptr || api->read_extended_mode == nullptr) return false;
    uint8_t extended = 0;
    if (!qol::contracts::succeeded(api->read_extended_mode(&extended)) || extended == 0 ||
        api->effective == nullptr) return false;
    uint32_t effective = 0;
    return qol::contracts::succeeded(api->effective(item, &effective));
}

bool virtual_bag_write_effective(void* item, uint32_t count) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (api == nullptr || api->write_effective == nullptr ||
        api->writeback_needed == nullptr || item == nullptr) return false;
    auto direct_write = [&]() {
        if (api->read_encoded_field == nullptr || api->field_from_canonical == nullptr ||
            api->write_encoded_field == nullptr) return false;
        uint32_t raw = 0;
        if (!qol::contracts::succeeded(api->read_encoded_field(item, &raw))) return false;
        const int32_t extended = virtual_bag_extended_mode(item) ? 1 : 0;
        uint32_t field = 0;
        if (!qol::contracts::succeeded(
                api->field_from_canonical(raw, count, extended, &field))) {
            return false;
        }
        return qol::contracts::succeeded(api->write_encoded_field(item, field));
    };
    int32_t needed = 0;
    const QolResult needed_result = api->writeback_needed(item, count, &needed);
    if (!qol::contracts::succeeded(needed_result)) {
        // ApplyStuff can run while the extended-count facade reports NotReady;
        // fall back to the encoded field write so a successful native apply
        // still consumes the extension material.
        return direct_write();
    }
    if (needed == 0) return true;
    const QolResult write_result = api->write_effective(item, count);
    if (qol::contracts::succeeded(write_result)) return true;
    return direct_write();
}
