// plugins/autosell 插件清单/资源 host 单测：
// 2 条声明式资源（入口 kCallPatch + own:popup.autosell kOwnership）的机制/门禁/handler，
// 以及 install/deactivate 对 infra/panels provider 的登记/反登记。
//
// 本 target 只链接 plugin.cpp + infra/panels.cpp；UI/扫描/配置/事件实现以桩替代（真机验证）。

#include "plugins/autosell/plugin.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "contracts/ownership.h"
#include "contracts/http.h"
#include "contracts/panels.h"
#include "contracts/plugin.h"
#include "contracts/world_facts.h"
#include "contracts/item_facts.h"
#include "contracts/slot_data.h"
#include "contracts/game_ports/inventory_extras_port.h"
#include "infra/panels.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// ---- plugin.cpp 外部依赖桩（UI/扫描/配置/事件；真机验证运行时行为）----
namespace qol::features::autosell {

void autosell_equip_desc_draw_wrapper() {}
void autosell_ui_set_enabled(bool) {}
bool autosell_provider_open() { return false; }
void autosell_provider_close() {}
bool autosell_provider_active() { return false; }
const char* autosell_provider_block_reason() { return nullptr; }
int32_t autosell_popup_acquire(const char*) { return 0; }
void autosell_popup_release(const char*) {}

}  // namespace qol::features::autosell

void autosell_set_global_enabled(bool) {}
void autosell_register_save_enter() {}
void autosell_register_save_exit() {}
void autosell_unregister_save_callbacks() {}
bool autosell_store_register(qol::contracts::QolOwnerToken owner,
                             const qol::contracts::QolSlotDataApi* api) {
    return owner != 0 && api != nullptr;
}
void autosell_store_unregister() {}

