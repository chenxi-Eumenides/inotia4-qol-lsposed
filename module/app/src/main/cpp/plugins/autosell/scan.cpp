// 自动出售扫描与主线程逐帧 tick。

#include "scan.h"

#include "../../contracts/bag.h"
#include "../../contracts/events.h"
#include "../../contracts/inventory_action.h"
#include "config.h"
#include "log.h"
#include "plugin.h"
#include "read_only.h"
#include "scan_gates.h"
#include "store.h"
#include "view.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace {

constexpr int kLastPhysicalBag = 4;      // 原版任务袋 5 排除
std::mutex g_task_mtx;
uint64_t g_task = 0;
uint64_t g_save_enter_subscription = 0;
uint64_t g_save_exit_subscription = 0;
std::atomic<bool> g_global_enabled{false};
std::atomic<bool> g_config_enabled{false};
std::atomic<uint64_t> g_scan_cancel_epoch{0};
enum class InventoryActionState : int { kAvailable, kCapabilityUnavailable, kSellUnavailable };
InventoryActionState g_inventory_action_state = InventoryActionState::kAvailable;
std::atomic<int> g_last_blocked_gate{-1};
std::unordered_map<int32_t, int32_t> g_last_bag_source_error_by_space{};
bool g_has_physical_scan_error = false;
int g_last_physical_scan_error = 0;
bool g_has_variant_scan_error = false;
int g_last_variant_scan_error = 0;
std::unordered_set<int> g_logged_view_reasons{};
std::unordered_set<int> g_logged_no_rule_categories{};
std::unordered_set<int> g_logged_no_sell_categories{};
std::unordered_set<int> g_logged_sell_statuses{};

struct InventoryActionLookup {
    QolResult result = qol::contracts::error(kQolErrNotReady, "not ready", 9);
    const qol::contracts::InventoryActionApi* api = nullptr;
};

InventoryActionLookup inventory_action_api_lookup() {
    InventoryActionLookup lookup{};
    const auto* sdk = qol::features::autosell::host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return lookup;
    const void* iface = nullptr;
    lookup.result = sdk->capabilities.require(
        qol::contracts::kInventoryActionCapabilityId,
        QolVersionRange{qol::contracts::kInventoryActionVersionMajor,
                        qol::contracts::kInventoryActionVersionMinor},
        &iface);
    if (lookup.result.ok == kQolOk && iface != nullptr) {
        lookup.api = static_cast<const qol::contracts::InventoryActionApi*>(iface);
    }
    return lookup;
}

void note_inventory_action_state(InventoryActionState state) {
    if (state == g_inventory_action_state) return;
    g_inventory_action_state = state;
    if (state != InventoryActionState::kAvailable)
        AUTOSELL_LOG_WARN("inventory.action sell unavailable");
}

struct ScanStats {
    int sold = 0;
    int failed = 0;
    int refs = 0;
    int view_skipped = 0;
    int no_rule = 0;
    int no_sell = 0;
    int stale = 0;
    int empty = 0;
    int no_handle = 0;
};

void log_blocked_gate_once(int64_t frame, ScanGateReason reason) {
    const int code = static_cast<int>(reason);
    const int previous = g_last_blocked_gate.exchange(code, std::memory_order_relaxed);
    if (previous != code) {
        AUTOSELL_LOG_INFO("scan gate frame=%lld result=blocked reason=%s",
                          static_cast<long long>(frame), autosell_scan_gate_name(reason));
    }
}

struct ScanRound {
    uint64_t epoch;
    int64_t frame;
    bool cancellation_logged = false;
};

bool scan_round_cancelled(ScanRound& round) {
    const uint64_t current_epoch = g_scan_cancel_epoch.load(std::memory_order_acquire);
    if (current_epoch == round.epoch) return false;
    if (!round.cancellation_logged) {
        round.cancellation_logged = true;
        AUTOSELL_LOG_INFO("scan cancelled frame=%lld epoch=%llu current_epoch=%llu",
                          static_cast<long long>(round.frame),
                          static_cast<unsigned long long>(round.epoch),
                          static_cast<unsigned long long>(current_epoch));
    }
    return true;
}

