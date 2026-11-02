#include "plugins/extension_bag/runtime/session.h"

#include <atomic>

namespace {

std::atomic<qol::contracts::QolSlotDataHandle> g_slot_data_handle{0};
std::atomic<uint64_t> g_admitted_epoch{0};
std::atomic<bool> g_session_blocked{true};

}  // namespace

void extension_bag_session_set_blocked(bool blocked) {
    g_session_blocked.store(blocked, std::memory_order_release);
}

void extension_bag_session_set_admitted_epoch(uint64_t epoch) {
    g_admitted_epoch.store(epoch, std::memory_order_release);
}

void extension_bag_session_set_handle(qol::contracts::QolSlotDataHandle handle) {
    g_slot_data_handle.store(handle, std::memory_order_release);
}

qol::contracts::QolSlotDataHandle extension_bag_session_exchange_handle() {
    return g_slot_data_handle.exchange(0, std::memory_order_acq_rel);
}

qol::contracts::QolSlotDataHandle extension_bag_session_handle() {
    return g_slot_data_handle.load(std::memory_order_acquire);
}

uint64_t extension_bag_session_epoch() {
    return g_admitted_epoch.load(std::memory_order_acquire);
}

bool extension_bag_session_admitted_locked() {
    return !g_session_blocked.load(std::memory_order_acquire) &&
           g_slot_data_handle.load(std::memory_order_acquire) != 0 &&
           g_admitted_epoch.load(std::memory_order_acquire) != 0;
}
