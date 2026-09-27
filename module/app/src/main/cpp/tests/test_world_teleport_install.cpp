#include "contracts/game_ports/choice_port.h"
#include "contracts/host_sdk.h"
#include "contracts/item_facts.h"
#include "contracts/plugin.h"
#include "contracts/version.h"
#include "plugins/world_teleport/plugin.h"

#include <cassert>
#include <cstring>

namespace qol::features::world_teleport {
void world_teleport_call_map_name() {}
}

namespace {

const qol::contracts::WorldFactsApi kWorldFacts{};
const qol::contracts::ItemFactsApi kItemFacts{};
bool g_item_available = false;

QolResult require_capability(const char* id, QolVersionRange, const void** out_iface) {
    if (std::strcmp(id, qol::contracts::kWorldFactsCapabilityId) == 0) {
        *out_iface = &kWorldFacts;
        return qol::contracts::ok();
    }
    if (std::strcmp(id, qol::contracts::kItemFactsCapabilityId) == 0 && g_item_available) {
        *out_iface = &kItemFacts;
        return qol::contracts::ok();
    }
    *out_iface = nullptr;
    return qol::contracts::error(kQolErrNotFound, "missing capability", 18);
}

}  // namespace

int main() {
    qol::contracts::QolHostSdk sdk{};
    sdk.capabilities.require = &require_capability;
    qol::contracts::QolGameHost game{};
    qol::contracts::QolChoicePort choice{};
    const void* ports[] = {&choice};
    qol::contracts::PluginContext context{};
    context.struct_size = sizeof(context);
    context.sdk_major = qol::contracts::kHostSdkMajor;
    context.sdk = &sdk;
    context.game = &game;
    context.ports = ports;
    context.port_count = 1;

    const auto& descriptor = qol::features::world_teleport::plugin_descriptor();
    assert(descriptor.requires_count == 2);
    assert(std::strcmp(descriptor.requires[0].id, qol::contracts::kWorldFactsCapabilityId) == 0);
    assert(std::strcmp(descriptor.requires[1].id, qol::contracts::kItemFactsCapabilityId) == 0);
    qol::contracts::PluginStatusOut status{};
    assert(descriptor.install(context) != 0);
    descriptor.fill_status(status);
    assert(status.error != nullptr);
    assert(std::strcmp(status.error, "install failed: item facts unavailable") == 0);

    g_item_available = true;
    assert(descriptor.install(context) == 0);
    assert(qol::features::world_teleport::item_facts() == &kItemFacts);
    descriptor.deactivate(context);
    assert(qol::features::world_teleport::item_facts() == nullptr);
    return 0;
}
