// 自动出售扫描与主线程逐帧 tick。

#include "scan.h"

#include "../../contracts/events.h"
#include "../../contracts/extension_bag.h"
#include "../../contracts/inventory_action.h"
#include "config.h"
#include "log.h"
#include "plugin.h"
#include "read_only.h"
#include "store.h"
#include "view.h"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace {

constexpr int kLastPhysicalBag = 4;      // 原版任务袋 5 排除
constexpr int kLogicalBagScanFirst = 6;  // = kExtensionLogicalBagFirst
constexpr int kLogicalBagScanCount = 5;  // = kBagCount
constexpr int kSlotCount = 16;

std::mutex g_scan_mtx;
std::mutex g_task_mtx;
uint64_t g_task = 0;
uint64_t g_save_enter_subscription = 0;
uint64_t g_save_exit_subscription = 0;
std::atomic<bool> g_global_enabled{false};
std::atomic<bool> g_save_active{false};
std::atomic<int> g_save_slot{-1};

const qol::contracts::ExtensionBagApi* extension_bag_api() {
    const auto* sdk = qol::features::autosell::host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    const QolResult result = sdk->capabilities.require(
        qol::contracts::kExtensionBagCapabilityId,
        QolVersionRange{static_cast<uint16_t>(qol::contracts::kExtensionBagVersionMajor),
                        static_cast<uint16_t>(qol::contracts::kExtensionBagVersionMinor)},
        &iface);
    if (result.ok != kQolOk || iface == nullptr) return nullptr;
    return static_cast<const qol::contracts::ExtensionBagApi*>(iface);
}

const qol::contracts::InventoryActionApi* inventory_action_api() {
    const auto* sdk = qol::features::autosell::host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    const QolResult result = sdk->capabilities.require(
        qol::contracts::kInventoryActionCapabilityId,
        QolVersionRange{qol::contracts::kInventoryActionVersionMajor,
                        qol::contracts::kInventoryActionVersionMinor},
        &iface);
    if (result.ok != kQolOk || iface == nullptr) return nullptr;
    return static_cast<const qol::contracts::InventoryActionApi*>(iface);
}

struct ScanStats {
    int sold = 0;
    int failed = 0;
};

struct PhysicalScanCtx {
    const autosell::Config* cfg;
    ScanStats* stats;
};

bool scan_gates_ok() {
    int32_t world_state = -1;
    if (!qol::features::autosell::read_only::world_state(&world_state) || world_state != 5) {
        return false;
    }
    int32_t state_gate = 0;
    if (!qol::features::autosell::read_only::game_state_gate(&state_gate) || state_gate == 0) {
        return false;
    }
    const auto* eb = extension_bag_api();
    return eb == nullptr || eb->module_view_installed == nullptr || !eb->module_view_installed();
}

void process_ref(const qol::features::autosell::read_only::InventoryItemRef& ref,
                 const autosell::Config& cfg, ScanStats* stats) {
    autosell::ItemView view;
    if (!autosell_build_view(ref, &view) || !autosell::should_sell(view, cfg)) return;

    int32_t no_sell = 0;
    if (qol::features::autosell::read_only::item_is_no_sell(ref.category, &no_sell) &&
        no_sell != 0) {
        return;
    }

    const auto* actions = inventory_action_api();
    if (actions == nullptr || actions->sell == nullptr) return;
    const qol::contracts::InventorySellResult result =
        actions->sell(ref.bag, ref.slot, ref.native_item, "autosell");
    if (result.status == qol::contracts::InventorySellStatus::kOk) {
        ++stats->sold;
        return;
    }
    if (result.status == qol::contracts::InventorySellStatus::kStale ||
        result.status == qol::contracts::InventorySellStatus::kEmptySlot ||
        result.status == qol::contracts::InventorySellStatus::kNoNativeItem) {
        return;
    }
    ++stats->failed;
    AUTOSELL_LOG_WARN("sell failed bag=%d slot=%d category=%d status=%d", ref.bag, ref.slot,
                      ref.category, static_cast<int>(result.status));
}

void scan_body(const autosell::Config& cfg, int64_t frame) {
    ScanStats stats;
    PhysicalScanCtx physical{&cfg, &stats};
    (void)qol::features::autosell::read_only::for_each_bag_slot(
        [](const qol::features::autosell::read_only::InventoryItemRef* source, void* ctx) -> int32_t {
            if (source == nullptr || source->bag > kLastPhysicalBag) return 0;
            auto* scan = static_cast<PhysicalScanCtx*>(ctx);
            auto ref = *source;
            qol::features::autosell::read_only::refresh_item_count(&ref);
            process_ref(ref, *scan->cfg, scan->stats);
            return 0;
        },
        &physical);

    const auto* eb = extension_bag_api();
    if (eb != nullptr && eb->is_logical_bag != nullptr) {
        for (int bag = kLogicalBagScanFirst; bag < kLogicalBagScanFirst + kLogicalBagScanCount;
             ++bag) {
            if (!eb->is_logical_bag(bag)) continue;
            for (int slot = 0; slot < kSlotCount; ++slot) {
                qol::features::autosell::read_only::InventoryItemRef ref{};
                const QolResult result =
                    qol::features::autosell::read_only::inventory_item_ref_at(bag, slot, &ref);
                if (result.ok != kQolOk ||
                    ref.kind != static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kProvider) ||
                    ref.native_item == nullptr || ref.category <= 0) {
                    continue;
                }
                qol::features::autosell::read_only::refresh_item_count(&ref);
                process_ref(ref, cfg, &stats);
            }
        }
    }

    autosell_note_scan(frame, stats.sold, stats.failed);
    if (stats.sold > 0 || stats.failed > 0) {
        AUTOSELL_LOG_INFO("scan frame=%lld sold=%d failed=%d", static_cast<long long>(frame),
                          stats.sold, stats.failed);
    }
}

