#include "core/native/inventory_trade.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "contracts/inventory_extension.h"
#include "contracts/item_facts.h"
#include "game/access/game_access.h"
#include "game/access/item_handle_registry.h"
#include "game/adapter_ports/inventory_extension_facade.h"
#include "game/adapter_ports/item_count_facade.h"
#include "game/state/game_state.h"
#include "infra/capabilities.h"

namespace {

alignas(8) uint8_t g_item[64]{};
InventoryItemKind g_kind = InventoryItemKind::kOriginal;
int g_count = 1;
int64_t g_money = 0;
int g_add_calls = 0;
int g_refund_calls = 0;
int g_generic_remove_calls = 0;
int g_finish_invalidation_calls = 0;
int g_invalidate_calls = 0;
int g_sell_price_calls = 0;
int g_quote_calls = 0;
uint32_t g_quote_num = 7;
uint32_t g_quote_den = 10;
int g_sync_calls = 0;
int g_remove_for_sale_calls = 0;
int g_callback_mode = 0;
bool g_available = true;
bool g_refund_fail = false;
bool g_provider_item_live = true;
uint32_t g_owner = qol::contracts::kQolItemHandleOwnerProvider;
const qol::contracts::QolItemHandle g_handle{17, 23};

bool provider_available() { return g_available; }

int32_t owns_bag(int32_t bag) { return bag >= 6 && bag <= 10 ? 1 : 0; }

QolResult provider_sell_price(qol::contracts::QolItemHandle, int32_t count,
                              int32_t, int64_t* out_price) {
    ++g_sell_price_calls;
    if (out_price == nullptr || count <= 0) return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    *out_price = 14 * count;
    return qol::contracts::ok();
}

QolResult item_facts_quote(qol::contracts::QolItemHandle, uint32_t sale_path,
                           qol::contracts::QolSaleQuote* out) {
    ++g_quote_calls;
    assert(sale_path == qol::contracts::kDetailSell);
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    out->struct_size = sizeof(*out);
    out->unit_price = 14;
    out->count = g_count;
    out->multiplier_num = g_quote_num;
    out->multiplier_den = g_quote_den;
    out->price = (14 * g_count * g_quote_num) / g_quote_den;
    return qol::contracts::ok();
}

qol::contracts::ItemFactsApi kItemFacts{
    sizeof(qol::contracts::ItemFactsApi), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &item_facts_quote};

QolResult remove_for_sale(int32_t bag, int32_t slot,
                          qol::contracts::QolItemHandle handle) {
    ++g_remove_for_sale_calls;
    assert(bag == 6 && slot == 2);
    assert(handle.slot == g_handle.slot && handle.generation == g_handle.generation);
    if (g_callback_mode == 1) {
        return qol::contracts::error(kQolErrNotReady, "not ready", 9);
    }
    if (g_callback_mode == 2) {
        return qol::contracts::error(kQolErrConflict, "remove failed", 13);
    }
    g_owner = qol::contracts::kQolItemHandleOwnerAny;
    g_provider_item_live = false;
    return qol::contracts::ok();
}

QolResult sync_slot(int32_t, int32_t) {
    ++g_sync_calls;
    return qol::contracts::ok();
}

qol::contracts::QolInventoryExtensionProvider kProvider{
    &owns_bag, nullptr, nullptr, nullptr, nullptr, nullptr, &provider_sell_price,
    &sync_slot, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    &remove_for_sale,
};

static_assert(qol::contracts::kQolInventoryExtensionVersion.major == 2 &&
                  qol::contracts::kQolInventoryExtensionVersion.minor == 1,
              "inventory extension provider callback is v2.1");
static_assert(offsetof(qol::contracts::QolInventoryExtensionProvider, use_item) ==
                  13 * sizeof(void*),
              "existing inventory extension provider prefix remains unchanged");
static_assert(offsetof(qol::contracts::QolInventoryExtensionProvider, remove_for_sale) ==
                  14 * sizeof(void*),
              "sale callback is appended after the v2.0 provider prefix");
static_assert(sizeof(qol::contracts::QolInventoryExtensionProvider) == 15 * sizeof(void*),
              "inventory extension provider has exactly one appended callback");

void reset(InventoryItemKind kind, int callback_mode = 0) {
    std::memset(g_item, 0, sizeof(g_item));
    g_kind = kind;
    g_count = 1;
    g_money = 0;
    g_add_calls = 0;
    g_refund_calls = 0;
    g_generic_remove_calls = 0;
    g_finish_invalidation_calls = 0;
    g_invalidate_calls = 0;
    g_sell_price_calls = 0;
    g_quote_calls = 0;
    g_quote_num = 7;
    g_quote_den = 10;
    g_sync_calls = 0;
    g_remove_for_sale_calls = 0;
    g_callback_mode = callback_mode;
    g_refund_fail = false;
    g_available = true;
    g_provider_item_live = true;
    g_owner = kind == InventoryItemKind::kProvider
                  ? qol::contracts::kQolItemHandleOwnerProvider
                  : qol::contracts::kQolItemHandleOwnerGame;
}

}  // namespace

