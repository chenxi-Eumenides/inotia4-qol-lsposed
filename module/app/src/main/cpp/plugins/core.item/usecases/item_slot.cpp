#include "item_slot.h"

#include <limits>

#include "../../../contracts/extension_bag.h"

namespace qol::features::inventory_hooks {
namespace {

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;
const contracts::ItemFactsApi* g_item_facts = nullptr;
const contracts::QolItemPort* g_item_port = nullptr;

QolResult slot_error(int32_t code, const char* message, uint32_t length) {
    return contracts::error(code, message, length);
}

const contracts::ExtensionBagApi* extension_bag(uint16_t minor) {
    if (g_sdk == nullptr || g_sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    const QolResult result = g_sdk->capabilities.require(
        contracts::kExtensionBagCapabilityId,
        {static_cast<uint16_t>(contracts::kExtensionBagVersionMajor), minor}, &iface);
    return contracts::succeeded(result)
        ? static_cast<const contracts::ExtensionBagApi*>(iface)
        : nullptr;
}

bool item_facts_ready() {
    return contracts::item_facts_api_size_valid(g_item_facts) &&
           g_item_facts->item_ref_at != nullptr && g_item_port != nullptr &&
           g_item_port->read.bag_count != nullptr && g_item_port->read.bag_info != nullptr &&
           g_item_port->read.item_at != nullptr &&
           contracts::qol_item_port_bag_space_size_valid(g_item_port);
}

bool space_count(int32_t space_id, int32_t* out_count) {
    if (out_count == nullptr || !item_facts_ready()) return false;
    int32_t count = 0;
    const QolResult result = g_item_port->read.bag_count(space_id, &count);
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
    return ref.space_id == contracts::kQolBagSpaceOriginal
               ? ref.bag
               : contracts::kQolOriginalBagCount + ref.bag;
}

bool flat_bag(const contracts::QolBagRef& ref, int32_t* out_bag) {
    if (out_bag == nullptr) return false;
    if (ref.space_id == contracts::kQolBagSpaceOriginal) {
        *out_bag = ref.bag;
        return true;
    }
    int32_t original_count = 0;
    int32_t c18_count = 0;
    if (!space_count(contracts::kQolBagSpaceOriginal, &original_count) ||
        !space_count(contracts::kQolBagSpaceC18, &c18_count) || ref.bag < 0) {
        return false;
    }
    int64_t offset = original_count;
    if (ref.space_id >= contracts::kQolBagSpaceFirstRegistered) {
        offset += c18_count;
    } else if (ref.space_id != contracts::kQolBagSpaceC18) {
        return false;
    }
    const int64_t flat = offset + ref.bag;
    if (flat > std::numeric_limits<int32_t>::max()) return false;
    *out_bag = static_cast<int32_t>(flat);
    return true;
}

bool resolve_flat_bag(int32_t flat_bag, contracts::QolBagRef* out_ref) {
    if (flat_bag < 0 || out_ref == nullptr) return false;
    int32_t original_count = 0;
    int32_t c18_count = 0;
    int32_t extension_count = 0;
    if (!space_count(contracts::kQolBagSpaceOriginal, &original_count) ||
        !space_count(contracts::kQolBagSpaceC18, &c18_count) ||
        !space_count(contracts::kQolBagSpaceFirstRegistered, &extension_count)) {
        return false;
    }
    if (flat_bag < original_count) {
        *out_ref = {contracts::kQolBagSpaceOriginal, flat_bag};
        return true;
    }
    int32_t remaining = flat_bag - original_count;
    if (remaining < c18_count) {
        *out_ref = {contracts::kQolBagSpaceC18, remaining};
        return true;
    }
    remaining -= c18_count;
    if (remaining >= extension_count) return false;
    *out_ref = {contracts::kQolBagSpaceFirstRegistered, remaining};
    return true;
}

bool bag_capacity(const contracts::QolBagRef& ref, int32_t* out_capacity) {
    if (out_capacity == nullptr || !item_facts_ready()) return false;
    contracts::QolBagInfo info{};
    if (!contracts::succeeded(g_item_port->read.bag_info(&ref, &info)) || info.capacity < 0) {
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
    return g_item_port->read.item_at(&bag, slot, out);
}

QolResult validate_logical_identity(const contracts::ExtensionBagApi* eb,
                                     int32_t bag, int32_t slot,
                                     const contracts::QolInventoryItemRef& ref) {
    if (eb == nullptr || eb->is_logical_bag == nullptr || eb->internal_index == nullptr ||
        eb->identify_native_item == nullptr) {
        return slot_error(kQolErrUnsupported, "extension inventory unavailable", 31);
    }
    if (!eb->is_logical_bag(bag)) {
        return slot_error(kQolErrConflict, "stale inventory slot", 21);
    }
    const int32_t internal = eb->internal_index(bag);
    int32_t identified_bag = -1;
    int32_t identified_slot = -1;
    if (internal < 0 || ref.kind != static_cast<int32_t>(contracts::QolInventoryItemKind::kProvider) ||
        !eb->identify_native_item(ref.item, &identified_bag, &identified_slot) ||
        identified_bag != internal || identified_slot != slot) {
        return slot_error(kQolErrConflict, "stale inventory slot", 21);
    }
    return contracts::ok();
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

    const contracts::ExtensionBagApi* eb = nullptr;
    if (bag_ref.space_id == contracts::kQolBagSpaceFirstRegistered) {
        eb = extension_bag(0);
        if (eb == nullptr) {
            return slot_error(kQolErrUnsupported, "extension inventory unavailable", 31);
        }
    }

    contracts::QolInventoryItemRef ref{};
    result = ref_at(bag_ref, slot, &ref);
    if (!contracts::succeeded(result)) {
        if (result.code != kQolErrNotFound) return result;
        if (!flat_bag(bag_ref, &out->bag)) {
            return slot_error(kQolErrNotReady, "item facts unavailable", 22);
        }
        out->slot = slot;
        return contracts::ok();
    }
    if (ref.space_id != bag_ref.space_id || ref.bag != bag_ref.bag || ref.slot != slot) {
        return slot_error(kQolErrConflict, "stale inventory slot", 21);
    }
    if (ref.item.slot == 0) {
        if (!flat_bag(bag_ref, &out->bag)) {
            return slot_error(kQolErrNotReady, "item facts unavailable", 22);
        }
        out->slot = slot;
        return contracts::ok();
    }
    if (bag_ref.space_id == contracts::kQolBagSpaceFirstRegistered) {
        result = validate_logical_identity(eb, port_bag(bag_ref), slot, ref);
        if (!contracts::succeeded(result)) return result;
    } else if (ref.kind != static_cast<int32_t>(contracts::QolInventoryItemKind::kOriginal)) {
        return slot_error(kQolErrConflict, "stale inventory slot", 20);
    }
    result = fill_view(ref, out);
    if (contracts::succeeded(result) && !flat_bag(bag_ref, &out->bag)) {
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

    const int32_t spaces[] = {contracts::kQolBagSpaceOriginal,
                              contracts::kQolBagSpaceC18,
                              contracts::kQolBagSpaceFirstRegistered};
    for (int32_t space_id : spaces) {
        int32_t count = 0;
        if (!space_count(space_id, &count)) {
            return slot_error(kQolErrNotReady, "item facts unavailable", 22);
        }
        const contracts::ExtensionBagApi* eb =
            space_id == contracts::kQolBagSpaceFirstRegistered ? extension_bag(0) : nullptr;
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
                if (space_id == contracts::kQolBagSpaceFirstRegistered) {
                    result = validate_logical_identity(eb, port_bag(bag_ref), slot, ref);
                    if (!contracts::succeeded(result)) return result;
                } else if (ref.kind !=
                           static_cast<int32_t>(contracts::QolInventoryItemKind::kOriginal)) {
                    return slot_error(kQolErrConflict, "stale inventory item", 20);
                }
                result = fill_view(ref, out);
                if (contracts::succeeded(result) && !flat_bag(bag_ref, &out->bag)) {
                    return slot_error(kQolErrNotReady, "item facts unavailable", 22);
                }
                return result;
            }
        }
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
        const contracts::ExtensionBagApi* eb = extension_bag(
            reason == contracts::kRemoveSell ? contracts::kExtensionBagSaleRemoveVersionMinor : 0);
        if (eb == nullptr) {
            return slot_error(kQolErrUnsupported, "extension inventory unavailable", 31);
        }
        if (reason == contracts::kRemoveSell) {
            if (eb->remove_for_sale == nullptr) {
                return slot_error(kQolErrUnsupported, "extension sale unavailable", 26);
            }
            return eb->remove_for_sale(port_bag(bag_ref), slot, expected);
        }
        if (eb->remove_native_item == nullptr || !eb->remove_native_item(expected)) {
            return slot_error(kQolErrConflict, "extension item removal failed", 29);
        }
        return contracts::ok();
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
    g_sdk = sdk;
    g_game = game;
    g_item_facts = item_facts;
    g_item_port = item_port;
}

}  // namespace qol::features::inventory_hooks
