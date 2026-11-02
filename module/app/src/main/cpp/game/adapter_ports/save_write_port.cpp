#include "game/adapter_ports/save_write_port.h"

#include "contracts/game_ports/ports.h"
#include "game/state/save_http.h"

namespace qol::game {
namespace {

QolResult write_adapter(int32_t kind, int32_t slot, int32_t class_idx, uint32_t attempt, char* out,
                        uint32_t capacity, uint32_t* out_size) {
    const int32_t outcome =
        build_save_write_result_json(kind, slot, class_idx, attempt, out, capacity, out_size);
    if (outcome == kSaveWriteNotReady) {
        // 未就绪：交由结果处理器请求延迟结算（票据保持 pending，不提前报告 done）。
        return contracts::error(kQolErrNotReady, "save write not ready", 21);
    }
    if (outcome == kSaveWriteFailed) {
        // 业务失败时 out 已携带原错误体；返回错误让结果处理器把票据置 failed（结果仍可回取）。
        return contracts::error(kQolErrInternal, "save write failed", 17);
    }
    return contracts::ok();
}

const contracts::QolSaveWritePort kPort{contracts::kQolPortSaveWriteVersion, {&write_adapter}};

}  // namespace

const contracts::QolSaveWritePort& save_write_port() { return kPort; }

}  // namespace qol::game
