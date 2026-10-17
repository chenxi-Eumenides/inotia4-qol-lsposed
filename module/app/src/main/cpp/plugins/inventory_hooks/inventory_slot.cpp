#include "inventory_slot.h"

#include "../../contracts/extension_bag.h"

namespace qol::features::inventory_hooks {
namespace {

constexpr int32_t kPhysicalBagFirst = 0;
constexpr int32_t kPhysicalBagLast = 5;
constexpr int32_t kLogicalBagFirst = 6;
constexpr int32_t kLogicalBagLast = 10;
constexpr int32_t kSlotsPerBag = 16;

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;
const contracts::ItemFactsApi* g_item_facts = nullptr;

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
           g_item_facts->item_ref_at != nullptr;
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

QolResult ref_at(int32_t bag, int32_t slot, contracts::QolInventoryItemRef* out) {
    if (!item_facts_ready()) {
        return slot_error(kQolErrNotReady, "item facts unavailable", 22);
    }
    return g_item_facts->item_ref_at(bag, slot, out);
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
    if (slot < 0 || slot >= kSlotsPerBag || bag < kPhysicalBagFirst || bag > kLogicalBagLast) {
        return slot_error(kQolErrInvalidArgument, "invalid inventory slot", 22);
    }

    const contracts::ExtensionBagApi* eb = nullptr;
    if (bag >= kLogicalBagFirst) {
        eb = extension_bag(0);
        if (eb == nullptr) {
            return slot_error(kQolErrUnsupported, "extension inventory unavailable", 31);
        }
    }

    contracts::QolInventoryItemRef ref{};
    result = ref_at(bag, slot, &ref);
    if (!contracts::succeeded(result)) {
        if (result.code != kQolErrNotFound) return result;
        out->bag = bag;
        out->slot = slot;
        return contracts::ok();
    }
    if (ref.item.slot == 0) {
        out->bag = bag;
        out->slot = slot;
        return contracts::ok();
    }
    if (bag >= kLogicalBagFirst) {
        result = validate_logical_identity(eb, bag, slot, ref);
        if (!contracts::succeeded(result)) return result;
    } else if (ref.kind != static_cast<int32_t>(contracts::QolInventoryItemKind::kOriginal)) {
        return slot_error(kQolErrConflict, "stale inventory slot", 20);
    }
    return fill_view(ref, out);
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

    const contracts::ExtensionBagApi* eb = extension_bag(0);
    if (eb != nullptr && eb->identify_native_item != nullptr && eb->internal_index != nullptr &&
        eb->is_logical_bag != nullptr) {
        int32_t internal = -1;
        int32_t slot = -1;
        if (eb->identify_native_item(item, &internal, &slot)) {
            if (slot < 0 || slot >= kSlotsPerBag) {
                return slot_error(kQolErrConflict, "stale inventory item", 20);
            }
            for (int32_t bag = kLogicalBagFirst; bag <= kLogicalBagLast; ++bag) {
                if (!eb->is_logical_bag(bag) || eb->internal_index(bag) != internal) continue;
                contracts::QolInventoryItemRef ref{};
                result = ref_at(bag, slot, &ref);
                if (!contracts::succeeded(result)) return result;
                if (ref.item.slot != item.slot || ref.item.generation != item.generation) {
                    return slot_error(kQolErrConflict, "stale inventory item", 20);
                }
                return fill_view(ref, out);
            }
            return slot_error(kQolErrConflict, "stale inventory item", 20);
        }
    }

    for (int32_t bag = kPhysicalBagFirst; bag <= kPhysicalBagLast; ++bag) {
        for (int32_t slot = 0; slot < kSlotsPerBag; ++slot) {
            contracts::QolInventoryItemRef ref{};
            result = ref_at(bag, slot, &ref);
            if (!contracts::succeeded(result)) {
                if (result.code == kQolErrNotFound) continue;
                return result;
            }
            if (ref.item.slot == 0 || ref.item.slot != item.slot ||
                ref.item.generation != item.generation) {
                continue;
            }
            if (ref.kind != static_cast<int32_t>(contracts::QolInventoryItemKind::kOriginal)) {
                return slot_error(kQolErrConflict, "stale inventory item", 20);
            }
            return fill_view(ref, out);
        }
    }
    return slot_error(kQolErrConflict, "stale inventory item", 20);
}

QolResult remove_item(int32_t bag, int32_t slot, contracts::QolItemHandle expected,
                      uint32_t reason) {
    if (bag == 5) return slot_error(kQolErrUnsupported, "task bag excluded", 17);
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
            return eb->remove_for_sale(bag, slot, expected);
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
                         const contracts::ItemFactsApi* item_facts) {
    g_sdk = sdk;
    g_game = game;
    g_item_facts = item_facts;
}

}  // namespace qol::features::inventory_hooks
