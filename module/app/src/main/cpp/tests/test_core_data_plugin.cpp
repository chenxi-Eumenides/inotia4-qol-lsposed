#include "contracts/host_sdk.h"
#include "contracts/http.h"
#include "contracts/static_data.h"
#include "host/http_dispatch.h"
#include "host/http_registry.h"
#include "plugins/core.data/plugin.h"
#include "plugins/core.data/json.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(condition) do { \
    if (condition) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } \
} while (0)

namespace {

using qol::contracts::HttpMethod;
using AssetMap = std::map<std::string, std::string>;

constexpr char kMapInfo[] =
    "{\"records\":[{\"u16\":[3513],\"text_0\":\"Map \\\"A\\\"\"},"
    "{\"u16\":[],\"text_0\":\"\"},null]}";
constexpr char kManifest[] = "{\"tables\":[\"SMALL\"],\"text_langs\":[\"en\"]}";
constexpr char kSmallTable[] =
    "{\"records\":[{\"u16\":[1],\"text_0\":\"Red \\\"Sword\\\" \\u2603\"},"
    "{\"text_0\":\"Blue blade\"},null]}";
constexpr char kEmptyTable[] = "{\"records\":[]}";
constexpr char kExits[] =
    "{\"m0\":{\"exits\":[{\"x\":1,\"y\":2,\"targetMapId\":3,\"targetX\":4,\"targetY\":5}]}}";

const qol::contracts::HttpRoute kRoutes[] = {
    {HttpMethod::kGet, "/api/world/maps/list", "core.data", 0, 1},
    {HttpMethod::kGet, "/api/world/maps/{map_id}", "core.data", 0, 1},
    {HttpMethod::kGet, "/api/world/maps/{map_id}/tiles", "core.data", 0, 1},
    {HttpMethod::kGet, "/api/world/maps/{map_id}/exits", "core.data", 0, 1},
    {HttpMethod::kGet, "/api/system/tables", "core.data", 0, 1},
    {HttpMethod::kGet, "/api/system/tables/{table}", "core.data", 0, 1},
    {HttpMethod::kGet, "/api/system/tables/{table}/search", "core.data", 0, 1},
    {HttpMethod::kGet, "/api/system/tables/{table}/download", "core.data", 0, 1},
    {HttpMethod::kGet, "/api/system/tables/story-events", "core.data", 0, 1},
    {HttpMethod::kGet, "/api/system/tables/text", "core.data", 0, 1},
    {HttpMethod::kGet, "/api/system/help", "core.data", 0, 1},
    {HttpMethod::kGet, "/api/system/download", "core.data", 0, 1},
};

qol::contracts::QolHostSdk make_sdk() {
    qol::contracts::QolHostSdk sdk{};
    sdk.abi = {sizeof(sdk), qol::contracts::kHostSdkMajor, qol::contracts::kHostSdkMinor};
    sdk.http = qol::host::http_registry_api();
    sdk.http_dispatch = qol::host::http_dispatch_api();
    return sdk;
}

qol::contracts::PluginContext make_context(const qol::contracts::QolHostSdk* sdk) {
    qol::contracts::PluginContext context{};
    context.struct_size = sizeof(context);
    context.sdk_major = qol::contracts::kHostSdkMajor;
    context.sdk_minor = qol::contracts::kHostSdkMinor;
    context.sdk = sdk;
    context.plugin_id = "core.data";
    return context;
}

bool import_assets(const AssetMap& assets) {
    uint32_t total = 0;
    for (const auto& [path, data] : assets) {
        (void)path;
        total += static_cast<uint32_t>(data.size());
    }
    if (!qol::contracts::succeeded(qol::features::core_data::begin_asset_import(
            static_cast<uint32_t>(assets.size()), total))) return false;
    for (const auto& [path, data] : assets) {
        const auto result = qol::features::core_data::import_asset(
            path.data(), static_cast<uint32_t>(path.size()),
            reinterpret_cast<const uint8_t*>(data.data()), static_cast<uint32_t>(data.size()));
        if (!qol::contracts::succeeded(result)) {
            qol::features::core_data::cancel_asset_import();
            return false;
        }
    }
    return qol::contracts::succeeded(qol::features::core_data::finish_asset_import());
}

std::string base64(const std::vector<uint8_t>& input) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve((input.size() + 2) / 3 * 4);
    for (size_t i = 0; i < input.size(); i += 3) {
        const uint32_t a = input[i];
        const uint32_t b = i + 1 < input.size() ? input[i + 1] : 0;
        const uint32_t c = i + 2 < input.size() ? input[i + 2] : 0;
        const uint32_t chunk = (a << 16) | (b << 8) | c;
        output.push_back(kAlphabet[(chunk >> 18) & 63]);
        output.push_back(kAlphabet[(chunk >> 12) & 63]);
        output.push_back(i + 1 < input.size() ? kAlphabet[(chunk >> 6) & 63] : '=');
        output.push_back(i + 2 < input.size() ? kAlphabet[chunk & 63] : '=');
    }
    return output;
}

