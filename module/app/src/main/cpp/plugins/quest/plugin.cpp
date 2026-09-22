#include "plugin.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "contracts/game_ports/quest_port.h"
#include "contracts/http.h"
#include "contracts/world_facts.h"
#include "feature.h"
#include "snapshot.h"

namespace qol::features::quest {
namespace {

constexpr const char* kPluginId = "quest";
constexpr const char* kPluginVersion = "0.1.0";
constexpr uint32_t kSetStateHandlerId = 0x51535401;
// 活动任务槽上限与状态表遍历上限（与 game/access/quest.cpp、迁移前 native JSON 构造一致）。
constexpr uint32_t kMaxActiveSlots = 20;
constexpr uint32_t kMaxCompleted = 512;
// 主线程快照节拍：任务状态变化低频（接取/交付），15 帧（≈4Hz）足够且不逐帧遍历状态表。
constexpr int32_t kSnapshotIntervalFrames = 15;
constexpr char kNotInGameBody[] = "{\"error\":\"not in game\"}";

struct SetStatePayload {
    uint32_t quest_id;
    int32_t state;
};
static_assert(sizeof(SetStatePayload) <= contracts::kTypedCommandPayloadCapacity,
              "quest typed command payload exceeds capacity");

enum class Query : uint8_t { kComposite, kActive, kDetails, kCompleted };

constexpr contracts::HttpRoute kRoutes[] = {
    {contracts::HttpMethod::kGet, "/api/quest", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/quest/active", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/quest/details", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/quest/completed", kPluginId, 0, 1},
};

constexpr Query kQueries[] = {Query::kComposite, Query::kActive, Query::kDetails,
                              Query::kCompleted};

enum class SnapshotState : uint8_t { kUnavailable, kNotInGame, kReady };

struct QuestSnapshot {
    SnapshotState state = SnapshotState::kUnavailable;
    uint32_t active_count = 0;
    uint32_t active_ids[kMaxActiveSlots]{};
    int32_t active_states[kMaxActiveSlots]{};
    uint32_t completed_count = 0;
    uint32_t completed_ids[kMaxCompleted]{};
};

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;
const contracts::WorldFactsApi* g_world_facts = nullptr;
const contracts::QolQuestPort* g_quest_port = nullptr;
uint64_t g_snapshot_task = 0;
Snapshot<QuestSnapshot> g_snapshot;

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kWorldFactsCapabilityId,
     {contracts::kWorldFactsVersionMajor, contracts::kWorldFactsVersionMinor}},
    {contracts::kHttpRegistryCapabilityId, {contracts::kHttpRegistryVersion.major,
                                             contracts::kHttpRegistryVersion.minor}},
    {contracts::kHttpDispatchCapabilityId, {contracts::kHttpDispatchVersion.major,
                                             contracts::kHttpDispatchVersion.minor}},
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_QUEST, {1, 0}},
};

bool set_state_handler(void*, const uint8_t* payload, uint32_t size) {
    if (g_quest_port == nullptr || g_quest_port->write.set_state == nullptr ||
        payload == nullptr || size != sizeof(SetStatePayload)) {
        return false;
    }
    SetStatePayload command{};
    std::memcpy(&command, payload, sizeof(command));
    return g_quest_port->write.set_state(command.quest_id, command.state).ok == kQolOk;
}

void log_message(const contracts::QolHostSdk* sdk, const char* message) {
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    uint32_t length = 0;
    while (message[length] != '\0') ++length;
    (void)sdk->log.write(1, 1, kPluginId, message, length);
}

const char* fragment_for(int32_t endpoint, uint32_t quest_id) {
    if (g_quest_port == nullptr || g_quest_port->read.fragment == nullptr) return nullptr;
    const char* fragment = nullptr;
    if (!contracts::succeeded(g_quest_port->read.fragment(endpoint, quest_id, &fragment))) {
        return nullptr;
    }
    return fragment;
}

void append_active_entry(std::string& out, uint32_t slot, uint32_t quest_id, bool deliverable,
                         int32_t endpoint) {
    out += "{\"slot\":" + std::to_string(slot) + ",\"quest_id\":" + std::to_string(quest_id) +
           ",\"deliverable\":" + (deliverable ? "true" : "false");
    const char* fragment = fragment_for(endpoint, quest_id);
    if (fragment != nullptr && fragment[0] != '\0') {
        out += ",";
        out += fragment;
    }
    out += "}";
}

void append_active_array(std::string& out, const QuestSnapshot& snapshot, int32_t endpoint) {
    out += "[";
    for (uint32_t i = 0; i < snapshot.active_count; ++i) {
        if (i > 0) out += ",";
        append_active_entry(out, i, snapshot.active_ids[i], snapshot.active_states[i] == 2,
                            endpoint);
    }
    out += "]";
}

