#include "contracts/extension_bag.h"
#include "contracts/inventory_action.h"
#include "contracts/inventory_slot.h"
#include "contracts/plugin.h"
#include "contracts/world_facts.h"
#include "contracts/item_facts.h"
#include "plugins/inventory_hooks/plugin.h"

#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_pass = 0;
static int g_fail = 0;
#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

using namespace qol::contracts;

QolInventoryItemRef g_item_ref{};
int32_t g_count = 1;
int64_t g_unit_price = 14;
int32_t g_world_state = 5;
int g_provider_minor = 1;
bool g_credit_fail = false;
bool g_refund_fail = false;
bool g_remove_fail = false;
bool g_identify_mismatch = false;
bool g_identify_handle_mismatch = false;
int g_identified_bag_override = -1;
bool g_internal_index_invalid = false;
bool g_logical_bag = false;
bool g_extension_available = true;
bool g_enforce_ref_location = false;
bool g_count_encoded = true;
int g_credit_calls = 0;
int g_refund_calls = 0;
int g_native_remove_calls = 0;
int g_extension_remove_calls = 0;
int g_identify_calls = 0;
int g_internal_index_calls = 0;
int32_t g_internal_index_argument = -1;
int32_t g_internal_index_result = -1;
int32_t g_identified_bag_result = -1;
QolItemHandle g_identify_handle_received{};
int g_price_variant = -1;
int g_extension_price_calls = 0;
int g_quote_calls = 0;
int g_quote_path = -1;
int g_quote_num = 7;
int g_quote_den = 10;
int g_quote_count_override = -1;
int g_quote_failure_code = kQolOk;
std::vector<int> g_order;

