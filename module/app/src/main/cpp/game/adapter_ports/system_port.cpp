#include "game/adapter_ports/system_port.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "contracts/game_ports/ports.h"
#include "game/state/system_snapshot.h"

namespace qol::game {
namespace {

struct SystemIdentity {
    std::string version;
    std::string package;
};

std::shared_ptr<const SystemIdentity> g_identity;
// 身份只下发一次，静态副本供不持有 shared_ptr 的仅读访问使用。
char g_version[128] = {};
char g_package[256] = {};

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
    const std::shared_ptr<const SystemIdentity> identity =
        std::atomic_load_explicit(&g_identity, std::memory_order_acquire);
    // 身份未下发前不产出含 version/package 的 body；快照与帧计数与其无关，始终可产出。
    const bool identity_ready = identity != nullptr && !identity->version.empty() &&
                                !identity->package.empty();
    switch (static_cast<contracts::QolSystemJsonKind>(kind)) {
        case contracts::QolSystemJsonKind::kSnapshot:
            return write_body(build_system_snapshot_json(), out, capacity, out_size);
        case contracts::QolSystemJsonKind::kGame:
            if (!identity_ready) {
                return contracts::error(kQolErrNotReady, "system identity not ready", 28);
            }
            return write_body(
                build_system_game_json(build_system_info_json(identity->version, identity->package)), out,
                capacity, out_size);
        case contracts::QolSystemJsonKind::kInfo:
            if (!identity_ready) {
                return contracts::error(kQolErrNotReady, "system identity not ready", 28);
            }
            return write_body(build_system_info_json(identity->version, identity->package), out,
                              capacity, out_size);
        case contracts::QolSystemJsonKind::kFrame:
            return write_body(build_system_frame_json(), out, capacity, out_size);
    }
    return contracts::error(kQolErrInvalidArgument, "unknown system json kind", 25);
}

QolResult events_json(char* out, uint32_t capacity, uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    return write_body(build_system_events_json(), out, capacity, out_size);
}

const contracts::QolSystemPortReadApi kRead{&body_json, &events_json};
const contracts::QolSystemPort kPort{contracts::kQolPortSystemVersion, kRead};

}  // namespace

const contracts::QolSystemPort& system_port() { return kPort; }

void system_port_set_identity(const char* version, const char* package) {
    auto identity = std::make_shared<SystemIdentity>();
    identity->version = version != nullptr ? version : "";
    identity->package = package != nullptr ? package : "";
    const std::string& value = identity->version;
    const std::size_t length =
        value.size() < sizeof(g_version) - 1 ? value.size() : sizeof(g_version) - 1;
    std::memcpy(g_version, value.data(), length);
    g_version[length] = '\0';
    const std::string& package_value = identity->package;
    const std::size_t package_length = package_value.size() < sizeof(g_package) - 1
        ? package_value.size() : sizeof(g_package) - 1;
    std::memcpy(g_package, package_value.data(), package_length);
    g_package[package_length] = '\0';
    std::atomic_store_explicit(&g_identity,
                               std::shared_ptr<const SystemIdentity>(std::move(identity)),
                               std::memory_order_release);
}

const char* system_port_version() { return g_version[0] != '\0' ? g_version : nullptr; }

const char* system_port_package() { return g_package[0] != '\0' ? g_package : nullptr; }

}  // namespace qol::game
