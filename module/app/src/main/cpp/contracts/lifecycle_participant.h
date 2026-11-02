#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace qol::contracts {

constexpr uint32_t kLifecycleParticipantAbiVersion = 1;
constexpr uint32_t kLifecycleParticipantCapacity = 8;

struct QolLifecycleParticipant {
    uint32_t struct_size;
    uint32_t abi_version;
    int32_t (*available)();
    int32_t (*prepare_save_slot_load)();
    int32_t (*is_inventory_enter)(uintptr_t enter);
    int32_t (*prepare_main_menu)();
    int32_t (*inventory_state_id)();
};

struct QolLifecycleDispatch {
    int32_t (*prepare_save_slot_load)();
    int32_t (*is_inventory_enter)(uintptr_t enter);
    int32_t (*prepare_main_menu)();
    int32_t (*inventory_state_id)();
};

using QolLifecycleParticipantRegisterFn = int32_t (*)(
    const QolLifecycleParticipant* participant);

static_assert(std::is_standard_layout<QolLifecycleParticipant>::value,
              "QolLifecycleParticipant must remain standard layout");
static_assert(std::is_trivially_copyable<QolLifecycleParticipant>::value,
              "QolLifecycleParticipant must remain trivially copyable");
static_assert(std::is_standard_layout<QolLifecycleDispatch>::value,
              "QolLifecycleDispatch must remain standard layout");
static_assert(std::is_trivially_copyable<QolLifecycleDispatch>::value,
              "QolLifecycleDispatch must remain trivially copyable");

}  // namespace qol::contracts