QolResult api_ok() { return qol::contracts::ok(); }
QolResult api_failure(int code, const char* message) {
    return qol::contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult fake_world_state(int32_t* out) {
    if (out == nullptr) return api_failure(kQolErrInvalidArgument, "out required");
    *out = g_world_state;
    return api_ok();
}

QolResult fake_item_ref_at(int32_t bag, int32_t slot, QolInventoryItemRef* out) {
    if (out == nullptr) return api_failure(kQolErrInvalidArgument, "out required");
    if (g_enforce_ref_location && (bag != g_item_ref.bag || slot != g_item_ref.slot)) {
        return api_failure(kQolErrNotFound, "slot empty");
    }
    *out = g_item_ref;
    out->bag = bag;
    out->slot = slot;
    return api_ok();
}

QolResult fake_no_sell(int32_t, int32_t* out) {
    if (out == nullptr) return api_failure(kQolErrInvalidArgument, "out required");
    *out = 0;
    return api_ok();
}

QolResult fake_count(QolItemHandle, int32_t* out) {
    if (out == nullptr) return api_failure(kQolErrInvalidArgument, "out required");
    *out = g_count;
    return api_ok();
}

QolResult fake_sell_price(QolItemHandle, int64_t* out) {
    if (out == nullptr) return api_failure(kQolErrInvalidArgument, "out required");
    *out = g_unit_price;
    return api_ok();
}

QolResult fake_quote(QolItemHandle, uint32_t sale_path, QolSaleQuote* out) {
    ++g_quote_calls;
    g_quote_path = static_cast<int>(sale_path);
    if (g_quote_failure_code != kQolOk) return api_failure(g_quote_failure_code, "quote failed");
    if (out == nullptr) return api_failure(kQolErrInvalidArgument, "out required");
    const int32_t count = g_quote_count_override >= 0 ? g_quote_count_override : g_count;
    out->struct_size = sizeof(*out);
    out->unit_price = g_unit_price;
    out->count = count;
    out->multiplier_num = static_cast<uint32_t>(g_quote_num);
    out->multiplier_den = static_cast<uint32_t>(g_quote_den);
    out->price = (g_unit_price * count * g_quote_num) / g_quote_den;
    return api_ok();
}

QolResult fake_count_encoding(QolItemHandle, QolItemCountEncoding* out) {
    if (out == nullptr) return api_failure(kQolErrInvalidArgument, "out required");
    *out = g_count_encoded ? QolItemCountEncoding::kEncoded
                           : QolItemCountEncoding::kNotEncoded;
    return api_ok();
}

QolResult fake_add_money(int64_t amount) {
    ++g_credit_calls;
    g_order.push_back(1);
    return g_credit_fail ? api_failure(kQolErrBlocked, "credit failed")
                         : (amount >= 0 ? api_ok() : api_failure(kQolErrInvalidArgument, "negative"));
}

QolResult fake_minus_money(int64_t amount) {
    ++g_refund_calls;
    g_order.push_back(3);
    return g_refund_fail ? api_failure(kQolErrBlocked, "refund failed")
                         : (amount >= 0 ? api_ok() : api_failure(kQolErrInvalidArgument, "negative"));
}

QolResult fake_native_remove(QolItemHandle) {
    ++g_native_remove_calls;
    g_order.push_back(2);
    return g_remove_fail ? api_failure(kQolErrNotFound, "native remove failed") : api_ok();
}

bool fake_is_logical_bag(int32_t) { return g_logical_bag; }

int32_t internal_index_for_logical_bag(int32_t bag) {
    if (bag < 6 || bag > 10) return -1;
    return bag - 6;
}

int32_t fake_internal_index(int32_t bag) {
    ++g_internal_index_calls;
    g_internal_index_argument = bag;
    g_internal_index_result = g_internal_index_invalid ? -1 : internal_index_for_logical_bag(bag);
    return g_internal_index_result;
}

bool fake_identify_native(QolItemHandle item, int32_t* bag, int32_t* slot) {
    ++g_identify_calls;
    g_identify_handle_received = item;
    if (bag == nullptr || slot == nullptr || !g_logical_bag || g_identify_handle_mismatch ||
        item.slot != g_item_ref.item.slot || item.generation != g_item_ref.item.generation) {
        return false;
    }
    *bag = g_identified_bag_override >= 0
               ? g_identified_bag_override
               : internal_index_for_logical_bag(g_item_ref.bag);
    g_identified_bag_result = *bag;
    *slot = g_identify_mismatch ? g_item_ref.slot + 1 : g_item_ref.slot;
    return true;
}

int64_t fake_extension_sell_price(QolItemHandle, int32_t count, bool discount) {
    ++g_extension_price_calls;
    g_price_variant = discount ? 1 : 0;
    return (static_cast<int64_t>(count) * g_unit_price * (discount ? 7 : 10)) / 10;
}

QolResult fake_extension_remove(int32_t bag, int32_t slot, QolItemHandle item) {
    ++g_extension_remove_calls;
    g_order.push_back(2);
    if (bag != g_item_ref.bag || slot != g_item_ref.slot || item.slot != g_item_ref.item.slot ||
        item.generation != g_item_ref.item.generation)
        return api_failure(kQolErrConflict, "extension identity mismatch");
    return g_remove_fail ? api_failure(kQolErrConflict, "extension delete failed") : api_ok();
}

void fake_sync_slot(int32_t, int32_t) {}

QolGameHost g_game{};
WorldFactsApi g_world{};
ItemFactsApi g_items{};
ExtensionBagApi g_extension{};
QolHostSdk g_sdk{};

QolResult fake_capability_require(const char* id, QolVersionRange range, const void** out) {
    if (id == nullptr || out == nullptr) return api_failure(kQolErrInvalidArgument, "bad require");
    *out = nullptr;
    if (std::strcmp(id, kWorldFactsCapabilityId) == 0) {
        *out = &g_world;
        return api_ok();
    }
    if (std::strcmp(id, kItemFactsCapabilityId) == 0) {
        *out = &g_items;
        return api_ok();
    }
    if (std::strcmp(id, kExtensionBagCapabilityId) == 0) {
        if (!g_extension_available) {
            return api_failure(kQolErrNotFound, "extension capability unavailable");
        }
        if (range.major != kExtensionBagVersionMajor || range.min_minor > g_provider_minor)
            return api_failure(kQolErrNotFound, "extension version unavailable");
        *out = &g_extension;
        return api_ok();
    }
    return api_failure(kQolErrNotFound, "capability missing");
}

void reset_fixture() {
    g_item_ref = {static_cast<int32_t>(QolInventoryItemKind::kOriginal), 0, 6, 42, 1, {12, 3}};
    g_count = 1;
    g_unit_price = 14;
    g_world_state = 5;
    g_provider_minor = 1;
    g_credit_fail = false;
    g_refund_fail = false;
    g_remove_fail = false;
    g_identify_mismatch = false;
    g_identify_handle_mismatch = false;
    g_identified_bag_override = -1;
    g_internal_index_invalid = false;
    g_logical_bag = false;
    g_credit_calls = 0;
    g_refund_calls = 0;
    g_native_remove_calls = 0;
    g_extension_remove_calls = 0;
    g_identify_calls = 0;
    g_internal_index_calls = 0;
    g_internal_index_argument = -1;
    g_internal_index_result = -1;
    g_identified_bag_result = -1;
    g_identify_handle_received = {};
    g_price_variant = -1;
    g_extension_price_calls = 0;
    g_quote_calls = 0;
    g_quote_path = -1;
    g_quote_num = 7;
    g_quote_den = 10;
    g_quote_count_override = -1;
    g_quote_failure_code = kQolOk;
    g_order.clear();
    g_extension_available = true;
    g_enforce_ref_location = false;
    g_count_encoded = true;
}

InventoryActionApi action_api() {
    const auto& descriptor = qol::features::inventory_hooks::plugin_descriptor();
    CHECK(descriptor.provides_count == 3);
    return *static_cast<const InventoryActionApi*>(descriptor.provides[1].iface);
}

void install_fixture() {
    const auto& descriptor = qol::features::inventory_hooks::plugin_descriptor();
    CHECK(descriptor.optional_count == 1);
    CHECK(std::strcmp(descriptor.optional_requires[0].id, "extension_bag") == 0);
    CHECK(descriptor.optional_requires[0].range.major == 3 &&
          descriptor.optional_requires[0].range.min_minor == 1);
    CHECK(descriptor.feature_count == 0);
    g_items = {};
    g_items.struct_size = sizeof(g_items);
    g_items.item_ref_at = &fake_item_ref_at;
    g_items.is_no_sell = &fake_no_sell;
    g_items.cumulate_count = &fake_count;
    g_items.sell_price = &fake_sell_price;
    g_items.count_encoding = &fake_count_encoding;
    g_items.quote = &fake_quote;
    g_world = {};
    g_world.state = &fake_world_state;
    g_game = {};
    g_game.world_action.add_money = &fake_add_money;
    g_game.world_action.minus_money = &fake_minus_money;
    g_game.inventory_write.remove_item = &fake_native_remove;
    g_extension = {};
    g_extension.is_logical_bag = &fake_is_logical_bag;
    g_extension.internal_index = &fake_internal_index;
    g_extension.identify_native_item = &fake_identify_native;
    g_extension.sell_price = &fake_extension_sell_price;
    g_extension.sync_projected_slot = &fake_sync_slot;
    g_extension.remove_for_sale = &fake_extension_remove;
    g_sdk = {};
    g_sdk.abi = {sizeof(g_sdk), kHostSdkMajor, kHostSdkMinor};
    g_sdk.capabilities.require = &fake_capability_require;
    PluginContext context{};
    context.struct_size = sizeof(context);
    context.sdk_major = kHostSdkMajor;
    context.sdk_minor = kHostSdkMinor;
    context.sdk = &g_sdk;
    context.game = &g_game;
    context.plugin_id = "inventory_hooks";
    CHECK(qol::features::inventory_hooks::plugin_descriptor().install(context) == 0);
}

void test_logical_sale_uses_v31_transaction() {
    reset_fixture();
    install_fixture();
    g_item_ref.kind = static_cast<int32_t>(QolInventoryItemKind::kProvider);
    g_item_ref.bag = 6;
    g_item_ref.slot = 2;
    g_item_ref.count = 3;
    g_count = 3;
    g_unit_price = 20;
    g_logical_bag = true;
    const InventorySellResult result = action_api().sell(6, 2, g_item_ref.item, "autosell");
    CHECK(result.status == InventorySellStatus::kOk && result.price == 42 && result.count == 3);
    CHECK(g_quote_calls == 1 && g_quote_path == static_cast<int>(kDetailSell));
    CHECK(g_price_variant == -1 && g_extension_price_calls == 0);
    CHECK(g_credit_calls == 1 && g_extension_remove_calls == 1 && g_native_remove_calls == 0);
    CHECK(g_internal_index_calls == 1 && g_internal_index_argument == 6);
    CHECK(g_identify_calls == 1 && g_identify_handle_received.slot == g_item_ref.item.slot &&
          g_identify_handle_received.generation == g_item_ref.item.generation);
    CHECK((g_order == std::vector<int>{1, 2}));

    reset_fixture();
    install_fixture();
    g_item_ref.kind = static_cast<int32_t>(QolInventoryItemKind::kProvider);
    g_item_ref.bag = 7;
    g_item_ref.slot = 1;
    g_item_ref.count = 2;
    g_count = 2;
    g_unit_price = 20;
    g_logical_bag = true;
    const InventorySellResult direct = action_api().sell(7, 1, g_item_ref.item, "api");
    CHECK(direct.status == InventorySellStatus::kOk && direct.price == 28 && direct.count == 2);
    CHECK(g_quote_calls == 1 && g_quote_path == static_cast<int>(kDetailSell));
    CHECK(g_price_variant == -1 && g_extension_price_calls == 0);
    CHECK(g_credit_calls == 1 && g_extension_remove_calls == 1 && g_native_remove_calls == 0);
    CHECK(g_internal_index_calls == 1 && g_internal_index_argument == 7);
    CHECK(g_identify_calls == 1 && g_identify_handle_received.slot == g_item_ref.item.slot &&
          g_identify_handle_received.generation == g_item_ref.item.generation);
    CHECK((g_order == std::vector<int>{1, 2}));
}

void test_internal_bag_identity_maps_all_extension_endpoints() {
    const int logical_bags[]{6, 7, 10};
    const int internal_bags[]{0, 1, 4};
    for (size_t i = 0; i < 3; ++i) {
        reset_fixture();
        install_fixture();
        g_item_ref.kind = static_cast<int32_t>(QolInventoryItemKind::kProvider);
        g_item_ref.bag = logical_bags[i];
        g_item_ref.slot = 3;
        g_item_ref.item = {32, 57};
        g_logical_bag = true;
        const InventorySellResult result =
            action_api().sell(logical_bags[i], 3, g_item_ref.item, "autosell");
        CHECK(result.status == InventorySellStatus::kOk);
        CHECK(g_internal_index_calls == 1 &&
              g_internal_index_argument == logical_bags[i] &&
              g_internal_index_result == internal_bags[i]);
        CHECK(g_identified_bag_result == internal_bags[i]);
        CHECK(g_identify_calls == 1 &&
              g_identify_handle_received.slot == 32 &&
              g_identify_handle_received.generation == 57);
        CHECK(g_extension_remove_calls == 1 && g_native_remove_calls == 0);
        CHECK((g_credit_calls == 1 && g_refund_calls == 0 &&
               g_order == std::vector<int>{1, 2}));
        CHECK(internal_bags[i] == internal_index_for_logical_bag(logical_bags[i]));
    }
}

void test_v30_provider_never_reads_tail_or_falls_back() {
    reset_fixture();
    install_fixture();
    g_item_ref.kind = static_cast<int32_t>(QolInventoryItemKind::kProvider);
    g_item_ref.bag = 7;
    g_item_ref.slot = 1;
    g_logical_bag = true;
    g_provider_minor = 0;
    const InventorySellResult result = action_api().sell(7, 1, g_item_ref.item, "autosell");
    CHECK(result.status == InventorySellStatus::kExtensionSaleUnavailable);
    CHECK(g_credit_calls == 0 && g_extension_remove_calls == 0 && g_native_remove_calls == 0);
}

void test_original_bag0_price_and_remove_path_unchanged() {
    reset_fixture();
    install_fixture();
    g_unit_price = 20;
    const InventorySellResult result = action_api().sell(0, 6, g_item_ref.item, "api");
    CHECK(result.status == InventorySellStatus::kOk && result.price == 14 && result.count == 1);
    CHECK(g_credit_calls == 1 && g_native_remove_calls == 1 && g_extension_remove_calls == 0);
    CHECK((g_order == std::vector<int>{1, 2}));
}

void test_credit_delete_and_refund_failure_semantics() {
    reset_fixture();
    install_fixture();
    g_item_ref.kind = static_cast<int32_t>(QolInventoryItemKind::kProvider);
    g_item_ref.bag = 6;
    g_item_ref.slot = 2;
    g_item_ref.count = 2;
    g_count = 2;
    g_logical_bag = true;
    g_credit_fail = true;
    auto result = action_api().sell(6, 2, g_item_ref.item, "autosell");
    CHECK(result.status == InventorySellStatus::kCreditFailed);
    CHECK(g_credit_calls == 1 && g_extension_remove_calls == 0 && g_refund_calls == 0);

    g_credit_fail = false;
    g_remove_fail = true;
    result = action_api().sell(6, 2, g_item_ref.item, "autosell");
    CHECK(result.status == InventorySellStatus::kRemoveFailed);
    CHECK(g_credit_calls == 2 && g_extension_remove_calls == 1 && g_refund_calls == 1);
    CHECK((g_order == std::vector<int>{1, 1, 2, 3}));

    g_refund_fail = true;
    result = action_api().sell(6, 2, g_item_ref.item, "autosell");
    CHECK(result.status == InventorySellStatus::kRollbackFailed);
    CHECK(g_refund_calls == 2);
}

void test_stale_identity_and_task_bag_reject_without_money() {
    reset_fixture();
    install_fixture();
    g_item_ref.kind = static_cast<int32_t>(QolInventoryItemKind::kProvider);
    g_item_ref.bag = 6;
    g_item_ref.slot = 2;
    g_logical_bag = true;
    g_identify_mismatch = true;
    CHECK(action_api().sell(6, 2, g_item_ref.item, "api").status == InventorySellStatus::kStale);
    CHECK(g_credit_calls == 0 && g_extension_remove_calls == 0);

    reset_fixture();
    install_fixture();
    g_item_ref.kind = static_cast<int32_t>(QolInventoryItemKind::kProvider);
    g_item_ref.bag = 7;
    g_item_ref.slot = 1;
    g_logical_bag = true;
    g_identified_bag_override = 0;
    CHECK(action_api().sell(7, 1, g_item_ref.item, "api").status ==
          InventorySellStatus::kStale);
    CHECK(g_internal_index_calls == 1 && g_internal_index_argument == 7);
    CHECK(g_identify_calls == 1 && g_credit_calls == 0 && g_extension_remove_calls == 0);

    reset_fixture();
    install_fixture();
    g_item_ref.kind = static_cast<int32_t>(QolInventoryItemKind::kProvider);
    g_item_ref.bag = 6;
    g_item_ref.slot = 2;
    g_logical_bag = true;
    g_internal_index_invalid = true;
    CHECK(action_api().sell(6, 2, g_item_ref.item, "api").status ==
          InventorySellStatus::kStale);
    CHECK(g_internal_index_calls == 1 && g_identify_calls == 0);
    CHECK(g_credit_calls == 0 && g_extension_remove_calls == 0);

    reset_fixture();
    install_fixture();
    g_item_ref.kind = static_cast<int32_t>(QolInventoryItemKind::kProvider);
    g_item_ref.bag = 6;
    g_item_ref.slot = 2;
    g_logical_bag = true;
    g_identify_handle_mismatch = true;
    CHECK(action_api().sell(6, 2, g_item_ref.item, "api").status ==
          InventorySellStatus::kStale);
    CHECK(g_identify_calls == 1 && g_identify_handle_received.slot == g_item_ref.item.slot &&
          g_identify_handle_received.generation == g_item_ref.item.generation);
    CHECK(g_credit_calls == 0 && g_extension_remove_calls == 0);

    reset_fixture();
    install_fixture();
    g_item_ref.kind = static_cast<int32_t>(QolInventoryItemKind::kProvider);
    g_item_ref.bag = 6;
    g_item_ref.slot = 2;
    g_logical_bag = true;
    g_identify_mismatch = true;
    CHECK(action_api().sell(6, 2, g_item_ref.item, "api").status ==
          InventorySellStatus::kStale);
    CHECK(g_identify_calls == 1 && g_credit_calls == 0 && g_extension_remove_calls == 0);

    reset_fixture();
    install_fixture();
    g_item_ref.kind = static_cast<int32_t>(QolInventoryItemKind::kProvider);
    g_item_ref.bag = 6;
    g_item_ref.slot = 2;
    g_logical_bag = true;
    g_extension.internal_index = nullptr;
    CHECK(action_api().sell(6, 2, g_item_ref.item, "api").status ==
          InventorySellStatus::kStale);
    CHECK(g_internal_index_calls == 0 && g_identify_calls == 0);
    CHECK(g_credit_calls == 0 && g_extension_remove_calls == 0);

    CHECK(action_api().sell(5, 2, g_item_ref.item, "api").status ==
          InventorySellStatus::kTaskBagExcluded);
    CHECK(g_credit_calls == 0 && g_native_remove_calls == 0);
}

void test_quote_prices_boundaries_and_missing_quote() {
    const int32_t counts[]{1, 99, 100, 127, 128, 199, 999};
    for (const int32_t count : counts) {
        reset_fixture();
        install_fixture();
        g_item_ref.bag = 0;
        g_item_ref.slot = 6;
        g_item_ref.count = count;
        g_enforce_ref_location = true;
        g_count = count;
        g_unit_price = 10;
        const InventorySellResult result = action_api().sell(0, 6, g_item_ref.item, "api");
        CHECK(result.status == InventorySellStatus::kOk);
        CHECK(result.count == count && result.price == (count * 10 * 7) / 10);
        CHECK(g_quote_calls == 1 && g_quote_path == static_cast<int>(kDetailSell));
    }

    // The action consumes quote multipliers for original/unknown, overhaul/monster and c18.
    const uint32_t numerators[]{0u, 0u, 7u, 7u, 85u};
    const uint32_t denominators[]{10u, 10u, 10u, 10u, 100u};
    const int64_t expected[]{0, 0, 21, 21, 25};
    for (size_t i = 0; i < 5; ++i) {
        reset_fixture();
        install_fixture();
        g_item_ref.bag = 0;
        g_item_ref.slot = 6;
        g_enforce_ref_location = true;
        g_count = 3;
        g_unit_price = 10;
        g_quote_num = static_cast<int>(numerators[i]);
        g_quote_den = static_cast<int>(denominators[i]);
        const InventorySellResult result = action_api().sell(0, 6, g_item_ref.item, "api");
        CHECK(result.status == InventorySellStatus::kOk && result.price == expected[i]);
    }

    reset_fixture();
    install_fixture();
    g_item_ref.bag = 0;
    g_item_ref.slot = 6;
    g_enforce_ref_location = true;
    g_items.struct_size = static_cast<uint32_t>(offsetof(ItemFactsApi, quote));
    const InventorySellResult missing_size = action_api().sell(0, 6, g_item_ref.item, "api");
    CHECK(missing_size.status == InventorySellStatus::kSymbolsUnresolved);
    CHECK(g_credit_calls == 0 && g_quote_calls == 0);

    reset_fixture();
    install_fixture();
    g_item_ref.bag = 0;
    g_item_ref.slot = 6;
    g_enforce_ref_location = true;
    g_items.quote = nullptr;
    const InventorySellResult missing_function = action_api().sell(0, 6, g_item_ref.item, "api");
    CHECK(missing_function.status == InventorySellStatus::kSymbolsUnresolved);
    CHECK(g_credit_calls == 0);
}

const QolInventorySlotApi* inventory_slot_api() {
    const auto& descriptor = qol::features::inventory_hooks::plugin_descriptor();
    CHECK(descriptor.provides_count == 3);
    CHECK(std::strcmp(descriptor.provides[2].id, kInventorySlotCapabilityId) == 0);
    return static_cast<const QolInventorySlotApi*>(descriptor.provides[2].iface);
}

void test_inventory_slot_resolve_view_remove() {
    reset_fixture();
    install_fixture();
    g_item_ref = {static_cast<int32_t>(QolInventoryItemKind::kOriginal), 0, 3, 42, 5, {21, 8}};
    g_enforce_ref_location = true;
    const QolInventorySlotApi* slots = inventory_slot_api();
    CHECK(slots != nullptr && slots->struct_size == sizeof(QolInventorySlotApi));
    QolInventorySlotView resolved{sizeof(QolInventorySlotView)};
    CHECK(qol::contracts::succeeded(slots->resolve(0, 3, &resolved)));
    CHECK(resolved.item.slot == 21 && resolved.item.generation == 8 && resolved.kind == 0u);
    CHECK(resolved.bag == 0 && resolved.slot == 3 && resolved.count == 5);
    CHECK((resolved.flags & 2u) != 0 && (resolved.flags & 1u) == 0);

    QolInventorySlotView empty{sizeof(QolInventorySlotView)};
    CHECK(qol::contracts::succeeded(slots->resolve(0, 4, &empty)));
    CHECK(empty.bag == 0 && empty.slot == 4 && empty.item.slot == 0 &&
          empty.item.generation == 0 && empty.kind == 0 && empty.category == 0 &&
          empty.count == 0 && empty.encoding == 0 && empty.flags == 0);
    g_item_ref.item = {};
    empty = {sizeof(QolInventorySlotView)};
    CHECK(qol::contracts::succeeded(slots->resolve(0, 3, &empty)));
    CHECK(empty.bag == 0 && empty.slot == 3 && empty.item.slot == 0 &&
          empty.item.generation == 0 && empty.count == 0 && empty.flags == 0);
    g_item_ref.item = {21, 8};

    QolInventorySlotView viewed{sizeof(QolInventorySlotView)};
    CHECK(qol::contracts::succeeded(slots->view({21, 8}, &viewed)));
    CHECK(viewed.item.slot == 21 && viewed.bag == 0 && viewed.slot == 3);
    CHECK(slots->view({21, 9}, &viewed).code == kQolErrConflict);

    CHECK(qol::contracts::succeeded(slots->remove(0, 3, {21, 8}, kRemoveDiscard)));
    CHECK(g_native_remove_calls == 1);
    g_remove_fail = true;
    CHECK(slots->remove(0, 3, {21, 8}, kRemoveDiscard).code == kQolErrNotFound);
    g_remove_fail = false;

    reset_fixture();
    install_fixture();
    g_item_ref = {static_cast<int32_t>(QolInventoryItemKind::kProvider), 6, 2, 51, 7, {32, 57}};
    g_enforce_ref_location = true;
    g_logical_bag = true;
    slots = inventory_slot_api();
    resolved = {sizeof(QolInventorySlotView)};
    CHECK(qol::contracts::succeeded(slots->resolve(6, 2, &resolved)));
    CHECK(resolved.kind == 1u && resolved.bag == 6 && resolved.slot == 2);
    viewed = {sizeof(QolInventorySlotView)};
    CHECK(qol::contracts::succeeded(slots->view({32, 57}, &viewed)));
    CHECK(viewed.kind == 1u && viewed.bag == 6 && viewed.slot == 2);
    CHECK(qol::contracts::succeeded(slots->remove(6, 2, {32, 57}, kRemoveSell)));
    CHECK(g_extension_remove_calls == 1);
    g_remove_fail = true;
    CHECK(slots->remove(6, 2, {32, 57}, kRemoveSell).code == kQolErrConflict);
    CHECK(g_extension_remove_calls == 2);

    CHECK(slots->remove(5, 3, {32, 57}, kRemoveSell).code == kQolErrUnsupported);
    CHECK(slots->remove(5, 3, {32, 57}, kRemoveDiscard).code == kQolErrUnsupported);

    reset_fixture();
    install_fixture();
    g_item_ref.kind = static_cast<int32_t>(QolInventoryItemKind::kProvider);
    g_item_ref.bag = 6;
    g_item_ref.slot = 2;
    g_enforce_ref_location = true;
    g_logical_bag = true;
    g_extension_available = false;
    slots = inventory_slot_api();
    resolved = {sizeof(QolInventorySlotView)};
    CHECK(slots->resolve(6, 2, &resolved).code == kQolErrUnsupported);
}

}  // namespace