struct UiBusyScan {
    bool busy = false;
};

void inspect_ui_layer(const qol::contracts::UiLayerInfo* layer, void* user) {
    if (layer == nullptr || user == nullptr) return;
    const auto type = static_cast<qol::contracts::UiLayerType>(layer->type);
    if (type != qol::contracts::UiLayerType::kPanel &&
        type != qol::contracts::UiLayerType::kDialog) {
        return;
    }
    // 自动出售门禁只拦背包页，其他 UI 打开时不阻塞扫描。
    if (layer->id == nullptr || std::strcmp(layer->id, "inventory") != 0) return;
    static_cast<UiBusyScan*>(user)->busy = true;
}

bool ui_busy() {
    const auto* api = qol::features::autosell::ui_state();
    if (api == nullptr || api->read_layers == nullptr) return true;
    UiBusyScan scan{};
    const QolResult result = api->read_layers(&inspect_ui_layer, &scan);
    return !qol::contracts::succeeded(result) || scan.busy;
}

bool scan_world_state(int32_t* out) {
    return qol::features::autosell::read_only::world_state(out);
}

bool scan_game_state_gate(int32_t* out) {
    return qol::features::autosell::read_only::game_state_gate(out);
}

bool scan_panel_active(int32_t* out) {
    const auto* sdk = qol::features::autosell::host_sdk();
    if (sdk == nullptr || sdk->panels.active == nullptr || out == nullptr) return false;
    return sdk->panels.active("autosell", out).ok == kQolOk;
}

bool scan_gates_ok(ScanGateReason* out_reason) {
    const ScanGateDeps deps{&scan_world_state, &scan_game_state_gate, &scan_panel_active, &ui_busy};
    return autosell_scan_gates_ok(deps, out_reason);
}

const qol::contracts::AttributeQualityApi* quality_api_provider() {
    const auto* sdk = qol::features::autosell::host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    const QolResult result = sdk->capabilities.require(
        qol::contracts::kAttributeQualityCapabilityId,
        QolVersionRange{qol::contracts::kAttributeQualityVersionMajor,
                        qol::contracts::kAttributeQualityVersionMinor},
        &iface);
    if (result.ok != kQolOk || iface == nullptr) return nullptr;
    return static_cast<const qol::contracts::AttributeQualityApi*>(iface);
}

