#include "contracts/game_ports/choice_port.h"
#include "contracts/game_ports/ports.h"
#include "contracts/game_ports/world_action_port.h"
#include "contracts/host_sdk.h"
#include "contracts/item_facts.h"
#include "contracts/plugin.h"
#include "contracts/version.h"
#include "contracts/world_facts.h"
#include "plugins/world_teleport/feature.h"
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
bool g_world_available = true;
bool g_item_available = false;

QolResult require_capability(const char* id, QolVersionRange, const void** out_iface) {
    if (std::strcmp(id, qol::contracts::kWorldFactsCapabilityId) == 0 && g_world_available) {
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

void check_failure(const qol::contracts::PluginDescriptor& descriptor,
                   qol::contracts::PluginContext& context, const char* expected_error) {
    assert(descriptor.install(context) == 1);
    qol::contracts::PluginStatusOut status{};
    descriptor.fill_status(status);
    assert(!status.enabled);
    assert(status.error != nullptr);
    assert(std::strcmp(status.error, expected_error) == 0);
    assert(qol::features::world_teleport::host_sdk() == nullptr);
    assert(qol::features::world_teleport::world_action_port() == nullptr);
    assert(qol::features::world_teleport::world_facts() == nullptr);
    assert(qol::features::world_teleport::item_facts() == nullptr);
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
    assert(descriptor.requires_count == 2);
    assert(std::strcmp(descriptor.requires[0].id, qol::contracts::kWorldFactsCapabilityId) == 0);
    assert(std::strcmp(descriptor.requires[1].id, qol::contracts::kItemFactsCapabilityId) == 0);
    assert(descriptor.port_count == 1);
    assert(descriptor.ports[0].id == qol::contracts::QolPortId::QOL_PORT_WORLD_ACTION);

    // 端口数量不符/缺失 → fail-closed
    context.port_count = 0;
    check_failure(descriptor, context,
                  "install failed: world teleport dependencies not ready");
    context.port_count = 1;
    const void* missing_action_port[] = {nullptr};
    context.ports = missing_action_port;
    check_failure(descriptor, context,
                  "install failed: world teleport dependencies not ready");
    context.ports = ports;

    g_world_available = false;
    g_item_available = true;
    check_failure(descriptor, context, "install failed: world facts unavailable");

    g_world_available = true;
    g_item_available = false;
    check_failure(descriptor, context, "install failed: item facts unavailable");

    g_item_available = true;
    assert(descriptor.install(context) == 0);
    assert(qol::features::world_teleport::host_sdk() == &sdk);
    assert(qol::features::world_teleport::world_action_port() == &actions);
    assert(qol::features::world_teleport::world_facts() == &kWorldFacts);
    assert(qol::features::world_teleport::item_facts() == &kItemFacts);

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
