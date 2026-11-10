#include "game/adapter_ports/game_host_port.h"

#include "game/adapter_ports/system_port.h"
#include "game/adapter_ports/world_action_port.h"

#include "contracts/game_ports/ui_write_port.h"
#include "contracts/game_ports/ports.h"

#include <algorithm>
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
#include "game/symbols/game_symbols.h"

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

QolResult read_item_pack_required_slots(int32_t category, int32_t* out_slots) {
    if (out_slots == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_slots = -1;
    if (category < 0 || category > UINT16_MAX || g_base == 0) return contracts::ok();

    auto* records_slot = reinterpret_cast<void**>(g_base + G_ITEMPACK_RECORDS_GOT_VMA);
    auto* stride_slot = reinterpret_cast<void**>(g_base + G_ITEMPACK_STRIDE_GOT_VMA);
    auto* count_slot = reinterpret_cast<void**>(g_base + G_ITEMPACK_COUNT_GOT_VMA);
    if (!game_memory_accessible(records_slot, sizeof(*records_slot), 'r') ||
        !game_memory_accessible(stride_slot, sizeof(*stride_slot), 'r') ||
        !game_memory_accessible(count_slot, sizeof(*count_slot), 'r')) {
        return contracts::ok();
    }

    // 三个 GOT 槽指向 .bss 中的指针变量，游戏加载表后才写入真正的表地址，因此需要二次解引用。
    const auto* records_holder = *reinterpret_cast<void* const*>(records_slot);
    const auto* stride_value = *reinterpret_cast<const uint8_t* const*>(stride_slot);
    const auto* count_value = *reinterpret_cast<const uint16_t* const*>(count_slot);
    if (records_holder == nullptr || stride_value == nullptr || count_value == nullptr ||
        !game_memory_accessible(stride_value, sizeof(*stride_value), 'r') ||
        !game_memory_accessible(count_value, sizeof(*count_value), 'r')) {
        return contracts::ok();
    }
    const auto* records = *reinterpret_cast<const uint8_t* const*>(records_holder);
    if (records == nullptr) return contracts::ok();

    const uint8_t stride = *stride_value;
    const uint16_t count = *count_value;
    if (stride < 5 || stride > 32 || count == 0 || count >= 4096) {
        return contracts::ok();
    }
    const size_t table_size = static_cast<size_t>(stride) * count;
    if (!game_memory_accessible(records, table_size, 'r')) return contracts::ok();

    bool matched = false;
    int32_t required_slots = 0;
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* record = records + static_cast<size_t>(i) * stride;
        uint16_t box_category = 0;
        uint16_t product_category = 0;
        std::memcpy(&box_category, record, sizeof(box_category));
        if (box_category != static_cast<uint16_t>(category)) continue;
        std::memcpy(&product_category, record + 2, sizeof(product_category));
        matched = true;

        int32_t product_slots = std::max(1, static_cast<int32_t>(record[4]));
        const auto* item_class = static_cast<const uint8_t*>(
            qol::game::item_class_record(product_category));
        if (item_class != nullptr && game_memory_accessible(item_class, 7, 'r') &&
            (item_class[6] & 1) != 0) {
            product_slots = 1;
        }
        required_slots += product_slots;
    }

    if (matched) *out_slots = required_slots;
    return contracts::ok();
}

bool read_item_effect_byte(int32_t category, size_t byte_offset, uint8_t* out_byte) {
    if (out_byte == nullptr || category < 0 || category > 0x3FF || g_base == 0) return false;
    uint8_t* class_base = *reinterpret_cast<uint8_t**>(
        *reinterpret_cast<void**>(g_base + G_ITEMCLASS_DATA_GOT_VMA));
    uint8_t* class_size = *reinterpret_cast<uint8_t**>(g_base + G_ITEMCLASS_SIZE_GOT_VMA);
    if (class_base == nullptr || class_size == nullptr || *class_size == 0) return false;
    const size_t class_offset = static_cast<size_t>(category) * (*class_size) + 2;
    if (!game_memory_accessible(class_base + class_offset, 1, 'r')) return false;
    const uint8_t use_type = class_base[class_offset];
    uint8_t* effect_base = *reinterpret_cast<uint8_t**>(
        *reinterpret_cast<void**>(g_base + G_EQUIP_SLOT_TABLE_DATA_GOT_VMA));
    uint8_t* effect_size = *reinterpret_cast<uint8_t**>(g_base + G_EQUIP_SLOT_TABLE_SIZE_GOT_VMA);
    if (effect_base == nullptr || effect_size == nullptr || *effect_size == 0) return false;
    const size_t effect_offset = static_cast<size_t>(use_type) * (*effect_size) + byte_offset;
    if (!game_memory_accessible(effect_base + effect_offset, 1, 'r')) return false;
    *out_byte = effect_base[effect_offset];
    return true;
}

