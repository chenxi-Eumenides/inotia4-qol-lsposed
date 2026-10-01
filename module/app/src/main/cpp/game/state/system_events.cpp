#include "game/state/system_snapshot.h"

namespace qol::game {
namespace {

void emit(std::string& out, bool& first, const char* type, int role, int64_t old_value,
          int64_t new_value) {
    if (!first) out += ",";
    first = false;
    out += "{\"type\":\"" + std::string(type) + "\"";
    if (role >= 0) out += ",\"role\":" + std::to_string(role);
    out += ",\"old\":" + std::to_string(old_value);
    out += ",\"new\":" + std::to_string(new_value);
    out += "}";
}

}  // namespace

std::string build_events_json(const SystemEventsSnapshot& current,
                              SystemEventsSnapshot& previous, bool& has_previous) {
    std::string json = "{\"events\":[";
    bool first = true;
    if (!has_previous) {
        has_previous = true;
        previous = current;
        return json + "]}";
    }
    if (current.money >= 0 && previous.money >= 0 && current.money != previous.money)
        emit(json, first, "money", -1, previous.money, current.money);
    if (current.inv_count >= 0 && previous.inv_count >= 0 &&
        current.inv_count != previous.inv_count)
        emit(json, first, "inventory", -1, previous.inv_count, current.inv_count);
    if (current.x >= 0 && previous.x >= 0 &&
        (current.x != previous.x || current.y != previous.y))
        emit(json, first, "move", -1, 0, 0);
    for (int role = 0; role < 3; ++role) {
        if (current.hp[role] >= 0 && previous.hp[role] >= 0 &&
            current.hp[role] != previous.hp[role])
            emit(json, first, "hp", role, previous.hp[role], current.hp[role]);
        if (current.mp[role] >= 0 && previous.mp[role] >= 0 &&
            current.mp[role] != previous.mp[role])
            emit(json, first, "mp", role, previous.mp[role], current.mp[role]);
        if (current.level[role] >= 0 && previous.level[role] >= 0 &&
            current.level[role] != previous.level[role])
            emit(json, first, "level_up", role, previous.level[role], current.level[role]);
        if (current.exp[role] >= 0 && previous.exp[role] >= 0 &&
            current.exp[role] != previous.exp[role])
            emit(json, first, "exp", role, previous.exp[role], current.exp[role]);
    }
    previous = current;
    return json + "]}";
}

}  // namespace qol::game
