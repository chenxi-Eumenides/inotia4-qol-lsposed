#include "game/adapter_ports/save_port.h"

#include <cstddef>
#include <cstring>

#include "game/access/save.h"

namespace qol::game {
namespace {

using QolResult = ::QolResult;

[[gnu::used]] constexpr char kSavePortBuildMarker[] = "save_port:v1.1";

constexpr const char* kInvalidArgument = "invalid argument";
constexpr const char* kNotReady = "not ready";
constexpr const char* kSaveLoadFailed = "save load failed";
constexpr const char* kSaveDataInvalid = "save data invalid";
constexpr int32_t kSaveSlotCount = 3;

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

uint16_t read_u16_le(const uint8_t* data) {
    return static_cast<uint16_t>(static_cast<uint32_t>(data[0]) |
                                 (static_cast<uint32_t>(data[1]) << 8));
}

uint64_t read_u64_le(const uint8_t* data) {
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(value); ++i) {
        value |= static_cast<uint64_t>(data[i]) << (i * 8);
    }
    return value;
}

QolResult read_slot_times(int32_t slot, uint64_t* out_creation_time_ms,
                          uint64_t* out_save_time_ms) {
    if (slot < 0 || slot >= kSaveSlotCount || out_creation_time_ms == nullptr ||
        out_save_time_ms == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!save_load_deps_ready()) return not_ready();

    void* buffer = nullptr;
    int length = 0;
    const int loaded = save_load_data(slot, &buffer, &length);
    if (loaded != 1 || buffer == nullptr) {
        if (buffer != nullptr) save_mem_free(buffer);
        return error_result(kQolErrInternal, kSaveLoadFailed);
    }
    if (length < 10) {
        save_mem_free(buffer);
        return error_result(kQolErrInternal, kSaveDataInvalid);
    }

    const auto* plain = static_cast<const uint8_t*>(buffer);
    const size_t plain_size = static_cast<size_t>(length);
    const size_t block0 = 8u + read_u16_le(plain + 8);
    constexpr size_t kBlock0TimeBytes = 0x0d + sizeof(uint64_t);
    if (block0 > plain_size || plain_size - block0 < kBlock0TimeBytes) {
        save_mem_free(buffer);
        return error_result(kQolErrInternal, kSaveDataInvalid);
    }

    const uint64_t creation_time_ms = read_u64_le(plain + block0 + 0x05);
    const uint64_t save_time_ms = read_u64_le(plain + block0 + 0x0d);
    save_mem_free(buffer);
    *out_creation_time_ms = creation_time_ms;
    *out_save_time_ms = save_time_ms;
    return contracts::ok();
}

const contracts::QolSavePort kSavePort{
    contracts::kQolPortSaveVersion,
    {&read_slot_state, &read_slot_exists, &read_slot_hero_index, &read_hero_level,
      &read_hero_class, &read_load_data, &read_hub_key, &read_map_id, &read_current_slot,
      &read_is_ok, &read_slot_times},
};

}  // namespace

const contracts::QolSavePort& save_port() { return kSavePort; }

}  // namespace qol::game
