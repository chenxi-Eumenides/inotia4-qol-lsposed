#include "contracts/game_ports/choice_port.h"
#include "contracts/game_ports/ports.h"
#include "contracts/game_ports/world_action_port.h"
#include "contracts/host_sdk.h"
#include "contracts/item_facts.h"
#include "contracts/plugin.h"
#include "contracts/world_facts.h"
#include "plugins/world_teleport/plugin.h"

#include <cassert>
#include <cstring>

namespace qol::features::world_teleport {
void world_teleport_call_map_name() {}
void invalidate_operations() {}
}

namespace {

const qol::contracts::WorldFactsApi kWorldFacts{};
const qol::contracts::ItemFactsApi kItemFacts{
    static_cast<uint32_t>(sizeof(qol::contracts::ItemFactsApi))};

QolResult require_capability(const char* id, QolVersionRange, const void** out_iface) {
    if (std::strcmp(id, qol::contracts::kWorldFactsCapabilityId) == 0) {
        *out_iface = &kWorldFacts;
    } else if (std::strcmp(id, qol::contracts::kItemFactsCapabilityId) == 0) {
        *out_iface = &kItemFacts;
    } else {
        *out_iface = nullptr;
        return qol::contracts::error(kQolErrNotFound, "missing capability", 18);
    }
    return qol::contracts::ok();
}

}  // namespace

int main() {
    qol::contracts::QolHostSdk sdk{};
    sdk.capabilities.require = &require_capability;
    qol::contracts::QolWorldActionPort actions{};
    actions.abi_version = qol::contracts::kQolPortWorldActionVersion;
    const void* ports[] = {&actions};
    qol::contracts::PluginContext context{};
    context.struct_size = sizeof(context);
    context.sdk_major = qol::contracts::kHostSdkMajor;
    context.sdk = &sdk;
    context.game = nullptr;  // 私有端口迁移后插件不再依赖聚合 Game Host
    context.ports = ports;
    context.port_count = 1;

    const auto& descriptor = qol::features::world_teleport::plugin_descriptor();
    assert(descriptor.install(context) == 0);
    assert(qol::features::world_teleport::host_sdk() == &sdk);
    assert(qol::features::world_teleport::world_action_port() == &actions);

    qol::contracts::PluginStatusOut status{};
    descriptor.fill_status(status);
    assert(!status.enabled);
    assert(status.error == nullptr);

    descriptor.on_enabled(context);
    descriptor.fill_status(status);
    assert(status.enabled);
    assert(status.error == nullptr);

    descriptor.on_disabled(context);
    descriptor.fill_status(status);
    assert(!status.enabled);
    assert(status.error == nullptr);

    descriptor.deactivate(context);
    descriptor.fill_status(status);
    assert(!status.enabled);
    assert(status.error == nullptr);
    assert(qol::features::world_teleport::host_sdk() == nullptr);
    assert(qol::features::world_teleport::world_action_port() == nullptr);
    assert(qol::features::world_teleport::world_facts() == nullptr);
    assert(qol::features::world_teleport::item_facts() == nullptr);
    return 0;
}
