#include "game/adapter_ports/save_port.h"

#include <cstring>

#include "game/access/save.h"

namespace qol::game {
namespace {

using QolResult = ::QolResult;

[[gnu::used]] constexpr char kSavePortBuildMarker[] = "save_port:v1";

constexpr const char* kInvalidArgument = "invalid argument";
constexpr const char* kNotReady = "not ready";

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult not_ready() { return error_result(kQolErrNotReady, kNotReady); }

QolResult read_slot_state(int32_t slot, int32_t* out) {
    if (slot < 0 || out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    void* save = save_slot_struct(slot);
    if (save == nullptr) return not_ready();
    *out = save_slot_exists(save) ? 1 : 0;
    return contracts::ok();
}

QolResult read_slot_exists(int32_t slot, int32_t* out) {
    if (slot < 0 || out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (!save_slot_meta_deps_ready()) return not_ready();
    save_slots_refresh_if_stale();
    void* save = save_slot_struct(slot);
    if (save == nullptr) return not_ready();
    *out = save_slot_exists(save) ? 1 : 0;
    return contracts::ok();
}

QolResult read_slot_hero_index(int32_t slot, int32_t* out) {
    if (slot < 0 || out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (!save_slot_meta_deps_ready()) return not_ready();
    save_slots_refresh_if_stale();
    void* save = save_slot_struct(slot);
    if (save == nullptr) return not_ready();
    *out = save_slot_hero_index(save);
    return contracts::ok();
}

QolResult read_hero_level(int32_t slot, int32_t* out) {
    if (slot < 0 || out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (!save_slot_meta_deps_ready()) return not_ready();
    save_slots_refresh_if_stale();
    void* save = save_slot_struct(slot);
    if (save == nullptr) return not_ready();
    void* hero = save_slot_hero(save);
    if (hero == nullptr) return not_ready();
    *out = save_hero_level(hero);
    return contracts::ok();
}

QolResult read_hero_class(int32_t slot, int32_t* out) {
    if (slot < 0 || out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (!save_slot_meta_deps_ready()) return not_ready();
    save_slots_refresh_if_stale();
    void* save = save_slot_struct(slot);
    if (save == nullptr) return not_ready();
    void* hero = save_slot_hero(save);
    if (hero == nullptr) return not_ready();
    *out = save_hero_class(hero);
    return contracts::ok();
}

QolResult read_load_data(int32_t slot, void** out_data, int32_t* out_size) {
    if (slot < 0 || out_data == nullptr || out_size == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_data = nullptr;
    *out_size = 0;
    if (!save_load_deps_ready()) return not_ready();
    if (save_load_data(slot, out_data, reinterpret_cast<int*>(out_size)) != 1 || *out_data == nullptr) {
        return error_result(kQolErrInternal, "save load failed");
    }
    return contracts::ok();
}

QolResult read_hub_key(const char** out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out = save_hub_get_key();
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult read_map_id(int32_t slot, int32_t* out) {
    if (slot < 0 || out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (!save_slot_meta_deps_ready()) return not_ready();
    save_slots_refresh_if_stale();
    void* save = save_slot_struct(slot);
    if (save == nullptr) return not_ready();
    *out = save_slot_map_id(save);
    return contracts::ok();
}

QolResult read_current_slot(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out = save_current_slot();
    return *out < 0 ? not_ready() : contracts::ok();
}

QolResult read_is_ok(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    const int value = save_is_ok();
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

const contracts::QolSavePort kSavePort{
    contracts::kQolPortSaveVersion,
    {&read_slot_state, &read_slot_exists, &read_slot_hero_index, &read_hero_level,
     &read_hero_class, &read_load_data, &read_hub_key, &read_map_id, &read_current_slot,
     &read_is_ok},
};

}  // namespace

const contracts::QolSavePort& save_port() { return kSavePort; }

}  // namespace qol::game
