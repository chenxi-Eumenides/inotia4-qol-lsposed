#include "game/adapter_ports/world_port.h"

#include <cstring>
#include <mutex>
#include <string>

#include "contracts/game_ports/ports.h"
#include "game/access/game_access.h"
#include "game/nav/game_nav.h"
#include "game/symbols/game_symbols.h"
#include "game/access/world.h"
#include "game/state/game_state.h"

namespace qol::game {
namespace {

using QolResult = ::QolResult;

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult not_ready() { return error_result(kQolErrNotReady, "not ready"); }

QolResult read_current_map_id(int32_t* out);

std::mutex g_map_exits_mutex;
std::string g_map_exits_json;

QolResult copy_json(const std::string& json, char* out, uint32_t capacity, uint32_t* out_size) {
    if (out == nullptr || out_size == nullptr || capacity == 0) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    if (json.size() >= capacity) return error_result(kQolErrBlocked, "json buffer too small");
    std::memcpy(out, json.data(), json.size());
    out[json.size()] = '\0';
    *out_size = static_cast<uint32_t>(json.size());
    return contracts::ok();
}

QolResult read_json(std::string (*builder)(), char* out, uint32_t capacity, uint32_t* out_size) {
    if (builder == nullptr) return error_result(kQolErrInvalidArgument, "invalid builder");
    return copy_json(builder(), out, capacity, out_size);
}

QolResult read_exits_json(char* out, uint32_t capacity, uint32_t* out_size) {
    int32_t map_id = -1;
    const QolResult map_result = read_current_map_id(&map_id);
    if (!contracts::succeeded(map_result)) return map_result;

    std::lock_guard<std::mutex> lock(g_map_exits_mutex);
    const std::string key = "\"m" + std::to_string(map_id) + "\"";
    const size_t map_start = g_map_exits_json.find(key);
    if (map_start == std::string::npos) return copy_json("{\"exits\":[]}", out, capacity, out_size);
    const size_t exits_key = g_map_exits_json.find("\"exits\"", map_start);
    const size_t array_start = g_map_exits_json.find('[', exits_key);
    if (exits_key == std::string::npos || array_start == std::string::npos) {
        return copy_json("{\"exits\":[]}", out, capacity, out_size);
    }
    size_t depth = 0;
    bool quoted = false;
    bool escaped = false;
    size_t array_end = std::string::npos;
    for (size_t i = array_start; i < g_map_exits_json.size(); ++i) {
        const char ch = g_map_exits_json[i];
        if (quoted) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                quoted = false;
            }
            continue;
        }
        if (ch == '"') {
            quoted = true;
        } else if (ch == '[') {
            ++depth;
        } else if (ch == ']' && --depth == 0) {
            array_end = i;
            break;
        }
    }
    if (array_end == std::string::npos) return copy_json("{\"exits\":[]}", out, capacity, out_size);
    return copy_json("{\"exits\":" + g_map_exits_json.substr(array_start, array_end - array_start + 1) + "}",
                     out, capacity, out_size);
}

QolResult read_navigation(contracts::QolWorldNavigation* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    std::memset(out, 0, sizeof(*out));
    if (!game_in_world()) return not_ready();
    void* hero = lead_member();
    const uint8_t* tiles = nav_tiles();
    if (hero == nullptr || tiles == nullptr) return not_ready();

    auto* hero_bytes = reinterpret_cast<uint8_t*>(hero);
    out->player_x = *reinterpret_cast<int16_t*>(hero_bytes + C_POS_X);
    out->player_y = *reinterpret_cast<int16_t*>(hero_bytes + C_POS_Y);
    for (uint32_t i = 0; i < contracts::kWorldNavTileCount; ++i) {
        const int tx = static_cast<int>(i % contracts::kWorldNavWidth);
        const int ty = static_cast<int>(i / contracts::kWorldNavWidth);
        out->blocked[i] = nav_blocked(tiles, tx, ty) ? 1 : 0;
    }
    bool unit_blocks[contracts::kWorldNavTileCount];
    nav_unit_blocks(unit_blocks);
    for (uint32_t i = 0; i < contracts::kWorldNavTileCount; ++i) {
        out->unit_blocks[i] = unit_blocks[i] ? 1 : 0;
    }
    out->valid = 1;
    return contracts::ok();
}

QolResult read_state(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    const int value = world_state();
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_game_state_gate(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    const int value = world_game_state_gate();
    if (value < 0) return not_ready();
    *out = value == 0 ? 1 : 0;
    return contracts::ok();
}

QolResult read_map_record_count(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    const int value = world_map_record_count();
    if (value <= 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_current_map_id(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    if (!world_ready()) return not_ready();
    *out = world_current_map_id();
    return contracts::ok();
}

QolResult read_map_name(int32_t map_id, const char** out) {
    if (map_id < 0 || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    *out = world_map_name(map_id);
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult read_active_player_direction(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    const int value = world_active_player_direction();
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_tutorial_state(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    *out = tutorial_state();
    return contracts::ok();
}

QolResult read_in_world(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    *out = game_in_world() ? 1 : 0;
    return contracts::ok();
}

QolResult read_units_json(char* out, uint32_t capacity, uint32_t* out_size) {
    return read_json(&qol::game::build_units_json, out, capacity, out_size);
}

QolResult read_enemies_json(char* out, uint32_t capacity, uint32_t* out_size) {
    return read_json(&qol::game::build_enemies_json, out, capacity, out_size);
}

QolResult read_interactives_json(char* out, uint32_t capacity, uint32_t* out_size) {
    return read_json(&qol::game::build_interactives_json, out, capacity, out_size);
}

QolResult read_drops_json(char* out, uint32_t capacity, uint32_t* out_size) {
    return read_json(&qol::game::build_drops_json, out, capacity, out_size);
}

const contracts::QolWorldPort kWorldPort{
    contracts::kQolPortWorldVersion,
    {&read_state, &read_game_state_gate, &read_map_record_count, &read_current_map_id,
     &read_map_name, &read_active_player_direction, &read_tutorial_state,
     &read_exits_json, &read_units_json, &read_enemies_json, &read_interactives_json,
     &read_drops_json, &read_navigation, &read_in_world},
};

}  // namespace

const contracts::QolWorldPort& world_port() { return kWorldPort; }

void world_port_set_map_exits_json(const char* json) {
    std::lock_guard<std::mutex> lock(g_map_exits_mutex);
    g_map_exits_json = json == nullptr ? "{}" : json;
}

}  // namespace qol::game
