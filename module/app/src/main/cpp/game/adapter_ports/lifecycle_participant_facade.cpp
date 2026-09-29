#include "game/adapter_ports/lifecycle_participant_facade.h"

namespace qol::game {
namespace {

contracts::QolLifecycleDispatch g_dispatch{};
bool g_bound = false;

}  // namespace

bool lifecycle_participant_bind_host(const contracts::QolLifecycleDispatch* dispatch) {
    if (dispatch == nullptr) return false;
    if (!g_bound) {
        g_dispatch = *dispatch;
        g_bound = true;
        return true;
    }
    return g_dispatch.prepare_save_slot_load == dispatch->prepare_save_slot_load &&
           g_dispatch.is_inventory_enter == dispatch->is_inventory_enter &&
           g_dispatch.prepare_main_menu == dispatch->prepare_main_menu &&
           g_dispatch.inventory_state_id == dispatch->inventory_state_id;
}

bool lifecycle_participant_prepare_save_slot_load() {
    return !g_bound || g_dispatch.prepare_save_slot_load == nullptr ||
           g_dispatch.prepare_save_slot_load() != 0;
}

bool lifecycle_participant_is_inventory_enter(uintptr_t enter) {
    return g_bound && g_dispatch.is_inventory_enter != nullptr &&
           g_dispatch.is_inventory_enter(enter) != 0;
}

bool lifecycle_participant_prepare_main_menu() {
    return !g_bound || g_dispatch.prepare_main_menu == nullptr ||
           g_dispatch.prepare_main_menu() != 0;
}

int32_t lifecycle_participant_inventory_state_id() {
    return !g_bound || g_dispatch.inventory_state_id == nullptr
               ? -1
               : g_dispatch.inventory_state_id();
}

}  // namespace qol::game