void append_completed_array(std::string& out, const QuestSnapshot& snapshot) {
    out += "[";
    for (uint32_t i = 0; i < snapshot.completed_count; ++i) {
        if (i > 0) out += ",";
        const uint32_t quest_id = snapshot.completed_ids[i];
        out += "{\"quest_id\":" + std::to_string(quest_id);
        const char* fragment = fragment_for(contracts::kQuestFragmentCompleted, quest_id);
        if (fragment != nullptr && fragment[0] != '\0') {
            out += ",";
            out += fragment;
        }
        out += "}";
    }
    out += "]";
}

void update_snapshot() {
    if (g_quest_port == nullptr || g_quest_port->read.active_slot_count == nullptr ||
        g_quest_port->read.active_slot_id == nullptr ||
        g_quest_port->read.quest_state == nullptr || g_quest_port->read.total_count == nullptr ||
        g_quest_port->read.static_ready == nullptr) {
        return;
    }
    if (g_world_facts == nullptr || g_world_facts->state == nullptr) return;

    int32_t state = 0;
    if (!contracts::succeeded(g_world_facts->state(&state))) return;
    QuestSnapshot next{};
    if (state != 5) {
        next.state = SnapshotState::kNotInGame;
        g_snapshot.publish(next);
        return;
    }
    // 静态片段表未下发时不发布，避免输出缺少静态字段的不完整响应。
    int32_t ready = 0;
    if (!contracts::succeeded(g_quest_port->read.static_ready(&ready)) || ready == 0) return;

    uint32_t count = 0;
    if (!contracts::succeeded(g_quest_port->read.active_slot_count(&count))) return;
    if (count > kMaxActiveSlots) count = kMaxActiveSlots;
    next.active_count = count;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t quest_id = 0;
        if (!contracts::succeeded(g_quest_port->read.active_slot_id(i, &quest_id))) {
            next.active_count = i;
            break;
        }
        next.active_ids[i] = quest_id;
        int32_t quest_state = 0;
        next.active_states[i] = contracts::succeeded(g_quest_port->read.quest_state(quest_id, &quest_state))
                                    ? quest_state
                                    : 0;
    }

    uint32_t total = 0;
    if (!contracts::succeeded(g_quest_port->read.total_count(&total))) return;
    if (total > kMaxCompleted) total = kMaxCompleted;
    for (uint32_t quest_id = 0; quest_id < total; ++quest_id) {
        int32_t quest_state = 0;
        if (!contracts::succeeded(g_quest_port->read.quest_state(quest_id, &quest_state))) continue;
        if (quest_state != 3) continue;
        next.completed_ids[next.completed_count++] = quest_id;
    }

    next.state = SnapshotState::kReady;
    g_snapshot.publish(next);
}