int32_t autosell_tick(int64_t frame, void*) {
    if (!g_global_enabled.load(std::memory_order_acquire)) return 1;
    const autosell::Config cfg = autosell_get_runtime_config();
    if (!cfg.enabled || !scan_gates_ok()) return 1;
    std::lock_guard<std::mutex> lock(g_scan_mtx);
    scan_body(cfg, frame);
    return 1;
}

void remove_task_if_any() {
    std::lock_guard<std::mutex> lock(g_task_mtx);
    if (g_task == 0) return;
    const auto* sdk = qol::features::autosell::host_sdk();
    if (sdk != nullptr && sdk->scheduler.remove != nullptr) (void)sdk->scheduler.remove(g_task);
    g_task = 0;
    AUTOSELL_LOG_INFO("task removed");
}

void sync_task() {
    const bool want = g_global_enabled.load(std::memory_order_acquire) &&
                      g_save_active.load(std::memory_order_acquire) &&
                      autosell_get_runtime_config().enabled;
    if (!want) {
        remove_task_if_any();
        return;
    }
    std::lock_guard<std::mutex> lock(g_task_mtx);
    if (g_task != 0) return;
    const auto* sdk = qol::features::autosell::host_sdk();
    if (sdk == nullptr || sdk->scheduler.add == nullptr) return;
    uint64_t task = 0;
    const QolResult result = sdk->scheduler.add(0, &autosell_tick, nullptr,
                                                kAutoSellScanIntervalFrames, 0, &task);
    if (result.ok != kQolOk) return;
    g_task = task;
    AUTOSELL_LOG_INFO("task registered id=%llu", static_cast<unsigned long long>(g_task));
}

void autosell_on_save_enter(void*, const void* payload) {
    const auto* event = static_cast<const qol::contracts::SaveEnterEvent*>(payload);
    const int slot = event == nullptr ? -1 : event->slot;
    g_save_slot.store(slot, std::memory_order_release);
    autosell_store_ensure_loaded(slot);
    g_save_active.store(true, std::memory_order_release);
    AUTOSELL_LOG_INFO("save enter slot=%d; sync task", slot);
    sync_task();
}

void autosell_on_save_exit(void*, const void*) {
    g_save_active.store(false, std::memory_order_release);
    g_save_slot.store(-1, std::memory_order_release);
    AUTOSELL_LOG_INFO("save exit; remove task");
    remove_task_if_any();
}

}  // namespace

void autosell_apply_config(const autosell::Config& config) {
    const bool old_enabled = autosell_get_runtime_config().enabled;
    autosell_set_runtime_config(config);
    if (old_enabled == config.enabled) return;
    AUTOSELL_LOG_INFO("save master switch %d -> %d", old_enabled ? 1 : 0, config.enabled ? 1 : 0);
    if (config.enabled) sync_task();
    else remove_task_if_any();
}

void autosell_set_global_enabled(bool enabled) {
    g_global_enabled.store(enabled, std::memory_order_release);
    AUTOSELL_LOG_INFO("global enabled=%d save_active=%d", enabled ? 1 : 0,
                      g_save_active.load(std::memory_order_acquire) ? 1 : 0);
    if (enabled) sync_task();
    else remove_task_if_any();
}

void autosell_register_save_enter() {
    if (g_save_enter_subscription != 0) return;
    const auto* sdk = qol::features::autosell::host_sdk();
    if (sdk == nullptr || sdk->events.subscribe == nullptr) return;
    (void)sdk->events.subscribe(qol::contracts::kEventSaveEnter, &autosell_on_save_enter, nullptr,
                                &g_save_enter_subscription);
}

void autosell_register_save_exit() {
    if (g_save_exit_subscription != 0) return;
    const auto* sdk = qol::features::autosell::host_sdk();
    if (sdk == nullptr || sdk->events.subscribe == nullptr) return;
    (void)sdk->events.subscribe(qol::contracts::kEventSaveExit, &autosell_on_save_exit, nullptr,
                                &g_save_exit_subscription);
}

void autosell_unregister_save_callbacks() {
    const auto* sdk = qol::features::autosell::host_sdk();
    if (sdk != nullptr && sdk->events.unsubscribe != nullptr) {
        if (g_save_enter_subscription != 0) (void)sdk->events.unsubscribe(g_save_enter_subscription);
        if (g_save_exit_subscription != 0) (void)sdk->events.unsubscribe(g_save_exit_subscription);
    }
    g_save_enter_subscription = 0;
    g_save_exit_subscription = 0;
}

int autosell_active_save_slot() { return g_save_slot.load(std::memory_order_acquire); }
