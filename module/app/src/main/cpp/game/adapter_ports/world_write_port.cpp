#include "game/adapter_ports/world_write_port.h"

#include "contracts/game_ports/ports.h"
#include "game/state/world_http.h"

namespace qol::game {
namespace {

QolResult write_adapter(int32_t kind, int32_t a, int32_t b, int32_t c, int32_t d, char* out,
                        uint32_t capacity, uint32_t* out_size) {
    const int32_t outcome = build_world_write_result_json(kind, a, b, c, d, out, capacity, out_size);
    if (outcome == kWorldWriteFailed) {
        // 业务失败时 out 已携带原错误体；返回错误让结果处理器把票据置 failed（结果仍可回取）。
        return contracts::error(kQolErrInternal, "world write failed", 18);
    }
    return contracts::ok();
}

const contracts::QolWorldWritePort kPort{contracts::kQolPortWorldWriteVersion, {&write_adapter}};

}  // namespace

const contracts::QolWorldWritePort& world_write_port() { return kPort; }

}  // namespace qol::game
