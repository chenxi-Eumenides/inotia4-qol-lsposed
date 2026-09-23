#include "plugin.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "contracts/game_ports/item_port.h"
#include "contracts/http.h"
#include "contracts/item_facts.h"
#include "response.h"
#include "snapshot.h"

namespace qol::features::core_item {
namespace {

constexpr const char* kPluginId = "core.item";
constexpr const char* kPluginVersion = "0.1.0";
constexpr uint32_t kInventoryCapacity = 65536;
constexpr uint32_t kShopCapacity = 32768;
// 主线程快照节拍：库存/商店 JSON 构造不宜逐帧执行；15 帧（≈4Hz）对只读查询足够。
constexpr int32_t kSnapshotIntervalFrames = 15;
constexpr char kNotInGameBody[] = "{\"error\":\"not in game\"}";

enum class Query : uint8_t {
    kInventory,
    kMoney,
    kItems,
    kBagInfo,
    kBagSlot,
    kShopItems,
};

constexpr contracts::HttpRoute kRoutes[] = {
    {contracts::HttpMethod::kGet, "/api/item/inventory", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/item/inventory/money", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/item/inventory/items", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/item/inventory/bag/{bag}/info", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/item/inventory/bag/{bag}/{slot}", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/item/shop/items", kPluginId, 0, 1},
};

constexpr Query kQueries[] = {
    Query::kInventory, Query::kMoney,        Query::kItems,
    Query::kBagInfo,   Query::kBagSlot,      Query::kShopItems,
};

enum class SnapshotState : uint8_t { kUnavailable, kNotInGame, kReady };

struct ItemSnapshot {
    SnapshotState state = SnapshotState::kUnavailable;
    int64_t money = 0;
    uint32_t inventory_size = 0;
    char inventory[kInventoryCapacity]{};
    uint32_t shop_size = 0;
    char shop[kShopCapacity]{};
};

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolItemPort* g_port = nullptr;
uint64_t g_frame_task = 0;
Snapshot<ItemSnapshot> g_snapshot;

QolResult not_ready() {
    return contracts::error(kQolErrNotReady, "item facts not ready", 20);
}

QolResult category(const void* item, int32_t* out_category) {
    if (g_port == nullptr || g_port->read.category == nullptr) return not_ready();
    return g_port->read.category(item, out_category);
}

QolResult is_real_equip(const void* item, int32_t* out_value) {
    if (g_port == nullptr || g_port->read.is_real_equip == nullptr) return not_ready();
    return g_port->read.is_real_equip(item, out_value);
}

QolResult is_jewel_category(int32_t category_value, int32_t* out_is_jewel) {
    if (g_port == nullptr || g_port->read.is_jewel_category == nullptr) {
        return not_ready();
    }
    return g_port->read.is_jewel_category(category_value, out_is_jewel);
}

QolResult is_no_sell(int32_t category_value, int32_t* out_value) {
    if (g_port == nullptr || g_port->read.is_no_sell == nullptr) return not_ready();
    return g_port->read.is_no_sell(category_value, out_value);
}

QolResult sell_price(const void* item, int64_t* out_price) {
    if (g_port == nullptr || g_port->read.sell_price == nullptr) return not_ready();
    return g_port->read.sell_price(item, out_price);
}

QolResult cumulate_count(const void* item, int32_t* out_count) {
    if (g_port == nullptr || g_port->read.cumulate_count == nullptr) return not_ready();
    return g_port->read.cumulate_count(item, out_count);
}

QolResult item_ref_at(int32_t bag, int32_t slot, contracts::QolInventoryItemRef* out) {
    if (g_port == nullptr || g_port->read.item_ref_at == nullptr) return not_ready();
    return g_port->read.item_ref_at(bag, slot, out);
}

QolResult physical_item_at(int32_t bag, int32_t slot, void** out_item) {
    if (g_port == nullptr || g_port->read.physical_item_at == nullptr) {
        return not_ready();
    }
    return g_port->read.physical_item_at(bag, slot, out_item);
}

QolResult detail_context(int32_t* out_desc_type, int32_t* out_bag, void** out_control) {
    if (g_port == nullptr || g_port->read.detail_context == nullptr) {
        return not_ready();
    }
    return g_port->read.detail_context(out_desc_type, out_bag, out_control);
}

QolResult cursor_index(const void* control, int32_t* out_slot) {
    if (g_port == nullptr || g_port->read.cursor_index == nullptr) {
        return not_ready();
    }
    return g_port->read.cursor_index(control, out_slot);
}

constexpr contracts::ItemFactsApi kItemFactsApi{
    &category,
    &is_real_equip,
    &is_jewel_category,
    &is_no_sell,
    &sell_price,
    &cumulate_count,
    &item_ref_at,
    &physical_item_at,
    &detail_context,
    &cursor_index,
};

const contracts::CapabilityProvisionRef kProvides[] = {
    {contracts::kItemFactsCapabilityId, contracts::kItemFactsVersion, &kItemFactsApi},
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_ITEM, {1, 0}},
};

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kHttpRegistryCapabilityId, {contracts::kHttpRegistryVersion.major,
                                             contracts::kHttpRegistryVersion.minor}},
    {contracts::kHttpDispatchCapabilityId, {contracts::kHttpDispatchVersion.major,
                                             contracts::kHttpDispatchVersion.minor}},
};