GetBitFn fn_get_bit = nullptr;
GetCumulateCountFn fn_get_cumulate_count = nullptr;
ItemGetSellPriceFn fn_item_get_sell_price = nullptr;
IntIntFn fn_item_is_no_sell = nullptr;
RemoveItemFn fn_remove_item = nullptr;
AddMoneyFn fn_add_money = nullptr;
AddMoneyFn fn_minus_money = nullptr;

bool game_in_world() { return true; }

bool inventory_item_ref_at(int bag, int slot, InventoryItemRef* out) {
    if (out == nullptr) return false;
    *out = {g_kind, bag, slot, 77, g_count, g_item};
    return true;
}

namespace qol::game {

const contracts::QolItemCountApi* item_count_api_if_resident() { return nullptr; }

contracts::QolItemHandle lookup_item(void*, uint32_t* out_provider_id) {
    if (out_provider_id != nullptr) *out_provider_id = g_owner;
    return g_handle;
}

contracts::QolItemHandle register_item(void*, uint32_t provider_id, ItemHandleError*) {
    g_owner = provider_id;
    return g_handle;
}

bool begin_item_invalidation(void*, uint32_t, contracts::QolItemHandle* out_checkpoint) {
    if (out_checkpoint != nullptr) *out_checkpoint = g_handle;
    return true;
}

bool finish_item_invalidation(contracts::QolItemHandle, bool) {
    ++g_finish_invalidation_calls;
    return true;
}

bool invalidate_item(void*, uint32_t) {
    ++g_invalidate_calls;
    return true;
}

}  // namespace qol::game

