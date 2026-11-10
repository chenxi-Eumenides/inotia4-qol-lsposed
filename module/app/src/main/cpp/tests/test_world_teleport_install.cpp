#include "contracts/game_ports/choice_port.h"
#include "contracts/game_ports/ports.h"
#include "contracts/game_ports/world_action_port.h"
#include "contracts/events.h"
#include "contracts/host_sdk.h"
#include "contracts/item_facts.h"
#include "contracts/plugin.h"
#include "contracts/static_data.h"
#include "contracts/ui.h"
#include "contracts/version.h"
#include "contracts/world_facts.h"
#include "plugins/world_teleport/feature.h"
#include "plugins/world_teleport/plugin.h"

#include <cassert>
#include <cstring>
#include <vector>

namespace qol::features::world_teleport {
void world_teleport_call_map_name() {}
void invalidate_operations() {}
}

namespace {

const qol::contracts::WorldFactsApi kWorldFacts{};
const qol::contracts::ItemFactsApi kItemFacts{
    static_cast<uint32_t>(sizeof(qol::contracts::ItemFactsApi))};
const qol::contracts::QolStaticDataApi kStaticData{};
bool g_world_available = true;
bool g_item_available = false;
bool g_static_available = false;
bool g_ui_state_available = false;
int g_register_state_calls = 0;
int g_unregister_state_calls = 0;

QolResult fake_register_state(const qol::contracts::QolUiStateRegistration*) {
    ++g_register_state_calls;
    return qol::contracts::ok();
}

QolResult fake_unregister_state(uint32_t, const char*) {
    ++g_unregister_state_calls;
    return qol::contracts::ok();
}

const qol::contracts::UiStateApi kUiState{nullptr, nullptr, nullptr, nullptr, nullptr,
                                          nullptr, &fake_register_state,
                                          &fake_unregister_state};

QolResult require_capability(const char* id, QolVersionRange, const void** out_iface) {
    if (std::strcmp(id, qol::contracts::kWorldFactsCapabilityId) == 0 && g_world_available) {
        *out_iface = &kWorldFacts;
        return qol::contracts::ok();
    }
    if (std::strcmp(id, qol::contracts::kItemFactsCapabilityId) == 0 && g_item_available) {
        *out_iface = &kItemFacts;
        return qol::contracts::ok();
    }
    if (std::strcmp(id, qol::contracts::kStaticDataCapabilityId) == 0 && g_static_available) {
        *out_iface = &kStaticData;
        return qol::contracts::ok();
    }
    if (std::strcmp(id, qol::contracts::kUiStateCapabilityId) == 0 && g_ui_state_available) {
        *out_iface = &kUiState;
        return qol::contracts::ok();
    }
    *out_iface = nullptr;
    return qol::contracts::error(kQolErrNotFound, "missing capability", 18);
}

// ---- 事件总线 fake：记录订阅并支持按类型注入 payload ----
struct FakeEventSubscription {
    uint32_t type = 0;
    qol::contracts::QolEventHandler handler = nullptr;
    void* ctx = nullptr;
    uint64_t id = 0;
};

std::vector<FakeEventSubscription> g_event_subscriptions;
uint64_t g_next_event_subscription = 1;
int g_event_unsubscribe_calls = 0;

QolResult fake_event_subscribe(uint32_t type, qol::contracts::QolEventHandler handler,
                               void* ctx, uint64_t* out_id) {
    assert(handler != nullptr && out_id != nullptr);
    FakeEventSubscription subscription{};
    subscription.type = type;
    subscription.handler = handler;
    subscription.ctx = ctx;
    subscription.id = g_next_event_subscription++;
    g_event_subscriptions.push_back(subscription);
    *out_id = subscription.id;
    return qol::contracts::ok();
}

QolResult fake_event_unsubscribe(uint64_t id) {
    ++g_event_unsubscribe_calls;
    for (auto it = g_event_subscriptions.begin(); it != g_event_subscriptions.end(); ++it) {
        if (it->id == id) {
            g_event_subscriptions.erase(it);
            return qol::contracts::ok();
        }
    }
    return qol::contracts::error(kQolErrNotFound, "not found", 9);
}

void publish_fake_event(uint32_t type, const void* payload) {
    const std::vector<FakeEventSubscription> snapshot = g_event_subscriptions;
    for (const FakeEventSubscription& subscription : snapshot) {
        if (subscription.type == type) subscription.handler(subscription.ctx, payload);
    }
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
    assert(qol::features::world_teleport::static_data() == nullptr);
}

}  // namespace

