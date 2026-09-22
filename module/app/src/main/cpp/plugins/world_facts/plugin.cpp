#include "plugin.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "contracts/game_ports/world_port.h"
#include "contracts/http.h"
#include "contracts/world_facts.h"
#include "snapshot.h"

namespace qol::features::world_facts {
namespace {

constexpr const char* kPluginId = "world_facts";
constexpr const char* kPluginVersion = "0.1.0";
// 单个 slot 的 JSON 上限；观测值（31 单位 + 掉落）远小于此值，超出即放弃本轮发布。
constexpr uint32_t kJsonCapacity = 32768;
constexpr uint32_t kNameCapacity = 512;
constexpr uint32_t kErrorCapacity = 512;
// 主线程快照节拍：3 次 4096 格 BFS + 若干 JSON 构造不宜每帧执行；4Hz 对只读查询足够。
constexpr int32_t kSnapshotIntervalFrames = 15;
constexpr char kNotInGameBody[] = "{\"error\":\"not in game\"}";

enum class Query : uint8_t { kId, kExits, kUnits, kEnemies, kInteractives, kDrops, kDistance };

constexpr contracts::HttpRoute kRoutes[] = {
    {contracts::HttpMethod::kGet, "/api/world/map/id", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/world/map/exits", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/world/map/units", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/world/map/enemies", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/world/map/interactives", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/world/map/drops", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/world/map/distance", kPluginId, 0, 1},
};

constexpr Query kQueries[] = {
    Query::kId, Query::kExits, Query::kUnits, Query::kEnemies,
    Query::kInteractives, Query::kDrops, Query::kDistance,
};

enum class SnapshotState : uint8_t { kUnavailable, kNotInGame, kReady };

struct WorldSnapshot {
    SnapshotState state = SnapshotState::kUnavailable;
    int32_t map_id = -1;
    uint32_t map_name_size = 0;
    char map_name[kNameCapacity]{};
    uint32_t exits_size = 0;
    char exits[kJsonCapacity]{};
    uint32_t units_size = 0;
    char units[kJsonCapacity]{};
    uint32_t enemies_size = 0;
    char enemies[kJsonCapacity]{};
    uint32_t interactives_size = 0;
    char interactives[kJsonCapacity]{};
    uint32_t drops_size = 0;
    char drops[kJsonCapacity]{};
    uint32_t error_size = 0;
    char error[kErrorCapacity]{};
    contracts::QolWorldNavigation navigation{};
};

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolWorldPort* g_port = nullptr;
uint64_t g_frame_task = 0;
Snapshot<WorldSnapshot> g_snapshot;

QolResult not_ready() {
    return contracts::error(kQolErrNotReady, "world facts not ready", 21);
}

QolResult state(int32_t* out_state) {
    if (g_port == nullptr || g_port->read.state == nullptr) return not_ready();
    return g_port->read.state(out_state);
}

QolResult game_state_gate(int32_t* out_open) {
    if (g_port == nullptr || g_port->read.game_state_gate == nullptr) return not_ready();
    return g_port->read.game_state_gate(out_open);
}

QolResult map_record_count(int32_t* out_count) {
    if (g_port == nullptr || g_port->read.map_record_count == nullptr) return not_ready();
    return g_port->read.map_record_count(out_count);
}

QolResult current_map_id(int32_t* out_map_id) {
    if (g_port == nullptr || g_port->read.current_map_id == nullptr) return not_ready();
    return g_port->read.current_map_id(out_map_id);
}

QolResult map_name(int32_t map_id, const char** out_name) {
    if (g_port == nullptr || g_port->read.map_name == nullptr) return not_ready();
    return g_port->read.map_name(map_id, out_name);
}

QolResult active_player_direction(int32_t* out_direction) {
    if (g_port == nullptr || g_port->read.active_player_direction == nullptr) return not_ready();
    return g_port->read.active_player_direction(out_direction);
}

QolResult money(int64_t* out_money) {
    if (g_port == nullptr || g_port->read.money == nullptr) return not_ready();
    return g_port->read.money(out_money);
}

QolResult tutorial_state(int32_t* out_state) {
    if (g_port == nullptr || g_port->read.tutorial_state == nullptr) return not_ready();
    return g_port->read.tutorial_state(out_state);
}

constexpr contracts::WorldFactsApi kWorldFactsApi{
    &state,
    &game_state_gate,
    &map_record_count,
    &current_map_id,
    &map_name,
    &active_player_direction,
    &money,
    &tutorial_state,
};

const contracts::CapabilityProvisionRef kProvides[] = {
    {contracts::kWorldFactsCapabilityId, contracts::kWorldFactsVersion, &kWorldFactsApi},
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_WORLD, {1, 1}},
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

bool json_has_error(const char* json) {
    return json != nullptr && std::strncmp(json, "{\"error\":", 9) == 0;
}

void publish_not_in_game(WorldSnapshot& snapshot, const char* error, uint32_t error_size) {
    static constexpr char kFallback[] = "{\"error\":\"not in game\"}";
    snapshot.state = SnapshotState::kNotInGame;
    if (!copy_buffer(snapshot.error, kErrorCapacity, &snapshot.error_size, error, error_size)) {
        (void)copy_buffer(snapshot.error, kErrorCapacity, &snapshot.error_size, kFallback,
                          static_cast<uint32_t>(sizeof(kFallback) - 1));
    }
    g_snapshot.publish(snapshot);
}

void update_snapshot() {
    WorldSnapshot next{};
    if (g_port == nullptr || g_port->read.units_json == nullptr ||
        g_port->read.exits_json == nullptr || g_port->read.enemies_json == nullptr ||
        g_port->read.interactives_json == nullptr || g_port->read.drops_json == nullptr ||
        g_port->read.navigation == nullptr || g_port->read.in_world == nullptr) {
        return;
    }

    // 非 world 态各 world 读取器口径不变（迁移前即 `{"error":"not in game"}`）。
    int32_t in_world = 0;
    if (!contracts::succeeded(g_port->read.in_world(&in_world))) return;
    if (in_world == 0) {
        publish_not_in_game(next, kNotInGameBody,
                            static_cast<uint32_t>(sizeof(kNotInGameBody) - 1));
        return;
    }

    char units[kJsonCapacity]{};
    uint32_t units_size = 0;
    if (!contracts::succeeded(g_port->read.units_json(units, kJsonCapacity, &units_size))) return;
    if (json_has_error(units)) {
        publish_not_in_game(next, units, units_size);
        return;
    }
    if (!copy_buffer(next.units, kJsonCapacity, &next.units_size, units, units_size)) return;

    if (!contracts::succeeded(g_port->read.current_map_id(&next.map_id))) return;
    const char* name = nullptr;
    if (g_port->read.map_name != nullptr &&
        contracts::succeeded(g_port->read.map_name(next.map_id, &name)) && name != nullptr) {
        const uint32_t name_size = static_cast<uint32_t>(std::strlen(name));
        if (!copy_buffer(next.map_name, kNameCapacity, &next.map_name_size, name, name_size)) {
            return;
        }
    }

    char* json_targets[] = {next.exits, next.enemies, next.interactives, next.drops};
    uint32_t* json_sizes[] = {&next.exits_size, &next.enemies_size, &next.interactives_size,
                              &next.drops_size};
    contracts::QolWorldJsonFn readers[] = {
        g_port->read.exits_json,
        g_port->read.enemies_json,
        g_port->read.interactives_json,
        g_port->read.drops_json,
    };
    for (uint32_t i = 0; i < 4; ++i) {
        if (!contracts::succeeded(readers[i](json_targets[i], kJsonCapacity, json_sizes[i]))) return;
    }
    if (!contracts::succeeded(g_port->read.navigation(&next.navigation))) return;
    next.state = SnapshotState::kReady;
    g_snapshot.publish(next);
}

int32_t frame_tick(int64_t, void*) {
    update_snapshot();
    return 1;
}

QolResult response_error(contracts::HttpRes* response, uint16_t status, const char* message) {
    const std::string body = std::string("{\"error\":\"") + message + "\"}";
    if (response == nullptr || response->body == nullptr || response->body_capacity < body.size()) {
        return contracts::error(kQolErrInternal, "http response buffer too small", 31);
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = status;
    return contracts::error(status == 503 ? kQolErrNotReady : kQolErrInvalidArgument, message,
                            static_cast<uint32_t>(std::strlen(message)));
}

QolResult response_body(contracts::HttpRes* response, const char* body, uint32_t size) {
    if (response == nullptr || response->body == nullptr || body == nullptr ||
        response->body_capacity < size) {
        return response_error(response, 500, "http response buffer too small");
    }
    std::memcpy(response->body, body, size);
    response->body_size = size;
    response->status = 200;
    return contracts::ok();
}

void append_json_string(std::string& out, const char* value, uint32_t size) {
    for (uint32_t i = 0; i < size; ++i) {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    static constexpr char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[ch >> 4];
                    out += kHex[ch & 0x0f];
                } else {
                    out += static_cast<char>(ch);
                }
                break;
        }
    }
}

bool parse_query_int(const contracts::HttpReq& request, const char* name, int32_t* out) {
    if (request.query == nullptr || name == nullptr || out == nullptr) return false;
    const std::string query(request.query, request.query_len);
    const std::string key = std::string(name) + "=";
    size_t start = 0;
    while (start <= query.size()) {
        const size_t end = query.find('&', start);
        const size_t length = end == std::string::npos ? query.size() - start : end - start;
        if (length > key.size() && query.compare(start, key.size(), key) == 0) {
            const char* number = query.data() + start + key.size();
            char* parsed_end = nullptr;
            errno = 0;
            const long value = std::strtol(number, &parsed_end, 10);
            if (errno == 0 && parsed_end == query.data() + start + length && value >= INT32_MIN &&
                value <= INT32_MAX) {
                *out = static_cast<int32_t>(value);
                return true;
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

bool navigation_bfs(const contracts::QolWorldNavigation& navigation, int tx, int ty,
                    int* out_distance, bool* out_found, int* out_nearest_x,
                    int* out_nearest_y, int* out_nearest_distance) {
    constexpr int kWidth = static_cast<int>(contracts::kWorldNavWidth);
    constexpr int kHeight = static_cast<int>(contracts::kWorldNavHeight);
    constexpr int kCount = static_cast<int>(contracts::kWorldNavTileCount);
    if (!navigation.valid) return false;
    int sx = navigation.player_x >> 4;
    int sy = navigation.player_y >> 4;
    if (sx < 0 || sx >= kWidth || sy < 0 || sy >= kHeight) return false;
    auto index = [](int x, int y) { return y * static_cast<int>(contracts::kWorldNavWidth) + x; };
    if (navigation.blocked[index(sx, sy)] != 0) {
        for (int i = 0; i < 4; ++i) {
            constexpr int dx[] = {0, -1, 0, 1};
            constexpr int dy[] = {1, 0, -1, 0};
            const int nx = sx + dx[i];
            const int ny = sy + dy[i];
            if (nx >= 0 && nx < kWidth && ny >= 0 && ny < kHeight &&
                navigation.blocked[index(nx, ny)] == 0) {
                sx = nx;
                sy = ny;
                break;
            }
        }
    }

    std::array<int, kCount> previous{};
    std::array<int, kCount> depth{};
    std::array<int, kCount> queue{};
    previous.fill(-1);
    const int start = index(sx, sy);
    previous[start] = start;
    int head = 0;
    int tail = 0;
    queue[tail++] = start;
    const bool target_in = tx >= 0 && tx < kWidth && ty >= 0 && ty < kHeight;
    const int target = target_in ? index(tx, ty) : -1;
    int best_manhattan = INT32_MAX;
    int best_tile = -1;
    int best_depth = -1;
    int found_tile = -1;
    constexpr int dx[] = {0, -1, 0, 1};
    constexpr int dy[] = {1, 0, -1, 0};
    while (head < tail) {
        const int current = queue[head++];
        const int cx = current % kWidth;
        const int cy = current / kWidth;
        const int current_depth = depth[current];
        if (target_in) {
            const int manhattan = std::abs(cx - tx) + std::abs(cy - ty);
            if (manhattan < best_manhattan) {
                best_manhattan = manhattan;
                best_tile = current;
                best_depth = current_depth;
            }
        }
        if (current == target) {
            found_tile = current;
            break;
        }
        for (int i = 0; i < 4; ++i) {
            const int nx = cx + dx[i];
            const int ny = cy + dy[i];
            if (nx < 0 || nx >= kWidth || ny < 0 || ny >= kHeight) continue;
            const int next = index(nx, ny);
            if (previous[next] != -1 || navigation.blocked[next] != 0 ||
                navigation.unit_blocks[next] != 0) {
                continue;
            }
            previous[next] = current;
            depth[next] = current_depth + 1;
            queue[tail++] = next;
        }
    }
    const int end_tile = found_tile != -1 ? found_tile : best_tile;
    if (end_tile == -1) return false;
    *out_found = found_tile != -1;
    *out_distance = depth[end_tile];
    *out_nearest_x = best_tile == -1 ? -1 : best_tile % kWidth;
    *out_nearest_y = best_tile == -1 ? -1 : best_tile / kWidth;
    *out_nearest_distance = best_depth;
    return true;
}

QolResult query_handler(const contracts::HttpReq* request, contracts::HttpRes* response, void* ctx) {
    if (request == nullptr || response == nullptr || ctx == nullptr) {
        return response_error(response, 400, "bad request");
    }
    WorldSnapshot snapshot{};
    if (!g_snapshot.read(snapshot)) return response_error(response, 503, "snapshot not ready");
    if (snapshot.state == SnapshotState::kNotInGame) {
        return response_body(response, snapshot.error, snapshot.error_size);
    }
    if (snapshot.state != SnapshotState::kReady) return response_error(response, 503, "snapshot not ready");

    const Query query = *static_cast<const Query*>(ctx);
    switch (query) {
        case Query::kId: {
            std::string body = "{\"map_id\":" + std::to_string(snapshot.map_id);
            if (snapshot.map_name_size != 0) {
                body += ",\"id_name\":\"";
                append_json_string(body, snapshot.map_name, snapshot.map_name_size);
                body += "\"";
            }
            body += "}";
            return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
        }
        case Query::kExits:
            return response_body(response, snapshot.exits, snapshot.exits_size);
        case Query::kUnits:
            return response_body(response, snapshot.units, snapshot.units_size);
        case Query::kEnemies:
            return response_body(response, snapshot.enemies, snapshot.enemies_size);
        case Query::kInteractives:
            return response_body(response, snapshot.interactives, snapshot.interactives_size);
        case Query::kDrops:
            return response_body(response, snapshot.drops, snapshot.drops_size);
        case Query::kDistance: {
            int32_t tx = 0;
            int32_t ty = 0;
            if (!parse_query_int(*request, "tx", &tx) || !parse_query_int(*request, "ty", &ty)) {
                return response_error(response, 400, "invalid distance query");
            }
            int distance = -1;
            bool found = false;
            int nearest_x = -1;
            int nearest_y = -1;
            int nearest_distance = -1;
            const bool navigable = navigation_bfs(snapshot.navigation, tx >> 4, ty >> 4,
                                                  &distance, &found, &nearest_x, &nearest_y,
                                                  &nearest_distance);
            std::string body = "{\"target\":{\"x\":" + std::to_string(tx) +
                               ",\"y\":" + std::to_string(ty) + "},\"start\":{\"x\":" +
                               std::to_string(snapshot.navigation.player_x) +
                               ",\"y\":" + std::to_string(snapshot.navigation.player_y) + "}";
            if (!navigable) {
                body += ",\"found\":false,\"distance\":-1,\"nearest\":null}";
            } else {
                body += ",\"found\":" + std::string(found ? "true" : "false") +
                        ",\"distance\":" + std::to_string(distance);
                if (nearest_x >= 0) {
                    body += ",\"nearest\":{\"x\":" + std::to_string(nearest_x * 16 + 8) +
                            ",\"y\":" + std::to_string(nearest_y * 16 + 8) +
                            ",\"distance\":" + std::to_string(nearest_distance) + "}";
                } else {
                    body += ",\"nearest\":null";
                }
                body += "}";
            }
            return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
        }
    }
    return response_error(response, 500, "unknown world query");
}

const contracts::HttpQueryHandlerDescriptor kHttpQueries[] = {
    {kRoutes[0].method, kRoutes[0].path, kPluginId, &query_handler,
     const_cast<Query*>(&kQueries[0])},
    {kRoutes[1].method, kRoutes[1].path, kPluginId, &query_handler,
     const_cast<Query*>(&kQueries[1])},
    {kRoutes[2].method, kRoutes[2].path, kPluginId, &query_handler,
     const_cast<Query*>(&kQueries[2])},
    {kRoutes[3].method, kRoutes[3].path, kPluginId, &query_handler,
     const_cast<Query*>(&kQueries[3])},
    {kRoutes[4].method, kRoutes[4].path, kPluginId, &query_handler,
     const_cast<Query*>(&kQueries[4])},
    {kRoutes[5].method, kRoutes[5].path, kPluginId, &query_handler,
     const_cast<Query*>(&kQueries[5])},
    {kRoutes[6].method, kRoutes[6].path, kPluginId, &query_handler,
     const_cast<Query*>(&kQueries[6])},
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
    WorldSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    if (ctx.struct_size < contracts::PLUGIN_CONTEXT_PORTS_STRUCT_SIZE ||
        ctx.sdk_major != contracts::kHostSdkMajor || g_sdk == nullptr || ctx.ports == nullptr ||
        ctx.port_count != 1 || ctx.ports[0] == nullptr) {
        g_sdk = nullptr;
        return 1;
    }
    g_port = static_cast<const contracts::QolWorldPort*>(ctx.ports[0]);
    if (!contracts::satisfies(g_port->abi_version, {1, 1}) || g_port->read.state == nullptr ||
        g_port->read.game_state_gate == nullptr || g_port->read.units_json == nullptr ||
        g_port->read.navigation == nullptr || g_port->read.in_world == nullptr) {
        g_port = nullptr;
        g_sdk = nullptr;
        return 1;
    }
    log_message(g_sdk, "[world_facts] provider install");
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    stop_frame_task(ctx.sdk);
    log_message(ctx.sdk, "[world_facts] provider deactivate");
    g_port = nullptr;
    g_sdk = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    WorldSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    if (g_frame_task == 0 && g_sdk != nullptr && g_sdk->scheduler.add != nullptr) {
        uint64_t task = 0;
        if (contracts::succeeded(g_sdk->scheduler.add(
                1, &frame_tick, nullptr, kSnapshotIntervalFrames, 0, &task))) {
            g_frame_task = task;
        } else {
            log_message(ctx.sdk, "[world_facts] snapshot scheduler unavailable");
        }
    }
    log_message(ctx.sdk, "[world_facts] provider enabled: capability published");
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    stop_frame_task(ctx.sdk);
    WorldSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    log_message(ctx.sdk, "[world_facts] provider disabled: capability withdrawn");
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

}  // namespace qol::features::world_facts
