#include "sell_item.h"
#include "../hooks/claims.h"
#include "../hooks/inventory_logging.h"
#include "../hooks/inventory_hooks.h"
#include "item_slot.h"

#include <cstdint>

#include "../../../contracts/extension_bag.h"
#include "../../../contracts/inventory_action.h"
#include "../../../contracts/inventory_refresh.h"
#include "../../../contracts/inventory_slot.h"
#include "../../../contracts/world_facts.h"
#include "../../../contracts/item_facts.h"
#include "sell_transaction.h"

namespace qol::features::inventory_hooks {
namespace {

const contracts::QolHostSdk* g_action_sdk = nullptr;
const contracts::QolGameHost* g_action_game = nullptr;
const contracts::ItemFactsApi* g_item_facts = nullptr;
const contracts::QolItemPort* g_item_port = nullptr;

bool read_space_count(int32_t space_id, int32_t* out_count) {
    if (out_count == nullptr || g_item_port == nullptr || g_item_port->read.bag_count == nullptr ||
        !contracts::qol_item_port_bag_space_size_valid(g_item_port)) {
        return false;
    }
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

bool resolve_flat_bag(int32_t flat_bag, contracts::QolBagRef* out_ref) {
    if (flat_bag < 0 || out_ref == nullptr) return false;
    int32_t original_count = 0;
    int32_t c18_count = 0;
    int32_t extension_count = 0;
    if (!read_space_count(contracts::kQolBagSpaceOriginal, &original_count) ||
        !read_space_count(contracts::kQolBagSpaceC18, &c18_count) ||
        !read_space_count(contracts::kQolBagSpaceFirstRegistered, &extension_count)) {
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

int32_t host_bag(const contracts::QolBagRef& ref) {
    return ref.space_id == contracts::kQolBagSpaceOriginal
               ? ref.bag
               : contracts::kQolOriginalBagCount + ref.bag;
}

struct SellTransactionContext {
    const contracts::QolGameHost* game = nullptr;
    const contracts::ExtensionBagApi* extension_prefix = nullptr;
    const contracts::ExtensionBagApi* extension_sale = nullptr;
    contracts::QolItemHandle item{};
    int32_t bag = -1;
    int32_t slot = -1;
    bool extension_item = false;
    int32_t refund_code = kQolOk;
};

QolResult sell_credit(void* raw, int64_t price) {
    auto* context = static_cast<SellTransactionContext*>(raw);
    if (context == nullptr || context->game == nullptr ||
        context->game->world_action.add_money == nullptr)
        return contracts::error(kQolErrNotReady, "sell credit unavailable", 23);
    return context->game->world_action.add_money(price);
}

QolResult sell_remove(void* raw) {
    auto* context = static_cast<SellTransactionContext*>(raw);
    if (context == nullptr || context->game == nullptr)
        return contracts::error(kQolErrInvalidArgument, "sell context unavailable", 24);
    if (context->extension_item) {
        if (context->extension_sale == nullptr ||
            context->extension_sale->remove_for_sale == nullptr)
            return contracts::error(kQolErrUnsupported, "extension sale unavailable", 26);
        return context->extension_sale->remove_for_sale(context->bag, context->slot,
                                                        context->item);
    }
    if (context->game->inventory_write.remove_item == nullptr)
        return contracts::error(kQolErrNotReady, "physical removal unavailable", 27);
    return context->game->inventory_write.remove_item(context->item);
}

QolResult sell_refund(void* raw, int64_t price) {
    auto* context = static_cast<SellTransactionContext*>(raw);
    if (context == nullptr || context->game == nullptr ||
        context->game->world_action.minus_money == nullptr)
        return contracts::error(kQolErrNotReady, "sell refund unavailable", 22);
    const QolResult result = context->game->world_action.minus_money(price);
    context->refund_code = result.code;
    return result;
}

contracts::InventorySellResult inventory_action_sell(int32_t bag, int32_t slot,
                                                     contracts::QolItemHandle expected_item,
                                                     const char* source) {
    contracts::InventorySellResult result{};
    const contracts::WorldFactsApi* facts = nullptr;
    if (g_action_sdk != nullptr && g_action_sdk->capabilities.require != nullptr) {
        const void* iface = nullptr;
        const QolResult required = g_action_sdk->capabilities.require(
            contracts::kWorldFactsCapabilityId,
            {contracts::kWorldFactsVersionMajor, contracts::kWorldFactsVersionMinor}, &iface);
        if (contracts::succeeded(required) && iface != nullptr) {
            facts = static_cast<const contracts::WorldFactsApi*>(iface);
        }
    }
    if (facts == nullptr || facts->state == nullptr || g_action_game == nullptr ||
        g_item_facts == nullptr || g_item_facts->item_ref_at == nullptr || g_item_port == nullptr ||
        g_item_port->read.item_at == nullptr || g_item_port->read.bag_info == nullptr ||
        !contracts::qol_item_port_bag_space_size_valid(g_item_port)) {
        result.status = contracts::InventorySellStatus::kSymbolsUnresolved;
        return result;
    }
    int32_t world_state = 0;
    if (!contracts::succeeded(facts->state(&world_state)) ||
        world_state != 5) {
        result.status = contracts::InventorySellStatus::kNotInWorld;
        return result;
    }
    contracts::QolBagRef bag_ref{};
    if (!resolve_flat_bag(bag, &bag_ref)) {
        result.status = contracts::InventorySellStatus::kStale;
        return result;
    }
    if (bag_ref.space_id == contracts::kQolBagSpaceOriginal &&
        bag_ref.bag == contracts::kQolTaskBagIndex) {
        result.status = contracts::InventorySellStatus::kTaskBagExcluded;
        return result;
    }
    contracts::QolBagInfo bag_info{};
    if (!contracts::succeeded(g_item_port->read.bag_info(&bag_ref, &bag_info)) ||
        bag_info.capacity < 0 || slot < 0 || slot >= bag_info.capacity) {
        result.status = contracts::InventorySellStatus::kEmptySlot;
        return result;
    }
    contracts::QolInventoryItemRef item_ref{};
    if (!contracts::succeeded(g_item_port->read.item_at(&bag_ref, slot, &item_ref))) {
        result.status = contracts::InventorySellStatus::kEmptySlot;
        return result;
    }
    if (item_ref.space_id != bag_ref.space_id || item_ref.bag != bag_ref.bag ||
        item_ref.slot != slot) {
        result.status = contracts::InventorySellStatus::kStale;
        return result;
    }
    if (item_ref.item.slot == 0 || item_ref.item.generation == 0) {
        result.status = contracts::InventorySellStatus::kNoItemHandle;
        return result;
    }
    if (expected_item.slot != item_ref.item.slot ||
        expected_item.generation != item_ref.item.generation) {
        result.status = contracts::InventorySellStatus::kStale;
        return result;
    }
    result.category = item_ref.category;
    result.count = item_ref.count;
    const auto kind = static_cast<contracts::QolInventoryItemKind>(item_ref.kind);
    if (kind != contracts::QolInventoryItemKind::kOriginal &&
        kind != contracts::QolInventoryItemKind::kProvider) {
        result.status = contracts::InventorySellStatus::kStale;
        return result;
    }
    const contracts::ExtensionBagApi* eb_prefix = nullptr;
    if (g_action_sdk != nullptr && g_action_sdk->capabilities.require != nullptr) {
        const void* iface = nullptr;
        const QolResult required = g_action_sdk->capabilities.require(
            contracts::kExtensionBagCapabilityId,
            {static_cast<uint16_t>(contracts::kExtensionBagVersionMajor),
             static_cast<uint16_t>(contracts::kExtensionBagVersionMinor)},
            &iface);
        if (contracts::succeeded(required) && iface != nullptr)
            eb_prefix = static_cast<const contracts::ExtensionBagApi*>(iface);
    }
    const int32_t resolved_host_bag = host_bag(bag_ref);
    const bool logical_bag = bag_ref.space_id == contracts::kQolBagSpaceFirstRegistered &&
                             eb_prefix != nullptr && eb_prefix->is_logical_bag != nullptr &&
                             eb_prefix->is_logical_bag(resolved_host_bag);
    const bool extension_item = kind == contracts::QolInventoryItemKind::kProvider;
    if (extension_item != logical_bag) {
        result.status = extension_item ? contracts::InventorySellStatus::kExtensionSaleUnavailable
                                       : contracts::InventorySellStatus::kStale;
        return result;
    }
    const contracts::ExtensionBagApi* eb_sale = nullptr;
    if (extension_item) {
        int32_t identified_bag = -1;
        int32_t identified_slot = -1;
        if (eb_prefix == nullptr || eb_prefix->internal_index == nullptr ||
            eb_prefix->identify_native_item == nullptr) {
            result.status = contracts::InventorySellStatus::kStale;
            return result;
        }
        const int32_t internal_bag = eb_prefix->internal_index(resolved_host_bag);
        if (internal_bag < 0 ||
            !eb_prefix->identify_native_item(item_ref.item, &identified_bag, &identified_slot) ||
            identified_bag != internal_bag || identified_slot != slot) {
            result.status = contracts::InventorySellStatus::kStale;
            return result;
        }
        if (g_action_sdk == nullptr || g_action_sdk->capabilities.require == nullptr) {
            result.status = contracts::InventorySellStatus::kExtensionSaleUnavailable;
            return result;
        }
        // This separate negotiation is the only point at which the v3.1 tail is read.
        const void* sale_iface = nullptr;
        const QolResult negotiated = g_action_sdk->capabilities.require(
            contracts::kExtensionBagCapabilityId,
            {static_cast<uint16_t>(contracts::kExtensionBagVersionMajor),
             contracts::kExtensionBagSaleRemoveVersionMinor},
            &sale_iface);
        if (!contracts::succeeded(negotiated) || sale_iface == nullptr) {
            result.status = contracts::InventorySellStatus::kExtensionSaleUnavailable;
            return result;
        }
        eb_sale = static_cast<const contracts::ExtensionBagApi*>(sale_iface);
        if (eb_sale->remove_for_sale == nullptr) {
            result.status = contracts::InventorySellStatus::kExtensionSaleUnavailable;
            return result;
        }
    } else if (bag_ref.space_id == contracts::kQolBagSpaceFirstRegistered) {
        result.status = contracts::InventorySellStatus::kStale;
        return result;
    }
    if (g_item_facts == nullptr || g_item_facts->is_no_sell == nullptr) {
        result.status = contracts::InventorySellStatus::kSymbolsUnresolved;
        return result;
    }
    int32_t no_sell = 0;
    if (!contracts::succeeded(
            g_item_facts->is_no_sell(result.category, &no_sell))) {
        result.status = contracts::InventorySellStatus::kSymbolsUnresolved;
        return result;
    }
    if (no_sell != 0) {
        result.status = contracts::InventorySellStatus::kNoSell;
        return result;
    }
    if (!contracts::item_facts_api_quote_size_valid(g_item_facts) ||
        g_item_facts->quote == nullptr) {
        result.status = contracts::InventorySellStatus::kSymbolsUnresolved;
        return result;
    }
    contracts::QolSaleQuote quote{};
    quote.struct_size = sizeof(quote);
    if (!contracts::succeeded(
            g_item_facts->quote(item_ref.item, contracts::kDetailSell, &quote)) ||
        quote.struct_size < sizeof(quote) || quote.count <= 0 || quote.price < 0 ||
        quote.multiplier_den == 0 || quote.multiplier_num > quote.multiplier_den) {
        result.status = contracts::InventorySellStatus::kPriceInvalid;
        return result;
    }
    const int32_t count = quote.count;
    const int64_t price = quote.price;
    if (price < 0 || g_action_game->world_action.add_money == nullptr ||
        g_action_game->world_action.minus_money == nullptr ||
        (!extension_item && g_action_game->inventory_write.remove_item == nullptr)) {
        result.status = contracts::InventorySellStatus::kPriceInvalid;
        return result;
    }
    result.price = price;
    result.count = count;
    SellTransactionContext transaction{g_action_game, eb_prefix, eb_sale, item_ref.item,
                                        resolved_host_bag, slot, extension_item, kQolOk};
    result.status = run_sale_transaction(
        price, SaleTransactionOps{&transaction, &sell_credit, &sell_remove, &sell_refund});
    if (result.status == contracts::InventorySellStatus::kRollbackFailed) {
        QOL_LOG_ERROR(QolDomain::kInventory,
                       "inventory sell rollback failed source=%s bag=%d slot=%d price=%lld remove=%s refund_code=%d",
                      source != nullptr ? source : "unknown", bag, slot,
                      static_cast<long long>(price), extension_item ? "extension" : "original",
                      transaction.refund_code);
    } else if (result.status == contracts::InventorySellStatus::kRemoveFailed) {
        QOL_LOG_WARN(QolDomain::kInventory,
                     "inventory sell removal failed; refund succeeded source=%s bag=%d slot=%d",
                     source != nullptr ? source : "unknown", bag, slot);
    }
    if (result.status == contracts::InventorySellStatus::kOk && !extension_item &&
        bag_ref.space_id == contracts::kQolBagSpaceOriginal &&
        eb_prefix != nullptr && eb_prefix->sync_projected_slot != nullptr)
        (void)eb_prefix->sync_projected_slot(bag, slot);
    return result;
}

}  // namespace

const contracts::InventoryRefreshApi kInventoryRefreshApi{
    &inventory_native_hook_call_refresh_item_area_original,
    &inventory_native_hook_update_shortcut,
};

const contracts::InventoryActionApi kInventoryActionApi{
    &inventory_action_sell,
};

bool attach(const contracts::QolHostSdk* sdk, const contracts::QolGameHost* game,
            const contracts::ItemFactsApi* item_facts) {
    const contracts::QolItemPort* const item_port = detail::bound_item_port();
    if (sdk == nullptr || game == nullptr || !contracts::item_facts_api_size_valid(item_facts) ||
        item_port == nullptr || !contracts::qol_item_port_bag_space_size_valid(item_port) ||
        !inventory_native_hook_bind_sdk(sdk) || !inventory_native_hook_bind_game(game) ||
        !inventory_native_hook_bind_item_facts(item_facts)) {
        detach();
        return false;
    }
    g_action_sdk = sdk;
    g_action_game = game;
    g_item_facts = item_facts;
    g_item_port = item_port;
    inventory_slot_bind(sdk, game, item_facts, item_port);
    inventory_native_hook_set_support(true);
    return true;
}

void detach() {
    g_action_sdk = nullptr;
    g_action_game = nullptr;
    g_item_facts = nullptr;
    g_item_port = nullptr;
    detail::bound_item_port() = nullptr;
    inventory_slot_bind(nullptr, nullptr, nullptr, nullptr);
    (void)inventory_native_hook_bind_sdk(nullptr);
    (void)inventory_native_hook_bind_game(nullptr);
    (void)inventory_native_hook_bind_item_facts(nullptr);
    inventory_native_hook_set_support(true);
}

const contracts::ResourceClaim* claims(uint32_t* out_count) {
    return inventory_hooks_claims(out_count);
}

int32_t bind_backup(const char* resource_id, void* backup) {
    return inventory_hooks_bind_backup(resource_id, backup);
}

}  // namespace qol::features::inventory_hooks
