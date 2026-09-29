#include "contracts/item_facts.h"
#include "contracts/character_state.h"
#include "contracts/item_state.h"
#include "contracts/extension_bag.h"
#include "contracts/game_ports/ports.h"
#include "plugins/core.item/plugin.h"

#include <cstdio>
#include <cstring>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

int main() {
    const auto& descriptor = qol::features::core_item::plugin_descriptor();
    CHECK(std::strcmp(descriptor.id, "core.item") == 0);
    CHECK(std::strcmp(descriptor.version, "0.1.0") == 0);
    CHECK(descriptor.requires_count == 2 && descriptor.requires != nullptr);
    // ITM-13：core.item 不再声明扩展袋可选依赖（中立 provider 经 Item Port 提供）。
    CHECK(descriptor.optional_count == 1 && descriptor.optional_requires != nullptr);
    CHECK(std::strcmp(descriptor.optional_requires[0].id,
                      qol::contracts::kCharacterStateCapabilityId) == 0);
    CHECK(descriptor.optional_requires[0].range.major == qol::contracts::kCharacterStateVersion.major);
    CHECK(descriptor.optional_requires[0].range.min_minor ==
          qol::contracts::kCharacterStateVersion.minor);
    CHECK(std::strcmp(descriptor.requires[0].id, qol::contracts::kHttpRegistryCapabilityId) == 0);
    CHECK(std::strcmp(descriptor.requires[1].id, qol::contracts::kHttpDispatchCapabilityId) == 0);
    CHECK(descriptor.http_query_count == 19 && descriptor.http_queries != nullptr);
    CHECK(descriptor.provides_count == 2 && descriptor.provides != nullptr);
    CHECK(std::strcmp(descriptor.provides[0].id, qol::contracts::kItemFactsCapabilityId) == 0);
    CHECK(descriptor.provides[0].version.major == 1 && descriptor.provides[0].version.minor == 1);
    CHECK(descriptor.provides[0].iface != nullptr);
    // T2h-B2：owner raw inventory 投影 capability（复用 serialize_inventory_json）。
    CHECK(std::strcmp(descriptor.provides[1].id, qol::contracts::kItemStateCapabilityId) == 0);
    CHECK(descriptor.provides[1].version.major == 1 && descriptor.provides[1].version.minor == 0);
    CHECK(descriptor.provides[1].iface != nullptr);
    CHECK(static_cast<const qol::contracts::ItemStateApi*>(descriptor.provides[1].iface)
              ->write_state_json != nullptr);
    CHECK(descriptor.port_count == 4 && descriptor.ports != nullptr);
    CHECK(descriptor.ports[0].id == qol::contracts::QolPortId::QOL_PORT_ITEM);
    CHECK(descriptor.ports[0].range.major == 1 && descriptor.ports[0].range.min_minor == 3);
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
    CHECK(api->detail_context != nullptr);
    CHECK(api->cursor_index != nullptr);
    CHECK(api->money != nullptr);
    CHECK(g_fail == 0);
    std::printf("item_facts_plugin: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
