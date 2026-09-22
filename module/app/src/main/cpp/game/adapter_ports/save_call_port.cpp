#include "game/adapter_ports/save_call_port.h"

#include <cstring>

#include "game/access/save.h"

namespace qol::game {
namespace {

using QolResult = ::QolResult;

[[gnu::used]] constexpr char kSaveCallPortBuildMarker[] = "save_call_port:v1";

constexpr const char* kInvalidArgument = "invalid argument";
constexpr const char* kNotReady = "not ready";

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult not_ready() { return error_result(kQolErrNotReady, kNotReady); }

QolResult free_data(void* data) {
    if (data == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    save_mem_free(data);
    return contracts::ok();
}

QolResult encrypt(void* data, int32_t size, int32_t mode, const char* key, int32_t* out_result) {
    if (data == nullptr || size < 0 || (mode != 0 && mode != 1) || key == nullptr ||
        out_result == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!save_encrypt_available()) return not_ready();
    *out_result = save_encrypt_process2(data, size, mode, key);
    return contracts::ok();
}

QolResult mark_stale() {
    save_slots_mark_stale();
    return contracts::ok();
}

const contracts::QolSaveCallPort kSaveCallPort{
    contracts::kQolPortSaveCallVersion,
    {&free_data, &encrypt, &mark_stale},
};

}  // namespace

const contracts::QolSaveCallPort& save_call_port() { return kSaveCallPort; }

}  // namespace qol::game
