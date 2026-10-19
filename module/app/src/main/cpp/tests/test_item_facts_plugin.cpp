#include "contracts/item_facts.h"
#include "contracts/inventory_slot.h"
#include "contracts/inventory_action.h"
#include "contracts/inventory_extension.h"
#include "contracts/extension_bag.h"
#include "contracts/character_state.h"
#include "contracts/item_state.h"
#include "contracts/extension_bag.h"
#include "contracts/game_ports/ports.h"
#include "contracts/game_ports/character_extras_port.h"
#include "contracts/game_host.h"
#include "contracts/attribute_quality.h"
#include "plugins/core.item/plugin.h"

#include <cstdio>
#include <cstddef>
#include <cstring>
#include <type_traits>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static_assert(std::is_same<qol::contracts::ItemFactsCategoryFn,
                           QolResult (*)(qol::contracts::QolItemHandle, int32_t*)>::value,
               "item.facts category must be handle-keyed");
static_assert(offsetof(qol::contracts::ItemFactsApi, count_limit) ==
                  offsetof(qol::contracts::ItemFactsApi, effective_count) +
                      sizeof(qol::contracts::ItemFactsEffectiveCountFn),
              "ItemFactsApi count_limit must be tail-appended");
static_assert(offsetof(qol::contracts::QolItemPortReadApi, count_limit) ==
                  offsetof(qol::contracts::QolItemPortReadApi, inventory_extension_access) +
                      sizeof(decltype(qol::contracts::QolItemPortReadApi::inventory_extension_access)),
              "Item Port count_limit must be tail-appended");
static_assert(offsetof(qol::contracts::ItemFactsApi, count_encoding) ==
                  offsetof(qol::contracts::ItemFactsApi, count_limit) +
                      sizeof(qol::contracts::ItemFactsCountLimitFn),
              "ItemFacts count_encoding must be tail-appended");
static_assert(offsetof(qol::contracts::QolItemPortReadApi, count_encoding) ==
                  offsetof(qol::contracts::QolItemPortReadApi, count_limit) +
                      sizeof(decltype(qol::contracts::QolItemPortReadApi::count_limit)),
              "Item Port count_encoding must be tail-appended");
static_assert(offsetof(qol::contracts::ItemFactsApi, quote) ==
                  offsetof(qol::contracts::ItemFactsApi, count_encoding) +
                      sizeof(qol::contracts::ItemFactsCountEncodingFn),
              "ItemFacts quote must be tail-appended");
static_assert(offsetof(qol::contracts::QolItemPortReadApi, quote) ==
                  offsetof(qol::contracts::QolItemPortReadApi, count_encoding) +
                      sizeof(decltype(qol::contracts::QolItemPortReadApi::count_encoding)),
              "Item Port quote must be tail-appended");
static_assert(std::is_same<qol::contracts::ItemFactsCountEncodingFn,
                           QolResult (*)(qol::contracts::QolItemHandle,
                                         qol::contracts::QolItemCountEncoding*)>::value,
               "item.facts count encoding must be handle-keyed");
static_assert(std::is_same<qol::contracts::ItemFactsQuoteFn,
                           QolResult (*)(qol::contracts::QolItemHandle, uint32_t,
                                         qol::contracts::QolSaleQuote*)>::value,
              "item.facts quote must be handle-keyed");
static_assert(std::is_same<qol::contracts::QolItemPortQuoteFn,
                           QolResult (*)(qol::contracts::QolItemHandle, uint32_t,
                                         qol::contracts::QolSaleQuote*)>::value,
              "Item Port quote must be handle-keyed");
static_assert(std::is_same<decltype(qol::contracts::QolInventorySlotApi::remove),
                           QolResult (*)(int32_t, int32_t, qol::contracts::QolItemHandle,
                                         uint32_t)>::value,
              "inventory.slot remove must carry the expected handle");
static_assert(offsetof(qol::contracts::QolCharacterExtrasReadApi, can_apply_stuff_items) ==
                  offsetof(qol::contracts::QolCharacterExtrasReadApi, party_member) +
                      sizeof(decltype(qol::contracts::QolCharacterExtrasReadApi::party_member)),
              "CharacterExtras typed apply-stuff must be tail-appended");
static_assert(offsetof(qol::contracts::QolCharacterExtrasReadApi, struct_size) ==
                  offsetof(qol::contracts::QolCharacterExtrasReadApi, can_apply_stuff_items) +
                      sizeof(decltype(qol::contracts::QolCharacterExtrasReadApi::can_apply_stuff_items)),
              "CharacterExtras size must follow the typed API tail");
