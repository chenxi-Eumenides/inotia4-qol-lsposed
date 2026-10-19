#include "contracts/world_facts.h"
#include "contracts/game_ports/ports.h"
#include "contracts/http.h"
#include "plugins/core.world/plugin.h"

#include <cstdio>
#include <cstring>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

int main() {
    const auto& descriptor = qol::features::core_world::plugin_descriptor();
    CHECK(std::strcmp(descriptor.id, "core.world") == 0);
    CHECK(std::strcmp(descriptor.version, "0.1.0") == 0);
    CHECK(descriptor.requires_count == 0 && descriptor.requires == nullptr);
    CHECK(descriptor.optional_count == 2 && descriptor.optional_requires != nullptr);
    CHECK(std::strcmp(descriptor.optional_requires[0].id,
                      qol::contracts::kHttpRegistryCapabilityId) == 0);
    CHECK(std::strcmp(descriptor.optional_requires[1].id,
                      qol::contracts::kHttpDispatchCapabilityId) == 0);
    for (uint32_t i = 0; i < 2; ++i) {
        CHECK(descriptor.optional_requires[i].range.major == 1);
        CHECK(descriptor.optional_requires[i].range.min_minor == 1);
    }
    CHECK(descriptor.provides_count == 1 && descriptor.provides != nullptr);
    CHECK(std::strcmp(descriptor.provides[0].id, qol::contracts::kWorldFactsCapabilityId) == 0);
    CHECK(descriptor.provides[0].version.major == 2 && descriptor.provides[0].version.minor == 1);
    CHECK(descriptor.provides[0].iface != nullptr);
    CHECK(descriptor.port_count == 2 && descriptor.ports != nullptr);
    CHECK(descriptor.ports[0].id == qol::contracts::QolPortId::QOL_PORT_WORLD);
    CHECK(descriptor.ports[0].range.major == 2 && descriptor.ports[0].range.min_minor == 0);
    CHECK(descriptor.http_query_count == 0 && descriptor.http_queries == nullptr);
    CHECK(descriptor.http_endpoint_count == 12 && descriptor.http_endpoints != nullptr);
    CHECK(descriptor.http_route_metadata_count == 0 && descriptor.http_route_metadata == nullptr);
    uint32_t op_endpoint_count = 0;
    for (uint32_t i = 0; i < descriptor.http_endpoint_count; ++i) {
        const auto& endpoint = descriptor.http_endpoints[i];
        CHECK(endpoint.route_owner != nullptr &&
              std::strcmp(endpoint.route_owner, "core.world") == 0);
        CHECK(endpoint.lease_required == 1);
        if (endpoint.op_required != 0) ++op_endpoint_count;
        if (endpoint.method == qol::contracts::HttpMethod::kGet) {
            CHECK(endpoint.kind == qol::contracts::HttpHandlerKind::kQuery);
            CHECK(endpoint.query_handler != nullptr && endpoint.command_handler == nullptr);
        } else {
            CHECK(endpoint.kind == qol::contracts::HttpHandlerKind::kCommand);
            CHECK(endpoint.query_handler == nullptr && endpoint.command_handler != nullptr);
        }
    }
    CHECK(op_endpoint_count == 1);

    const auto* api = static_cast<const qol::contracts::WorldFactsApi*>(descriptor.provides[0].iface);
    CHECK(api->state != nullptr);
    CHECK(api->game_state_gate != nullptr);
    CHECK(api->map_record_count != nullptr);
    CHECK(api->current_map_id != nullptr);
    CHECK(api->map_name != nullptr);
    CHECK(api->active_player_direction != nullptr);
    CHECK(api->tutorial_state != nullptr);
    CHECK(api->series != nullptr);
    int32_t series = -1;
    CHECK(qol::contracts::succeeded(api->series(&series)));
    CHECK(series == qol::contracts::kWorldFactsSeriesUnknown);
    CHECK(g_fail == 0);
    std::printf("world_facts_plugin: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
