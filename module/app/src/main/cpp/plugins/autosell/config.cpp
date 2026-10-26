// autosell_config.cpp —— 自动出售运行时配置/状态存储（阶段 B）。

#include "config.h"

#include <atomic>
#include <cstdint>
#include <mutex>

#include "config_codec.h"

namespace {

std::mutex g_config_mtx;
autosell::Config g_config;  // 仅持 g_config_mtx 访问

std::atomic<int64_t> g_sold_total{0};
std::atomic<int64_t> g_failed_total{0};
std::atomic<int64_t> g_last_scan_frame{-1};
std::atomic<bool> g_host_installed{false};
std::atomic<int> g_store_slot{-1};
std::atomic<int> g_store_loaded_slot{-1};
std::atomic<bool> g_store_persisted{false};

}  // namespace

void autosell_set_runtime_config(const autosell::Config& config) {
    std::lock_guard<std::mutex> lock(g_config_mtx);
    g_config = config;
}

autosell::Config autosell_get_runtime_config() {
    std::lock_guard<std::mutex> lock(g_config_mtx);
    return g_config;
}

void autosell_note_scan(int64_t frame, int sold, int failed) {
    if (sold > 0) g_sold_total.fetch_add(sold, std::memory_order_relaxed);
    if (failed > 0) g_failed_total.fetch_add(failed, std::memory_order_relaxed);
    g_last_scan_frame.store(frame, std::memory_order_relaxed);
}

void autosell_set_host_installed(bool installed) {
    g_host_installed.store(installed, std::memory_order_release);
}

void autosell_set_store_status(int slot, int loaded_slot, bool persisted) {
    g_store_slot.store(slot, std::memory_order_relaxed);
    g_store_loaded_slot.store(loaded_slot, std::memory_order_relaxed);
    g_store_persisted.store(persisted, std::memory_order_relaxed);
}

autosell::StatusSnapshot autosell::autosell_status_snapshot() {
    StatusSnapshot snapshot{};
    snapshot.config = autosell_get_runtime_config();
    snapshot.sold = g_sold_total.load(std::memory_order_relaxed);
    snapshot.failed = g_failed_total.load(std::memory_order_relaxed);
    snapshot.last_scan_frame = g_last_scan_frame.load(std::memory_order_relaxed);
    snapshot.host_installed = g_host_installed.load(std::memory_order_acquire);
    snapshot.slot = g_store_slot.load(std::memory_order_relaxed);
    snapshot.loaded_slot = g_store_loaded_slot.load(std::memory_order_relaxed);
    snapshot.persisted = g_store_persisted.load(std::memory_order_relaxed);
    return snapshot;
}
