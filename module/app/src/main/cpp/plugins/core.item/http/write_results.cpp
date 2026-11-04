#include "plugins/core.item/runtime/core_item_internal.h"
#include "plugins/core.item/api/native_inventory.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "contracts/character_state.h"
#include "plugins/core.item/http/json_lite.h"
#include "plugins/core.item/http/response.h"
#include "plugins/core.item/http/write_result_inventory.h"
#include "plugins/core.item/shop/buy.h"

namespace qol::features::core_item {
namespace {

constexpr char kDiceRejectSuccess[] = "{\"ok\":true}";
constexpr char kDiceRejectNotInGame[] = "{\"ok\":false,\"error\":\"not in game\"}";
constexpr char kDiceRejectNoPending[] =
    "{\"ok\":false,\"error\":\"no dice result pending\"}";
constexpr char kDiceRejectFailed[] = "{\"ok\":false,\"error\":\"dice reject failed\"}";

struct WriteRef {
    int32_t space_id;
    int32_t bag;
    int32_t slot;
};

using SlotKey = write_result::SlotKey;
using InventoryResultSnapshot = write_result::InventorySnapshot;
using ForcedSlots = std::set<SlotKey>;

bool result_space_count(int32_t space_id, int32_t* out_count) {
    if (out_count == nullptr) return false;
    if (space_id >= contracts::kQolBagSpaceFirstRegistered) {
        const contracts::QolBagSource* source = find_bag_source(space_id);
        if (source == nullptr || source->count == nullptr) {
            *out_count = 0;
            return true;
        }
        const int32_t count = source->count();
        if (count < 0) return false;
        *out_count = count;
        return true;
    }
    if (out_count == nullptr || g_port == nullptr || inventory_native_api() == nullptr ||
        !contracts::qol_item_port_native_bag_size_valid(g_port)) {
        return false;
    }
    int32_t count = 0;
    const QolResult result = inventory_native_api()->native_bag_count(space_id, &count);
    if (!contracts::succeeded(result)) {
        if (space_id != contracts::kQolBagSpaceOriginal &&
            (result.code == kQolErrUnsupported || result.code == kQolErrNotReady)) {
            *out_count = 0;
            return true;
        }
        return false;
    }
    if (count < 0) return false;
    *out_count = count;
    return true;
}

QolResult result_bag_info(const contracts::QolBagRef& ref, contracts::QolBagInfo* out) {
    if (ref.space_id >= contracts::kQolBagSpaceFirstRegistered) {
        const contracts::QolBagSource* source = find_bag_source(ref.space_id);
        if (source == nullptr || source->info == nullptr) {
            return contracts::error(kQolErrUnsupported, "registered bag source unavailable", 33);
        }
        return source->info(ref.bag, out);
    }
    const auto* api = inventory_native_api();
    return api == nullptr ? contracts::error(kQolErrNotReady, "inventory access unavailable", 28)
                          : api->native_bag_info(&ref, out);
}

QolResult result_item_at(const contracts::QolBagRef& ref, int32_t slot,
                          contracts::QolInventoryItemRef* out) {
    if (ref.space_id >= contracts::kQolBagSpaceFirstRegistered) {
        const contracts::QolBagSource* source = find_bag_source(ref.space_id);
        if (source == nullptr || source->item_at == nullptr) {
            return contracts::error(kQolErrUnsupported, "registered bag source unavailable", 33);
        }
        return source->item_at(ref.bag, slot, out);
    }
    const auto* api = inventory_native_api();
    return api == nullptr ? contracts::error(kQolErrNotReady, "inventory access unavailable", 28)
                          : api->native_item_at(&ref, slot, out);
}

int32_t write_port_bag(const WriteRef& ref) {
    if (ref.space_id == contracts::kQolBagSpaceOriginal) return ref.bag;
    int32_t original_count = 0;
    int32_t c18_count = 0;
    if (!result_space_count(contracts::kQolBagSpaceOriginal, &original_count) ||
        !result_space_count(contracts::kQolBagSpaceVariant, &c18_count)) {
        return -1;
    }
    int64_t offset = original_count;
    if (ref.space_id == contracts::kQolBagSpaceVariant) return original_count + ref.bag;
    offset += c18_count;
    for (uint32_t index = 0; index < registered_bag_source_count(); ++index) {
        const contracts::QolBagSource* source = registered_bag_source_at(index);
        if (source == nullptr) continue;
        if (source->space_id == ref.space_id) {
            const int64_t flat = offset + ref.bag;
            return flat <= INT32_MAX ? static_cast<int32_t>(flat) : -1;
        }
        int32_t count = 0;
        if (!result_space_count(source->space_id, &count)) return -1;
        offset += count;
    }
    return -1;
}

bool decode_write_bag(int32_t host_bag, WriteRef* out) {
    if (host_bag < 0 || out == nullptr) return false;
    int32_t original_count = 0;
    int32_t c18_count = 0;
    if (!result_space_count(contracts::kQolBagSpaceOriginal, &original_count) ||
        !result_space_count(contracts::kQolBagSpaceVariant, &c18_count)) {
        return false;
    }
    if (host_bag < original_count) {
        *out = {contracts::kQolBagSpaceOriginal, host_bag, -1};
        return true;
    }
    if (host_bag < contracts::kQolOriginalBagCount) return false;
    int32_t remaining = host_bag - original_count;
    if (remaining < c18_count) {
        *out = {contracts::kQolBagSpaceVariant, remaining, -1};
        return true;
    }
    remaining -= c18_count;
    for (uint32_t index = 0; index < registered_bag_source_count(); ++index) {
        const contracts::QolBagSource* source = registered_bag_source_at(index);
        if (source == nullptr) continue;
        int32_t count = 0;
        if (!result_space_count(source->space_id, &count)) return false;
        if (remaining < count) {
            *out = {source->space_id, remaining, -1};
            return true;
        }
        remaining -= count;
    }
    return false;
}

bool write_slot_key(int32_t host_bag, int32_t slot, SlotKey* out) {
    if (out == nullptr || slot < 0) return false;
    WriteRef ref{};
    if (!decode_write_bag(host_bag, &ref)) return false;
    *out = {ref.space_id, ref.bag, slot};
    return true;
}

bool find_item_slot(int32_t category, int32_t* bag_out, int32_t* slot_out) {
    if (g_port == nullptr || inventory_native_api() == nullptr ||
        bag_out == nullptr || slot_out == nullptr ||
        !contracts::qol_item_port_native_bag_size_valid(g_port)) {
        return false;
    }
    const uint32_t space_count = 2 + registered_bag_source_count();
    for (uint32_t space_index = 0; space_index < space_count; ++space_index) {
        const contracts::QolBagSource* registered_source =
            space_index < 2 ? nullptr : registered_bag_source_at(space_index - 2);
        const int32_t space_id = space_index == 0
                                     ? contracts::kQolBagSpaceOriginal
                                     : space_index == 1
                                           ? contracts::kQolBagSpaceVariant
                                           : registered_source != nullptr
                                                 ? registered_source->space_id
                                                 : -1;
        if (space_id < 0) continue;
        int32_t count = 0;
        if (!result_space_count(space_id, &count)) return false;
        for (int32_t bag = 0; bag < count; ++bag) {
            const contracts::QolBagRef ref{space_id, bag};
            contracts::QolBagInfo info{};
            if (!contracts::succeeded(result_bag_info(ref, &info)) ||
                info.capacity < 0) {
                return false;
            }
            for (int32_t slot = 0; slot < info.capacity; ++slot) {
                contracts::QolInventoryItemRef item{};
                const QolResult result = result_item_at(ref, slot, &item);
                if (!contracts::succeeded(result)) {
                    if (result.code == kQolErrNotFound || result.code == kQolErrNotReady ||
                        result.code == kQolErrUnsupported) {
                        continue;
                    }
                    return false;
                }
                if (item.item.slot == 0 || item.item.generation == 0 ||
                    item.category != category) {
                    continue;
                }
                const int32_t host_bag = write_port_bag({space_id, bag, slot});
                if (host_bag < 0) continue;
                *bag_out = host_bag;
                *slot_out = item.slot;
                return true;
            }
        }
    }
    return false;
}

const contracts::CharacterStateApi* character_state_capability() {
    if (g_sdk == nullptr || g_sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    const QolResult result = g_sdk->capabilities.require(
        contracts::kCharacterStateCapabilityId,
        QolVersionRange{contracts::kCharacterStateVersion.major,
                        contracts::kCharacterStateVersion.minor},
        &iface);
    if (!contracts::succeeded(result) || iface == nullptr) return nullptr;
    return static_cast<const contracts::CharacterStateApi*>(iface);
}

bool read_enriched_item(const contracts::QolBagRef& bag, int32_t slot, std::string* out) {
    if (out == nullptr || g_port == nullptr || inventory_native_api() == nullptr ||
        g_port->read.inventory_item_view == nullptr) {
        return false;
    }
    contracts::QolInventoryItemRef item_ref{};
    const QolResult ref_result = result_item_at(bag, slot, &item_ref);
    if (!contracts::succeeded(ref_result)) {
        if (ref_result.code == kQolErrNotFound || ref_result.code == kQolErrNotReady ||
            ref_result.code == kQolErrUnsupported) {
            *out = "null";
            return true;
        }
        return false;
    }
    if (item_ref.space_id != bag.space_id || item_ref.bag != bag.bag || item_ref.slot != slot) {
        return false;
    }
    const contracts::QolItemHandle item_handle = item_ref.item;
    if (item_handle.slot == 0 || item_handle.generation == 0) {
        *out = "null";
        return true;
    }
    contracts::QolItemInventoryView view{};
    if (!contracts::succeeded(g_port->read.inventory_item_view(item_handle, &view)) ||
        !inventory_option_counts_valid(view)) {
        return false;
    }
    uint32_t count = 0;
    bool count_present = false;
    if (!effective_item_count(item_handle, view.count_supported != 0, &count,
                              &count_present)) {
        return false;
    }
    std::string raw;
    append_inventory_item(&raw, slot, view, count_present, count);
    JsonValue item{};
    if (!json_parse(raw.data(), static_cast<uint32_t>(raw.size()), &item) ||
        !enrich_item_value(item, g_port, out)) {
        return false;
    }
    return true;
}

bool capture_inventory_result(InventoryResultSnapshot* out) {
    if (out == nullptr || g_port == nullptr || inventory_native_api() == nullptr ||
        !contracts::qol_item_port_native_bag_size_valid(g_port)) {
        return false;
    }
    InventoryResultSnapshot snapshot;
    const uint32_t space_count = 2 + registered_bag_source_count();
    for (uint32_t space_index = 0; space_index < space_count; ++space_index) {
        const contracts::QolBagSource* registered_source =
            space_index < 2 ? nullptr : registered_bag_source_at(space_index - 2);
        const int32_t space_id = space_index == 0
                                     ? contracts::kQolBagSpaceOriginal
                                     : space_index == 1
                                           ? contracts::kQolBagSpaceVariant
                                           : registered_source != nullptr
                                                 ? registered_source->space_id
                                                 : -1;
        if (space_id < 0) continue;
        int32_t bag_count = 0;
        if (!result_space_count(space_id, &bag_count)) return false;
        for (int32_t bag = 0; bag < bag_count; ++bag) {
            const contracts::QolBagRef ref{space_id, bag};
            contracts::QolBagInfo info{};
            if (!contracts::succeeded(result_bag_info(ref, &info)) ||
                info.capacity < 0) {
                return false;
            }
            for (int32_t slot = 0; slot < info.capacity; ++slot) {
                std::string item_json;
                if (!read_enriched_item(ref, slot, &item_json)) return false;
                snapshot.emplace(SlotKey{space_id, bag, slot}, std::move(item_json));
            }
        }
    }
    *out = std::move(snapshot);
    return true;
}

std::string result_item_at(const InventoryResultSnapshot& snapshot, const SlotKey& key) {
    const auto found = snapshot.find(key);
    return found == snapshot.end() ? "null" : found->second;
}

void append_result_slot(std::string& body, const SlotKey& key,
                        const InventoryResultSnapshot& snapshot, bool* first) {
    if (!*first) body.push_back(',');
    *first = false;
    body += "{\"space\":" + std::to_string(key.space_id) + ",\"bag\":" +
            std::to_string(key.bag) + ",\"slot\":" + std::to_string(key.slot) +
            ",\"item\":" + result_item_at(snapshot, key) + "}";
}

void append_inventory_changes(std::string& body, const InventoryResultSnapshot& before,
                              const InventoryResultSnapshot& after,
                              const ForcedSlots& forced, bool* first) {
    std::set<SlotKey> keys = forced;
    for (const auto& entry : before) keys.insert(entry.first);
    for (const auto& entry : after) keys.insert(entry.first);
    for (const SlotKey& key : keys) {
        if (forced.count(key) == 0 && result_item_at(before, key) == result_item_at(after, key)) {
            continue;
        }
        append_result_slot(body, key, after, first);
    }
}

bool copy_result_truncated(char* out, uint32_t capacity, uint32_t* out_size,
                           const std::string& body) {
    if (out == nullptr || out_size == nullptr) return false;
    const uint32_t copied = static_cast<uint32_t>(std::min<std::size_t>(body.size(), capacity));
    if (copied != 0) std::memcpy(out, body.data(), copied);
    *out_size = static_cast<uint32_t>(body.size());
    return true;
}

bool read_character_state(contracts::QolCharacterStateKind kind, std::string* out) {
    if (out == nullptr) return false;
    const contracts::CharacterStateApi* state = character_state_capability();
    if (state == nullptr || state->write_state_json == nullptr) return false;
    std::vector<char> buffer(kind == contracts::QolCharacterStateKind::kParty ? 65536 : 4096);
    uint32_t size = 0;
    if (!contracts::succeeded(state->write_state_json(kind, buffer.data(),
                                                      static_cast<uint32_t>(buffer.size()),
                                                      &size)) ||
        size > buffer.size()) {
        return false;
    }
    out->assign(buffer.data(), size);
    return true;
}

const JsonValue* party_member(const JsonValue& party, int32_t role) {
    if (party.type == JsonValue::Type::kArray) {
        return role >= 0 && static_cast<std::size_t>(role) < party.items.size()
                   ? &party.items[static_cast<std::size_t>(role)]
                   : nullptr;
    }
    const JsonValue* roles = party.find("roles");
    if (roles == nullptr || roles->type != JsonValue::Type::kArray) return nullptr;
    for (const JsonValue& member : roles->items) {
        long long slot = -1;
        const JsonValue* slot_value = member.find("slot");
        if (slot_value != nullptr && slot_value->as_int(&slot) && slot == role) return &member;
    }
    return nullptr;
}

bool json_int(const JsonValue* object, const char* key, int32_t* out) {
    if (object == nullptr || out == nullptr) return false;
    const JsonValue* value = object->find(key);
    long long parsed = 0;
    if (value == nullptr || !value->as_int(&parsed) || parsed < INT32_MIN || parsed > INT32_MAX) {
        return false;
    }
    *out = static_cast<int32_t>(parsed);
    return true;
}

bool active_role(int32_t* out_role) {
    std::string player_json;
    JsonValue player{};
    if (!read_character_state(contracts::QolCharacterStateKind::kPlayer, &player_json) ||
        !json_parse(player_json.data(), static_cast<uint32_t>(player_json.size()), &player)) {
        return false;
    }
    return json_int(&player, "leader_slot", out_role);
}

bool read_active_health(int32_t* out_hp, int32_t* out_mp) {
    int32_t role = -1;
    std::string party_json;
    JsonValue party{};
    if (!active_role(&role) ||
        !read_character_state(contracts::QolCharacterStateKind::kParty, &party_json) ||
        !json_parse(party_json.data(), static_cast<uint32_t>(party_json.size()), &party)) {
        return false;
    }
    const JsonValue* member = party_member(party, role);
    return json_int(member, "hp", out_hp) && json_int(member, "mp", out_mp);
}

bool read_equipment_item(int32_t role, int32_t equip_slot, std::string* out) {
    if (out == nullptr || role < 0 || equip_slot < 0) return false;
    std::string party_json;
    JsonValue party{};
    if (!read_character_state(contracts::QolCharacterStateKind::kParty, &party_json) ||
        !json_parse(party_json.data(), static_cast<uint32_t>(party_json.size()), &party)) {
        return false;
    }
    const JsonValue* member = party_member(party, role);
    const JsonValue* equipment = member != nullptr ? member->find("equipment") : nullptr;
    if (equipment == nullptr || equipment->type != JsonValue::Type::kArray ||
        static_cast<std::size_t>(equip_slot) >= equipment->items.size()) {
        return false;
    }
    const JsonValue& item = equipment->items[static_cast<std::size_t>(equip_slot)];
    if (item.type == JsonValue::Type::kNull) {
        *out = "null";
        return true;
    }
    return enrich_item_value(item, g_port, out);
}

bool json_value_equal(const JsonValue& left, const JsonValue& right) {
    if (left.type != right.type) return false;
    switch (left.type) {
        case JsonValue::Type::kNull: return true;
        case JsonValue::Type::kBool: return left.boolean == right.boolean;
        case JsonValue::Type::kNumber: return left.integer == right.integer;
        case JsonValue::Type::kString: return left.string == right.string;
        case JsonValue::Type::kArray:
            if (left.items.size() != right.items.size()) return false;
            for (std::size_t i = 0; i < left.items.size(); ++i) {
                if (!json_value_equal(left.items[i], right.items[i])) return false;
            }
            return true;
        case JsonValue::Type::kObject:
            if (left.member_keys != right.member_keys ||
                left.member_values.size() != right.member_values.size()) {
                return false;
            }
            for (std::size_t i = 0; i < left.member_values.size(); ++i) {
                if (!json_value_equal(left.member_values[i], right.member_values[i])) return false;
            }
            return true;
    }
    return false;
}

int32_t changed_equipment_slot(int32_t role, const std::string& before_json,
                               const std::string& after_json) {
    JsonValue before{};
    JsonValue after{};
    if (!json_parse(before_json.data(), static_cast<uint32_t>(before_json.size()), &before) ||
        !json_parse(after_json.data(), static_cast<uint32_t>(after_json.size()), &after)) {
        return -1;
    }
    const JsonValue* before_member = party_member(before, role);
    const JsonValue* after_member = party_member(after, role);
    const JsonValue* before_equipment = before_member != nullptr
                                            ? before_member->find("equipment")
                                            : nullptr;
    const JsonValue* after_equipment = after_member != nullptr
                                           ? after_member->find("equipment")
                                           : nullptr;
    if (before_equipment == nullptr || after_equipment == nullptr ||
        before_equipment->type != JsonValue::Type::kArray ||
        after_equipment->type != JsonValue::Type::kArray) {
        return -1;
    }
    const std::size_t count = std::min(before_equipment->items.size(), after_equipment->items.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (!json_value_equal(before_equipment->items[i], after_equipment->items[i])) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}


bool write_item_inventory_state_result(const contracts::QolItemWriteArgs& args, char* out,
                                       uint32_t capacity, uint32_t* out_size) {
    if (out == nullptr || out_size == nullptr || g_item_state_write == nullptr) return false;
    *out_size = 0;
    contracts::QolItemInventoryStateWriteResult operation_data{};
    QolResult operation = contracts::error(kQolErrUnsupported, "item operation unavailable", 26);
    const auto kind = static_cast<contracts::QolItemWriteKind>(args.kind);
    InventoryResultSnapshot inventory_before{};
    const bool tracks_inventory =
        kind == contracts::QolItemWriteKind::kUseItem ||
        kind == contracts::QolItemWriteKind::kSellItem ||
        kind == contracts::QolItemWriteKind::kDiscardItem;
    if (tracks_inventory) (void)capture_inventory_result(&inventory_before);
    int32_t hp_before = 0;
    int32_t mp_before = 0;
    const bool health_before_available =
        kind == contracts::QolItemWriteKind::kUseItem &&
        read_active_health(&hp_before, &mp_before);
    switch (kind) {
        case contracts::QolItemWriteKind::kUseItem:
            operation = g_item_state_write->use_item(args.i0, args.i1, &operation_data);
            break;
        case contracts::QolItemWriteKind::kSellItem:
            operation = g_item_state_write->sell_item(args.i0, args.i1, &operation_data);
            break;
        case contracts::QolItemWriteKind::kDiscardItem:
            operation = g_item_state_write->discard_item(args.i0, args.i1, &operation_data);
            break;
        case contracts::QolItemWriteKind::kAddItem: {
            operation = g_item_state_write->add_item(
                args.i0, args.i1, args.i2, args.i3, args.i4,
                nullptr, &operation_data);
            break;
        }
        default:
            return false;
    }

    if (!contracts::succeeded(operation)) {
        const char* message = operation.message != nullptr ? operation.message : "item operation failed";
        const uint32_t message_size = operation.message != nullptr
                                          ? operation.message_len
                                          : static_cast<uint32_t>(std::strlen(message));
        const std::string body = std::string("{\"ok\":false,\"error\":\"") +
                                 std::string(message, message_size) + "\"}";
        (void)copy_buffer(out, capacity, out_size, body.data(),
                          static_cast<uint32_t>(body.size()));
        return false;
    }
    request_inventory_refresh();

    if (kind == contracts::QolItemWriteKind::kAddItem) {
        constexpr char kAddItemSuccess[] = "{\"ok\":true}";
        return copy_buffer(out, capacity, out_size, kAddItemSuccess,
                           sizeof(kAddItemSuccess) - 1);
    }

    std::string body = "{\"ok\":true";
    if (kind == contracts::QolItemWriteKind::kUseItem &&
        (operation_data.flags & contracts::kQolItemInventoryStateHasGained) != 0) {
        if (operation_data.gained_count > contracts::kQolItemInventoryStateGainedCapacity) {
            constexpr char kBadGained[] = "{\"ok\":false,\"error\":\"invalid gained item count\"}";
            (void)copy_buffer(out, capacity, out_size, kBadGained, sizeof(kBadGained) - 1);
            return false;
        }
        body += ",\"gained\":[";
        for (uint32_t i = 0; i < operation_data.gained_count; ++i) {
            if (i != 0) body.push_back(',');
            const auto& item = operation_data.gained[i];
            WriteRef ref{};
            if (!decode_write_bag(item.bag, &ref) || item.slot < 0) {
                constexpr char kInvalidGained[] =
                    "{\"ok\":false,\"error\":\"invalid gained item location\"}";
                (void)copy_buffer(out, capacity, out_size, kInvalidGained,
                                  sizeof(kInvalidGained) - 1);
                return false;
            }
            body += "{\"space\":" + std::to_string(ref.space_id) +
                    ",\"bag\":" + std::to_string(ref.bag) +
                    ",\"slot\":" + std::to_string(item.slot) +
                    ",\"category\":" + std::to_string(item.category) +
                    ",\"count\":" + std::to_string(item.count) + "}";
        }
        body.push_back(']');
    }
    if (kind == contracts::QolItemWriteKind::kSellItem) {
        body += ",\"price\":" + std::to_string(operation_data.value);
    }

    InventoryResultSnapshot inventory_after{};
    if (tracks_inventory && !capture_inventory_result(&inventory_after)) {
        constexpr char kInventoryUnavailable[] =
            "{\"ok\":false,\"error\":\"inventory state unavailable\"}";
        (void)copy_buffer(out, capacity, out_size, kInventoryUnavailable,
                          sizeof(kInventoryUnavailable) - 1);
        return false;
    }
    if (tracks_inventory) {
        body += ",\"slots\":[";
        ForcedSlots forced;
        if (kind == contracts::QolItemWriteKind::kUseItem ||
            kind == contracts::QolItemWriteKind::kSellItem ||
            kind == contracts::QolItemWriteKind::kDiscardItem) {
            SlotKey source{};
            if (write_slot_key(args.i0, args.i1, &source)) {
                forced.insert(source);
            }
        }
        if (kind == contracts::QolItemWriteKind::kUseItem &&
            (operation_data.flags & contracts::kQolItemInventoryStateHasGained) != 0) {
            for (uint32_t i = 0; i < operation_data.gained_count; ++i) {
                const auto& gained = operation_data.gained[i];
                SlotKey key{};
                if (write_slot_key(gained.bag, gained.slot, &key)) {
                    forced.insert(key);
                }
            }
        }
        bool first_slot = true;
        append_inventory_changes(body, inventory_before, inventory_after, forced, &first_slot);
        body.push_back(']');
    }
    if (kind == contracts::QolItemWriteKind::kSellItem) {
        int64_t money_after = 0;
        if (!contracts::succeeded(money(&money_after))) {
            constexpr char kMoneyUnavailable[] =
                "{\"ok\":false,\"error\":\"money unavailable\"}";
            (void)copy_buffer(out, capacity, out_size, kMoneyUnavailable,
                              sizeof(kMoneyUnavailable) - 1);
            return false;
        }
        body += ",\"money\":" + std::to_string(money_after);
    }
    if (kind == contracts::QolItemWriteKind::kUseItem && health_before_available) {
        int32_t hp_after = 0;
        int32_t mp_after = 0;
        if (read_active_health(&hp_after, &mp_after) &&
            (hp_before != hp_after || mp_before != mp_after)) {
            body += ",\"player\":{";
            bool first_health = true;
            if (hp_before != hp_after) {
                body += "\"hp\":" + std::to_string(hp_after);
                first_health = false;
            }
            if (mp_before != mp_after) {
                if (!first_health) body.push_back(',');
                body += "\"mp\":" + std::to_string(mp_after);
            }
            body.push_back('}');
        }
    }
    body.push_back('}');
    return copy_result_truncated(out, capacity, out_size, body);
}

bool write_item_party_state_result(const contracts::QolItemWriteArgs& args, char* out,
                                   uint32_t capacity, uint32_t* out_size) {
    if (out == nullptr || out_size == nullptr) return false;
    *out_size = 0;
    const contracts::CharacterStateApi* state = character_state_capability();
    if (state == nullptr || state->write_state_json == nullptr) {
        constexpr char kStateUnavailable[] =
            "{\"ok\":false,\"error\":\"character state unavailable\"}";
        (void)copy_buffer(out, capacity, out_size, kStateUnavailable,
                          sizeof(kStateUnavailable) - 1);
        return false;
    }
    if (g_item_state_write == nullptr) {
        constexpr char kOperationUnavailable[] =
            "{\"ok\":false,\"error\":\"item operation unavailable\"}";
        (void)copy_buffer(out, capacity, out_size, kOperationUnavailable,
                          sizeof(kOperationUnavailable) - 1);
        return false;
    }

    contracts::QolItemInventoryStateWriteResult operation_data{};
    QolResult operation = contracts::error(kQolErrUnsupported, "item operation unavailable", 26);
    const auto kind = static_cast<contracts::QolItemWriteKind>(args.kind);
    InventoryResultSnapshot inventory_before{};
    const bool tracks_inventory = kind != contracts::QolItemWriteKind::kEnchant;
    if (tracks_inventory) (void)capture_inventory_result(&inventory_before);
    std::string party_before_json;
    if (kind == contracts::QolItemWriteKind::kEquipItem &&
        !read_character_state(contracts::QolCharacterStateKind::kParty, &party_before_json)) {
        constexpr char kStateUnavailable[] =
            "{\"ok\":false,\"error\":\"character state unavailable\"}";
        (void)copy_buffer(out, capacity, out_size, kStateUnavailable,
                          sizeof(kStateUnavailable) - 1);
        return false;
    }
    switch (kind) {
        case contracts::QolItemWriteKind::kEquipItem: {
            int32_t bag = args.i1;
            int32_t slot = args.i2;
            if (args.i5 >= 0 && !find_item_slot(args.i5, &bag, &slot)) {
                constexpr char kNotFound[] = "{\"ok\":false,\"error\":\"item not found\"}";
                (void)copy_buffer(out, capacity, out_size, kNotFound, sizeof(kNotFound) - 1);
                return false;
            }
            WriteRef ref{};
            if (!decode_write_bag(bag, &ref)) {
                operation = contracts::error(kQolErrInvalidArgument,
                                             "invalid inventory bag", 21);
                break;
            }
            if (ref.space_id >= contracts::kQolBagSpaceFirstRegistered) {
                operation = contracts::error(
                    kQolErrUnsupported, "registered bag source does not support equipment writes",
                    55);
                break;
            }
            operation = g_item_state_write->equip_item(args.i0, bag, slot, &operation_data);
            break;
        }
        case contracts::QolItemWriteKind::kUnequipItem:
            operation = g_item_state_write->unequip_item(args.i0, args.i1, &operation_data);
            break;
        case contracts::QolItemWriteKind::kPutJewel: {
            WriteRef ref{};
            if (!decode_write_bag(args.i1, &ref)) {
                operation = contracts::error(kQolErrInvalidArgument,
                                             "invalid inventory bag", 21);
                break;
            }
            if (ref.space_id >= contracts::kQolBagSpaceFirstRegistered) {
                operation = contracts::error(
                    kQolErrUnsupported, "registered bag source does not support jewel writes",
                    51);
                break;
            }
            operation = g_item_state_write->put_jewel(args.i0, args.i1, args.i2, args.i3,
                                                       &operation_data);
            break;
        }
        case contracts::QolItemWriteKind::kEnchant: {
            WriteRef ref{};
            if (!decode_write_bag(args.i1, &ref)) {
                operation = contracts::error(kQolErrInvalidArgument,
                                             "invalid inventory bag", 21);
                break;
            }
            if (ref.space_id >= contracts::kQolBagSpaceFirstRegistered) {
                operation = contracts::error(
                    kQolErrUnsupported, "registered bag source does not support enchant writes",
                    53);
                break;
            }
            operation = g_item_state_write->enchant(args.i0, args.i1, args.i2, args.i3,
                                                     &operation_data);
            break;
        }
        default:
            return false;
    }
    if (!contracts::succeeded(operation)) {
        const char* message = operation.message != nullptr ? operation.message
                                                           : "item operation failed";
        const uint32_t message_size = operation.message != nullptr
                                          ? operation.message_len
                                          : static_cast<uint32_t>(std::strlen(message));
        const std::string body = std::string("{\"ok\":false,\"error\":\"") +
                                 std::string(message, message_size) + "\"}";
        (void)copy_buffer(out, capacity, out_size, body.data(),
                          static_cast<uint32_t>(body.size()));
        return false;
    }
    request_inventory_refresh();

    std::string party_after_json;
    int32_t equip_slot = kind == contracts::QolItemWriteKind::kUnequipItem ? args.i1 : args.i3;
    if (kind == contracts::QolItemWriteKind::kEquipItem) {
        if (!read_character_state(contracts::QolCharacterStateKind::kParty, &party_after_json)) {
            constexpr char kStateUnavailable[] =
                "{\"ok\":false,\"error\":\"character state unavailable\"}";
            (void)copy_buffer(out, capacity, out_size, kStateUnavailable,
                              sizeof(kStateUnavailable) - 1);
            return false;
        }
        equip_slot = changed_equipment_slot(args.i0, party_before_json, party_after_json);
    }
    std::string equipment_item = "null";
    if (kind == contracts::QolItemWriteKind::kUnequipItem) {
        equipment_item = "null";
    } else if (equip_slot < 0 || !read_equipment_item(args.i0, equip_slot, &equipment_item)) {
        constexpr char kStateUnavailable[] =
            "{\"ok\":false,\"error\":\"character state unavailable\"}";
        (void)copy_buffer(out, capacity, out_size, kStateUnavailable,
                          sizeof(kStateUnavailable) - 1);
        return false;
    }
    std::string body = "{\"ok\":true,\"equipment\":{\"role\":" +
                       std::to_string(args.i0) + ",\"slot\":" +
                       std::to_string(equip_slot) + ",\"item\":" + equipment_item + "}";
    InventoryResultSnapshot inventory_after{};
    if (tracks_inventory) {
        if (!capture_inventory_result(&inventory_after)) {
            constexpr char kInventoryUnavailable[] =
                "{\"ok\":false,\"error\":\"inventory state unavailable\"}";
            (void)copy_buffer(out, capacity, out_size, kInventoryUnavailable,
                              sizeof(kInventoryUnavailable) - 1);
            return false;
        }
        body += ",\"inventory\":[";
        const ForcedSlots forced;
        bool first_slot = true;
        append_inventory_changes(body, inventory_before, inventory_after, forced, &first_slot);
        body.push_back(']');
    }
    body.push_back('}');
    return copy_result_truncated(out, capacity, out_size, body);
}

bool write_item_dice_accept_result(const contracts::QolItemWriteArgs& args, char* out,
                                   uint32_t capacity, uint32_t* out_size) {
    (void)args;
    if (out == nullptr || out_size == nullptr) return false;
    *out_size = 0;
    // 状态能力缺失即 fail-closed，accept 不执行（写未发生）。
    const contracts::CharacterStateApi* state = character_state_capability();
    if (state == nullptr || state->write_state_json == nullptr) {
        constexpr char kStateUnavailable[] =
            "{\"ok\":false,\"error\":\"character state unavailable\"}";
        (void)copy_buffer(out, capacity, out_size, kStateUnavailable,
                          sizeof(kStateUnavailable) - 1);
        return false;
    }
    if (g_item_dice_write == nullptr || g_item_dice_write->accept == nullptr) {
        constexpr char kOperationUnavailable[] =
            "{\"ok\":false,\"error\":\"item operation unavailable\"}";
        (void)copy_buffer(out, capacity, out_size, kOperationUnavailable,
                          sizeof(kOperationUnavailable) - 1);
        return false;
    }
    int32_t role = -1;
    std::string party_before_json;
    JsonValue party_before{};
    if (!active_role(&role) ||
        !read_character_state(contracts::QolCharacterStateKind::kParty, &party_before_json) ||
        !json_parse(party_before_json.data(), static_cast<uint32_t>(party_before_json.size()),
                    &party_before)) {
        constexpr char kStateUnavailable[] =
            "{\"ok\":false,\"error\":\"character state unavailable\"}";
        (void)copy_buffer(out, capacity, out_size, kStateUnavailable,
                          sizeof(kStateUnavailable) - 1);
        return false;
    }
    contracts::QolItemDiceAcceptResult accept{};
    const QolResult operation = g_item_dice_write->accept(&accept);
    if (!contracts::succeeded(operation)) {
        const char* message = operation.message != nullptr ? operation.message
                                                           : "dice accept failed";
        const uint32_t message_size = operation.message != nullptr
                                          ? operation.message_len
                                          : static_cast<uint32_t>(std::strlen(message));
        const std::string body = std::string("{\"ok\":false,\"error\":\"") +
                                 std::string(message, message_size) + "\"}";
        (void)copy_buffer(out, capacity, out_size, body.data(),
                          static_cast<uint32_t>(body.size()));
        return false;
    }
    std::string party_after_json;
    JsonValue party_after{};
    if (!read_character_state(contracts::QolCharacterStateKind::kParty, &party_after_json) ||
        !json_parse(party_after_json.data(), static_cast<uint32_t>(party_after_json.size()),
                    &party_after)) {
        constexpr char kStateUnavailable[] =
            "{\"ok\":false,\"error\":\"character state unavailable\"}";
        (void)copy_buffer(out, capacity, out_size, kStateUnavailable,
                          sizeof(kStateUnavailable) - 1);
        return false;
    }
    const JsonValue* before_member = party_member(party_before, role);
    const JsonValue* after_member = party_member(party_after, role);
    const JsonValue* before_stats = before_member != nullptr ? before_member->find("main_stats")
                                                             : nullptr;
    const JsonValue* after_stats = after_member != nullptr ? after_member->find("main_stats")
                                                           : nullptr;
    int32_t status_point = 0;
    if (before_stats == nullptr || after_stats == nullptr ||
        before_stats->type != JsonValue::Type::kArray ||
        after_stats->type != JsonValue::Type::kArray ||
        !json_int(after_member, "status_point", &status_point)) {
        constexpr char kStateUnavailable[] =
            "{\"ok\":false,\"error\":\"character state unavailable\"}";
        (void)copy_buffer(out, capacity, out_size, kStateUnavailable,
                          sizeof(kStateUnavailable) - 1);
        return false;
    }
    std::string body = "{\"ok\":true,\"base\":[";
    for (uint32_t i = 0; i < contracts::kQolItemDiceStatCount; ++i) {
        if (i != 0) body.push_back(',');
        body += std::to_string(accept.base[i]);
    }
    body += "],\"applied\":[";
    for (uint32_t i = 0; i < contracts::kQolItemDiceStatCount; ++i) {
        if (i != 0) body.push_back(',');
        body += std::to_string(accept.applied[i]);
    }
    body += "],\"delta\":[";
    for (uint32_t i = 0; i < contracts::kQolItemDiceStatCount; ++i) {
        if (i != 0) body.push_back(',');
        body += std::to_string(accept.delta[i]);
    }
    body += "],\"player\":{\"main_stats\":[";
    bool first_stat = true;
    const std::size_t stat_count = std::min(before_stats->items.size(), after_stats->items.size());
    for (std::size_t i = 0; i < stat_count; ++i) {
        long long before_value = 0;
        long long after_value = 0;
        if (!before_stats->items[i].as_int(&before_value) ||
            !after_stats->items[i].as_int(&after_value) || before_value == after_value) {
            continue;
        }
        if (!first_stat) body.push_back(',');
        first_stat = false;
        body += "{\"attr\":" + std::to_string(i) + ",\"value\":" +
                std::to_string(after_value) + "}";
    }
    body += "],\"status_point\":" + std::to_string(status_point) + "}";
    body.push_back('}');
    return copy_result_truncated(out, capacity, out_size, body);
}

}  // namespace

bool write_result::SlotKey::operator<(const SlotKey& other) const {
    return std::tie(space_id, bag, slot) < std::tie(other.space_id, other.bag, other.slot);
}

bool write_result::capture_inventory_snapshot(InventorySnapshot* out) {
    return capture_inventory_result(out);
}

void write_result::append_changed_slots(std::string& body, const InventorySnapshot& before,
                                        const InventorySnapshot& after) {
    const ForcedSlots forced;
    bool first = true;
    append_inventory_changes(body, before, after, forced, &first);
}

bool item_write_result(void*, const uint8_t* payload, uint32_t size, char* out, uint32_t capacity,
                       uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (payload == nullptr || out == nullptr || out_size == nullptr ||
        (size != sizeof(ItemWritePayload) &&
         size != sizeof(contracts::QolItemWriteArgs))) {
        return false;
    }
    ItemWritePayload command{};
    command.from_space = -1;
    command.to_space = -1;
    if (size == sizeof(ItemWritePayload)) {
        std::memcpy(&command, payload, size);
    } else {
        std::memcpy(&command.args, payload, size);
    }
    const contracts::QolItemWriteArgs& args = command.args;
    const auto kind = static_cast<contracts::QolItemWriteKind>(args.kind);
    bool shop_result_handled = false;
    const bool shop_result =
        shop::write_result_if_requested(args, out, capacity, out_size, &shop_result_handled);
    if (shop_result_handled) return shop_result;
    if (kind == contracts::QolItemWriteKind::kEquipItem ||
        kind == contracts::QolItemWriteKind::kUnequipItem ||
        kind == contracts::QolItemWriteKind::kPutJewel ||
        kind == contracts::QolItemWriteKind::kEnchant) {
        return write_item_party_state_result(args, out, capacity, out_size);
    }
    if (kind == contracts::QolItemWriteKind::kAcceptDice) {
        return write_item_dice_accept_result(args, out, capacity, out_size);
    }
    if (kind == contracts::QolItemWriteKind::kAddItem) {
        return write_item_inventory_state_result(args, out, capacity, out_size);
    }
    if (kind == contracts::QolItemWriteKind::kUseItem ||
        kind == contracts::QolItemWriteKind::kSellItem ||
        kind == contracts::QolItemWriteKind::kDiscardItem) {
        return write_item_inventory_state_result(args, out, capacity, out_size);
    }
    // move/money 已迁往 core.inventory 的 kInventoryWriteHandlerId；本 handler 不再消费。
    // g_item_money_write 仍是 reject-dice 守卫的一部分，端口保留。
    if (g_item_money_write == nullptr || g_item_money_write->set_money == nullptr ||
        g_item_dice_write == nullptr || g_item_dice_write->reject == nullptr) {
        return false;
    }
    if (args.kind == static_cast<int32_t>(contracts::QolItemWriteKind::kRejectDice)) {
        const QolResult operation = g_item_dice_write->reject();
        const char* body = kDiceRejectSuccess;
        uint32_t body_size = sizeof(kDiceRejectSuccess) - 1;
        if (!contracts::succeeded(operation)) {
            if (operation.code == kQolErrNotReady) {
                body = kDiceRejectNotInGame;
                body_size = sizeof(kDiceRejectNotInGame) - 1;
            } else if (operation.code == kQolErrNotFound) {
                body = kDiceRejectNoPending;
                body_size = sizeof(kDiceRejectNoPending) - 1;
            } else {
                body = kDiceRejectFailed;
                body_size = sizeof(kDiceRejectFailed) - 1;
            }
        }
        const uint32_t copied = std::min(body_size, capacity);
        if (copied != 0) std::memcpy(out, body, copied);
        *out_size = body_size;
        return contracts::succeeded(operation);
    }
    return false;
}
}  // namespace qol::features::core_item
