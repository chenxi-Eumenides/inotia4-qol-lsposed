#include "game/adapter_ports/item_write_port.h"

#include <cstdint>

#include "contracts/game_ports/ports.h"
#include "game/state/item_http.h"

namespace qol::game {
namespace {

QolResult item_write_adapter(const contracts::QolItemWriteArgs* args, char* out, uint32_t capacity,
                             uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (args == nullptr || out == nullptr || out_size == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid item write args", 24);
    }
    const int32_t code = build_item_write_result_json(*args, out, capacity, out_size);
    if (code == kItemWriteOk) return contracts::ok();
    return contracts::error(kQolErrInternal, "item write failed", 17);
}

const contracts::QolItemWritePortReadApi kRead{&item_write_adapter};
const contracts::QolItemWritePort kPort{contracts::kQolPortItemWriteVersion, kRead};

}  // namespace

const contracts::QolItemWritePort& item_write_port() { return kPort; }

}  // namespace qol::game