QolResult read_item_native_save_domain(int32_t category, int32_t* out_domain) {
    if (out_domain == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_domain = -1;
    uint8_t effect = 0;
    if (read_item_effect_byte(category, 2, &effect)) *out_domain = (effect >> 3) & 1;
    return contracts::ok();
}

QolResult read_item_two_handed(int32_t category, int32_t* out_value) {
    if (out_value == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_value = -1;
    uint8_t effect = 0;
    if (read_item_effect_byte(category, 2, &effect)) *out_value = (effect >> 4) & 1;
    return contracts::ok();
}

// G_INVEN_SAVED_BAG_VMA 是 C18 改版在映像内为其原生追加袋/扁平袋号保留的 scratch
// 单元；原版、overhaul、monster 映像没有该单元，读写会落到无关数据，因此只在
// C18 变体上访问。
bool c18_saved_bag_supported() {
    return game_has_capability(contracts::kQolTargetCapabilityC18NativeBagLayoutCandidate) != 0;
}

QolResult read_saved_bag(int32_t* out_bag) {
    if (out_bag == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_bag = -1;
    if (!c18_saved_bag_supported()) return unsupported();
    if (g_base == 0) return not_ready();
    auto* value = reinterpret_cast<int32_t*>(g_base + G_INVEN_SAVED_BAG_VMA);
    if (!game_memory_accessible(value, sizeof(*value), 'r')) return not_ready();
    *out_bag = *value;
    return contracts::ok();
}

QolResult write_saved_bag(int32_t bag) {
    if (bag < 0) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (!c18_saved_bag_supported()) return unsupported();
    if (g_base == 0) return not_ready();
    auto* value = reinterpret_cast<int32_t*>(g_base + G_INVEN_SAVED_BAG_VMA);
    if (!game_memory_accessible(value, sizeof(*value), 'w')) return not_ready();
    *value = bag;
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

QolResult write_char_reset_equipped_item(void* character, int32_t slot) {
    if (character == nullptr || slot < 0 || slot >= 10) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_char_reset_equip_item == nullptr) return not_ready();
    fn_char_reset_equip_item(character, slot);
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

// 原始 ITEM* → 物品类别（ITEMDATABASE 行号）。变体差异只在这里收口：
// C18 把物品 id 扩到 12bit（bits[6..16) | bits[1:0]<<10），其余变体（含 unknown）按原版
// 10bit 读取。读取失败/空指针返回 InvalidArgument，调用方按 fail-soft 放弃重掷。
QolResult read_item_category(const void* item, int32_t* out) {
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    const uint16_t raw =
        *reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(item) + I_TYPE);
    int32_t category = static_cast<int32_t>(
        (raw >> I_TYPE_CATEGORY_SHIFT) & I_TYPE_CATEGORY_MASK);
    if (qol::game_variant_init() && qol::game_variant().series == qol::GameSeries::kC18) {
        category |= static_cast<int32_t>(raw & 0x3u) << 10;
    }
    *out = category;
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

QolResult call_world_desc_set_off() {
    if (!qol::game::game_host_is_main_thread() || fn_ui_desc_set_off == nullptr) {
        return not_ready();
    }
    fn_ui_desc_set_off();
    return contracts::ok();
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

QolResult read_attr_jewel_generation_range(int32_t attribute_type,
                                           contracts::QolItemHandle handle,
                                           int32_t* out_min, int32_t* out_max) {
    if (handle.slot == 0 || handle.generation == 0 || out_min == nullptr || out_max == nullptr ||
        attribute_type < 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    // 仅 C18L v507+ 有表驱动宝石数值；其他变体直接 Unsupported，调用方走随机捕获/采样。
    if (!qol::c18_gem_table_available()) {
        return error_result(kQolErrUnsupported, kUnsupported);
    }
    const auto resolved = item_handle_registry().resolve_item(handle, contracts::kQolItemHandleOwnerAny);
    if (resolved.error == ItemHandleError::kWrongThread) {
        return error_result(kQolErrBlocked, "wrong thread");
    }
    if (resolved.error != ItemHandleError::kNone || resolved.item == nullptr) {
        return error_result(kQolErrNotFound, "stale item handle");
    }
    return qol::game::attr_c18_jewel_generation_range(resolved.item, attribute_type, out_min,
                                                      out_max);
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

QolResult game_shortcut_item_category(int32_t shortcut, int32_t* out_category) {
    if (out_category == nullptr || shortcut < 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_category = 0;
    if (!qol::game::shortcut_item_category(shortcut, out_category)) {
        return error_result(kQolErrNotFound, "shortcut item unavailable");
    }
    return contracts::ok();
}

QolResult read_character_attr(void* character, int32_t attr_id, int32_t* out_value) {
    if (character == nullptr || out_value == nullptr || attr_id < 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    // 只读直调与原函数在被调线程上的自调用等价，不设主线程门禁（线程域由调用方决定，
    // 与 item 只读面同一纪律）。复用既有 F_GET_ATTR_VMA 登记，不另造同址符号。
    using GetAttrFn = int32_t (*)(void*, int32_t);
    static GetAttrFn get_attr = nullptr;
    if (get_attr == nullptr && g_base != 0) {
        get_attr = reinterpret_cast<GetAttrFn>(
            g_base + fn_resolve("F_GET_ATTR_VMA", F_GET_ATTR_VMA));
    }
    if (get_attr == nullptr) return not_ready();
    *out_value = get_attr(character, attr_id);
    return contracts::ok();
}

QolResult calculate_physical_defense_formula(void* character, int32_t first_arg,
                                              int32_t* out_value) {
    if (character == nullptr || out_value == nullptr || g_base == 0)
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    const uint8_t* const* index_slot =
        reinterpret_cast<const uint8_t* const*>(g_base + 0x2f3e70);
    const auto* text_list_slot = reinterpret_cast<const uintptr_t*>(g_base + 0x2f55a0);
    if (!game_memory_accessible(index_slot, sizeof(*index_slot), 'r') ||
        !game_memory_accessible(text_list_slot, sizeof(*text_list_slot), 'r')) return not_ready();
    const uint8_t* index_ptr = *index_slot;
    const uintptr_t list_indirect = *text_list_slot;
    if (index_ptr == nullptr || list_indirect == 0 ||
        !game_memory_accessible(index_ptr, 1, 'r') ||
        !game_memory_accessible(reinterpret_cast<void*>(list_indirect), sizeof(uintptr_t), 'r'))
        return not_ready();
    const uint8_t index = *index_ptr;
    const uintptr_t list = *reinterpret_cast<const uintptr_t*>(list_indirect);
    if (list == 0 || !game_memory_accessible(reinterpret_cast<void*>(list),
                                              (static_cast<size_t>(index) + 1) * 8, 'r'))
        return not_ready();
    const uint16_t text_id = *reinterpret_cast<const uint16_t*>(list +
                                                                 static_cast<uintptr_t>(index) * 8);
    using GetTextE = const char* (*)(uint16_t);
    using Calculate = int32_t (*)(const char*, int32_t*, int32_t);
    const auto get_text = reinterpret_cast<GetTextE>(g_base + 0x1186cc);
    const auto calculate = reinterpret_cast<Calculate>(g_base + 0xd9968);
    const char* formula = get_text(text_id);
    if (formula == nullptr || !game_memory_accessible(formula, 1, 'r')) return not_ready();
    const auto* character_bytes = static_cast<const uint8_t*>(character);
    if (!game_memory_accessible(character_bytes, 15, 'r')) return not_ready();
    int32_t args[2]{first_arg, static_cast<int32_t>(character_bytes[14])};
    *out_value = calculate(formula, args, 2);
    return contracts::ok();
}

QolResult read_character_main_stat(void* character, int32_t index, int32_t* out_value) {
    if (character == nullptr || out_value == nullptr || index < 0 || index > 4)
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out_value = char_stat_total(character, index);
    return contracts::ok();
}

// 面板派生 getter 与原版绘制同线程域：内部经 CHAR_GetAttr 在脏位置位时重算写回，
// 物抗还使用 CAL 全局计算器栈，因此只允许游戏主线程调用。
template <typename Fn>
QolResult call_panel_getter(const void* character, const char* symbol, uintptr_t fallback,
                            Fn& cache, int32_t* out_value) {
    if (character == nullptr || out_value == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!game_is_main_thread()) return error_result(kQolErrBlocked, "wrong thread");
    if (cache == nullptr) {
        if (g_base == 0) return not_ready();
        cache = reinterpret_cast<Fn>(g_base + fn_resolve(symbol, fallback));
    }
    if (cache == nullptr) return not_ready();
    *out_value = cache(const_cast<void*>(character));
    return contracts::ok();
}

QolResult read_character_normal_damage(void* character, int32_t* out_value) {
    static int32_t (*getter)(void*) = nullptr;
    return call_panel_getter(character, "F_CHAR_GET_NORMAL_DAMAGE_VMA",
                             F_CHAR_GET_NORMAL_DAMAGE_VMA, getter, out_value);
}

QolResult read_character_normal_magic_damage(void* character, int32_t* out_value) {
    static int32_t (*getter)(void*) = nullptr;
    return call_panel_getter(character, "F_CHAR_GET_NORMAL_MAGIC_DAMAGE_VMA",
                             F_CHAR_GET_NORMAL_MAGIC_DAMAGE_VMA, getter, out_value);
}

QolResult read_character_evasion_rate_1000(void* character, int32_t* out_value) {
    static int32_t (*getter)(void*) = nullptr;
    return call_panel_getter(character, "F_CHAR_GET_NORMAL_EVASION_RATE_1000_VMA",
                             F_CHAR_GET_NORMAL_EVASION_RATE_1000_VMA, getter, out_value);
}

QolResult read_character_display_damage_reduce_rate(void* character, int32_t* out_value) {
    static int32_t (*getter)(void*) = nullptr;
    return call_panel_getter(character, "F_CHAR_GET_DISPLAY_DAMAGE_REDUCE_RATE_VMA",
                             F_CHAR_GET_DISPLAY_DAMAGE_REDUCE_RATE_VMA, getter, out_value);
}

// 命中率 getter 的第二个参数是目标；原版人物面板固定传 NULL，只按自身等级计算。
QolResult read_character_hit_rate_1000(void* character, int32_t* out_value) {
    if (character == nullptr || out_value == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!game_is_main_thread()) return error_result(kQolErrBlocked, "wrong thread");
    static int32_t (*getter)(void*, void*) = nullptr;
    if (getter == nullptr) {
        if (g_base == 0) return not_ready();
        getter = reinterpret_cast<int32_t (*)(void*, void*)>(
            g_base + fn_resolve("F_CHAR_GET_HIT_RATE_1000_VMA", F_CHAR_GET_HIT_RATE_1000_VMA));
    }
    if (getter == nullptr) return not_ready();
    *out_value = getter(character, nullptr);
    return contracts::ok();
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
        &read_item_count_encoding, &read_item_pack_required_slots,
        &read_item_native_save_domain, &read_item_two_handed};
    host.character_write = {&write_char_max_hp, &write_char_hp, &write_char_equipped_item,
                            &write_char_reset_equipped_item};
    host.inventory_write = {&write_inventory_remove_item_data,
                            &write_inventory_remove_item,
                            &write_inventory_create_perfect_item,
                            &write_inventory_create_item,
                            &write_inventory_save_item,
                            &write_inventory_free_item,
                            &write_inventory_remove_item_direct,
                            &read_saved_bag,
                            &write_saved_bag};
    host.world_action = game_world_action_api();
    host.item_abi = {&game_inventory_find_item_hook,
                     &game_inventory_bind_find_item_backup,
                     &game_original_find_item_slot,
                      &game_item_identity_at,
                      &game_begin_original_only,
                      &game_end_original_only,
                      &game_native_item_at,
                      &game_native_item_for_handle,
                      &game_shortcut_item_category};
    host.game_identity = {&game_query_identity};
    host.attr_range_c18 = {&read_attr_jewel_generation_range};
    host.item_category = {&read_item_category};
    host.character_read = {&read_character_attr,
                           &calculate_physical_defense_formula,
                           &read_character_main_stat,
                           &read_character_normal_damage,
                           &read_character_normal_magic_damage,
                           &read_character_hit_rate_1000,
                           &read_character_evasion_rate_1000,
                           &read_character_display_damage_reduce_rate};
    return host;
}

}  // namespace

const contracts::QolWorldActionApi& game_world_action_api() {
    static const contracts::QolWorldActionApi api = {
        &call_world_create_yesno, &call_world_minus_money, &call_world_map_change,
        &call_world_close_popup, &call_world_restore_hud, &call_world_show_banner,
        &call_world_add_money, &call_world_cancel_tutorial, &call_world_desc_set_off};
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
