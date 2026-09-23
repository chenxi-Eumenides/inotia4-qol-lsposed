#include "game/adapter_ports/system_port.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "contracts/game_ports/ports.h"
#include "game/state/system_snapshot.h"

namespace qol::game {
namespace {

std::string g_version;
std::string g_package;

// system_facts 帧任务在主线程调用 body_json；实现在 legacy/system（可访问 infra/legacy
// 数据源），本适配端口只依赖 game 层声明，保持层依赖方向。
QolResult write_body(const std::string& body, char* out, uint32_t capacity, uint32_t* out_size) {
    if (out == nullptr || out_size == nullptr || body.size() >= capacity) {
        return contracts::error(kQolErrBlocked, "http response buffer too small", 31);
    }
    std::memcpy(out, body.data(), body.size());
    out[body.size()] = '\0';
    *out_size = static_cast<uint32_t>(body.size());
    return contracts::ok();
}

QolResult body_json(int32_t kind, char* out, uint32_t capacity, uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    // 身份未下发前不产出含 version/package 的 body；快照与帧计数与其无关，始终可产出。
    const bool identity_ready = !g_version.empty() && !g_package.empty();
    switch (static_cast<contracts::QolSystemJsonKind>(kind)) {
        case contracts::QolSystemJsonKind::kSnapshot:
            return write_body(build_system_snapshot_json(), out, capacity, out_size);
        case contracts::QolSystemJsonKind::kGame:
            if (!identity_ready) {
                return contracts::error(kQolErrNotReady, "system identity not ready", 28);
            }
            return write_body(
                build_system_game_json(build_system_info_json(g_version, g_package)), out,
                capacity, out_size);
        case contracts::QolSystemJsonKind::kInfo:
            if (!identity_ready) {
                return contracts::error(kQolErrNotReady, "system identity not ready", 28);
            }
            return write_body(build_system_info_json(g_version, g_package), out, capacity, out_size);
        case contracts::QolSystemJsonKind::kFrame:
            return write_body(build_system_frame_json(), out, capacity, out_size);
    }
    return contracts::error(kQolErrInvalidArgument, "unknown system json kind", 25);
}

const contracts::QolSystemPortReadApi kRead{&body_json};
const contracts::QolSystemPort kPort{contracts::kQolPortSystemVersion, kRead};

}  // namespace

const contracts::QolSystemPort& system_port() { return kPort; }

void system_port_set_identity(const char* version, const char* package) {
    g_version = version != nullptr ? version : "";
    g_package = package != nullptr ? package : "";
}

}  // namespace qol::game
