#include "item_slot.h"

#include <limits>
#include <initializer_list>

#include "contracts/bag.h"
#include "plugins/core.item/api/native_inventory.h"

namespace qol::features::core_item {
const contracts::QolBagSource* find_bag_source(int32_t space_id);
uint32_t registered_bag_source_count();
const contracts::QolBagSource* registered_bag_source_at(uint32_t index);
}

namespace qol::features::inventory_hooks {
namespace {

const contracts::QolGameHost* g_game = nullptr;
const contracts::ItemFactsApi* g_item_facts = nullptr;
const contracts::QolItemPort* g_item_port = nullptr;

QolResult slot_error(int32_t code, const char* message, uint32_t length) {
    return contracts::error(code, message, length);
}

bool item_facts_ready() {
    return contracts::item_facts_api_size_valid(g_item_facts) &&
           g_item_facts->item_ref_at != nullptr && g_item_port != nullptr &&
            qol::features::core_item::inventory_native_api() != nullptr &&
           contracts::qol_item_port_native_bag_size_valid(g_item_port);
}

bool space_count(int32_t space_id, int32_t* out_count) {
    if (out_count == nullptr || !item_facts_ready()) return false;
    if (space_id >= contracts::kQolBagSpaceFirstRegistered) {
        const contracts::QolBagSource* source =
            qol::features::core_item::find_bag_source(space_id);
        if (source == nullptr || source->count == nullptr) {
            *out_count = 0;
            return true;
        }
        const int32_t count = source->count();
        if (count < 0) return false;
        *out_count = count;
        return true;
    }
    int32_t count = 0;
    const QolResult result = qol::features::core_item::inventory_native_api()->native_bag_count(space_id, &count);
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

int32_t port_bag(const contracts::QolBagRef& ref) {
    if (ref.space_id == contracts::kQolBagSpaceOriginal) return ref.bag;
    int32_t original_count = 0;
    int32_t c18_count = 0;
    if (!space_count(contracts::kQolBagSpaceOriginal, &original_count) ||
        !space_count(contracts::kQolBagSpaceVariant, &c18_count)) {
        return -1;
    }
    int64_t offset = original_count + c18_count;
    if (ref.space_id == contracts::kQolBagSpaceVariant) return original_count + ref.bag;
    for (uint32_t index = 0;
         index < qol::features::core_item::registered_bag_source_count(); ++index) {
        const contracts::QolBagSource* source =
            qol::features::core_item::registered_bag_source_at(index);
        if (source == nullptr || source->count == nullptr) continue;
        if (source->space_id == ref.space_id) {
            const int64_t flat = offset + ref.bag;
            return flat <= std::numeric_limits<int32_t>::max()
                       ? static_cast<int32_t>(flat)
                       : -1;
        }
        const int32_t count = source->count();
        if (count < 0) return -1;
        offset += count;
    }
    return -1;
}

bool resolve_flat_bag(int32_t flat_bag, contracts::QolBagRef* out_ref) {
    if (flat_bag < 0 || out_ref == nullptr) return false;
    int32_t original_count = 0;
    int32_t c18_count = 0;
    if (!space_count(contracts::kQolBagSpaceOriginal, &original_count) ||
        !space_count(contracts::kQolBagSpaceVariant, &c18_count)) {
        return false;
    }
    if (flat_bag < original_count) {
        *out_ref = {contracts::kQolBagSpaceOriginal, flat_bag};
        return true;
    }
    int32_t remaining = flat_bag - original_count;
    if (remaining < c18_count) {
        *out_ref = {contracts::kQolBagSpaceVariant, remaining};
        return true;
    }
    remaining -= c18_count;
    for (uint32_t index = 0;
         index < qol::features::core_item::registered_bag_source_count(); ++index) {
        const contracts::QolBagSource* source =
            qol::features::core_item::registered_bag_source_at(index);
        if (source == nullptr || source->count == nullptr) continue;
        const int32_t count = source->count();
        if (count < 0) return false;
        if (remaining < count) {
            *out_ref = {source->space_id, remaining};
            return true;
        }
        remaining -= count;
    }
    return false;
}

bool bag_capacity(const contracts::QolBagRef& ref, int32_t* out_capacity) {
    if (out_capacity == nullptr || !item_facts_ready()) return false;
    if (ref.space_id >= contracts::kQolBagSpaceFirstRegistered) {
        const contracts::QolBagSource* source =
            qol::features::core_item::find_bag_source(ref.space_id);
        if (source == nullptr || source->info == nullptr) return false;
        contracts::QolBagInfo info{};
        if (!contracts::succeeded(source->info(ref.bag, &info)) || info.capacity < 0) {
            return false;
        }
        *out_capacity = info.capacity;
        return true;
    }
    contracts::QolBagInfo info{};
    if (!contracts::succeeded(qol::features::core_item::inventory_native_api()->native_bag_info(&ref, &info)) || info.capacity < 0) {
        return false;
    }
    *out_capacity = info.capacity;
    return true;
}

QolResult validate_output(contracts::QolInventorySlotView* out) {
    if (out == nullptr || out->struct_size < sizeof(*out)) {
        return slot_error(kQolErrInvalidArgument, "invalid inventory slot view", 27);
    }
    *out = {};
    out->struct_size = sizeof(*out);
    return contracts::ok();
}

QolResult fill_view(const contracts::QolInventoryItemRef& ref,
                    contracts::QolInventorySlotView* out) {
    if (ref.item.slot == 0 || ref.item.generation == 0) {
        return slot_error(kQolErrNotFound, "inventory slot empty", 20);
    }
    if (ref.kind != static_cast<int32_t>(contracts::QolInventoryItemKind::kOriginal) &&
        ref.kind != static_cast<int32_t>(contracts::QolInventoryItemKind::kProvider)) {
        return slot_error(kQolErrConflict, "stale inventory item", 20);
    }
    if (g_item_facts->is_no_sell == nullptr ||
        !contracts::item_facts_api_count_encoding_size_valid(g_item_facts) ||
        g_item_facts->count_encoding == nullptr) {
        return slot_error(kQolErrUnsupported, "inventory item facts unavailable", 31);
    }

    int32_t no_sell = 0;
    QolResult result = g_item_facts->is_no_sell(ref.category, &no_sell);
    if (!contracts::succeeded(result)) return result;
    contracts::QolItemCountEncoding count_encoding = contracts::QolItemCountEncoding::kUnknown;
    result = g_item_facts->count_encoding(ref.item, &count_encoding);
    if (!contracts::succeeded(result)) return result;

    out->item = ref.item;
    out->kind = static_cast<uint32_t>(ref.kind);
    out->bag = ref.bag;
    out->slot = ref.slot;
    out->category = ref.category;
    out->count = ref.count;
    out->encoding = count_encoding == contracts::QolItemCountEncoding::kEncoded ? 1u : 0u;
    out->flags = (no_sell != 0 ? 1u : 0u) |
                 (count_encoding == contracts::QolItemCountEncoding::kEncoded ? 2u : 0u);
    return contracts::ok();
}

QolResult ref_at(const contracts::QolBagRef& bag, int32_t slot,
                 contracts::QolInventoryItemRef* out) {
    if (!item_facts_ready()) {
        return slot_error(kQolErrNotReady, "item facts unavailable", 22);
    }
    if (bag.space_id >= contracts::kQolBagSpaceFirstRegistered) {
        const contracts::QolBagSource* source =
            qol::features::core_item::find_bag_source(bag.space_id);
        return source != nullptr && source->item_at != nullptr
                   ? source->item_at(bag.bag, slot, out)
                   : slot_error(kQolErrUnsupported, "registered bag source unavailable", 33);
    }
    return qol::features::core_item::inventory_native_api()->native_item_at(&bag, slot, out);
}

QolResult resolve_slot(int32_t bag, int32_t slot, contracts::QolInventorySlotView* out) {
    QolResult result = validate_output(out);
    if (!contracts::succeeded(result)) return result;
    contracts::QolBagRef bag_ref{};
    int32_t capacity = 0;
    if (slot < 0 || !resolve_flat_bag(bag, &bag_ref) ||
        !bag_capacity(bag_ref, &capacity) || slot >= capacity) {
        return slot_error(kQolErrInvalidArgument, "invalid inventory slot", 22);
    }

    if (bag_ref.space_id >= contracts::kQolBagSpaceFirstRegistered) {
        if (qol::features::core_item::find_bag_source(bag_ref.space_id) == nullptr)
            return slot_error(kQolErrUnsupported, "registered bag source unavailable", 32);
    }

    contracts::QolInventoryItemRef ref{};
    result = ref_at(bag_ref, slot, &ref);
    if (!contracts::succeeded(result)) {
        if (result.code != kQolErrNotFound) return result;
        out->bag = port_bag(bag_ref);
        if (out->bag < 0) {
            return slot_error(kQolErrNotReady, "item facts unavailable", 22);
        }
        out->slot = slot;
        return contracts::ok();
    }
    if (ref.space_id != bag_ref.space_id || ref.bag != bag_ref.bag || ref.slot != slot) {
        return slot_error(kQolErrConflict, "stale inventory slot", 21);
    }
    if (ref.item.slot == 0) {
        out->bag = port_bag(bag_ref);
        if (out->bag < 0) {
            return slot_error(kQolErrNotReady, "item facts unavailable", 22);
        }
        out->slot = slot;
        return contracts::ok();
    }
    if (bag_ref.space_id < contracts::kQolBagSpaceFirstRegistered &&
        ref.kind != static_cast<int32_t>(contracts::QolInventoryItemKind::kOriginal)) {
        return slot_error(kQolErrConflict, "stale inventory slot", 20);
    }
    result = fill_view(ref, out);
    out->bag = port_bag(bag_ref);
    if (contracts::succeeded(result) && out->bag < 0) {
        return slot_error(kQolErrNotReady, "item facts unavailable", 22);
    }
    return result;
}

QolResult view_item(contracts::QolItemHandle item, contracts::QolInventorySlotView* out) {
    QolResult result = validate_output(out);
    if (!contracts::succeeded(result)) return result;
    if (item.slot == 0 || item.generation == 0) {
        return slot_error(kQolErrInvalidArgument, "invalid item handle", 19);
    }
    if (!item_facts_ready()) {
        return slot_error(kQolErrNotReady, "item facts unavailable", 22);
    }

    const auto search_space = [&](int32_t space_id) -> QolResult {
        int32_t count = 0;
        if (!space_count(space_id, &count)) {
            return slot_error(kQolErrNotReady, "item facts unavailable", 22);
        }
        for (int32_t bag = 0; bag < count; ++bag) {
            const contracts::QolBagRef bag_ref{space_id, bag};
            int32_t capacity = 0;
            if (!bag_capacity(bag_ref, &capacity)) {
                return slot_error(kQolErrNotReady, "bag info unavailable", 21);
            }
            for (int32_t slot = 0; slot < capacity; ++slot) {
                contracts::QolInventoryItemRef ref{};
                result = ref_at(bag_ref, slot, &ref);
                if (!contracts::succeeded(result)) {
                    if (result.code == kQolErrNotFound || result.code == kQolErrNotReady ||
                        result.code == kQolErrUnsupported) {
                        continue;
                    }
                    return result;
                }
                if (ref.space_id != space_id || ref.bag != bag || ref.slot != slot) {
                    return slot_error(kQolErrConflict, "stale inventory item", 20);
                }
                if (ref.item.slot != item.slot || ref.item.generation != item.generation) continue;
                if (space_id < contracts::kQolBagSpaceFirstRegistered && ref.kind !=
                           static_cast<int32_t>(contracts::QolInventoryItemKind::kOriginal)) {
                    return slot_error(kQolErrConflict, "stale inventory item", 20);
                }
                result = fill_view(ref, out);
                out->bag = port_bag(bag_ref);
                if (contracts::succeeded(result) && out->bag < 0) {
                    return slot_error(kQolErrNotReady, "item facts unavailable", 22);
                }
                return result;
            }
        }
        return slot_error(kQolErrConflict, "stale inventory item", 20);
    };
    for (int32_t space_id : {contracts::kQolBagSpaceOriginal, contracts::kQolBagSpaceVariant}) {
        result = search_space(space_id);
        if (contracts::succeeded(result) || result.code != kQolErrConflict) return result;
    }
    for (uint32_t index = 0;
         index < qol::features::core_item::registered_bag_source_count(); ++index) {
        const contracts::QolBagSource* source =
            qol::features::core_item::registered_bag_source_at(index);
        if (source == nullptr) continue;
        result = search_space(source->space_id);
        if (contracts::succeeded(result) || result.code != kQolErrConflict) return result;
    }
    return slot_error(kQolErrConflict, "stale inventory item", 20);
}

QolResult remove_item(int32_t bag, int32_t slot, contracts::QolItemHandle expected,
                      uint32_t reason) {
    contracts::QolBagRef bag_ref{};
    if (resolve_flat_bag(bag, &bag_ref) &&
        bag_ref.space_id == contracts::kQolBagSpaceOriginal &&
        bag_ref.bag == contracts::kQolTaskBagIndex) {
        return slot_error(kQolErrUnsupported, "task bag excluded", 17);
    }
    if (reason != contracts::kRemoveSell && reason != contracts::kRemoveDiscard) {
        return slot_error(kQolErrInvalidArgument, "invalid inventory remove reason", 31);
    }

    contracts::QolInventorySlotView current{};
    current.struct_size = sizeof(current);
    QolResult result = resolve_slot(bag, slot, &current);
    if (!contracts::succeeded(result)) return result;
    if (expected.slot == 0 || expected.generation == 0 ||
        current.item.slot != expected.slot || current.item.generation != expected.generation) {
        return slot_error(kQolErrConflict, "stale inventory item", 20);
    }

    if (current.kind == 1u) {
        if (reason == contracts::kRemoveSell) {
            return slot_error(kQolErrUnsupported,
                              "registered bag sale requires inventory action", 45);
        }
        const contracts::QolBagSource* source =
            qol::features::core_item::find_bag_source(bag_ref.space_id);
        if (source == nullptr || source->remove == nullptr) {
            return slot_error(kQolErrUnsupported,
                              "registered bag source does not support removal", 46);
        }
        contracts::QolInventoryItemRef item{};
        result = ref_at(bag_ref, slot, &item);
        if (!contracts::succeeded(result) || item.item.slot != expected.slot ||
            item.item.generation != expected.generation || item.count <= 0) {
            return slot_error(kQolErrConflict, "stale inventory item", 20);
        }
        contracts::QolBagMutationResult mutation{};
        return source->remove(expected, item.count, &mutation);
    }

    if (g_game == nullptr || g_game->inventory_write.remove_item == nullptr) {
        return slot_error(kQolErrNotReady, "inventory removal unavailable", 29);
    }
    return g_game->inventory_write.remove_item(expected);
}

}  // namespace

const contracts::QolInventorySlotApi kInventorySlotApi{
    sizeof(contracts::QolInventorySlotApi), &resolve_slot, &view_item, &remove_item};

void inventory_slot_bind(const contracts::QolHostSdk* sdk,
                         const contracts::QolGameHost* game,
                         const contracts::ItemFactsApi* item_facts,
                         const contracts::QolItemPort* item_port) {
    (void)sdk;
    g_game = game;
    g_item_facts = item_facts;
    g_item_port = item_port;
}

}  // namespace qol::features::inventory_hooks