struct DispatchResult {
    uint16_t status = 0;
    std::string body;
    int32_t dispatch_ok = kQolErrInternal;
};

DispatchResult dispatch(const char* path, const char* query = nullptr,
                        const qol::contracts::HttpPathParam* params = nullptr,
                        uint32_t param_count = 0, uint32_t max_response = 32 * 1024 * 1024) {
    const std::string path_string(path);
    const std::string query_string(query == nullptr ? "" : query);
    qol::contracts::HttpReq request{HttpMethod::kGet, path_string.c_str(),
        static_cast<uint32_t>(path_string.size()), nullptr, 0, query_string.c_str(),
        static_cast<uint32_t>(query_string.size()), params, param_count};
    std::vector<uint8_t> storage;
    uint16_t status = 0;
    uint32_t size = 0;
    const QolResult result = qol::host::http_dispatch_dynamic(
        &request, &storage, max_response, &status, &size);
    return {status, std::string(reinterpret_cast<const char*>(storage.data()), size), result.ok};
}

void register_routes() {
    for (const auto& route : kRoutes)
        CHECK(qol::host::http_registry_api().register_route(&route).ok == kQolOk);
}

void test_descriptor_and_basic_responses(const qol::contracts::PluginDescriptor& descriptor,
                                         qol::contracts::PluginContext& context) {
    CHECK(std::strcmp(descriptor.id, "core.data") == 0);
    CHECK(std::strcmp(descriptor.version, "0.1.0") == 0);
    CHECK(descriptor.requires_count == 2 && descriptor.requires != nullptr);
    CHECK(std::strcmp(descriptor.requires[0].id, qol::contracts::kHttpRegistryCapabilityId) == 0);
    CHECK(std::strcmp(descriptor.requires[1].id, qol::contracts::kHttpDispatchCapabilityId) == 0);
    CHECK(descriptor.provides_count == 1 && descriptor.provides != nullptr);
    CHECK(std::strcmp(descriptor.provides[0].id, qol::contracts::kStaticDataCapabilityId) == 0);
    CHECK(descriptor.http_query_count == 0);
    CHECK(!qol::contracts::succeeded(qol::features::core_data::begin_asset_import(129, 0)));
    CHECK(!qol::contracts::succeeded(qol::features::core_data::begin_asset_import(
        1, 32 * 1024 * 1024 + 1)));
    CHECK(qol::contracts::succeeded(qol::features::core_data::begin_asset_import(
        1, 16 * 1024 * 1024 + 1)));
    CHECK(!qol::contracts::succeeded(qol::features::core_data::import_asset(
        "oversized.json", 14, reinterpret_cast<const uint8_t*>("x"), 16 * 1024 * 1024 + 1)));
    qol::features::core_data::cancel_asset_import();
    CHECK(qol::contracts::succeeded(qol::features::core_data::begin_asset_import(1, 1)));
    CHECK(!qol::contracts::succeeded(qol::features::core_data::import_asset(
        "../escape.json", 14, reinterpret_cast<const uint8_t*>("x"), 1)));
    qol::features::core_data::cancel_asset_import();

    const std::string escaped_name = "Map \"A\"";
    CHECK(descriptor.install(context) == 0);
    const DispatchResult unavailable = dispatch("/api/system/tables");
    CHECK(unavailable.status == 503);
    qol::contracts::HttpRoute registered[16]{};
    uint32_t registered_count = 0;
    CHECK(qol::contracts::succeeded(context.sdk->http.list(registered, 16, &registered_count)));
    CHECK(registered_count == 12);
    uint32_t core_data_routes = 0;
    for (uint32_t i = 0; i < registered_count; ++i)
        if (std::strcmp(registered[i].owner, "core.data") == 0) ++core_data_routes;
    CHECK(core_data_routes == 12);

    const AssetMap assets{
        {"manifest.json", kManifest},
        {"tables/MAPINFOBASE.json", kMapInfo},
        {"tables/SMALL.json", kSmallTable},
        {"tables/EMPTY.json", kEmptyTable},
        {"maps/exits.json", kExits},
    };
    CHECK(import_assets(assets));
    char map_name[32]{};
    uint32_t map_name_size = 0;
    const auto* static_data = static_cast<const qol::contracts::QolStaticDataApi*>(
        descriptor.provides[0].iface);
    CHECK(qol::contracts::succeeded(static_data->map_name(0, map_name, sizeof(map_name),
                                                          &map_name_size)));
    CHECK(std::string(map_name, map_name_size) == escaped_name);

    const DispatchResult list = dispatch("/api/world/maps/list");
    CHECK(list.status == 200);
    if (list.status != 200) std::printf("map list actual status=%u body=%s\n", list.status, list.body.c_str());
    const std::string expected_list = "{\"maps\":[{\"map_id\":3513,\"name\":" +
                                      qol::features::core_data::quote_json(escaped_name) + "}]}";
    if (list.body != expected_list) std::printf("map list actual: %s\nexpected: %s\n",
                                                 list.body.c_str(), expected_list.c_str());
    CHECK(list.body == expected_list);

    const qol::contracts::HttpPathParam map_id{"map_id", 6, "0", 1};
    const DispatchResult map = dispatch("/api/world/maps/0", nullptr, &map_id, 1);
    CHECK(map.status == 200);
    CHECK(map.body == "{\"map_id\":0,\"text_id\":3513,\"name\":" +
                       qol::features::core_data::quote_json(escaped_name) +
                       ",\"raw\":{\"u16\":[3513],\"text_0\":" +
                       qol::features::core_data::quote_json(escaped_name) + "}}");

    const qol::contracts::HttpPathParam legacy_id{"map_id", 6, "3513", 4};
    const DispatchResult legacy_map = dispatch("/api/world/maps/3513", nullptr, &legacy_id, 1);
    CHECK(legacy_map.status == 200 && legacy_map.body.find("\"index\":0") != std::string::npos);

    const DispatchResult tables = dispatch("/api/system/tables");
    CHECK(tables.status == 200 && tables.body == "{\"tables\":[\"SMALL\"]}");
    const qol::contracts::HttpPathParam small{"table", 5, "SMALL", 5};
    const DispatchResult table = dispatch("/api/system/tables/SMALL", nullptr, &small, 1);
    CHECK(table.status == 200 && table.body == kSmallTable);
    const qol::contracts::HttpPathParam search_table{"table", 5, "SMALL", 5};
    const DispatchResult search = dispatch("/api/system/tables/SMALL/search", "q=RED",
                                           &search_table, 1);
    CHECK(search.status == 200);
    if (search.body.find("☃") == std::string::npos) std::printf("search actual status=%u body=%s\n", search.status, search.body.c_str());
    const std::string expected_search =
        "{\"items\":[{\"index\":0,\"name\":\"Red \\\"Sword\\\" ☃\",\"raw\":"
        "{\"u16\":[1],\"text_0\":\"Red \\\"Sword\\\" ☃\"}}]}";
    if (search.body != expected_search) std::printf("search actual: %s\nexpected: %s\n",
                                                    search.body.c_str(), expected_search.c_str());
    CHECK(search.body == expected_search);
    const qol::contracts::HttpPathParam empty_table{"table", 5, "EMPTY", 5};
    const DispatchResult empty_search = dispatch("/api/system/tables/EMPTY/search", "q=%20%20",
                                                 &empty_table, 1);
    CHECK(empty_search.status == 200 && empty_search.body == "{\"items\":[]}");

    const AssetMap missing_key{{"tables/SMALL.json", "{\"other\":[]}"}};
    CHECK(import_assets(missing_key));
    const DispatchResult missing_records = dispatch("/api/system/tables/SMALL/search", "q=x",
                                                    &search_table, 1);
    CHECK(missing_records.status == 404);
    const DispatchResult missing_asset = dispatch("/api/system/tables/UNKNOWN", nullptr,
                                                   &small, 1);
    CHECK(missing_asset.status == 404);
    CHECK(import_assets({{"tables/SMALL.json", "{broken"}}));
    const DispatchResult malformed_table = dispatch("/api/system/tables/SMALL/search", "q=x",
                                                    &search_table, 1);
    CHECK(malformed_table.status == 404);

    const DispatchResult not_implemented = dispatch(
        "/api/system/tables/SMALL/download", nullptr, &small, 1);
    CHECK(not_implemented.status == 501);
    CHECK(not_implemented.body == "{\"ok\":false,\"error\":\"not implemented\"}");
    const DispatchResult help = dispatch("/api/system/help");
    const DispatchResult download = dispatch("/api/system/download");
    CHECK(help.status == 501 && help.body == not_implemented.body);
    CHECK(download.status == 501 && download.body == not_implemented.body);
    CHECK(descriptor.deactivate != nullptr);
}