void process_ref(const qol::features::autosell::read_only::InventoryItemRef& ref,
                 const autosell::Config& cfg, ScanStats* stats, ScanRound& round) {
    ++stats->refs;
    autosell::ItemView view;
    const AutosellViewBuildReason view_reason = autosell_build_view(
        ref, &view, nullptr, &quality_api_provider);
    if (view_reason != AutosellViewBuildReason::kBuilt) {
        ++stats->view_skipped;
        const int reason = static_cast<int>(view_reason);
        if (g_logged_view_reasons.insert(reason).second) {
            AUTOSELL_LOG_WARN("scan item view skipped reason=%d category=%d space_id=%d bag=%d slot=%d",
                              reason, ref.category, ref.space_id, ref.bag, ref.slot);
        }
        return;
    }

    const auto decision = autosell::should_sell_with_reason(view, cfg);
    if (decision != autosell::DecisionReason::kRuleMatch) {
        ++stats->no_rule;
        if (g_logged_no_rule_categories.insert(ref.category).second) {
            AUTOSELL_LOG_WARN(
                "scan no-rule category=%d space=%d bag=%d slot=%d rarity=%d equip=%d jewel=%d "
                "backpack=%d badge=%d dice=%d cfg(rarity=%d,enhance=%d,socket=%d,gemTier=%d,"
                "gemRange=%d,backpack=%d,badge=%d,dice=%d)",
                ref.category, ref.space_id, ref.bag, ref.slot, view.rarity,
                view.is_equip ? 1 : 0, view.is_jewel ? 1 : 0, view.is_backpack ? 1 : 0,
                view.badge_rank, view.dice_tier, cfg.rarity, cfg.enhance, cfg.socket,
                cfg.gem_tier, cfg.gem_range, cfg.backpack_tier, cfg.badge_tier, cfg.dice_tier);
        }
        return;
    }

    int32_t no_sell = 0;
    if (qol::features::autosell::read_only::item_is_no_sell(ref.category, &no_sell) &&
        no_sell != 0) {
        ++stats->no_sell;
        if (g_logged_no_sell_categories.insert(ref.category).second) {
            AUTOSELL_LOG_WARN("scan no-sell category=%d space=%d bag=%d slot=%d", ref.category,
                              ref.space_id, ref.bag, ref.slot);
        }
        return;
    }

    const InventoryActionLookup action_lookup = inventory_action_api_lookup();
    const auto* actions = action_lookup.api;
    if (actions == nullptr) {
        note_inventory_action_state(InventoryActionState::kCapabilityUnavailable);
        return;
    }
    if (actions->sell == nullptr) {
        note_inventory_action_state(InventoryActionState::kSellUnavailable);
        return;
    }
    note_inventory_action_state(InventoryActionState::kAvailable);
    if (scan_round_cancelled(round)) return;
    const qol::contracts::InventorySellResult result =
        actions->sell({ref.space_id, ref.bag, ref.slot}, ref.item, ref.count, "autosell");
    if (result.status == qol::contracts::InventorySellStatus::kOk) {
        ++stats->sold;
        return;
    }
    if (result.status == qol::contracts::InventorySellStatus::kStale) {
        ++stats->stale;
        if (g_logged_sell_statuses.insert(static_cast<int>(result.status)).second) {
            AUTOSELL_LOG_WARN("sell stale space_id=%d bag=%d slot=%d category=%d",
                              ref.space_id, ref.bag, ref.slot, ref.category);
        }
        return;
    }
    if (result.status == qol::contracts::InventorySellStatus::kEmptySlot) {
        ++stats->empty;
        if (g_logged_sell_statuses.insert(static_cast<int>(result.status)).second) {
            AUTOSELL_LOG_WARN("sell empty slot space_id=%d bag=%d slot=%d category=%d",
                              ref.space_id, ref.bag, ref.slot, ref.category);
        }
        return;
    }
    if (result.status == qol::contracts::InventorySellStatus::kNoItemHandle) {
        ++stats->no_handle;
        if (g_logged_sell_statuses.insert(static_cast<int>(result.status)).second) {
            AUTOSELL_LOG_WARN("sell no handle space_id=%d bag=%d slot=%d category=%d",
                              ref.space_id, ref.bag, ref.slot, ref.category);
        }
        return;
    }
    ++stats->failed;
    AUTOSELL_LOG_WARN("sell failed space_id=%d bag=%d slot=%d category=%d status=%d",
                      ref.space_id, ref.bag, ref.slot, ref.category,
                      static_cast<int>(result.status));
}

const qol::contracts::QolBagSourceRegistryApi* bag_source_registry() {
    const auto* sdk = qol::features::autosell::host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    const QolResult result = sdk->capabilities.require(
        qol::contracts::kBagSourceCapabilityId,
        QolVersionRange{qol::contracts::kBagSourceVersion.major,
                        qol::contracts::kBagSourceVersion.minor},
        &iface);
    if (result.ok != kQolOk || iface == nullptr) return nullptr;
    const auto* registry =
        static_cast<const qol::contracts::QolBagSourceRegistryApi*>(iface);
    return qol::contracts::qol_bag_source_registry_api_valid(registry) ? registry : nullptr;
}

struct RegisteredSourceScanCtx {
    const autosell::Config* cfg;
    ScanStats* stats;
    ScanRound* round;
};

