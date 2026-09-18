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
#include "contracts/panels.h"
#include "contracts/plugin.h"
#include "infra/config.h"
#include "infra/events.h"
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

namespace qol::infra {
bool config_get_bool(const char*, bool) { return false; }
SubscriptionId subscribe(EventTypeId, EventHandler, void*) { return 0; }
bool unsubscribe(SubscriptionId) { return true; }
}  // namespace qol::infra

namespace {

using qol::contracts::OwnershipOps;
using qol::contracts::ResourceFailure;
using qol::contracts::ResourceMechanism;
using qol::contracts::ResourceRequirement;

void test_manifest() {
    const qol::contracts::PluginDescriptor& d =
        qol::features::autosell::plugin_descriptor();
    CHECK(std::strcmp(d.id, "autosell") == 0);
    CHECK(d.requires_count == 0 && d.optional_count == 0 && d.provides_count == 0);
    CHECK(d.resources_count == 2);
    CHECK(d.on_resource_ready == nullptr);  // kCallPatch/kOwnership 无 backup

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
    qol::contracts::PluginContext ctx{};
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

}  // namespace

int main() {
    test_manifest();
    test_provider_registration();
    std::printf("autosell_plugin: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