void test_tiles_and_exits() {
    std::vector<uint8_t> tiles(4096, 0);
    tiles[0] = 255;
    tiles[1] = 64;
    const std::string tiles_json = "{\"m0\":{\"width\":63,\"height\":62,\"tiles\":" +
                                   qol::features::core_data::quote_json(base64(tiles)) + "}}";
    CHECK(import_assets({{"maps/tiles.json", tiles_json}, {"maps/exits.json", kExits}}));
    const qol::contracts::HttpPathParam map_id{"map_id", 6, "0", 1};
    const DispatchResult result = dispatch("/api/world/maps/0/tiles", nullptr, &map_id, 1);
    CHECK(result.status == 200);
    CHECK(result.body.find("\"width\":63,\"height\":62") != std::string::npos);
    CHECK(result.body.find("[255,64,0") != std::string::npos);
    const DispatchResult exits = dispatch("/api/world/maps/0/exits", nullptr, &map_id, 1);
    CHECK(exits.status == 200);
    CHECK(exits.body == "{\"map_id\":0,\"src\":\"static\",\"exits\":[{\"x\":1,\"y\":2,"
                       "\"targetMapId\":3,\"targetX\":4,\"targetY\":5}]}");
}

void test_dynamic_response_sizes() {
    constexpr size_t kTextBytes = 640 * 1024;
    std::string text = "[\"" + std::string(kTextBytes - 5, 't') + "\"]";
    CHECK(text.size() >= 640 * 1024 - 1);
    CHECK(import_assets({{"text/en.json", text}}));
    const DispatchResult text_result = dispatch("/api/system/tables/text", "lang=en");
    CHECK(text_result.status == 200 && text_result.body == text);
    CHECK(text_result.body.size() > 262144);

    constexpr size_t kEventPayloadBytes = 11 * 1024 * 1024;
    std::string events = "{\"events\":\"" + std::string(kEventPayloadBytes, 'e') + "\"}";
    CHECK(events.size() > 11 * 1024 * 1024);
    CHECK(import_assets({{"reverse/events.json", events}}));
    const DispatchResult event_result = dispatch("/api/system/tables/story-events");
    CHECK(event_result.status == 200 && event_result.body == events);
    CHECK(event_result.body.size() > 11 * 1024 * 1024);

    const DispatchResult capped = dispatch("/api/system/tables/story-events", nullptr, nullptr, 0,
                                           1024 * 1024);
    CHECK(capped.status == 500);
    CHECK(capped.body == "{\"ok\":false,\"error\":\"response too large\"}");
    CHECK(import_assets({}));
}

}  // namespace

int main() {
    qol::host::http_registry_reset_for_test();
    qol::host::http_dispatch_reset_for_test();
    register_routes();
    const auto& descriptor = qol::features::core_data::plugin_descriptor();
    const auto sdk = make_sdk();
    auto context = make_context(&sdk);
    test_descriptor_and_basic_responses(descriptor, context);
    test_tiles_and_exits();
    test_dynamic_response_sizes();
    descriptor.deactivate(context);
    CHECK(qol::host::http_dispatch_api().unregister_owner("core.data").ok == kQolOk);
    qol::features::core_data::cancel_asset_import();
    std::printf("core_data_plugin: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