int32_t scan_registered_item(const qol::contracts::QolInventoryItemRef* ref, void* user) {
    auto* scan = static_cast<RegisteredSourceScanCtx*>(user);
    if (scan == nullptr || scan->cfg == nullptr || scan->stats == nullptr ||
        scan->round == nullptr) {
        return 0;
    }
    if (scan_round_cancelled(*scan->round)) return 1;
    if (ref == nullptr || ref->space_id < qol::contracts::kQolBagSpaceFirstRegistered ||
        ref->item.slot == 0 || ref->item.generation == 0 || ref->category <= 0) {
        return 0;
    }
    qol::features::autosell::read_only::InventoryItemRef copy = *ref;
    qol::features::autosell::read_only::refresh_item_count(&copy);
    process_ref(copy, *scan->cfg, scan->stats, *scan->round);
    return scan_round_cancelled(*scan->round) ? 1 : 0;
}

void scan_registered_sources(const autosell::Config& cfg, ScanStats* stats,
                             ScanRound& round) {
    const auto* registry = bag_source_registry();
    if (registry == nullptr || registry->source_count == nullptr ||
        registry->source_at == nullptr || stats == nullptr || scan_round_cancelled(round)) {
        return;
    }

    const uint32_t source_count = registry->source_count();
    RegisteredSourceScanCtx scan{&cfg, stats, &round};
    for (uint32_t i = 0; i < source_count && !scan_round_cancelled(round); ++i) {
        const qol::contracts::QolBagSource* source = nullptr;
        if (registry->source_at(i, &source).ok != kQolOk || source == nullptr ||
            source->for_each_item == nullptr) {
            continue;
        }
        const QolResult result = source->for_each_item(&scan_registered_item, &scan);
        if (result.ok == kQolOk) {
            g_last_bag_source_error_by_space.erase(source->space_id);
        } else if (result.code == kQolErrNotReady) {
            g_last_bag_source_error_by_space.erase(source->space_id);
        } else {
            const auto previous = g_last_bag_source_error_by_space.find(source->space_id);
            if (previous == g_last_bag_source_error_by_space.end() ||
                previous->second != result.code) {
                g_last_bag_source_error_by_space[source->space_id] = result.code;
                AUTOSELL_LOG_WARN("scan bag source failed space_id=%d code=%d",
                                  source->space_id, result.code);
            }
        }
    }
}

struct SpaceScanCtx {
    int32_t space_id;
    int32_t max_bag;
    const autosell::Config* cfg;
    ScanStats* stats;
    ScanRound* round;
};

int32_t scan_space_item(const qol::contracts::QolInventoryItemRef* ref, void* user) {
    auto* scan = static_cast<SpaceScanCtx*>(user);
    if (scan == nullptr || scan->cfg == nullptr || scan->stats == nullptr ||
        scan->round == nullptr) {
        return 0;
    }
    if (scan_round_cancelled(*scan->round)) return 1;
    if (ref == nullptr || ref->space_id != scan->space_id || ref->item.slot == 0 ||
        ref->item.generation == 0 || ref->category <= 0) {
        return 0;
    }
    if (scan->max_bag >= 0 && ref->bag > scan->max_bag) return 0;
    qol::features::autosell::read_only::InventoryItemRef copy = *ref;
    qol::features::autosell::read_only::refresh_item_count(&copy);
    process_ref(copy, *scan->cfg, scan->stats, *scan->round);
    return scan_round_cancelled(*scan->round) ? 1 : 0;
}

void scan_native_space(int32_t space_id, int32_t max_bag, const autosell::Config& cfg,
                       ScanStats* stats, ScanRound& round, bool* has_error,
                       int* last_error) {
    if (stats == nullptr || has_error == nullptr || last_error == nullptr ||
        scan_round_cancelled(round)) {
        return;
    }
    SpaceScanCtx scan{space_id, max_bag, &cfg, stats, &round};
    const QolResult result = qol::features::autosell::read_only::for_each_space_slot(
        space_id, &scan_space_item, &scan);
    if (result.ok == kQolOk) {
        *has_error = false;
    } else if (!*has_error || *last_error != result.code) {
        *has_error = true;
        *last_error = result.code;
        AUTOSELL_LOG_WARN("scan native space failed space_id=%d code=%d", space_id, result.code);
    }
}