void log_message(const contracts::QolHostSdk* sdk, const char* message) {
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    uint32_t length = 0;
    while (message[length] != '\0') ++length;
    (void)sdk->log.write(1, 1, kPluginId, message, length);
}

bool copy_buffer(char* destination, uint32_t capacity, uint32_t* destination_size,
                 const char* source, uint32_t source_size) {
    if (destination == nullptr || destination_size == nullptr || source == nullptr ||
        source_size >= capacity) {
        return false;
    }
    std::memcpy(destination, source, source_size);
    destination[source_size] = '\0';
    *destination_size = source_size;
    return true;
}

void publish_not_in_game(ItemSnapshot& snapshot) {
    snapshot.state = SnapshotState::kNotInGame;
    (void)copy_buffer(snapshot.inventory, kInventoryCapacity, &snapshot.inventory_size,
                      kNotInGameBody, static_cast<uint32_t>(sizeof(kNotInGameBody) - 1));
    (void)copy_buffer(snapshot.shop, kShopCapacity, &snapshot.shop_size, kNotInGameBody,
                      static_cast<uint32_t>(sizeof(kNotInGameBody) - 1));
    g_snapshot.publish(snapshot);
}

void update_snapshot() {
    if (g_port == nullptr || g_port->read.inventory_json == nullptr ||
        g_port->read.shop_items_json == nullptr || g_port->read.money == nullptr ||
        g_port->read.in_world == nullptr) {
        return;
    }
    // 静态名称表未下发时不发布，避免输出空名称；HTTP 侧表现为 not ready。
    if (g_port->read.static_ready != nullptr) {
        int32_t ready = 0;
        if (!contracts::succeeded(g_port->read.static_ready(&ready)) || ready == 0) return;
    }

    ItemSnapshot next{};
    int32_t in_world = 0;
    if (!contracts::succeeded(g_port->read.in_world(&in_world))) return;
    if (in_world == 0) {
        publish_not_in_game(next);
        return;
    }

    char inventory[kInventoryCapacity]{};
    uint32_t inventory_size = 0;
    if (!contracts::succeeded(g_port->read.inventory_json(inventory, kInventoryCapacity,
                                                          &inventory_size))) {
        return;
    }
    if (std::strncmp(inventory, "{\"error\":", 9) == 0) {
        publish_not_in_game(next);
        return;
    }
    if (!copy_buffer(next.inventory, kInventoryCapacity, &next.inventory_size, inventory,
                     inventory_size)) {
        return;
    }

    char shop[kShopCapacity]{};
    uint32_t shop_size = 0;
    if (!contracts::succeeded(g_port->read.shop_items_json(shop, kShopCapacity, &shop_size))) {
        return;
    }
    if (!copy_buffer(next.shop, kShopCapacity, &next.shop_size, shop, shop_size)) return;

    if (!contracts::succeeded(g_port->read.money(&next.money))) return;
    next.state = SnapshotState::kReady;
    g_snapshot.publish(next);
}

