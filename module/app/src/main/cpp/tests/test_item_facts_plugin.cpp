#include "contracts/item_facts.h"
#include "contracts/game_ports/ports.h"
#include "plugins/item_facts/plugin.h"

#include <cstdio>
#include <cstring>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

int main() {
    const auto& descriptor = qol::features::item_facts::plugin_descriptor();
    CHECK(std::strcmp(descriptor.id, "item_facts") == 0);
    CHECK(std::strcmp(descriptor.version, "0.1.0") == 0);
    CHECK(descriptor.requires_count == 0 && descriptor.optional_count == 0);
    CHECK(descriptor.provides_count == 1 && descriptor.provides != nullptr);
    CHECK(std::strcmp(descriptor.provides[0].id, qol::contracts::kItemFactsCapabilityId) == 0);
    CHECK(descriptor.provides[0].version.major == 1 && descriptor.provides[0].version.minor == 0);
    CHECK(descriptor.provides[0].iface != nullptr);
    CHECK(descriptor.port_count == 1 && descriptor.ports != nullptr);
    CHECK(descriptor.ports[0].id == qol::contracts::QolPortId::QOL_PORT_ITEM);

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
    CHECK(g_fail == 0);
    std::printf("item_facts_plugin: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