void scan_physical_bags(const autosell::Config& cfg, ScanStats* stats, ScanRound& round) {
    scan_native_space(qol::contracts::kQolBagSpaceOriginal, kLastPhysicalBag, cfg, stats, round,
                      &g_has_physical_scan_error, &g_last_physical_scan_error);
}

void scan_variant_bags(const autosell::Config& cfg, ScanStats* stats, ScanRound& round) {
    scan_native_space(qol::contracts::kQolBagSpaceVariant, -1, cfg, stats, round,
                      &g_has_variant_scan_error, &g_last_variant_scan_error);
}

void scan_body(const autosell::Config& cfg, int64_t frame, ScanRound& round) {
    if (scan_round_cancelled(round)) return;
    ScanStats stats;
    scan_physical_bags(cfg, &stats, round);
    if (!scan_round_cancelled(round)) scan_variant_bags(cfg, &stats, round);
    if (!scan_round_cancelled(round)) scan_registered_sources(cfg, &stats, round);

    autosell_note_scan(frame, stats.sold, stats.failed);
    static int last_refs = -1;
    static int last_view_skipped = -1;
    static int last_no_rule = -1;
    static int last_no_sell = -1;
    static int last_sold = -1;
    static int last_failed = -1;
    static int last_stale = -1;
    static int last_empty = -1;
    static int last_no_handle = -1;
    static bool zero_refs_logged = false;
    const bool changed = stats.refs != last_refs || stats.view_skipped != last_view_skipped ||
                         stats.no_rule != last_no_rule || stats.no_sell != last_no_sell ||
                         stats.sold != last_sold || stats.failed != last_failed ||
                         stats.stale != last_stale || stats.empty != last_empty ||
                         stats.no_handle != last_no_handle;
    if (stats.refs == 0) {
        if (!zero_refs_logged) {
            zero_refs_logged = true;
            AUTOSELL_LOG_WARN("scan frame=%lld refs=0", static_cast<long long>(frame));
        }
    } else if (changed) {
        last_refs = stats.refs;
        last_view_skipped = stats.view_skipped;
        last_no_rule = stats.no_rule;
        last_no_sell = stats.no_sell;
        last_sold = stats.sold;
        last_failed = stats.failed;
        last_stale = stats.stale;
        last_empty = stats.empty;
        last_no_handle = stats.no_handle;
        AUTOSELL_LOG_WARN(
            "scan frame=%lld refs=%d view_skipped=%d no_rule=%d no_sell=%d sold=%d failed=%d "
            "stale=%d empty=%d no_handle=%d",
            static_cast<long long>(frame), stats.refs, stats.view_skipped, stats.no_rule,
            stats.no_sell, stats.sold, stats.failed, stats.stale, stats.empty, stats.no_handle);
    }
    if (stats.sold > 0 || stats.failed > 0) {
        AUTOSELL_LOG_INFO("scan frame=%lld sold=%d failed=%d", static_cast<long long>(frame),
                          stats.sold, stats.failed);
    }
}