int main() {
    qol::contracts::QolHostSdk sdk{};
    sdk.capabilities.require = &require_capability;
    sdk.events.subscribe = &fake_event_subscribe;
    sdk.events.unsubscribe = &fake_event_unsubscribe;
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
    assert(descriptor.requires_count == 3);
    assert(std::strcmp(descriptor.requires[0].id, qol::contracts::kWorldFactsCapabilityId) == 0);
    assert(std::strcmp(descriptor.requires[1].id, qol::contracts::kItemFactsCapabilityId) == 0);
    assert(std::strcmp(descriptor.requires[2].id, qol::contracts::kStaticDataCapabilityId) == 0);
    assert(descriptor.port_count == 1);
    assert(descriptor.ports[0].id == qol::contracts::QolPortId::QOL_PORT_WORLD_ACTION);
    assert(descriptor.resources_count == 1);
    assert(descriptor.resources[0].mechanism ==
           qol::contracts::ResourceMechanism::kEntryHook);
    assert(std::strcmp(descriptor.resources[0].id, "hook:UIPlay_CallMapName") == 0);
    assert(std::strcmp(descriptor.resources[0].symbol, "F_UI_PLAY_CALL_MAP_NAME_VMA") == 0);

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
    g_static_available = false;
    check_failure(descriptor, context, "install failed: static data unavailable");

    g_static_available = true;
    assert(descriptor.install(context) == 0);
    assert(qol::features::world_teleport::host_sdk() == &sdk);
    assert(qol::features::world_teleport::world_action_port() == &actions);
    assert(qol::features::world_teleport::world_facts() == &kWorldFacts);
    assert(qol::features::world_teleport::item_facts() == &kItemFacts);
    assert(qol::features::world_teleport::static_data() == &kStaticData);

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

    // ui.state 可选能力变更：install 已订阅；available=1 补注册，available=0 注销。
    assert(g_event_subscriptions.size() == 1);
    assert(g_event_subscriptions[0].type == qol::contracts::kEventCapabilityChanged);
    g_ui_state_available = true;
    const qol::contracts::CapabilityChangedEvent ui_state_available{
        qol::contracts::kUiStateCapabilityId, qol::contracts::kUiStateVersionMajor,
        qol::contracts::kUiStateVersionMinor, 1, {}};
    publish_fake_event(qol::contracts::kEventCapabilityChanged, &ui_state_available);
    assert(g_register_state_calls == 1);

    // 无关能力、空 payload、空 id 均不影响注册状态。
    const qol::contracts::CapabilityChangedEvent other_event{"character.facts", 1, 0, 1, {}};
    publish_fake_event(qol::contracts::kEventCapabilityChanged, &other_event);
    publish_fake_event(qol::contracts::kEventCapabilityChanged, nullptr);
    const qol::contracts::CapabilityChangedEvent null_id_event{nullptr, 1, 0, 1, {}};
    publish_fake_event(qol::contracts::kEventCapabilityChanged, &null_id_event);
    assert(g_register_state_calls == 1);

    const qol::contracts::CapabilityChangedEvent ui_state_unavailable{
        qol::contracts::kUiStateCapabilityId, qol::contracts::kUiStateVersionMajor,
        qol::contracts::kUiStateVersionMinor, 0, {}};
    publish_fake_event(qol::contracts::kEventCapabilityChanged, &ui_state_unavailable);
    assert(g_unregister_state_calls == 1);

    // 能力恢复后再注册，deactivate 应注销并取消能力变更订阅。
    publish_fake_event(qol::contracts::kEventCapabilityChanged, &ui_state_available);
    assert(g_register_state_calls == 2);

    // on_resource_ready：只接受本插件的入口 hook id 与宿主交还的非空 backup。
    assert(descriptor.on_resource_ready != nullptr);
    int fake_backup = 0;
    assert(descriptor.on_resource_ready(context, "hook:unknown", &fake_backup) != 0);
    assert(descriptor.on_resource_ready(context, "hook:UIPlay_CallMapName", nullptr) != 0);
    assert(descriptor.on_resource_ready(context,
                                        "hook:UIPlay_CallMapName", &fake_backup) == 0);
    assert(descriptor.on_resource_ready(context,
                                        "hook:UIPlay_CallMapName", &fake_backup) == 0);

    descriptor.deactivate(context);
    descriptor.fill_status(status);
    assert(!status.enabled);
    assert(status.error == nullptr);
    assert(g_unregister_state_calls == 2);
    assert(g_event_subscriptions.empty());
    assert(qol::features::world_teleport::host_sdk() == nullptr);
    assert(qol::features::world_teleport::world_action_port() == nullptr);
    assert(qol::features::world_teleport::world_facts() == nullptr);
    assert(qol::features::world_teleport::item_facts() == nullptr);
    assert(qol::features::world_teleport::static_data() == nullptr);
    return 0;
}