int32_t snapshot_tick(int64_t, void*) {
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

QolResult response_body(contracts::HttpRes* response, const std::string& body) {
    if (response == nullptr || response->body == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid response", 16);
    }
    if (response->body_capacity < body.size()) {
        return response_error(response, 500, "http response buffer too small");
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = 200;
    return contracts::ok();
}

QolResult query_handler(const contracts::HttpReq* request, contracts::HttpRes* response, void* ctx) {
    if (request == nullptr || response == nullptr || ctx == nullptr) {
        return response_error(response, 400, "bad request");
    }
    QuestSnapshot snapshot{};
    if (!g_snapshot.read(snapshot)) return response_error(response, 503, "snapshot not ready");
    if (snapshot.state == SnapshotState::kNotInGame) {
        if (response->body == nullptr ||
            response->body_capacity < sizeof(kNotInGameBody) - 1) {
            return response_error(response, 500, "http response buffer too small");
        }
        std::memcpy(response->body, kNotInGameBody, sizeof(kNotInGameBody) - 1);
        response->body_size = static_cast<uint32_t>(sizeof(kNotInGameBody) - 1);
        response->status = 200;
        return contracts::ok();
    }
    if (snapshot.state != SnapshotState::kReady) {
        return response_error(response, 503, "snapshot not ready");
    }

    const Query query = *static_cast<const Query*>(ctx);
    std::string body;
    switch (query) {
        case Query::kActive:
            append_active_array(body, snapshot, contracts::kQuestFragmentActive);
            break;
        case Query::kDetails:
            append_active_array(body, snapshot, contracts::kQuestFragmentDetails);
            break;
        case Query::kCompleted:
            append_completed_array(body, snapshot);
            break;
        case Query::kComposite:
            body += "{\"active\":";
            append_active_array(body, snapshot, contracts::kQuestFragmentActive);
            body += ",\"details\":";
            append_active_array(body, snapshot, contracts::kQuestFragmentDetails);
            body += ",\"completed\":";
            append_completed_array(body, snapshot);
            body += "}";
            break;
    }
    if (query != Query::kComposite) {
        body = "{\"quests\":" + body + "}";
    }
    return response_body(response, body);
}

const contracts::HttpQueryHandlerDescriptor kHttpQueries[] = {
    {kRoutes[0].method, kRoutes[0].path, kPluginId, &query_handler, const_cast<Query*>(&kQueries[0])},
    {kRoutes[1].method, kRoutes[1].path, kPluginId, &query_handler, const_cast<Query*>(&kQueries[1])},
    {kRoutes[2].method, kRoutes[2].path, kPluginId, &query_handler, const_cast<Query*>(&kQueries[2])},
    {kRoutes[3].method, kRoutes[3].path, kPluginId, &query_handler, const_cast<Query*>(&kQueries[3])},
};

void stop_snapshot_task(const contracts::QolHostSdk* sdk) {
    if (g_snapshot_task != 0 && sdk != nullptr && sdk->scheduler.remove != nullptr) {
        (void)sdk->scheduler.remove(g_snapshot_task);
    }
    g_snapshot_task = 0;
}

void start_snapshot_task() {
    if (g_snapshot_task != 0 || g_sdk == nullptr || g_sdk->scheduler.add == nullptr) return;
    uint64_t task = 0;
    if (contracts::succeeded(g_sdk->scheduler.add(1, &snapshot_tick, nullptr,
                                                  kSnapshotIntervalFrames, 0, &task))) {
        g_snapshot_task = task;
    }
}

int plugin_install(contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_game = ctx.game;
    g_world_facts = nullptr;
    g_quest_port = nullptr;
    g_snapshot_task = 0;
    QuestSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    if (ctx.struct_size < contracts::PLUGIN_CONTEXT_PORTS_STRUCT_SIZE ||
        ctx.sdk_major != contracts::kHostSdkMajor || g_sdk == nullptr || g_game == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        return 1;
    }
    if (ctx.ports == nullptr || ctx.port_count != 1 || ctx.ports[0] == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        return 1;
    }
    g_quest_port = static_cast<const contracts::QolQuestPort*>(ctx.ports[0]);
    if (!contracts::satisfies(g_quest_port->abi_version, {1, 0}) ||
        g_quest_port->read.active_slot_count == nullptr ||
        g_quest_port->read.fragment == nullptr || g_quest_port->read.static_ready == nullptr ||
        g_quest_port->write.set_state == nullptr ||
        g_sdk->commands.register_typed_handler == nullptr ||
        g_sdk->commands.register_typed_handler(kSetStateHandlerId, &set_state_handler, nullptr).ok !=
            kQolOk) {
        g_quest_port = nullptr;
        g_sdk = nullptr;
        g_game = nullptr;
        return 1;
    }
    const void* world_facts_iface = nullptr;
    if (g_sdk->capabilities.require == nullptr ||
        !contracts::succeeded(g_sdk->capabilities.require(
            contracts::kWorldFactsCapabilityId,
            {contracts::kWorldFactsVersionMajor, contracts::kWorldFactsVersionMinor},
            &world_facts_iface)) ||
        world_facts_iface == nullptr) {
        (void)g_sdk->commands.unregister_typed_handler(kSetStateHandlerId);
        g_sdk = nullptr;
        g_game = nullptr;
        g_quest_port = nullptr;
        return 1;
    }
    g_world_facts = static_cast<const contracts::WorldFactsApi*>(world_facts_iface);
    set_gate(true);
    start_resync_task();
    start_snapshot_task();
    log_message(g_sdk, "[quest] owner installed: http queries registered");
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    if (g_sdk != nullptr && g_sdk->commands.unregister_typed_handler != nullptr) {
        (void)g_sdk->commands.unregister_typed_handler(kSetStateHandlerId);
    }
    set_gate(false);
    cancel_resync_task();
    stop_snapshot_task(g_sdk);
    QuestSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    g_world_facts = nullptr;
    g_quest_port = nullptr;
    g_game = nullptr;
    g_sdk = nullptr;
}

// 零资源、无门禁键：启停只切业务门禁，不做资源操作（§3.3）。
void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    set_gate(true);
    start_resync_task();
    QuestSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    start_snapshot_task();
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    set_gate(false);
    cancel_resync_task();
    stop_snapshot_task(g_sdk);
    QuestSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = gate_enabled();
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    3, kRequires,  // requires=world.facts + http.registry + http.dispatch
    0, nullptr,   // optional_requires
    0, nullptr,   // provides
    0, nullptr,   // resources（零资源）
    0, nullptr,   // jni
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,      // on_resource_ready（无资源）
    kPorts,
    1,
    kHttpQueries,
    static_cast<uint32_t>(sizeof(kHttpQueries) / sizeof(kHttpQueries[0])),
};

}  // namespace

const contracts::QolHostSdk* host_sdk() { return g_sdk; }

const contracts::QolGameHost* game_host() { return g_game; }

const contracts::WorldFactsApi* world_facts() { return g_world_facts; }

const contracts::QolQuestPort* quest_port() { return g_quest_port; }

bool post_set_state(uint32_t quest_id, int32_t state) {
    if (g_sdk == nullptr || g_sdk->commands.post_typed == nullptr) return false;
    const SetStatePayload payload{quest_id, state};
    contracts::QolCommandTicket ticket = 0;
    return g_sdk->commands.post_typed(kSetStateHandlerId, &payload, sizeof(payload), &ticket).ok ==
           kQolOk;
}

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::quest