static_assert(std::is_same<qol::contracts::ItemFactsPhysicalItemAtFn,
                           QolResult (*)(int32_t, int32_t,
                                         qol::contracts::QolItemHandle*)>::value,
              "item.facts physical lookup must return a handle");
static_assert(std::is_same<decltype(qol::contracts::QolItemPortReadApi::inventory_item_view),
                           QolResult (*)(qol::contracts::QolItemHandle,
                                         qol::contracts::QolItemInventoryView*)>::value,
              "Item Port view must be handle-keyed");
static_assert(std::is_same<decltype(qol::contracts::QolItemPortReadApi::physical_item_at),
                           QolResult (*)(int32_t, int32_t,
                                         qol::contracts::QolItemHandle*)>::value,
               "Item Port physical lookup must not return an object pointer");
static_assert(std::is_same<qol::contracts::ItemFactsDetailItemContextFn,
                           QolResult (*)(qol::contracts::QolItemDetailContext*)>::value,
              "item.facts detail context must be an atomic handle snapshot");
static_assert(std::is_same<decltype(qol::contracts::QolItemPortReadApi::detail_item_context),
                            QolResult (*)(qol::contracts::QolItemDetailContext*)>::value,
               "Item Port detail context must be an atomic handle snapshot");
static_assert(qol::contracts::kQolItemInventoryOptionCapacity == 32,
              "inventory option capacity is an ABI constant");
static_assert(offsetof(qol::contracts::QolItemInventoryView, options) == 100,
              "option completeness flags must not change the inventory view layout");
static_assert(offsetof(qol::contracts::QolItemInventoryView, native_option_count) ==
                  offsetof(qol::contracts::QolItemInventoryView, options) +
                      sizeof(qol::contracts::QolItemInventoryView::options),
              "native option list must be tail-appended");
static_assert(offsetof(qol::contracts::QolItemInventoryView, jewel_option_count) ==
                  offsetof(qol::contracts::QolItemInventoryView, native_options) +
                      sizeof(qol::contracts::QolItemInventoryView::native_options),
              "jewel option list must follow the native option list");
static_assert(offsetof(qol::contracts::QolItemInventoryView, enchant_option_count) ==
                  offsetof(qol::contracts::QolItemInventoryView, jewel_options) +
                      sizeof(qol::contracts::QolItemInventoryView::jewel_options),
              "enchant option list must follow the jewel option list");
static_assert(sizeof(qol::contracts::QolItemInventoryView) == 1648,
              "split option lists must only append inventory view storage");
static_assert(qol::contracts::kQolItemInventoryHasOptionCompleteness == (1u << 7) &&
                  qol::contracts::kQolItemInventoryOptionsTruncated == (1u << 8) &&
                  qol::contracts::kQolItemInventoryHasNativeOptions == (1u << 9) &&
                  qol::contracts::kQolItemInventoryHasJewelOptions == (1u << 10) &&
                  qol::contracts::kQolItemInventoryHasEnchantOptions == (1u << 11),
              "option completeness and split-list flags must occupy the appended bits");
static_assert(qol::contracts::kAttributeQualityVersionMajor == 2 &&
                  qol::contracts::kAttributeQualityVersionMinor == 0,
              "attribute.quality contract must be v2.0");
static_assert(qol::contracts::kGameHostMajor == 2 && qol::contracts::kGameHostMinor == 4,
              "Game Host contract must be v2.4");
static_assert(std::is_same<qol::contracts::InventorySellFn,
                           qol::contracts::InventorySellResult (*)(
                               int32_t, int32_t, qol::contracts::QolItemHandle,
                               const char*)>::value,
              "cross-plugin sell must carry an expected handle");
static_assert(sizeof(qol::contracts::QolInventoryExtensionItemRef) ==
                  sizeof(int32_t) * 5 + sizeof(qol::contracts::QolItemHandle),
              "inventory extension Ref must not carry an object pointer");
static_assert(std::is_same<qol::contracts::ExtensionBagItemFn,
                           int32_t (*)(const qol::contracts::QolInventoryExtensionItemRef*,
                                       void*)>::value,
              "extension bag enumeration must not carry an object pointer");
static_assert(sizeof(qol::contracts::QolItemHandleApi) == sizeof(void*) * 6,
              "Host SDK handle table must not expose resolve_item");
static_assert(sizeof(qol::contracts::QolInventoryItemRef) ==
                  sizeof(int32_t) * 5 + sizeof(qol::contracts::QolItemHandle),
              "inventory reference must not carry a native object pointer");

