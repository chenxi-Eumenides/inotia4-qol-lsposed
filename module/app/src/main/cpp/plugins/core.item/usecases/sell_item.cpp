#include "sell_item.h"

#include "../hooks/claims.h"
#include "../hooks/inventory_hooks.h"
#include "../hooks/inventory_logging.h"
#include "item_slot.h"
#include "sell_transaction.h"

#include <cstdint>

#include "../../../contracts/bag.h"
#include "../../../contracts/item_facts.h"
#include "../../../contracts/world_facts.h"
#include "plugins/core.item/bags/transfer.h"

namespace qol::features::core_item {
const contracts::QolBagSource* find_bag_source(int32_t space_id);
}

namespace qol::features::inventory_hooks {
namespace {

const contracts::QolHostSdk* g_action_sdk = nullptr;
const contracts::QolGameHost* g_action_game = nullptr;
const contracts::ItemFactsApi* g_item_facts = nullptr;
const contracts::QolItemPort* g_item_port = nullptr;

bool valid_ref(const contracts::QolActionRef& ref) {
    return ref.space_id >= contracts::kQolBagSpaceOriginal &&
           ref.space_id <= contracts::kQolBagSpaceFirstRegistered && ref.bag >= 0 &&
           ref.slot >= 0;
}

bool valid_handle(contracts::QolItemHandle item) {
    return item.slot != 0 && item.generation != 0;
}

struct NativeSaleContext {
    const contracts::QolGameHost* game;
    contracts::QolItemHandle item;
    int32_t refund_code;
};

QolResult sell_credit(void* raw, int64_t price) {
    auto* context = static_cast<NativeSaleContext*>(raw);
    if (context == nullptr || context->game == nullptr ||
        context->game->world_action.add_money == nullptr) {
        constexpr char kMessage[] = "sell credit unavailable";
        return contracts::error(kQolErrNotReady, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    return context->game->world_action.add_money(price);
}

QolResult native_remove(void* raw) {
    auto* context = static_cast<NativeSaleContext*>(raw);
    if (context == nullptr || context->game == nullptr ||
        context->game->inventory_write.remove_item == nullptr) {
        constexpr char kMessage[] = "native removal unavailable";
        return contracts::error(kQolErrNotReady, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    return context->game->inventory_write.remove_item(context->item);
}

QolResult native_refund(void* raw, int64_t price) {
    auto* context = static_cast<NativeSaleContext*>(raw);
    if (context == nullptr || context->game == nullptr ||
        context->game->world_action.minus_money == nullptr) {
        constexpr char kMessage[] = "sell refund unavailable";
        return contracts::error(kQolErrNotReady, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    const QolResult result = context->game->world_action.minus_money(price);
    context->refund_code = result.code;
    return result;
}

contracts::InventorySellResult sell_item(contracts::QolActionRef ref,
                                         contracts::QolItemHandle expected_item, int32_t count,
                                         const char* source_name) {
    contracts::InventorySellResult result{};
    result.status = contracts::InventorySellStatus::kSymbolsUnresolved;
    if (!valid_ref(ref)) {
        result.status = contracts::InventorySellStatus::kStale;
        return result;
    }
    if (count <= 0) {
        result.status = contracts::InventorySellStatus::kPriceInvalid;
        return result;
    }
    if (!valid_handle(expected_item)) {
        result.status = contracts::InventorySellStatus::kNoItemHandle;
        return result;
    }
    const contracts::WorldFactsApi* world_facts = nullptr;
    if (g_action_sdk != nullptr && g_action_sdk->capabilities.require != nullptr) {
        const void* iface = nullptr;
        const QolResult required = g_action_sdk->capabilities.require(
            contracts::kWorldFactsCapabilityId,
            {contracts::kWorldFactsVersionMajor, contracts::kWorldFactsVersionMinor}, &iface);
        if (contracts::succeeded(required) && iface != nullptr) {
            world_facts = static_cast<const contracts::WorldFactsApi*>(iface);
        }
    }
    if (world_facts == nullptr || world_facts->state == nullptr || g_action_game == nullptr ||
        g_item_facts == nullptr ||
        g_item_facts->is_no_sell == nullptr || g_item_port == nullptr ||
        !contracts::qol_item_port_bag_space_size_valid(g_item_port) ||
        g_item_port->read.item_at == nullptr) {
        return result;
    }
    if (ref.space_id == contracts::kQolBagSpaceOriginal &&
        ref.bag == contracts::kQolTaskBagIndex) {
        result.status = contracts::InventorySellStatus::kTaskBagExcluded;
        return result;
    }

    int32_t world_state = 0;
    if (!contracts::succeeded(world_facts->state(&world_state)) || world_state != 5) {
        result.status = contracts::InventorySellStatus::kNotInWorld;
        return result;
    }

    const contracts::QolBagRef bag{ref.space_id, ref.bag};
    contracts::QolInventoryItemRef item{};
    const QolResult read = g_item_port->read.item_at(&bag, ref.slot, &item);
    if (!contracts::succeeded(read)) {
        result.status = read.code == kQolErrNotFound
                            ? contracts::InventorySellStatus::kEmptySlot
                            : contracts::InventorySellStatus::kStale;
        return result;
    }
    if (item.space_id != ref.space_id || item.bag != ref.bag || item.slot != ref.slot) {
        result.status = contracts::InventorySellStatus::kStale;
        return result;
    }
    if (!valid_handle(item.item)) {
        result.status = contracts::InventorySellStatus::kNoItemHandle;
        return result;
    }
    if (item.item.slot != expected_item.slot ||
        item.item.generation != expected_item.generation) {
        result.status = contracts::InventorySellStatus::kStale;
        return result;
    }
    result.category = item.category;
    result.count = item.count;
    if (item.count < count || item.category <= 0) {
        result.status = contracts::InventorySellStatus::kStale;
        return result;
    }
    const auto kind = static_cast<contracts::QolInventoryItemKind>(item.kind);
    const bool extension_item = ref.space_id >= contracts::kQolBagSpaceFirstRegistered;
    if ((extension_item && kind != contracts::QolInventoryItemKind::kProvider) ||
        (!extension_item && kind != contracts::QolInventoryItemKind::kOriginal)) {
        result.status = contracts::InventorySellStatus::kStale;
        return result;
    }

    int32_t no_sell = 0;
    if (!contracts::succeeded(g_item_facts->is_no_sell(item.category, &no_sell))) {
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
    if (!contracts::succeeded(g_item_facts->quote(item.item, contracts::kDetailSell, &quote)) ||
        quote.struct_size < sizeof(quote) || quote.count < count || quote.unit_price < 0 ||
        quote.multiplier_den == 0 || quote.multiplier_num > quote.multiplier_den ||
        !contracts::sell_price::calculate(quote.unit_price, static_cast<uint32_t>(count),
                                          quote.multiplier_num, quote.multiplier_den,
                                          &result.price)) {
        result.status = contracts::InventorySellStatus::kPriceInvalid;
        result.price = 0;
        return result;
    }
    result.count = count;
    if (g_action_game->world_action.add_money == nullptr) {
        result.status = contracts::InventorySellStatus::kPriceInvalid;
        return result;
    }

    if (!extension_item) {
        if (g_action_game->inventory_write.remove_item == nullptr ||
            g_action_game->world_action.minus_money == nullptr) {
            result.status = contracts::InventorySellStatus::kPriceInvalid;
            return result;
        }
        NativeSaleContext transaction{g_action_game, item.item, kQolOk};
        result.status = run_sale_transaction(
            result.price,
            SaleTransactionOps{&transaction, &sell_credit, &native_remove, &native_refund});
        if (result.status == contracts::InventorySellStatus::kRollbackFailed) {
            QOL_LOG_ERROR(QolDomain::kInventory,
                          "inventory sell rollback failed source=%s space=%d bag=%d slot=%d price=%lld refund_code=%d",
                          source_name != nullptr ? source_name : "unknown", ref.space_id, ref.bag,
                          ref.slot, static_cast<long long>(result.price), transaction.refund_code);
        } else if (result.status == contracts::InventorySellStatus::kRemoveFailed) {
            QOL_LOG_WARN(QolDomain::kInventory,
                         "inventory sell removal failed; refund succeeded source=%s space=%d bag=%d slot=%d",
                         source_name != nullptr ? source_name : "unknown", ref.space_id, ref.bag,
                         ref.slot);
        }
        return result;
    }

    const contracts::QolBagSource* bag_source =
        qol::features::core_item::find_bag_source(ref.space_id);
    if (bag_source == nullptr || bag_source->export_item == nullptr ||
        bag_source->remove == nullptr || bag_source->import_item == nullptr) {
        result.status = contracts::InventorySellStatus::kSymbolsUnresolved;
        return result;
    }
    qol::features::core_item::bags::BagTransfer transfer{};
    if (!contracts::succeeded(bag_source->export_item(item.item, count, &transfer)) ||
        transfer.category != item.category || transfer.count != count ||
        transfer.payload_size == 0 ||
        transfer.payload_size > contracts::kQolBagTransferPayloadMaxSize) {
        result.status = contracts::InventorySellStatus::kSymbolsUnresolved;
        return result;
    }
    contracts::QolBagMutationResult removed{};
    if (!contracts::succeeded(bag_source->remove(item.item, count, &removed))) {
        result.status = contracts::InventorySellStatus::kRemoveFailed;
        return result;
    }
    if (contracts::succeeded(g_action_game->world_action.add_money(result.price))) {
        result.status = contracts::InventorySellStatus::kOk;
        return result;
    }
    contracts::QolBagMutationResult restored{};
    const QolResult rollback = bag_source->import_item(&transfer, ref.bag, ref.slot, &restored);
    result.status = contracts::succeeded(rollback)
                        ? contracts::InventorySellStatus::kCreditFailed
                        : contracts::InventorySellStatus::kRollbackFailed;
    return result;
}

}  // namespace

const contracts::InventoryRefreshApi kInventoryRefreshApi{
    &inventory_native_hook_call_refresh_item_area_original,
    &inventory_native_hook_update_shortcut,
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

contracts::InventorySellResult inventory_action_sell(contracts::QolActionRef ref,
                                                     contracts::QolItemHandle expected_item,
                                                     int32_t count, const char* source) {
    return sell_item(ref, expected_item, count, source);
}

const contracts::ResourceClaim* claims(uint32_t* out_count) {
    return inventory_hooks_claims(out_count);
}

int32_t bind_backup(const char* resource_id, void* backup) {
    return inventory_hooks_bind_backup(resource_id, backup);
}

}  // namespace qol::features::inventory_hooks