int32_t frame_tick(int64_t, void*) {
    update_snapshot();
    return 1;
}

QolResult response_error(contracts::HttpRes* response, uint16_t status, const char* message) {
    const std::string body = std::string("{\"ok\":false,\"error\":\"") + message + "\"}";
    if (response == nullptr || response->body == nullptr || response->body_capacity < body.size()) {
        return contracts::error(kQolErrInternal, "http response buffer too small", 31);
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = status;
    return contracts::error(status == 503 ? kQolErrNotReady : kQolErrInvalidArgument, message,
                            static_cast<uint32_t>(std::strlen(message)));
}

QolResult response_body(contracts::HttpRes* response, const char* body, uint32_t size,
                        uint16_t status = 200) {
    if (response == nullptr || response->body == nullptr || body == nullptr ||
        response->body_capacity < size) {
        return response_error(response, 500, "http response buffer too small");
    }
    std::memcpy(response->body, body, size);
    response->body_size = size;
    response->status = status;
    return contracts::ok();
}

bool parse_path_int(const contracts::HttpReq& request, const char* name, int32_t* out) {
    if (request.path_params == nullptr || name == nullptr || out == nullptr) return false;
    for (uint32_t i = 0; i < request.path_param_count; ++i) {
        const contracts::HttpPathParam& param = request.path_params[i];
        if (param.name == nullptr || param.name_len != std::strlen(name) ||
            std::strncmp(param.name, name, param.name_len) != 0) {
            continue;
        }
        if (param.value == nullptr || param.value_len == 0) return false;
        const std::string value(param.value, param.value_len);
        char* end = nullptr;
        const long parsed = std::strtol(value.c_str(), &end, 10);
        if (end == nullptr || *end != '\0') return false;
        *out = static_cast<int32_t>(parsed);
        return true;
    }
    return false;
}

QolResult query_handler(const contracts::HttpReq* request, contracts::HttpRes* response, void* ctx) {
    if (request == nullptr || response == nullptr || ctx == nullptr) {
        return response_error(response, 400, "bad request");
    }
    ItemSnapshot snapshot{};
    if (!g_snapshot.read(snapshot)) return response_error(response, 503, "snapshot not ready");
    if (snapshot.state == SnapshotState::kNotInGame) {
        return response_body(response, kNotInGameBody,
                             static_cast<uint32_t>(sizeof(kNotInGameBody) - 1));
    }
    if (snapshot.state != SnapshotState::kReady) {
        return response_error(response, 503, "snapshot not ready");
    }

    const Query query = *static_cast<const Query*>(ctx);
    switch (query) {
        case Query::kInventory: {
            std::string body;
            if (!enrich_inventory(snapshot.inventory, snapshot.inventory_size, g_port, &body)) {
                return response_error(response, 500, "internal error");
            }
            return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
        }
        case Query::kMoney: {
            const std::string body = "{\"money\":" + std::to_string(snapshot.money) + "}";
            return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
        }
        case Query::kItems: {
            std::string body;
            if (!enrich_items_list(snapshot.inventory, snapshot.inventory_size, g_port, &body)) {
                return response_error(response, 500, "internal error");
            }
            return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
        }
        case Query::kBagInfo: {
            int32_t bag = 0;
            if (!parse_path_int(*request, "bag", &bag)) {
                return response_error(response, 400, "bad request");
            }
            std::string body;
            bool found = false;
            if (!enrich_bag_info(snapshot.inventory, snapshot.inventory_size, bag, &body, &found)) {
                return response_error(response, 500, "internal error");
            }
            if (!found) return response_error(response, 404, "not found");
            return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
        }
        case Query::kBagSlot: {
            int32_t bag = 0;
            int32_t slot = 0;
            if (!parse_path_int(*request, "bag", &bag) ||
                !parse_path_int(*request, "slot", &slot)) {
                return response_error(response, 400, "bad request");
            }
            std::string body;
            bool bag_found = false;
            if (!enrich_bag_slot(snapshot.inventory, snapshot.inventory_size, bag, slot, g_port,
                                 &body, &bag_found)) {
                return response_error(response, 500, "internal error");
            }
            if (!bag_found) return response_error(response, 404, "not found");
            return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
        }
        case Query::kShopItems: {
            std::string body;
            if (!enrich_shop_items(snapshot.shop, snapshot.shop_size, g_port, &body)) {
                return response_error(response, 500, "internal error");
            }
            return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
        }
    }
    return response_error(response, 500, "internal error");
}

const contracts::HttpQueryHandlerDescriptor kHttpQueries[] = {
    {kRoutes[0].method, kRoutes[0].path, kPluginId, &query_handler, const_cast<Query*>(&kQueries[0])},
    {kRoutes[1].method, kRoutes[1].path, kPluginId, &query_handler, const_cast<Query*>(&kQueries[1])},
    {kRoutes[2].method, kRoutes[2].path, kPluginId, &query_handler, const_cast<Query*>(&kQueries[2])},
    {kRoutes[3].method, kRoutes[3].path, kPluginId, &query_handler, const_cast<Query*>(&kQueries[3])},
    {kRoutes[4].method, kRoutes[4].path, kPluginId, &query_handler, const_cast<Query*>(&kQueries[4])},
    {kRoutes[5].method, kRoutes[5].path, kPluginId, &query_handler, const_cast<Query*>(&kQueries[5])},
};

void stop_frame_task(const contracts::QolHostSdk* sdk) {
    if (g_frame_task != 0 && sdk != nullptr && sdk->scheduler.remove != nullptr) {
        (void)sdk->scheduler.remove(g_frame_task);
    }
    g_frame_task = 0;
}

int plugin_install(contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_port = nullptr;
    g_frame_task = 0;
    ItemSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    if (ctx.struct_size < contracts::PLUGIN_CONTEXT_PORTS_STRUCT_SIZE ||
        ctx.sdk_major != contracts::kHostSdkMajor || g_sdk == nullptr || ctx.ports == nullptr ||
        ctx.port_count != 1 || ctx.ports[0] == nullptr) {
        g_sdk = nullptr;
        return 1;
    }
    g_port = static_cast<const contracts::QolItemPort*>(ctx.ports[0]);
    if (!contracts::satisfies(g_port->abi_version, {1, 0}) ||
        g_port->read.category == nullptr || g_port->read.item_ref_at == nullptr ||
        g_port->read.inventory_json == nullptr || g_port->read.shop_items_json == nullptr ||
        g_port->read.in_world == nullptr || g_port->read.money == nullptr ||
        g_port->read.static_text == nullptr || g_port->read.static_ready == nullptr) {
        g_port = nullptr;
        g_sdk = nullptr;
        return 1;
    }
    log_message(g_sdk, "[core.item] provider install");
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    stop_frame_task(ctx.sdk);
    log_message(ctx.sdk, "[core.item] provider deactivate");
    g_port = nullptr;
    g_sdk = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    ItemSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    if (g_frame_task == 0 && g_sdk != nullptr && g_sdk->scheduler.add != nullptr) {
        uint64_t task = 0;
        if (contracts::succeeded(g_sdk->scheduler.add(
                1, &frame_tick, nullptr, kSnapshotIntervalFrames, 0, &task))) {
            g_frame_task = task;
        } else {
            log_message(ctx.sdk, "[core.item] snapshot scheduler unavailable");
        }
    }
    log_message(ctx.sdk, "[core.item] provider enabled: capability + http queries");
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    stop_frame_task(ctx.sdk);
    ItemSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    log_message(ctx.sdk, "[core.item] provider disabled: capability + http queries withdrawn");
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_port != nullptr;
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    2,
    kRequires,
    0,
    nullptr,
    1,
    kProvides,
    0,
    nullptr,
    0,
    nullptr,
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,
    kPorts,
    1,
    kHttpQueries,
    static_cast<uint32_t>(sizeof(kHttpQueries) / sizeof(kHttpQueries[0])),
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::core_item
