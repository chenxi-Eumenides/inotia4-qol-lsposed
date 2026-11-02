#include "game/adapter_ports/choice_write_port.h"

#include "contracts/game_ports/ports.h"
#include "game/state/choice_http.h"

namespace qol::game {
namespace {

QolResult write_adapter(int32_t kind, const char* action, int32_t index, char* out, uint32_t capacity,
                        uint32_t* out_size) {
    const int32_t outcome = build_choice_write_result_json(kind, action, index, out, capacity, out_size);
    if (outcome == kChoiceWriteFailed) {
        // 业务失败时 out 已携带原错误体；返回错误让结果处理器把票据置 failed（结果仍可回取）。
        return contracts::error(kQolErrInternal, "choice write failed", 19);
    }
    return contracts::ok();
}

const contracts::QolChoiceWritePort kPort{contracts::kQolPortChoiceWriteVersion, {&write_adapter}};

}  // namespace

const contracts::QolChoiceWritePort& choice_write_port() { return kPort; }

}  // namespace qol::game
