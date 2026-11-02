#include "scan.h"

#include "store.h"

#include <atomic>

namespace {

std::atomic<bool> g_session_active{false};
std::atomic<int> g_session_slot{-1};
std::atomic<uint64_t> g_session_epoch{0};

}  // namespace

void autosell_scan_session_loaded(int slot, uint64_t epoch) {
    if (slot < 0 || epoch == 0 || !autosell_store_session_matches(slot)) return;
    g_session_slot.store(slot, std::memory_order_release);
    g_session_epoch.store(epoch, std::memory_order_release);
    g_session_active.store(true, std::memory_order_release);
    autosell_scan_sync_after_session_change();
}

void autosell_scan_session_closed(int slot, uint64_t epoch) {
    const int active_slot = g_session_slot.load(std::memory_order_acquire);
    const uint64_t active_epoch = g_session_epoch.load(std::memory_order_acquire);
    if (slot >= 0 && (slot != active_slot || (epoch != 0 && epoch != active_epoch))) return;
    g_session_active.store(false, std::memory_order_release);
    g_session_slot.store(-1, std::memory_order_release);
    g_session_epoch.store(0, std::memory_order_release);
    autosell_scan_sync_after_session_change();
}

bool autosell_scan_save_enter(int slot) {
    if (!autosell_store_session_matches(slot)) {
        autosell_store_reject_session();
        autosell_scan_session_closed(-1, 0);
        return false;
    }
    const uint64_t epoch = autosell_store_session_epoch(slot);
    if (epoch == 0) return false;
    g_session_slot.store(slot, std::memory_order_release);
    g_session_epoch.store(epoch, std::memory_order_release);
    g_session_active.store(true, std::memory_order_release);
    autosell_scan_sync_after_session_change();
    return true;
}

bool autosell_scan_session_active(int slot) {
    return g_session_active.load(std::memory_order_acquire) &&
           g_session_slot.load(std::memory_order_acquire) == slot &&
           autosell_store_session_matches(slot);
}

int autosell_scan_session_slot() {
    return g_session_active.load(std::memory_order_acquire)
               ? g_session_slot.load(std::memory_order_acquire)
               : -1;
}