namespace {

int get_bit(int, int, int) { return 77; }
int get_count(void*) { return g_count; }
int unit_sell_price(void*) { return 14; }
int is_no_sell(int32_t) { return 0; }
int add_money(int64_t amount) {
    ++g_add_calls;
    g_money += amount;
    return 1;
}
int minus_money(int64_t amount) {
    ++g_refund_calls;
    if (g_refund_fail) return 0;
    g_money -= amount;
    return 1;
}
int generic_remove(void*) {
    ++g_generic_remove_calls;
    return 1;
}

void test_provider_success_uses_only_neutral_sale_callback() {
    reset(InventoryItemKind::kProvider);
    const auto result = inventory_trade::sell(6, 2, g_item, "api");
    assert(result.status == inventory_trade::Status::kOk && result.price == 9);
    assert(result.count == 1 && g_add_calls == 1 && g_refund_calls == 0 && g_money == 9);
    assert(g_remove_for_sale_calls == 1 && g_generic_remove_calls == 0);
    assert(g_quote_calls == 1 && g_sell_price_calls == 0);
    assert(!g_provider_item_live);
    assert(g_finish_invalidation_calls == 0 && g_invalidate_calls == 0 && g_sync_calls == 0);
}

void test_provider_failure_refunds_without_mutating_slot() {
    for (int mode : {1, 2}) {
        reset(InventoryItemKind::kProvider, mode);
        const auto result = inventory_trade::sell(6, 2, g_item, "api");
        assert(result.status == inventory_trade::Status::kRemoveFailed);
        assert(g_add_calls == 1 && g_refund_calls == 1 && g_money == 0);
        assert(g_remove_for_sale_calls == 1 && g_generic_remove_calls == 0);
        assert(g_owner == qol::contracts::kQolItemHandleOwnerProvider);
        assert(g_provider_item_live);
        assert(g_finish_invalidation_calls == 0 && g_invalidate_calls == 0 && g_sync_calls == 0);
    }
}

void test_provider_missing_or_unavailable_callback_fails_closed() {
    reset(InventoryItemKind::kProvider);
    g_available = false;
    const auto unavailable = inventory_trade::sell(6, 2, g_item, "api");
    assert(unavailable.status == inventory_trade::Status::kRemoveFailed);
    assert(g_add_calls == 1 && g_refund_calls == 1 && g_money == 0);
    assert(g_generic_remove_calls == 0 && g_remove_for_sale_calls == 0);
    assert(g_provider_item_live);

    reset(InventoryItemKind::kProvider);
    qol::game::inventory_extension_bind_host();
    auto* const remove_callback = kProvider.remove_for_sale;
    kProvider.remove_for_sale = nullptr;
    const auto missing = inventory_trade::sell(6, 2, g_item, "api");
    assert(missing.status == inventory_trade::Status::kRemoveFailed);
    assert(g_add_calls == 1 && g_refund_calls == 1 && g_money == 0);
    assert(g_generic_remove_calls == 0 && g_remove_for_sale_calls == 0);
    assert(g_provider_item_live);
    kProvider.remove_for_sale = remove_callback;
}

void test_original_bag_keeps_generic_remove_path() {
    reset(InventoryItemKind::kOriginal);
    g_count = 3;
    const auto result = inventory_trade::sell(0, 6, g_item, "api");
    assert(result.status == inventory_trade::Status::kOk && result.price == 29);
    assert(g_add_calls == 1 && g_refund_calls == 0 && g_money == 29);
    assert(g_quote_calls == 1 && g_sell_price_calls == 0);
    assert(g_generic_remove_calls == 1 && g_remove_for_sale_calls == 0);
    assert(g_finish_invalidation_calls == 1 && g_invalidate_calls == 0 && g_sync_calls == 1);
}

void test_refund_failure_status_and_task_bag_exclusion() {
    reset(InventoryItemKind::kProvider, 2);
    g_refund_fail = true;
    const auto failed = inventory_trade::sell(6, 2, g_item, "api");
    assert(failed.status == inventory_trade::Status::kRollbackFailed);
    assert(g_add_calls == 1 && g_refund_calls == 1 && g_money == 9);

    reset(InventoryItemKind::kOriginal);
    const auto excluded = inventory_trade::sell(5, 2, g_item, "api");
    assert(excluded.status == inventory_trade::Status::kTaskBagExcluded);
    assert(g_add_calls == 0 && g_refund_calls == 0 && g_generic_remove_calls == 0);
}

}  // namespace

int main() {
    fn_get_bit = &get_bit;
    fn_get_cumulate_count = &get_count;
    fn_item_get_sell_price = &unit_sell_price;
    fn_item_is_no_sell = &is_no_sell;
    fn_remove_item = &generic_remove;
    fn_add_money = &add_money;
    fn_minus_money = &minus_money;
    qol::game::inventory_extension_bind_host();
    assert(qol::game::inventory_extension_set_provider(&kProvider, &provider_available));
    assert(qol::infra::register_capability({
        qol::contracts::kItemFactsCapabilityId,
        {qol::contracts::kItemFactsVersionMajor, qol::contracts::kItemFactsVersionMinor},
        &kItemFacts}));

    test_provider_success_uses_only_neutral_sale_callback();
    test_provider_failure_refunds_without_mutating_slot();
    test_provider_missing_or_unavailable_callback_fails_closed();
    test_original_bag_keeps_generic_remove_path();
    test_refund_failure_status_and_task_bag_exclusion();
    return 0;
}