int32_t autosell_tick(int64_t frame, void*) {
    const uint64_t round_epoch = g_scan_cancel_epoch.load(std::memory_order_acquire);
    ScanRound round{round_epoch, frame};
    if (!g_global_enabled.load(std::memory_order_acquire)) {
        log_blocked_gate_once(frame, ScanGateReason::kGlobalDisabled);
        (void)scan_round_cancelled(round);
        return 1;
    }
    const int slot = autosell_active_save_slot();
    if (!autosell_scan_session_active(slot)) {
        log_blocked_gate_once(frame, ScanGateReason::kSessionInactive);
        (void)scan_round_cancelled(round);
        return 1;
    }
    const autosell::Config cfg = autosell_get_runtime_config();
    if (!cfg.enabled) {
        log_blocked_gate_once(frame, ScanGateReason::kConfigDisabled);
        (void)scan_round_cancelled(round);
        return 1;
    }
    ScanGateReason gate_reason = ScanGateReason::kReady;
    if (!scan_gates_ok(&gate_reason)) {
        log_blocked_gate_once(frame, gate_reason);
        (void)scan_round_cancelled(round);
        return 1;
    }
    g_last_blocked_gate.store(-1, std::memory_order_relaxed);
    // Scheduler ticks are serialized on the main thread; do not hold a plugin-private lock
    // across calls into other plugins.
    if (!scan_round_cancelled(round)) scan_body(cfg, frame, round);
    return 1;
}

void remove_task_if_any() {
    g_scan_cancel_epoch.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard<std::mutex> lock(g_task_mtx);
    if (g_task == 0) return;
    const auto* sdk = qol::features::autosell::host_sdk();
    if (sdk != nullptr && sdk->scheduler.remove != nullptr) (void)sdk->scheduler.remove(g_task);
    g_task = 0;
    AUTOSELL_LOG_INFO("task removed");
}

void sync_task() {
    if (!g_global_enabled.load(std::memory_order_acquire)) {
        log_blocked_gate_once(-1, ScanGateReason::kGlobalDisabled);
        remove_task_if_any();
        return;
    }
    if (!autosell_scan_session_active(autosell_scan_session_slot())) {
        log_blocked_gate_once(-1, ScanGateReason::kSessionInactive);
        remove_task_if_any();
        return;
    }
    if (!g_config_enabled.load(std::memory_order_acquire)) {
        log_blocked_gate_once(-1, ScanGateReason::kConfigDisabled);
        remove_task_if_any();
        return;
    }
    std::lock_guard<std::mutex> lock(g_task_mtx);
    if (!g_global_enabled.load(std::memory_order_acquire) ||
        !autosell_scan_session_active(autosell_scan_session_slot()) ||
        !g_config_enabled.load(std::memory_order_acquire)) {
        return;
    }
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
    if (!autosell_scan_save_enter(slot)) {
        AUTOSELL_LOG_WARN("save enter rejected slot mismatch slot=%d", slot);
        remove_task_if_any();
        return;
    }
    sync_task();
    AUTOSELL_LOG_INFO("save enter slot=%d; sync task", slot);
}

void autosell_on_save_exit(void*, const void*) {
    // Save-exit rejects slot-data writes until the next successful sd_load marks the session ready.
    autosell_store_reject_session();
    AUTOSELL_LOG_INFO("save exit; remove task");
    remove_task_if_any();
}

}  // namespace

void autosell_apply_config(const autosell::Config& config) {
    const bool old_enabled = autosell_get_runtime_config().enabled;
    autosell_set_runtime_config(config);
    g_config_enabled.store(config.enabled, std::memory_order_release);
    if (old_enabled == config.enabled) return;
    AUTOSELL_LOG_INFO("save master switch %d -> %d", old_enabled ? 1 : 0, config.enabled ? 1 : 0);
    if (config.enabled) sync_task();
    else {
        log_blocked_gate_once(-1, ScanGateReason::kConfigDisabled);
        remove_task_if_any();
    }
}

void autosell_scan_sync_after_session_change() { sync_task(); }

void autosell_set_global_enabled(bool enabled) {
    g_global_enabled.store(enabled, std::memory_order_release);
    AUTOSELL_LOG_INFO("global enabled=%d save_active=%d", enabled ? 1 : 0,
                      autosell_scan_session_slot() >= 0 ? 1 : 0);
    if (enabled) sync_task();
    else {
        log_blocked_gate_once(-1, ScanGateReason::kGlobalDisabled);
        remove_task_if_any();
    }
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

int autosell_active_save_slot() {
    return autosell_scan_session_slot();
}
