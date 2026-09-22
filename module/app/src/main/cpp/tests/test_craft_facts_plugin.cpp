#include "contracts/craft_facts.h"
#include "contracts/game_ports/ports.h"
#include "plugins/craft_facts/plugin.h"

#include <cstdio>
#include <cstring>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(condition) do { \
    if (condition) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } \
} while (0)

int main() {
    const auto& descriptor = qol::features::craft_facts::plugin_descriptor();
    CHECK(std::strcmp(descriptor.id, "craft_facts") == 0);
    CHECK(std::strcmp(descriptor.version, "0.1.0") == 0);
    CHECK(descriptor.requires_count == 0 && descriptor.optional_count == 0);
    CHECK(descriptor.provides_count == 1 && descriptor.provides != nullptr);
    CHECK(std::strcmp(descriptor.provides[0].id, qol::contracts::kCraftFactsCapabilityId) == 0);
    CHECK(descriptor.provides[0].version.major == 1 && descriptor.provides[0].version.minor == 0);
    CHECK(descriptor.provides[0].iface != nullptr);
    CHECK(descriptor.port_count == 1 && descriptor.ports != nullptr);
    CHECK(descriptor.ports[0].id == qol::contracts::QolPortId::QOL_PORT_CRAFT);

    const auto* api = static_cast<const qol::contracts::CraftFactsApi*>(descriptor.provides[0].iface);
    CHECK(api->read.ui_type != nullptr);
    CHECK(api->read.panel_state != nullptr);
    CHECK(api->read.mix_type != nullptr);
    CHECK(api->read.selected_stuff_index != nullptr);
    CHECK(api->read.selected_stuff_item != nullptr);
    CHECK(api->read.stuff_item != nullptr);
    CHECK(api->read.category_held_count != nullptr);
    CHECK(api->read.character_level != nullptr);
    CHECK(api->read_only.ui_type != nullptr);
    CHECK(api->read_only.get_ui_type != nullptr);
    CHECK(api->read_only.panel_state != nullptr);
    CHECK(api->read_only.mix_type != nullptr);
    CHECK(api->read_only.selected_stuff_index != nullptr);
    CHECK(api->read_only.selected_stuff_item != nullptr);
    CHECK(api->read_only.stuff_item != nullptr);
    CHECK(api->read_only.category_held_count != nullptr);
    CHECK(api->read_only.character_level != nullptr);
    CHECK(api->read_only.stuff_slot_control != nullptr);
    CHECK(api->read_only.target_slot_item != nullptr);
    CHECK(api->read_only.selected_inventory_item != nullptr);
    CHECK(api->read_only.selected_recipe != nullptr);
    CHECK(api->read_only.stuff_slot_count != nullptr);
    CHECK(g_fail == 0);
    std::printf("craft_facts_plugin: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
