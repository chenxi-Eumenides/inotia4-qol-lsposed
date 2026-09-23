#include "contracts/save_facts.h"
#include "contracts/game_ports/ports.h"
#include "plugins/core.save/plugin.h"

#include <cstdio>
#include <cstring>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(condition) do { \
    if (condition) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } \
} while (0)

int main() {
    const auto& descriptor = qol::features::core_save::plugin_descriptor();
    CHECK(std::strcmp(descriptor.id, "core.save") == 0);
    CHECK(std::strcmp(descriptor.version, "0.1.0") == 0);
    CHECK(descriptor.requires_count == 0 && descriptor.optional_count == 0);
    CHECK(descriptor.provides_count == 1 && descriptor.provides != nullptr);
    CHECK(std::strcmp(descriptor.provides[0].id, qol::contracts::kSaveFactsCapabilityId) == 0);
    CHECK(descriptor.provides[0].version.major == 1 && descriptor.provides[0].version.minor == 0);
    CHECK(descriptor.provides[0].iface != nullptr);
    CHECK(descriptor.port_count == 2 && descriptor.ports != nullptr);
    CHECK(descriptor.ports[0].id == qol::contracts::QolPortId::QOL_PORT_SAVE);
    CHECK(descriptor.ports[1].id == qol::contracts::QolPortId::QOL_PORT_SAVE_WRITE);
    // 写面（阶段 5 票据契约）由同域 owner 声明式提供 3 条 POST。
    CHECK(descriptor.http_query_count == 3 && descriptor.http_queries != nullptr);

    const auto* api = static_cast<const qol::contracts::SaveFactsApi*>(descriptor.provides[0].iface);
    CHECK(api->slot_state != nullptr);
    CHECK(api->slot_exists != nullptr);
    CHECK(api->slot_hero_index != nullptr);
    CHECK(api->hero_level != nullptr);
    CHECK(api->hero_class != nullptr);
    CHECK(api->load_data != nullptr);
    CHECK(api->hub_key != nullptr);
    CHECK(api->map_id != nullptr);
    CHECK(api->current_slot != nullptr);
    CHECK(api->is_ok != nullptr);
    CHECK(g_fail == 0);
    std::printf("save_facts_plugin: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
