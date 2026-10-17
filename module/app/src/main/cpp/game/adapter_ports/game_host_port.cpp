#include "game/adapter_ports/game_host_port.h"

#include "game/adapter_ports/system_port.h"
#include "game/adapter_ports/world_action_port.h"

#include "contracts/game_ports/ui_write_port.h"
#include "contracts/game_ports/ports.h"

#include <atomic>
#include <cstring>
#include <pthread.h>
#include <string>

#include "game/access/ui_state.h"
#include "game/access/game_access.h"
#include "game/access/item_handle_registry.h"
#include "game/access/attr_range_ui.h"
#include "game/access/char.h"
#include "game/access/craft.h"
#include "game/access/item_class.h"
#include "game/access/inventory.h"
#include "game/access/symbols.h"
#include "game/access/ui_choice.h"
#include "game/access/world.h"
#include "game/clock.h"
#include "game/state/game_state.h"
#include "game/state/ui_http.h"
#include "game/feature/game_feature.h"
#include "game/variants/game_variant.h"

namespace qol::game {
namespace {

using QolResult = ::QolResult;

constexpr const char* kInvalidArgument = "invalid argument";
constexpr const char* kNotReady = "not ready";
constexpr const char* kUnsupported = "unsupported";
constexpr uint32_t kUiResultStateMaxAttempts = 60;

struct PendingUiResult {
    uint64_t operation_id = 0;
    int32_t kind = -1;
    std::string panel;
    std::string operation_json;
};

PendingUiResult g_pending_ui_result;

pthread_t g_main_thread{};
std::atomic<bool> g_main_thread_set{false};

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult not_ready() { return error_result(kQolErrNotReady, kNotReady); }
QolResult unsupported() { return error_result(kQolErrUnsupported, kUnsupported); }

QolResult ui_write_legacy(int32_t kind, const char* panel, char* out, uint32_t capacity,
                          uint32_t* out_size) {
    if (!game_host_is_main_thread()) return error_result(kQolErrBlocked, "wrong thread");
    if (out == nullptr || out_size == nullptr || capacity == 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (build_ui_write_result_json(kind, panel, out, capacity, out_size) == kUiWriteFailed) {
        constexpr char kPendingBody[] = "{\"ok\":false,\"error\":\"panel close pending\"}";
        if (*out_size == sizeof(kPendingBody) - 1 &&
            std::memcmp(out, kPendingBody, sizeof(kPendingBody) - 1) == 0) {
            return error_result(kQolErrNotReady, "panel close pending");
        }
        return error_result(kQolErrInternal, "ui write failed");
    }
    return contracts::ok();
}

QolResult ui_write_step(uint64_t operation_id, uint32_t attempt, int32_t kind,
                        const char* panel, char* out, uint32_t capacity,
                        uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (operation_id == 0 || out == nullptr || out_size == nullptr || capacity == 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!game_host_is_main_thread()) return error_result(kQolErrBlocked, "wrong thread");

    const std::string requested_panel = panel != nullptr ? panel : "";
    if (g_pending_ui_result.operation_id != 0 &&
        g_pending_ui_result.operation_id != operation_id) {
        return error_result(kQolErrNotReady, "another UI result is pending");
    }

    if (g_pending_ui_result.operation_id == operation_id) {
        if (g_pending_ui_result.kind != kind || g_pending_ui_result.panel != requested_panel) {
            g_pending_ui_result = PendingUiResult{};
            return error_result(kQolErrInvalidArgument, "invalid UI write operation");
        }
    } else {
        const QolResult stepped = ui_state_write_step(operation_id, attempt, kind, panel, out,
                                                      capacity, out_size);
        if (!contracts::succeeded(stepped)) return stepped;
        if (out == nullptr || out_size == nullptr || *out_size == 0 || *out_size > capacity) {
            return error_result(kQolErrInternal, "invalid UI write result");
        }
        // Presence of this record means the UI action is applied; retries only fetch its state.
        g_pending_ui_result.operation_id = operation_id;
        g_pending_ui_result.kind = kind;
        g_pending_ui_result.panel = requested_panel;
        g_pending_ui_result.operation_json.assign(out, *out_size);
    }

    const std::string& operation = g_pending_ui_result.operation_json;
    if (operation.empty() || operation.back() != '}' ||
        operation.find("\"ok\":false") != std::string::npos) {
        if (operation.size() > capacity) {
            g_pending_ui_result = PendingUiResult{};
            return error_result(kQolErrInternal, "UI write result too large");
        }
        std::memcpy(out, operation.data(), operation.size());
        *out_size = static_cast<uint32_t>(operation.size());
        g_pending_ui_result = PendingUiResult{};
        return contracts::ok();
    }

    const bool player_state = kind == contracts::kQolUiWriteMainMenu;
    std::string state;
    const QolResult state_result = ui_write_result_state_json(kind, &state);
    if (state_result.code == kQolErrNotReady) {
        if (attempt < kUiResultStateMaxAttempts - 1) {
            *out_size = 0;
            return error_result(kQolErrNotReady, "UI result state not ready");
        }
        constexpr char kStateFailure[] = "{\"ok\":false,\"error\":\"not ready\"}";
        if (capacity < sizeof(kStateFailure) - 1) {
            g_pending_ui_result = PendingUiResult{};
            return error_result(kQolErrInternal, "UI result state unavailable");
        }
        std::memcpy(out, kStateFailure, sizeof(kStateFailure) - 1);
        *out_size = sizeof(kStateFailure) - 1;
        g_pending_ui_result = PendingUiResult{};
        return error_result(kQolErrInternal, "UI result state unavailable");
    }
    if (!contracts::succeeded(state_result)) {
        g_pending_ui_result = PendingUiResult{};
        return error_result(kQolErrInternal, "UI result state unavailable");
    }

    const std::string body =
        operation.substr(0, operation.size() - 1) + ",\"state\":" + state + "}";
    if (body.size() > capacity) {
        g_pending_ui_result = PendingUiResult{};
        return error_result(kQolErrInternal, "UI write result too large");
    }
    std::memcpy(out, body.data(), body.size());
    *out_size = static_cast<uint32_t>(body.size());
    g_pending_ui_result = PendingUiResult{};
    return contracts::ok();
}

QolResult ui_write_cancel_pending(uint64_t operation_id) {
    if (operation_id == 0 || g_pending_ui_result.operation_id == operation_id) {
        g_pending_ui_result = PendingUiResult{};
    }
    return ui_state_write_cancel_pending(operation_id);
}

const contracts::QolUiWritePort kUiWritePort{
    contracts::kQolPortUiWriteVersion,
    {&ui_write_legacy, &ui_write_step, &ui_write_cancel_pending},
};

QolResult game_query_variant(contracts::QolTargetVariant* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (!qol::game_variant_init()) return not_ready();
    const qol::GameVariant& variant = qol::game_variant();
    out->id = static_cast<uint32_t>(variant.series);
    out->capabilities = variant.capabilities;
    return contracts::ok();
}

QolResult game_query_identity(contracts::QolGameIdentity* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out = contracts::QolGameIdentity{};
    (void)qol::game_variant_init();
    const qol::GameVariant& variant = qol::game_variant();
    out->series_name = qol::game_series_name(variant.series);
    out->version_label = variant.version_label != nullptr ? variant.version_label : "";
    out->game_version_name = qol::game::system_port_game_version();
    out->android_release = qol::game::system_port_android_release();
    out->android_sdk_int = qol::game::system_port_android_sdk_int();
    out->package_name = qol::game::system_port_package();
    out->known = variant.known ? 1 : 0;
    return contracts::ok();
}

int32_t game_has_capability(uint64_t bit) {
    if (!qol::game_variant_init() || bit > UINT32_MAX) return 0;
    return (qol::game_variant().capabilities & static_cast<uint32_t>(bit)) != 0 ? 1 : 0;
}

int32_t game_symbol_available(const char* symbol_id) {
    if (symbol_id == nullptr) return 0;
    bool followed = false;
    return qol::game::resolve_hook_entry(symbol_id, &followed) != 0 ? 1 : 0;
}

QolResult game_warehouse_companion_file_state(contracts::QolTargetFeatureState* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out = static_cast<contracts::QolTargetFeatureState>(
        static_cast<int32_t>(qol::game_feature_state(qol::GameFeature::kWarehouseCompanionFile)));
    return contracts::ok();
}

int32_t game_is_main_thread() {
    if (!g_main_thread_set.load(std::memory_order_acquire)) return 0;
    return pthread_equal(g_main_thread, pthread_self()) != 0 ? 1 : 0;
}

int64_t game_frame_count() { return qol::game::frame_count(); }

QolResult read_char_party(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (!qol::game::char_party_index_resolved()) return not_ready();
    const int value = qol::game::char_party_index(character);
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_item_ability(const void* item, int32_t* out) {
    if (item == nullptr || out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out = qol::game::attr_ability_level(item);
    return contracts::ok();
}

QolResult read_item_enchant_remaining(const void* item, int32_t* out) {
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    const int value = qol::game::item_enchant_remaining(item);
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_item_socket_filled(const void* item, int32_t* out) {
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    const int value = qol::game::item_socket_filled(item);
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_item_socket_total(const void* item, int32_t* out) {
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    const int value = qol::game::item_socket_total(item);
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_item_backpack_category(int32_t category, int32_t* out) {
    if (category < 0 || category > 0x3FF || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!qol::game::item_class_table_ready()) return not_ready();
    *out = qol::game::item_is_backpack_category(category) ? 1 : 0;
    return contracts::ok();
}

QolResult read_item_jewel_value(const void* item, int32_t* out) {
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    const int value = qol::game::item_jewel_value(item);
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_item_jewel_attribute_type(const void* item, int32_t* out) {
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    const int value = qol::game::item_jewel_attribute_type(item);
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_item_category_predicate(int32_t category, int32_t* out, int value) {
    if (category < 0 || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_item_enchant_scroll(int32_t category, int32_t* out) {
    return read_item_category_predicate(category, out, qol::game::item_is_enchant_scroll(category));
}

QolResult read_item_mercenary_seal(int32_t category, int32_t* out) {
    return read_item_category_predicate(category, out, qol::game::item_is_mercenary_seal(category));
}

QolResult read_item_usable(int32_t category, int32_t* out) {
    return read_item_category_predicate(category, out, qol::game::item_is_usable(category));
}

QolResult read_item_count_encoding(int32_t category,
                                   contracts::QolItemCountEncoding* out) {
    if (category < 0 || category > 0x3FF || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    const auto encoding = qol::game::item_count_encoding_for_category(category);
    if (encoding == contracts::QolItemCountEncoding::kUnknown) return not_ready();
    *out = encoding;
    return contracts::ok();
}

QolResult read_world_position(const void*, int32_t*, int32_t*) { return unsupported(); }

QolResult read_inventory_for_each_bag_slot(contracts::QolInventoryBagSlotFn callback, void* ctx) {
    if (callback == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    return qol::game::for_each_bag_slot(callback, ctx);
}

QolResult read_inventory_bag_size(int32_t bag, int32_t* out_size) {
    return qol::game::bag_size(bag, out_size);
}

QolResult read_inventory_is_empty_bag(int32_t bag, int32_t* out_empty) {
    return qol::game::is_empty_bag(bag, out_empty);
}

QolResult read_inventory_find_save_slot(const void* item, int32_t* out_slot) {
    return qol::game::find_save_slot(item, out_slot);
}

QolResult read_inventory_cursor_item(const void* control, void** out_item) {
    if (control == nullptr || out_item == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_control_object_get_cursor == nullptr) return not_ready();
    *out_item = fn_control_object_get_cursor(const_cast<void*>(control));
    return *out_item == nullptr ? not_ready() : contracts::ok();
}

QolResult read_inventory_detail_item(void** out_item) {
    if (out_item == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (fn_ui_desc_get_data == nullptr) return not_ready();
    *out_item = fn_ui_desc_get_data();
    return *out_item == nullptr ? error_result(kQolErrNotFound, "detail item not found")
                                : contracts::ok();
}

QolResult read_char_party_member(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!qol::game::char_faction_deps_ready()) return not_ready();
    *out = qol::game::char_is_party_member(const_cast<void*>(character)) ? 1 : 0;
    return contracts::ok();
}

QolResult read_char_summoner(const void* character, void** out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!qol::game::char_summoner_resolved()) return not_ready();
    *out = qol::game::char_get_summoner(const_cast<void*>(character));
    return contracts::ok();
}

QolResult read_char_level(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    const int value = qol::game::char_level(character);
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_char_merc_slot(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    const int value = qol::game::char_merc_slot(character);
    *out = value;
    return contracts::ok();
}

QolResult read_char_can_change_equip(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!qol::game::char_can_change_equip_resolved()) return not_ready();
    *out = qol::game::char_can_change_equip(character) ? 1 : 0;
    return contracts::ok();
}

QolResult read_char_can_equip(const void* character, const void* item, int32_t* out) {
    if (character == nullptr || item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!qol::game::char_can_equip_resolved()) return not_ready();
    *out = qol::game::char_can_equip(character, item) ? 1 : 0;
    return contracts::ok();
}

QolResult read_char_can_apply_stuff(const void* target, const void* material, int32_t* out) {
    if (target == nullptr || material == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    const int32_t allowed =
        qol::game::craft_is_apply_stuff(const_cast<void*>(target), const_cast<void*>(material));
    if (allowed < 0) return not_ready();
    *out = allowed;
    return contracts::ok();
}

QolResult read_char_party_member(int32_t role, void** out) {
    if (out == nullptr || role < 0 || role >= 3) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out = qol::game::char_party_member(role);
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult read_char_find_equip_slot(const void* character, const void* item, int32_t* out) {
    if (character == nullptr || item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!qol::game::char_find_equip_slot_resolved()) return not_ready();
    *out = qol::game::char_find_equip_slot(character, item);
    return *out < 0 ? error_result(kQolErrNotFound, "equip slot not found") : contracts::ok();
}

QolResult read_char_equipped_item(const void* character, int32_t slot, void** out) {
    if (character == nullptr || out == nullptr || slot < 0 || slot >= 10) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!qol::game::char_equipped_item_resolved()) return not_ready();
    *out = qol::game::char_equipped_item(character, slot);
    return contracts::ok();
}

QolResult read_char_has_empty_mercenary_slot(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    const int value = qol::game::char_has_empty_mercenary_slot();
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_char_menu_character(void** out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (!qol::game::char_menu_character_resolved()) return not_ready();
    *out = qol::game::char_menu_character();
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult read_char_pool_slot(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    int slot = 0;
    if (!qol::game::char_pool_slot(character, &slot)) return not_ready();
    *out = slot;
    return contracts::ok();
}

QolResult read_char_max_hp(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    return qol::game::char_read_max_hp(character, out) ? contracts::ok() : not_ready();
}

QolResult read_char_hp(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    return qol::game::char_read_hp(character, out) ? contracts::ok() : not_ready();
}

QolResult write_char_max_hp(void* character, int32_t value) {
    if (character == nullptr || value < 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    return qol::game::char_write_max_hp(character, value) ? contracts::ok() : not_ready();
}

QolResult write_char_hp(void* character, int32_t value) {
    if (character == nullptr || value < 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    return qol::game::char_write_hp(character, value) ? contracts::ok() : not_ready();
}

QolResult write_char_equipped_item(void* character, int32_t slot, void* item) {
    if (character == nullptr || slot < 0 || slot >= 10) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_set_equip_item == nullptr) return not_ready();
    fn_set_equip_item(character, slot, item);
    return contracts::ok();
}

QolResult read_item_rarity(const void* item, int32_t* out) {
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_get_rarity == nullptr) return not_ready();
    *out = fn_get_rarity(const_cast<void*>(item));
    return contracts::ok();
}

QolResult read_item_equip_level(const void* item, int32_t* out) {
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!qol::game::item_equip_primitives_ready()) return not_ready();
    *out = qol::game::item_equip_level(item);
    return contracts::ok();
}

QolResult read_item_dice(int32_t category, int32_t* out) {
    if (category < 0 || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_is_dice == nullptr) return not_ready();
    *out = fn_is_dice(category) != 0 ? 1 : 0;
    return contracts::ok();
}

QolResult read_item_class_no_equip_bit(int32_t category, int32_t* out) {
    if (category < 0 || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    void* record = qol::game::item_class_record(static_cast<uint16_t>(category));
    if (record == nullptr) return not_ready();
    *out = qol::game::item_class_no_equip_bit(record) ? 1 : 0;
    return contracts::ok();
}

QolResult read_item_equip_desc_role_type(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    const int value = qol::game::equip_desc_role_type();
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult write_inventory_remove_item_data(int32_t category, int32_t count) {
    if (category < 0 || count < 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_inven_remove_item_data == nullptr) return not_ready();
    fn_inven_remove_item_data(category, count);
    return contracts::ok();
}

QolResult write_inventory_remove_item(contracts::QolItemHandle handle) {
    if (handle.slot == 0 || handle.generation == 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_remove_item == nullptr) return not_ready();
    const auto resolved = item_handle_registry().resolve_item(
        handle, contracts::kQolItemHandleOwnerAny);
    if (resolved.error == ItemHandleError::kWrongThread) {
        return error_result(kQolErrBlocked, "wrong thread");
    }
    if (resolved.error != ItemHandleError::kNone || resolved.item == nullptr) {
        return error_result(kQolErrNotFound, "stale item handle");
    }
    return fn_remove_item(resolved.item) != 0 ? contracts::ok() : not_ready();
}

QolResult write_inventory_create_perfect_item(int32_t category, void** out_item) {
    if (category < 0 || out_item == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_item = nullptr;
    if (fn_item_create_perfect_item == nullptr) return not_ready();
    *out_item = fn_item_create_perfect_item(category);
    return *out_item == nullptr ? not_ready() : contracts::ok();
}

QolResult write_inventory_create_item(int32_t category, int32_t arg1, int32_t arg2, int32_t arg3,
                                      void** out_item) {
    if (category < 0 || out_item == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_item = nullptr;
    if (fn_create_item == nullptr) return not_ready();
    *out_item = fn_create_item(category, arg1, arg2, arg3);
    return *out_item == nullptr ? not_ready() : contracts::ok();
}

QolResult write_inventory_save_item(void* item, void* param, int32_t* out_saved) {
    if (item == nullptr || out_saved == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_inven_save_item == nullptr) return not_ready();
    *out_saved = fn_inven_save_item(item, param);
    return contracts::ok();
}

QolResult write_inventory_free_item(void* item) {
    if (item == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (fn_itempool_free == nullptr) return not_ready();
    invalidate_item(item);
    fn_itempool_free(item);
    return contracts::ok();
}

QolResult write_inventory_remove_item_direct(int32_t bag, int32_t slot) {
    if (bag < 0 || bag >= 6 || slot < 0 || slot >= 16) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_remove_item_direct == nullptr) return not_ready();
    fn_remove_item_direct(bag, slot);
    return contracts::ok();
}

QolResult call_world_create_yesno(const char* text, uint32_t text_len, void* ok, void* cancel,
                                  void* param) {
    if (text == nullptr || text_len == 0 || text_len > 4096 || ok == nullptr || cancel == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!qol::game::world_create_yesno_ready()) return not_ready();
    return qol::game::world_create_yesno(text, text_len, ok, cancel, param)
               ? contracts::ok()
               : not_ready();
}

QolResult call_world_minus_money(int64_t amount) {
    if (amount < 0) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (!qol::game::world_money_ready()) return not_ready();
    return qol::game::world_minus_money(amount) ? contracts::ok() : not_ready();
}

QolResult call_world_map_change(int32_t map_id, int32_t x, int32_t y, int32_t direction) {
    if (map_id < 0) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (!qol::game::world_map_change_ready()) return not_ready();
    qol::game::world_request_map_change(map_id, x, y, direction);
    return contracts::ok();
}

QolResult call_world_close_popup() {
    return qol::game::world_close_popup_process() ? contracts::ok() : not_ready();
}

QolResult call_world_restore_hud() {
    qol::game::world_restore_hud_gate();
    return contracts::ok();
}

QolResult call_world_show_banner(const char* text) {
    if (text == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    return qol::game::world_show_banner(text) ? contracts::ok() : not_ready();
}

QolResult call_world_add_money(int64_t amount) {
    if (amount < 0) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (fn_add_money == nullptr || fn_add_money(amount) == 0) return not_ready();
    return contracts::ok();
}

QolResult call_world_cancel_tutorial() {
    tutorial_cancel();
    return contracts::ok();
}

QolResult read_choice_state_id(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    const int value = qol::game::ui_choice_state_id();
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_attr_option_value(int32_t option_index, int32_t level, int32_t flag,
                                 contracts::QolItemHandle handle, int32_t* out_value) {
    if (handle.slot == 0 || handle.generation == 0 || out_value == nullptr ||
        option_index < 0 || level < 0 || flag < 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    const auto resolved = item_handle_registry().resolve_item(handle, contracts::kQolItemHandleOwnerAny);
    if (resolved.error == ItemHandleError::kWrongThread) {
        return error_result(kQolErrBlocked, "wrong thread");
    }
    if (resolved.error != ItemHandleError::kNone || resolved.item == nullptr) {
        return error_result(kQolErrNotFound, "stale item handle");
    }
    if (!qol::game::attr_call_option_value(option_index, level, flag, resolved.item, out_value)) {
        return not_ready();
    }
    return contracts::ok();
}

QolResult read_attr_jewel_value(int32_t attribute_type, contracts::QolItemHandle handle,
                                int32_t* out_value) {
    if (handle.slot == 0 || handle.generation == 0 || out_value == nullptr || attribute_type < 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    const auto resolved = item_handle_registry().resolve_item(handle, contracts::kQolItemHandleOwnerAny);
    if (resolved.error == ItemHandleError::kWrongThread) {
        return error_result(kQolErrBlocked, "wrong thread");
    }
    if (resolved.error != ItemHandleError::kNone || resolved.item == nullptr) {
        return error_result(kQolErrNotFound, "stale item handle");
    }
    if (!qol::game::attr_call_jewel_value(attribute_type, resolved.item, out_value)) {
        return not_ready();
    }
    return contracts::ok();
}

QolResult read_attr_uses_fixed_option_table(int32_t category, int32_t* out_value) {
    if (category < 0 || out_value == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!game_host_is_main_thread()) return error_result(kQolErrBlocked, "wrong thread");
    return qol::game::attr_item_uses_fixed_option_table(category, out_value);
}

QolResult read_attr_fixed_option_percent_pair(int32_t category, int32_t option_id,
                                              int32_t* out_p1, int32_t* out_p2) {
    if (category < 0 || option_id < 0 || out_p1 == nullptr || out_p2 == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!game_host_is_main_thread()) return error_result(kQolErrBlocked, "wrong thread");
    return qol::game::attr_fixed_option_percent_pair(category, option_id, out_p1, out_p2);
}

contracts::QolGameHost make_game_host() {
    contracts::QolGameHost host{};
    host.abi = {sizeof(contracts::QolGameHost), contracts::kGameHostMajor,
                contracts::kGameHostMinor};
    host.target = {&game_query_variant, &game_has_capability, &game_symbol_available,
                   &game_warehouse_companion_file_state};
    host.thread = {&game_is_main_thread, &game_frame_count};
    host.read.attr_range = {&read_attr_option_value, &read_attr_jewel_value,
                            &read_attr_uses_fixed_option_table,
                            &read_attr_fixed_option_percent_pair};
    host.read_only.item = {
        &read_item_rarity, &read_item_equip_level, &read_item_dice,
        &read_item_class_no_equip_bit,
        &read_item_equip_desc_role_type, &read_item_enchant_remaining,
        &read_item_socket_filled, &read_item_socket_total, &read_item_backpack_category,
        &read_item_jewel_value, &read_item_jewel_attribute_type,
        &read_item_enchant_scroll, &read_item_mercenary_seal, &read_item_usable,
        &read_item_count_encoding};
    host.character_write = {&write_char_max_hp, &write_char_hp, &write_char_equipped_item};
    host.inventory_write = {&write_inventory_remove_item_data,
                            &write_inventory_remove_item,
                            &write_inventory_create_perfect_item,
                            &write_inventory_create_item,
                            &write_inventory_save_item,
                            &write_inventory_free_item,
                            &write_inventory_remove_item_direct};
    host.world_action = game_world_action_api();
    host.item_abi = {&game_inventory_find_item_hook,
                     &game_inventory_bind_find_item_backup,
                     &game_original_find_item_slot,
                      &game_item_identity_at,
                      &game_begin_original_only,
                      &game_end_original_only,
                      &game_native_item_at,
                      &game_native_item_for_handle};
    host.game_identity = {&game_query_identity};
    return host;
}

}  // namespace

const contracts::QolWorldActionApi& game_world_action_api() {
    static const contracts::QolWorldActionApi api = {
        &call_world_create_yesno, &call_world_minus_money, &call_world_map_change,
        &call_world_close_popup, &call_world_restore_hud, &call_world_show_banner,
        &call_world_add_money, &call_world_cancel_tutorial};
    return api;
}

void game_host_mark_main_thread() {
    if (g_main_thread_set.load(std::memory_order_acquire)) return;
    g_main_thread = pthread_self();
    g_main_thread_set.store(true, std::memory_order_release);
}

bool game_host_is_main_thread() { return game_is_main_thread() != 0; }

const contracts::QolUiWritePort& ui_write_port() { return kUiWritePort; }

const contracts::QolGameHost& game_host_adapter() {
    static const contracts::QolGameHost host = make_game_host();
    return host;
}

}  // namespace qol::game
