#include "core/native/inventory_trade.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "contracts/item_facts.h"
#include "game/access/game_access.h"
#include "game/access/item_handle_registry.h"
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
int g_quote_calls = 0;
uint32_t g_quote_num = 7;
uint32_t g_quote_den = 10;
bool g_refund_fail = false;
uint32_t g_owner = qol::contracts::kQolItemHandleOwnerGame;
const qol::contracts::QolItemHandle g_handle{17, 23};

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

void reset(InventoryItemKind kind) {
    std::memset(g_item, 0, sizeof(g_item));
    g_kind = kind;
    g_count = 1;
    g_money = 0;
    g_add_calls = 0;
    g_refund_calls = 0;
    g_generic_remove_calls = 0;
    g_finish_invalidation_calls = 0;
    g_invalidate_calls = 0;
    g_quote_calls = 0;
    g_quote_num = 7;
    g_quote_den = 10;
    g_refund_fail = false;
    g_owner = qol::contracts::kQolItemHandleOwnerGame;
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

void test_registered_source_sell_fails_closed() {
    reset(InventoryItemKind::kProvider);
    const auto result = inventory_trade::sell(6, 2, g_item, "api");
    assert(result.status == inventory_trade::Status::kNoNativeItem);
    assert(g_add_calls == 0 && g_refund_calls == 0 && g_money == 0);
    assert(g_generic_remove_calls == 0 && g_quote_calls == 0);
    const auto discarded = inventory_trade::discard(6, 2, g_item);
    assert(discarded == inventory_trade::Status::kNoNativeItem);
    assert(g_generic_remove_calls == 0);
}

void test_original_bag_keeps_generic_remove_path() {
    reset(InventoryItemKind::kOriginal);
    g_count = 3;
    const auto result = inventory_trade::sell(0, 6, g_item, "api");
    assert(result.status == inventory_trade::Status::kOk && result.price == 29);
    assert(g_add_calls == 1 && g_refund_calls == 0 && g_money == 29);
    assert(g_quote_calls == 1);
    assert(g_generic_remove_calls == 1);
    assert(g_finish_invalidation_calls == 1 && g_invalidate_calls == 0);
}

void test_task_bag_exclusion() {
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
    assert(qol::infra::register_capability({
        qol::contracts::kItemFactsCapabilityId,
        {qol::contracts::kItemFactsVersionMajor, qol::contracts::kItemFactsVersionMinor},
        &kItemFacts}));

    test_registered_source_sell_fails_closed();
    test_original_bag_keeps_generic_remove_path();
    test_task_bag_exclusion();
    return 0;
}