namespace {

using qol::contracts::OwnershipOps;
using qol::contracts::ResourceFailure;
using qol::contracts::ResourceMechanism;
using qol::contracts::ResourceRequirement;

QolResult fake_config_get_bool(const char*, int32_t default_value, int32_t* out_value) {
    if (out_value == nullptr) return QolResult{kQolErrInvalidArgument, kQolErrInvalidArgument, nullptr, 0};
    *out_value = default_value;
    return qol::contracts::ok();
}

QolResult fake_events_subscribe(uint32_t, qol::contracts::QolEventHandler, void*, uint64_t* out_id) {
    if (out_id == nullptr) return QolResult{kQolErrInvalidArgument, kQolErrInvalidArgument, nullptr, 0};
    *out_id = 1;
    return qol::contracts::ok();
}

QolResult fake_events_unsubscribe(uint64_t id) {
    return id == 1 ? qol::contracts::ok()
                   : QolResult{kQolErrNotFound, kQolErrNotFound, nullptr, 0};
}

QolResult fake_panels_register_provider(const qol::contracts::PanelProviderSpec* provider) {
    if (provider == nullptr) {
        return QolResult{kQolErrInvalidArgument, kQolErrInvalidArgument, nullptr, 0};
    }
    return qol::infra::panels_register_provider(*provider)
               ? qol::contracts::ok()
               : QolResult{kQolErrConflict, kQolErrConflict, nullptr, 0};
}

QolResult fake_panels_unregister_owner(const char* owner) {
    if (owner == nullptr) {
        return QolResult{kQolErrInvalidArgument, kQolErrInvalidArgument, nullptr, 0};
    }
    qol::infra::panels_unregister_owner(owner);
    return qol::contracts::ok();
}

QolResult fake_capability_require(const char* id, QolVersionRange, const void** out_iface) {
    static const qol::contracts::WorldFactsApi api{};
    static const qol::contracts::ItemFactsApi item_api{sizeof(qol::contracts::ItemFactsApi)};
    static const qol::contracts::QolSlotDataApi slot_data_api{};
    if (id == nullptr || out_iface == nullptr) {
        return QolResult{kQolErrNotFound, kQolErrNotFound, nullptr, 0};
    }
    if (std::strcmp(id, qol::contracts::kWorldFactsCapabilityId) == 0) {
        *out_iface = &api;
    } else if (std::strcmp(id, qol::contracts::kItemFactsCapabilityId) == 0) {
        *out_iface = &item_api;
    } else if (std::strcmp(id, qol::contracts::kQolSlotDataCapabilityId) == 0) {
        *out_iface = &slot_data_api;
    } else {
        return QolResult{kQolErrNotFound, kQolErrNotFound, nullptr, 0};
    }
    return qol::contracts::ok();
}

qol::contracts::QolHostSdk fake_sdk() {
    qol::contracts::QolHostSdk sdk{};
    sdk.abi = {sizeof(qol::contracts::QolHostSdk), qol::contracts::kHostSdkMajor,
               qol::contracts::kHostSdkMinor};
    sdk.config.get_bool = &fake_config_get_bool;
    sdk.events.subscribe = &fake_events_subscribe;
    sdk.events.unsubscribe = &fake_events_unsubscribe;
    sdk.panels.register_provider = &fake_panels_register_provider;
    sdk.panels.unregister_owner = &fake_panels_unregister_owner;
    sdk.capabilities.require = &fake_capability_require;
    return sdk;
}

void test_manifest() {
    const qol::contracts::PluginDescriptor& d =
        qol::features::autosell::plugin_descriptor();
    CHECK(std::strcmp(d.id, "autosell") == 0);
    CHECK(d.requires_count == 5 && d.optional_count == 2 && d.provides_count == 0);
    CHECK(std::strcmp(d.requires[0].id, "attribute.quality") == 0);
    CHECK(std::strcmp(d.requires[1].id, "inventory.action") == 0);
    CHECK(std::strcmp(d.requires[2].id, "world.facts") == 0);
    CHECK(std::strcmp(d.requires[3].id, "item.facts") == 0);
    CHECK(std::strcmp(d.requires[4].id, "save.slot_data") == 0);
    CHECK(std::strcmp(d.optional_requires[0].id, "extension_bag") == 0);
    CHECK(std::strcmp(d.optional_requires[1].id, qol::contracts::kHttpRegistryCapabilityId) == 0);
    CHECK(d.http_query_count == 0 && d.http_queries == nullptr);
    CHECK(d.http_endpoint_count == 0 && d.http_endpoints == nullptr);
    CHECK(d.http_route_metadata_count == 2 && d.http_route_metadata != nullptr);
    CHECK(d.http_route_metadata[0].method == qol::contracts::HttpMethod::kGet);
    CHECK(std::strcmp(d.http_route_metadata[0].path, "/api/feature/autosell/config") == 0);
    CHECK(d.http_route_metadata[1].method == qol::contracts::HttpMethod::kPost);
    CHECK(std::strcmp(d.http_route_metadata[1].path, "/api/feature/autosell/config") == 0);
    CHECK(std::strcmp(d.http_route_metadata[0].route_owner, "autosell") == 0);
    CHECK(d.http_route_metadata[0].op_required == 0 && d.http_route_metadata[0].lease_required == 1);
    CHECK(d.http_route_metadata[1].op_required == 0 && d.http_route_metadata[1].lease_required == 1);
    CHECK(d.resources_count == 2);
    CHECK(d.on_resource_ready == nullptr);  // kCallPatch/kOwnership 无 backup
    CHECK(d.port_count == 1 &&
          d.ports[0].id == qol::contracts::QolPortId::QOL_PORT_INVENTORY_EXTRAS);

    const qol::contracts::ResourceClaim& patch = d.resources[0];
    CHECK(std::strcmp(patch.id, "patch:SCENE_DRAW_EQUIP+0x1cc") == 0);
    CHECK(std::strcmp(patch.symbol, "P_SCENE_DRAW_EQUIP_DESC") == 0);
    CHECK(patch.mechanism == ResourceMechanism::kCallPatch);
    CHECK(patch.requirement == ResourceRequirement::kRequired);
    CHECK(patch.failure == ResourceFailure::kFailClosed);
    CHECK(patch.gate == nullptr);
    CHECK(patch.resumable);
    CHECK(patch.handler ==
          reinterpret_cast<void*>(&qol::features::autosell::autosell_equip_desc_draw_wrapper));

    const qol::contracts::ResourceClaim& popup = d.resources[1];
    CHECK(std::strcmp(popup.id, "own:popup.autosell") == 0);
    CHECK(popup.mechanism == ResourceMechanism::kOwnership);
    CHECK(popup.requirement == ResourceRequirement::kRequired);
    CHECK(popup.failure == ResourceFailure::kFailSoft);
    CHECK(popup.gate == nullptr);
    CHECK(popup.resumable);
    const auto* ops = static_cast<const OwnershipOps*>(popup.handler);
    CHECK(ops != nullptr);
    CHECK(ops->acquire == &qol::features::autosell::autosell_popup_acquire);
    CHECK(ops->release == &qol::features::autosell::autosell_popup_release);
}

void test_provider_registration() {
    qol::infra::panels_reset_for_test();
    const qol::contracts::QolHostSdk sdk = fake_sdk();
    static const qol::contracts::QolInventoryExtrasPort inventory_port{{1, 0}, {}};
    const void* ports[] = {&inventory_port};
    qol::contracts::PluginContext ctx{
        sizeof(qol::contracts::PluginContext),
        qol::contracts::kHostSdkMajor,
        qol::contracts::kHostSdkMinor,
        &sdk,
        nullptr,
        "autosell",
        ports,
        1,
    };
    ctx.owner_token = 1;
    const qol::contracts::PluginDescriptor& d =
        qol::features::autosell::plugin_descriptor();

    CHECK(d.install(ctx) == 0);
    const qol::contracts::PanelProviderSpec* provider =
        qol::infra::panels_find_provider("autosell");
    CHECK(provider != nullptr);
    CHECK(std::strcmp(provider->owner, "autosell") == 0);
    CHECK(provider->open == &qol::features::autosell::autosell_provider_open);
    CHECK(provider->close == &qol::features::autosell::autosell_provider_close);
    CHECK(provider->active == &qol::features::autosell::autosell_provider_active);
    CHECK(provider->block_reason == &qol::features::autosell::autosell_provider_block_reason);
    CHECK(provider->block_reason() == nullptr);

    d.deactivate(ctx);
    CHECK(qol::infra::panels_find_provider("autosell") == nullptr);
}

void test_null_sdk_is_safe() {
    qol::contracts::PluginContext ctx{};
    const qol::contracts::PluginDescriptor& d =
        qol::features::autosell::plugin_descriptor();
    CHECK(d.install(ctx) == 1);
    d.deactivate(ctx);
}

}  // namespace

int main() {
    test_manifest();
    test_null_sdk_is_safe();
    test_provider_registration();
    std::printf("autosell_plugin: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
