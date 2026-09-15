// autosell_config.cpp —— 自动出售运行时配置/状态存储（阶段 B）。

#include "feature/autosell/autosell_config.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

// 名字 <-> 位映射复用 store 头唯一表（autosell_special_to_array）。
#include "feature/autosell/autosell_store.h"

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

const char* json_bool(bool value) { return value ? "true" : "false"; }

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

std::string autosell_status_json() {
    const autosell::Config cfg = autosell_get_runtime_config();
    const int64_t sold = g_sold_total.load(std::memory_order_relaxed);
    const int64_t failed = g_failed_total.load(std::memory_order_relaxed);
    const int64_t frame = g_last_scan_frame.load(std::memory_order_relaxed);
    const bool host_installed = g_host_installed.load(std::memory_order_acquire);
    const int store_slot = g_store_slot.load(std::memory_order_relaxed);
    const int store_loaded_slot = g_store_loaded_slot.load(std::memory_order_relaxed);
    const bool store_persisted = g_store_persisted.load(std::memory_order_relaxed);

    // special 为类型名数组（v2 对外契约；位掩码仅内部实现，见 autosell_store.h 名字表）。
    const std::string special = autosell_special_to_array(cfg.special_mask);
    char buf[768];
    std::snprintf(buf, sizeof(buf),
                  "{\"enabled\":%s,\"rarity\":%d,\"enhance\":%d,\"socket\":%d,"
                  "\"gemTier\":%d,\"gemRange\":%d,\"special\":%s,"
                  "\"sold\":%lld,\"failed\":%lld,\"lastScanFrame\":%lld,"
                  "\"slot\":%d,\"loadedSlot\":%d,\"persisted\":%s,\"hostInstalled\":%s}",
                  json_bool(cfg.enabled), cfg.rarity, cfg.enhance, cfg.socket, cfg.gem_tier,
                  cfg.gem_range, special.c_str(),
                  static_cast<long long>(sold), static_cast<long long>(failed),
                  static_cast<long long>(frame), store_slot,
                  store_loaded_slot, json_bool(store_persisted), json_bool(host_installed));
    return std::string(buf);
}