int main() {
    CHECK(qol::contracts::kItemFactsVersion.major == 3 &&
          qol::contracts::kItemFactsVersion.minor == 5);
    CHECK(qol::contracts::kQolPortItemVersion.major == 3 &&
          qol::contracts::kQolPortItemVersion.minor == 5);
    CHECK(qol::contracts::kQolPortCharacterExtrasVersion.major == 1 &&
          qol::contracts::kQolPortCharacterExtrasVersion.minor == 2);
    qol::contracts::ItemFactsApi old_size_api{};
    old_size_api.struct_size = static_cast<uint32_t>(
        offsetof(qol::contracts::ItemFactsApi, count_encoding));
    CHECK(qol::contracts::item_facts_api_size_valid(&old_size_api));
    CHECK(!qol::contracts::item_facts_api_count_encoding_size_valid(&old_size_api));
    CHECK(!qol::contracts::item_facts_api_quote_size_valid(&old_size_api));
    old_size_api.struct_size = sizeof(old_size_api);
    CHECK(qol::contracts::item_facts_api_size_valid(&old_size_api));
    CHECK(qol::contracts::item_facts_api_count_encoding_size_valid(&old_size_api));
    CHECK(qol::contracts::item_facts_api_quote_size_valid(&old_size_api));
    qol::contracts::QolItemPort old_item_port{};
    old_item_port.struct_size = static_cast<uint32_t>(
        offsetof(qol::contracts::QolItemPort, read) +
        offsetof(qol::contracts::QolItemPortReadApi, count_encoding));
    CHECK(qol::contracts::qol_item_port_size_valid(&old_item_port));
    CHECK(!qol::contracts::qol_item_port_count_encoding_size_valid(&old_item_port));
    CHECK(!qol::contracts::qol_item_port_quote_size_valid(&old_item_port));
    old_item_port.struct_size = sizeof(old_item_port);
    CHECK(qol::contracts::qol_item_port_size_valid(&old_item_port));
    CHECK(qol::contracts::qol_item_port_count_encoding_size_valid(&old_item_port));
    CHECK(qol::contracts::qol_item_port_quote_size_valid(&old_item_port));
    CHECK(!qol::contracts::satisfies(qol::contracts::kItemFactsVersion, {1, 1}));
    CHECK(!qol::contracts::satisfies(qol::contracts::kQolPortItemVersion, {1, 4}));
    CHECK(!qol::contracts::satisfies(qol::contracts::kItemFactsVersion, {2, 0}));
    CHECK(!qol::contracts::satisfies(qol::contracts::kQolPortItemVersion, {2, 0}));
    CHECK(!qol::contracts::satisfies({2, 0}, {3, 0}));
    CHECK(!qol::contracts::satisfies(
        {2, 0}, {qol::contracts::kQolPortItemVersion.major,
                 qol::contracts::kQolPortItemVersion.minor}));
    CHECK(qol::contracts::satisfies(qol::contracts::kItemFactsVersion, {3, 0}));
    CHECK(qol::contracts::satisfies(qol::contracts::kQolPortItemVersion, {3, 0}));
    CHECK(qol::contracts::satisfies(qol::contracts::kQolPortItemVersion, {3, 2}));
    const auto& descriptor = qol::features::core_item::plugin_descriptor();
    CHECK(std::strcmp(descriptor.id, "core.item") == 0);
    CHECK(std::strcmp(descriptor.version, "0.1.0") == 0);
    CHECK(descriptor.requires_count == 0 && descriptor.requires == nullptr);
    // ITM-13：core.item 不再声明扩展袋可选依赖（中立 provider 经 Item Port 提供）。
    CHECK(descriptor.optional_count == 3 && descriptor.optional_requires != nullptr);
    CHECK(std::strcmp(descriptor.optional_requires[0].id,
                      qol::contracts::kCharacterStateCapabilityId) == 0);
    CHECK(descriptor.optional_requires[0].range.major == qol::contracts::kCharacterStateVersion.major);
    CHECK(descriptor.optional_requires[0].range.min_minor ==
          qol::contracts::kCharacterStateVersion.minor);
    CHECK(std::strcmp(descriptor.optional_requires[1].id,
                      qol::contracts::kHttpRegistryCapabilityId) == 0);
    CHECK(std::strcmp(descriptor.optional_requires[2].id,
                      qol::contracts::kHttpDispatchCapabilityId) == 0);
    for (uint32_t i = 1; i < 3; ++i) {
        CHECK(descriptor.optional_requires[i].range.major == 1);
        CHECK(descriptor.optional_requires[i].range.min_minor == 1);
    }
    CHECK(descriptor.http_query_count == 0 && descriptor.http_queries == nullptr);
    CHECK(descriptor.http_endpoint_count == 19 && descriptor.http_endpoints != nullptr);
    CHECK(descriptor.http_route_metadata_count == 2 &&
          descriptor.http_route_metadata != nullptr);
    uint32_t native_op_count = 0;
    for (uint32_t i = 0; i < descriptor.http_endpoint_count; ++i) {
        const auto& endpoint = descriptor.http_endpoints[i];
        CHECK(endpoint.route_owner != nullptr &&
              std::strcmp(endpoint.route_owner, "core.item") == 0);
        CHECK(endpoint.lease_required == 1);
        if (endpoint.op_required != 0) ++native_op_count;
        if (endpoint.method == qol::contracts::HttpMethod::kGet) {
            CHECK(endpoint.kind == qol::contracts::HttpHandlerKind::kQuery);
            CHECK(endpoint.query_handler != nullptr && endpoint.command_handler == nullptr);
        } else {
            CHECK(endpoint.kind == qol::contracts::HttpHandlerKind::kCommand);
            CHECK(endpoint.query_handler == nullptr && endpoint.command_handler != nullptr);
        }
    }
    CHECK(native_op_count == 2);
    CHECK(std::strcmp(descriptor.http_route_metadata[0].path,
                      "/api/op/inventory/set-slot") == 0);
    CHECK(std::strcmp(descriptor.http_route_metadata[1].path,
                      "/api/op/inventory/set-equip") == 0);
    for (uint32_t i = 0; i < descriptor.http_route_metadata_count; ++i) {
        CHECK(descriptor.http_route_metadata[i].method == qol::contracts::HttpMethod::kPost);
        CHECK(descriptor.http_route_metadata[i].op_required == 1);
        CHECK(descriptor.http_route_metadata[i].lease_required == 1);
    }
    CHECK(descriptor.provides_count == 2 && descriptor.provides != nullptr);
    CHECK(std::strcmp(descriptor.provides[0].id, qol::contracts::kItemFactsCapabilityId) == 0);
    CHECK(descriptor.provides[0].version.major == 3 && descriptor.provides[0].version.minor == 5);
    CHECK(descriptor.provides[0].iface != nullptr);
    // T2h-B2：owner raw inventory 投影 capability（复用 serialize_inventory_json）。
    CHECK(std::strcmp(descriptor.provides[1].id, qol::contracts::kItemStateCapabilityId) == 0);
    CHECK(descriptor.provides[1].version.major == 1 && descriptor.provides[1].version.minor == 1);
    CHECK(descriptor.provides[1].iface != nullptr);
    CHECK(static_cast<int32_t>(qol::contracts::QolItemStateKind::kRawInventory) == 0);
    CHECK(static_cast<int32_t>(qol::contracts::QolItemStateKind::kEnrichedInventory) == 1);
    CHECK(static_cast<const qol::contracts::ItemStateApi*>(descriptor.provides[1].iface)
              ->write_state_json != nullptr);
    CHECK(descriptor.port_count == 4 && descriptor.ports != nullptr);
    CHECK(descriptor.ports[0].id == qol::contracts::QolPortId::QOL_PORT_ITEM);
    CHECK(descriptor.ports[0].range.major == 3 && descriptor.ports[0].range.min_minor == 5);
    CHECK(descriptor.ports[1].id == qol::contracts::QolPortId::QOL_PORT_ITEM_MONEY_WRITE);
    CHECK(descriptor.ports[2].id == qol::contracts::QolPortId::QOL_PORT_ITEM_DICE_WRITE);
    CHECK(descriptor.ports[3].id ==
          qol::contracts::QolPortId::QOL_PORT_ITEM_INVENTORY_STATE_WRITE);

    const auto* api = static_cast<const qol::contracts::ItemFactsApi*>(descriptor.provides[0].iface);
    CHECK(api->category != nullptr);
    CHECK(api->is_real_equip != nullptr);
    CHECK(api->is_jewel_category != nullptr);
    CHECK(api->is_no_sell != nullptr);
    CHECK(api->sell_price != nullptr);
    CHECK(api->cumulate_count != nullptr);
    CHECK(api->item_ref_at != nullptr);
    CHECK(api->physical_item_at != nullptr);
    CHECK(api->detail_item_context != nullptr);
    CHECK(api->money != nullptr);
    CHECK(api->view != nullptr);
    CHECK(api->effective_count != nullptr);
    CHECK(api->count_limit != nullptr);
    CHECK(api->count_encoding != nullptr);
    CHECK(api->quote != nullptr);
    CHECK(g_fail == 0);
    std::printf("item_facts_plugin: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
