#include "game/adapter_ports/ui_write_port.h"

#include "contracts/game_ports/ports.h"
#include "game/state/ui_http.h"

namespace qol::game {
namespace {

QolResult write_adapter(int32_t kind, const char* panel, char* out, uint32_t capacity,
                        uint32_t* out_size) {
    const int32_t outcome = build_ui_write_result_json(kind, panel, out, capacity, out_size);
    if (outcome == kUiWriteFailed) {
        // 业务失败时 out 已携带原错误体；返回错误让结果处理器把票据置 failed（结果仍可回取）。
        return contracts::error(kQolErrInternal, "ui write failed", 15);
    }
    return contracts::ok();
}

const contracts::QolUiWritePort kPort{contracts::kQolPortUiWriteVersion, {&write_adapter}};

}  // namespace

const contracts::QolUiWritePort& ui_write_port() { return kPort; }

}  // namespace qol::game
