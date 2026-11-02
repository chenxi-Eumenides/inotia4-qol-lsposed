#include "host/lifecycle_participants.h"

#include <array>
#include <cstring>
#include <mutex>

namespace qol::host {
namespace {

std::array<contracts::QolLifecycleParticipant,
           contracts::kLifecycleParticipantCapacity> g_participants{};
uint32_t g_participant_count = 0;
std::mutex g_participant_mutex;

std::array<contracts::QolLifecycleParticipant,
           contracts::kLifecycleParticipantCapacity> participant_snapshot(uint32_t* count) {
    std::lock_guard<std::mutex> lock(g_participant_mutex);
    *count = g_participant_count;
    return g_participants;
}

bool participant_available(const contracts::QolLifecycleParticipant& participant) {
    return participant.available != nullptr && participant.available() != 0;
}

int32_t dispatch_prepare_save_slot_load() {
    uint32_t count = 0;
    const auto participants = participant_snapshot(&count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& participant = participants[i];
        if (!participant_available(participant) || participant.prepare_save_slot_load == nullptr)
            continue;
        if (participant.prepare_save_slot_load() == 0) return 0;
    }
    return 1;
}

int32_t dispatch_is_inventory_enter(uintptr_t enter) {
    uint32_t count = 0;
    const auto participants = participant_snapshot(&count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& participant = participants[i];
        if (participant_available(participant) && participant.is_inventory_enter != nullptr &&
            participant.is_inventory_enter(enter) != 0)
            return 1;
    }
    return 0;
}

int32_t dispatch_prepare_main_menu() {
    uint32_t count = 0;
    const auto participants = participant_snapshot(&count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& participant = participants[i];
        if (!participant_available(participant) || participant.prepare_main_menu == nullptr)
            continue;
        if (participant.prepare_main_menu() == 0) return 0;
    }
    return 1;
}

int32_t dispatch_inventory_state_id() {
    uint32_t count = 0;
    const auto participants = participant_snapshot(&count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& participant = participants[i];
        if (!participant_available(participant) || participant.inventory_state_id == nullptr)
            continue;
        const int32_t state_id = participant.inventory_state_id();
        if (state_id >= 0) return state_id;
    }
    return -1;
}

const contracts::QolLifecycleDispatch kDispatch{
    &dispatch_prepare_save_slot_load,
    &dispatch_is_inventory_enter,
    &dispatch_prepare_main_menu,
    &dispatch_inventory_state_id,
};

}  // namespace

int32_t host_lifecycle_participant_register(
    const contracts::QolLifecycleParticipant* participant) {
    if (participant == nullptr || participant->struct_size < sizeof(*participant) ||
        participant->abi_version != contracts::kLifecycleParticipantAbiVersion ||
        participant->available == nullptr)
        return 0;

    std::lock_guard<std::mutex> lock(g_participant_mutex);
    if (g_participant_count >= g_participants.size()) return 0;
    for (uint32_t i = 0; i < g_participant_count; ++i) {
        const auto& registered = g_participants[i];
        if (registered.available == participant->available &&
            registered.prepare_save_slot_load == participant->prepare_save_slot_load &&
            registered.is_inventory_enter == participant->is_inventory_enter &&
            registered.prepare_main_menu == participant->prepare_main_menu &&
            registered.inventory_state_id == participant->inventory_state_id)
            return 0;
    }
    std::memcpy(&g_participants[g_participant_count], participant, sizeof(*participant));
    ++g_participant_count;
    return 1;
}

const contracts::QolLifecycleDispatch* host_lifecycle_dispatch() { return &kDispatch; }

}  // namespace qol::host
