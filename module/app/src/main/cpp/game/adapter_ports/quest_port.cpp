#include "game/adapter_ports/quest_port.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "contracts/game_ports/ports.h"
#include "game/access/quest.h"
#include "game/state/quest_http.h"

namespace qol::game {
namespace {

using QolResult = ::QolResult;

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult not_ready() { return error_result(kQolErrNotReady, "not ready"); }
QolResult unsupported() { return error_result(kQolErrUnsupported, "unsupported"); }

QolResult read_state(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    return unsupported();
}

QolResult read_active_slot_count(uint32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    *out = quest_active_slot_count();
    return contracts::ok();
}

QolResult read_active_slot_id(uint32_t index, uint32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    uint16_t id = 0;
    if (!quest_active_slot_id(index, &id)) return not_ready();
    *out = id;
    return contracts::ok();
}

QolResult read_total_count(uint32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    *out = quest_total_count();
    return contracts::ok();
}

QolResult read_quest_state(uint32_t quest_id, int32_t* out, bool complete) {
    if (out == nullptr || quest_id > UINT16_MAX) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    if (complete) {
        *out = quest_is_complete(static_cast<uint16_t>(quest_id)) ? 1 : 0;
        return contracts::ok();
    }
    const int32_t value = quest_state(static_cast<uint16_t>(quest_id));
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_quest_value(uint32_t quest_id, int32_t* out) {
    return read_quest_state(quest_id, out, false);
}

QolResult read_quest_complete(uint32_t quest_id, int32_t* out) {
    return read_quest_state(quest_id, out, true);
}

QolResult write_quest_state(uint32_t quest_id, int32_t state) {
    if (quest_id > UINT16_MAX) return error_result(kQolErrInvalidArgument, "invalid argument");
    return quest_set_state(static_cast<uint16_t>(quest_id), state)
               ? contracts::ok()
               : not_ready();
}

// 退出任务并把迁移前的结果 JSON 写入 out（实现在 legacy 聚合 TU，见 game/state/quest_http.h）。
QolResult write_quest_quit_result(int32_t quest_id, char* out, uint32_t capacity,
                                  uint32_t* out_size) {
    if (out == nullptr || out_size == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    *out_size = 0;
    const std::string body = build_quest_quit_result_json(quest_id);
    if (body.size() > capacity) return error_result(kQolErrBlocked, "result buffer too small");
    std::memcpy(out, body.data(), body.size());
    *out_size = static_cast<uint32_t>(body.size());
    return contracts::ok();
}

// 启动期下发的静态 enrichment 片段：只在启动期写入，运行期只读。片段是 JSON 对象正文，
// 由 Kotlin 用与迁移前相同的 JSONObject 序列化产生，保证字段顺序与转义逐字节一致。
constexpr int32_t kFragmentEndpointCount = 3;
std::unordered_map<uint32_t, std::string> g_quest_fragments[kFragmentEndpointCount];
std::mutex g_quest_fragment_mutex;
std::atomic<bool> g_quest_fragments_ready{false};

QolResult read_static_ready(int32_t* out_ready) {
    if (out_ready == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    *out_ready = g_quest_fragments_ready.load() ? 1 : 0;
    return contracts::ok();
}

QolResult read_fragment(int32_t endpoint, uint32_t quest_id, const char** out_fragment) {
    if (out_fragment == nullptr || endpoint < 0 || endpoint >= kFragmentEndpointCount) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    *out_fragment = nullptr;
    std::lock_guard<std::mutex> lock(g_quest_fragment_mutex);
    const auto& table = g_quest_fragments[endpoint];
    const auto it = table.find(quest_id);
    if (it == table.end()) return error_result(kQolErrNotFound, "fragment not found");
    *out_fragment = it->second.c_str();
    return contracts::ok();
}

const contracts::QolQuestPort kQuestPort{
    contracts::kQolPortQuestVersion,
    {&read_state, &read_active_slot_count, &read_active_slot_id, &read_total_count,
     &read_quest_value, &read_quest_complete, &read_fragment, &read_static_ready},
    {&write_quest_state, &write_quest_quit_result},
};

}  // namespace

const contracts::QolQuestPort& quest_port() { return kQuestPort; }

// 解析 Kotlin 下发的行式负载：每行 `A|D|C\t<quest_id>\t<fragment>`；片段不含真实制表符与换行。
void quest_port_load_static_fragments(const char* payload) {
    if (payload == nullptr) return;
    std::unordered_map<uint32_t, std::string> parsed[kFragmentEndpointCount];
    const char* cursor = payload;
    while (*cursor != '\0') {
        const char* line_end = std::strchr(cursor, '\n');
        const size_t line_length = line_end == nullptr ? std::strlen(cursor)
                                                       : static_cast<size_t>(line_end - cursor);
        if (line_length >= 4 && cursor[1] == '\t') {
            int32_t endpoint = -1;
            switch (cursor[0]) {
                case 'A': endpoint = contracts::kQuestFragmentActive; break;
                case 'D': endpoint = contracts::kQuestFragmentDetails; break;
                case 'C': endpoint = contracts::kQuestFragmentCompleted; break;
                default: break;
            }
            const char* id_start = cursor + 2;
            const char* id_end = static_cast<const char*>(memchr(id_start, '\t', line_length - 2));
            if (endpoint >= 0 && id_end != nullptr) {
                const uint32_t quest_id =
                    static_cast<uint32_t>(std::strtoul(id_start, nullptr, 10));
                const char* value_start = id_end + 1;
                const size_t value_length = line_length - static_cast<size_t>(value_start - cursor);
                parsed[endpoint][quest_id] = std::string(value_start, value_length);
            }
        }
        if (line_end == nullptr) break;
        cursor = line_end + 1;
    }
    {
        std::lock_guard<std::mutex> lock(g_quest_fragment_mutex);
        for (int32_t endpoint = 0; endpoint < kFragmentEndpointCount; ++endpoint) {
            g_quest_fragments[endpoint] = std::move(parsed[endpoint]);
        }
    }
    g_quest_fragments_ready.store(true);
}

}  // namespace qol::game
