#include "contracts/host_sdk.h"
#include "contracts/http.h"
#include "host/http_dispatch.h"
#include "host/http_registry.h"
#include "plugins/core.feature/plugin.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(condition) do { \
    if (condition) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } \
} while (0)

int main() {
    using namespace qol;
    constexpr char kPath[] = "/api/character/combat/{role}/set_skill_usage";
    constexpr char kExpectedBody[] = "{\"ok\":false,\"error\":\"not implemented\"}";
    const auto& descriptor = features::core_feature::plugin_descriptor();
    CHECK(std::strcmp(descriptor.id, "core.feature") == 0);
    CHECK(std::strcmp(descriptor.version, "0.1.0") == 0);
    CHECK(descriptor.requires_count == 2 && descriptor.requires != nullptr);
    CHECK(std::strcmp(descriptor.requires[0].id, contracts::kHttpRegistryCapabilityId) == 0);
    CHECK(std::strcmp(descriptor.requires[1].id, contracts::kHttpDispatchCapabilityId) == 0);
    CHECK(descriptor.provides_count == 0 && descriptor.port_count == 0);

    const contracts::HttpRoute route{contracts::HttpMethod::kPost, kPath, "core.feature", 0, 1};
    CHECK(host::http_registry_api().register_route(&route).ok == kQolOk);
    contracts::QolHostSdk sdk{};
    sdk.abi = {sizeof(sdk), contracts::kHostSdkMajor, contracts::kHostSdkMinor};
    sdk.http = host::http_registry_api();
    sdk.http_dispatch = host::http_dispatch_api();
    contracts::PluginContext context{};
    context.struct_size = sizeof(context);
    context.sdk_major = contracts::kHostSdkMajor;
    context.sdk_minor = contracts::kHostSdkMinor;
    context.sdk = &sdk;
    context.plugin_id = "core.feature";
    CHECK(descriptor.install(context) == 0);

    constexpr char kRequestPath[] = "/api/character/combat/2/set_skill_usage";
    constexpr char kRequestBody[] = "{\"action_id\":3,\"mode\":\"normal\"}";
    const contracts::HttpPathParam param{"role", 4, "2", 1};
    contracts::HttpReq request{contracts::HttpMethod::kPost, kRequestPath,
                               sizeof(kRequestPath) - 1,
                               reinterpret_cast<const uint8_t*>(kRequestBody),
                               sizeof(kRequestBody) - 1, nullptr, 0, &param, 1};
    char storage[128]{};
    contracts::HttpRes response{reinterpret_cast<uint8_t*>(storage), sizeof(storage), 0, 0};
    CHECK(contracts::succeeded(host::http_dispatch_api().dispatch(&request, &response)));
    CHECK(response.status == 501);
    CHECK(std::string(storage, response.body_size) == kExpectedBody);

    descriptor.deactivate(context);
    response = {reinterpret_cast<uint8_t*>(storage), sizeof(storage), 0, 0};
    CHECK(!contracts::succeeded(host::http_dispatch_api().dispatch(&request, &response)));
    CHECK(response.status == 503);
    CHECK(host::http_registry_api().unregister_owner("core.feature").ok == kQolOk);
    CHECK(g_fail == 0);
    std::printf("core_feature_plugin: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