int main() {
    test_logical_sale_uses_v31_transaction();
    test_internal_bag_identity_maps_all_extension_endpoints();
    test_v30_provider_never_reads_tail_or_falls_back();
    test_original_bag0_price_and_remove_path_unchanged();
    test_credit_delete_and_refund_failure_semantics();
    test_stale_identity_and_task_bag_reject_without_money();
    test_quote_prices_boundaries_and_missing_quote();
    test_inventory_slot_resolve_view_remove();
    std::printf("inventory_action_sell_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

bool inventory_native_hook_bind_sdk(const qol::contracts::QolHostSdk*) { return true; }
bool inventory_native_hook_bind_game(const qol::contracts::QolGameHost*) { return true; }
bool inventory_native_hook_bind_item_facts(const qol::contracts::ItemFactsApi*) { return true; }
bool inventory_native_hook_call_refresh_item_area_original() { return true; }
const qol::contracts::ResourceClaim* inventory_hooks_claims(uint32_t* out_count) {
    if (out_count != nullptr) *out_count = 0;
    return nullptr;
}
int32_t inventory_hooks_bind_backup(const char*, void*) { return 0; }
void inventory_log_bind_sdk(const qol::contracts::QolHostSdk*) {}
bool inventory_log_debug_enabled() { return false; }
bool inventory_log_debug_due(int64_t*, int64_t) { return false; }
void inventory_log_write(int32_t, const char*, int, const char*, ...) {}
