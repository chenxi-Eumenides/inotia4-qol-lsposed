#include "game/adapter_ports/quest_port.h"

#include <cstring>

#include "game/access/quest.h"
#include "contracts/game_ports/ports.h"

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

const contracts::QolQuestPort kQuestPort{
    contracts::kQolPortQuestVersion,
    {&read_state, &read_active_slot_count, &read_active_slot_id, &read_total_count,
     &read_quest_value, &read_quest_complete},
    {&write_quest_state},
};

}  // namespace

const contracts::QolQuestPort& quest_port() { return kQuestPort; }

}  // namespace qol::game
