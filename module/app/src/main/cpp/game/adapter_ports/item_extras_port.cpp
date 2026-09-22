#include "game/adapter_ports/item_extras_port.h"

#include <cstring>

#include "contracts/game_ports/ports.h"
#include "game/access/attr_range_ui.h"

namespace qol::game {
namespace {

using QolResult = ::QolResult;

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult read_ability_level(const void* item, int32_t* out) {
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    const int value = attr_ability_level(item);
    if (value < 0) return error_result(kQolErrNotReady, "not ready");
    *out = value;
    return contracts::ok();
}

const contracts::QolItemExtrasPort kPort{
    contracts::kQolPortItemExtrasVersion,
    {&read_ability_level},
};

}  // namespace

const contracts::QolItemExtrasPort& item_extras_port() { return kPort; }

}  // namespace qol::game
