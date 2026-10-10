// 自动出售扫描与主线程逐帧 tick。

#include "scan.h"

#include "../../contracts/events.h"
#include "../../contracts/extension_bag.h"
#include "../../contracts/inventory_action.h"
#include "config.h"
#include "log.h"
#include "plugin.h"
#include "read_only.h"
#include "view.h"

#include <array>
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
std::atomic<int> g_last_blocked_gate{-1};
std::atomic<bool> g_diagnostic_debug_enabled{false};
std::atomic<bool> g_reset_diagnostic_cache{false};

bool diagnostic_debug_enabled();
void update_diagnostic_debug_state();

struct ExtensionApiLookup {
    QolResult result = qol::contracts::error(kQolErrNotReady, "not ready", 9);
    const qol::contracts::ExtensionBagApi* api = nullptr;
};

ExtensionApiLookup extension_bag_api_lookup() {
    ExtensionApiLookup lookup{};
    const auto* sdk = qol::features::autosell::host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return lookup;
    const void* iface = nullptr;
    lookup.result = sdk->capabilities.require(
        qol::contracts::kExtensionBagCapabilityId,
        QolVersionRange{static_cast<uint16_t>(qol::contracts::kExtensionBagVersionMajor),
                        static_cast<uint16_t>(qol::contracts::kExtensionBagVersionMinor)},
        &iface);
    if (lookup.result.ok == kQolOk && iface != nullptr) {
        lookup.api = static_cast<const qol::contracts::ExtensionBagApi*>(iface);
    }
    return lookup;
}

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

struct ScanStats {
    int sold = 0;
    int failed = 0;
};

enum class ScanGateReason : int {
    kReady,
    kGlobalDisabled,
    kSessionInactive,
    kConfigDisabled,
    kWorldUnavailable,
    kNotInWorld,
    kGameStateUnavailable,
    kGameStateBlocked,
    kModuleViewInstalled,
};

const char* scan_gate_name(ScanGateReason reason) {
    switch (reason) {
        case ScanGateReason::kReady: return "ready";
        case ScanGateReason::kGlobalDisabled: return "global_disabled";
        case ScanGateReason::kSessionInactive: return "session_inactive";
        case ScanGateReason::kConfigDisabled: return "config_disabled";
        case ScanGateReason::kWorldUnavailable: return "world_unavailable";
        case ScanGateReason::kNotInWorld: return "not_in_world";
        case ScanGateReason::kGameStateUnavailable: return "game_state_unavailable";
        case ScanGateReason::kGameStateBlocked: return "game_state_blocked";
        case ScanGateReason::kModuleViewInstalled: return "module_view_installed";
    }
    return "unknown";
}

void log_blocked_gate_once(int64_t frame, ScanGateReason reason) {
    if (!diagnostic_debug_enabled()) return;
    const int code = static_cast<int>(reason);
    const int previous = g_last_blocked_gate.exchange(code, std::memory_order_relaxed);
    if (previous != code) {
        AUTOSELL_LOG_DEBUG("scan gate frame=%lld result=blocked reason=%s",
                           static_cast<long long>(frame), scan_gate_name(reason));
    }
}

struct CandidateDiagnostics {
    int eb_require_ok = 0;
    int eb_require_code = kQolErrNotReady;
    bool eb_interface = false;
    bool logical_callback = false;
    int logical_bag = -1;
    bool ref_attempted = false;
    int ref_ok = 0;
    int ref_code = kQolErrNotReady;
    int kind = -1;
    int ref_bag = -1;
    int ref_slot = -1;
    int category = 0;
    bool handle_valid = false;
    AutosellViewBuildReason view_reason = AutosellViewBuildReason::kNotAttempted;
    AutosellViewDiagnostics view{};
    autosell::DecisionReason decision = autosell::DecisionReason::kNoRuleMatch;
    bool decision_attempted = false;
    int no_sell_available = 0;
    int no_sell = -1;
    int action_require_ok = 0;
    int action_require_code = kQolErrNotReady;
    bool action_interface = false;
    bool action_sell_fn = false;
    bool sell_called = false;
    int sell_status = -1;
};

struct LastCandidateDiagnostic {
    bool emitted = false;
    uint64_t fingerprint = 0;
};

std::array<std::array<LastCandidateDiagnostic, kSlotCount>, kLogicalBagScanCount>
    g_last_candidate_diagnostics{};

bool diagnostic_debug_enabled() {
    const auto* sdk = qol::features::autosell::host_sdk();
    return sdk != nullptr && sdk->log.debug_enabled != nullptr &&
           sdk->log.debug_enabled() != 0;
}

void update_diagnostic_debug_state() {
    const bool enabled = diagnostic_debug_enabled();
    const bool previous = g_diagnostic_debug_enabled.exchange(enabled, std::memory_order_relaxed);
    if (enabled && !previous) {
        g_reset_diagnostic_cache.store(true, std::memory_order_relaxed);
        g_last_blocked_gate.store(-1, std::memory_order_relaxed);
    }
}

uint64_t mix_diagnostic(uint64_t hash, uint64_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    return hash;
}

uint64_t candidate_fingerprint(const CandidateDiagnostics& d, const autosell::Config& cfg) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    const auto add = [&hash](uint64_t value) { hash = mix_diagnostic(hash, value); };
    add(cfg.enabled); add(static_cast<uint32_t>(cfg.rarity)); add(static_cast<uint32_t>(cfg.enhance));
    add(static_cast<uint32_t>(cfg.socket)); add(static_cast<uint32_t>(cfg.gem_tier));
    add(static_cast<uint32_t>(cfg.gem_range)); add(cfg.special_mask);
    add(static_cast<uint32_t>(d.eb_require_ok)); add(static_cast<uint32_t>(d.eb_require_code));
    add(d.eb_interface); add(d.logical_callback); add(static_cast<uint32_t>(d.logical_bag));
    add(d.ref_attempted);
    add(static_cast<uint32_t>(d.ref_ok)); add(static_cast<uint32_t>(d.ref_code));
    add(static_cast<uint32_t>(d.kind)); add(static_cast<uint32_t>(d.ref_bag));
    add(static_cast<uint32_t>(d.ref_slot)); add(static_cast<uint32_t>(d.category));
    add(d.handle_valid); add(static_cast<uint32_t>(d.view_reason)); add(d.view.fields);
    add(static_cast<uint32_t>(d.view.rarity)); add(static_cast<uint32_t>(d.view.equip));
    add(static_cast<uint32_t>(d.view.jewel)); add(d.view.special_types);
    add(static_cast<uint32_t>(d.decision)); add(d.decision_attempted);
    add(static_cast<uint32_t>(d.no_sell_available)); add(static_cast<uint32_t>(d.no_sell));
    add(static_cast<uint32_t>(d.action_require_ok)); add(static_cast<uint32_t>(d.action_require_code));
    add(d.action_interface); add(d.action_sell_fn); add(d.sell_called);
    add(static_cast<uint32_t>(d.sell_status));
    return hash;
}

const char* view_reason_name(AutosellViewBuildReason reason) {
    switch (reason) {
        case AutosellViewBuildReason::kNotAttempted: return "not_attempted";
        case AutosellViewBuildReason::kBuilt: return "ok";
        case AutosellViewBuildReason::kNullOutput: return "null_output";
        case AutosellViewBuildReason::kInvalidHandle: return "invalid_handle";
        case AutosellViewBuildReason::kInvalidCategory: return "invalid_category";
        case AutosellViewBuildReason::kFactsUnavailable: return "facts_unavailable";
        case AutosellViewBuildReason::kMissingRequiredFields: return "missing_required_fields";
        case AutosellViewBuildReason::kJewelPredicateUnavailable: return "jewel_predicate_unavailable";
        case AutosellViewBuildReason::kInvalidJewelCategory: return "invalid_jewel_category";
    }
    return "unknown";
}

const char* decision_reason_name(autosell::DecisionReason reason) {
    switch (reason) {
        case autosell::DecisionReason::kRuleMatch: return "match";
        case autosell::DecisionReason::kDisabled: return "disabled";
        case autosell::DecisionReason::kHardProtected: return "hard_protected";
        case autosell::DecisionReason::kNoRuleMatch: return "no_rule_match";
    }
    return "unknown";
}

const char* sell_status_name(int status) {
    using Status = qol::contracts::InventorySellStatus;
    switch (static_cast<Status>(status)) {
        case Status::kOk: return "ok";
        case Status::kNotInWorld: return "not_in_world";
        case Status::kTaskBagExcluded: return "task_bag_excluded";
        case Status::kEmptySlot: return "empty_slot";
        case Status::kNoItemHandle: return "no_item_handle";
        case Status::kStale: return "stale";
        case Status::kSymbolsUnresolved: return "symbols_unresolved";
        case Status::kNoSell: return "no_sell";
        case Status::kPriceInvalid: return "price_invalid";
        case Status::kCreditFailed: return "credit_failed";
        case Status::kRemoveFailed: return "remove_callback_failed";
        case Status::kRollbackFailed: return "rollback_failed";
        case Status::kExtensionSaleUnavailable: return "extension_sale_unavailable";
    }
    return "not_called";
}

void emit_candidate_diagnostic(int64_t frame, int bag, int slot,
                               const autosell::Config& cfg,
                               const CandidateDiagnostics& diagnostic) {
    if (!diagnostic_debug_enabled()) return;
    const size_t bag_index = static_cast<size_t>(bag - kLogicalBagScanFirst);
    const size_t slot_index = static_cast<size_t>(slot);
    LastCandidateDiagnostic& previous = g_last_candidate_diagnostics[bag_index][slot_index];
    const uint64_t fingerprint = candidate_fingerprint(diagnostic, cfg);
    if (previous.emitted && previous.fingerprint == fingerprint) return;
    previous = {true, fingerprint};

    AUTOSELL_LOG_DEBUG(
        "EB scan frame=%lld bag=%d slot=%d cfg=%016llx eb_require=%d/%d eb_api=%d "
        "logical_fn=%d logical=%d ref_attempted=%d ref=%d/%d kind=%d item_bag=%d item_slot=%d "
        "category=%d handle_valid=%d view=%s fields=0x%08x rarity=%d equip=%d jewel=%d "
        "special=0x%08x no_sell=%d/%d decision=%s action_api=%d/%d/%d "
        "action_sell_fn=%d sell_called=%d sell_status=%s(%d)",
        static_cast<long long>(frame), bag, slot,
        static_cast<unsigned long long>(fingerprint), diagnostic.eb_require_ok,
        diagnostic.eb_require_code, diagnostic.eb_interface ? 1 : 0,
        diagnostic.logical_callback ? 1 : 0, diagnostic.logical_bag,
        diagnostic.ref_attempted ? 1 : 0, diagnostic.ref_ok, diagnostic.ref_code,
        diagnostic.kind, diagnostic.ref_bag, diagnostic.ref_slot, diagnostic.category,
        diagnostic.handle_valid ? 1 : 0, view_reason_name(diagnostic.view_reason),
        diagnostic.view.fields, diagnostic.view.rarity, diagnostic.view.equip,
        diagnostic.view.jewel, diagnostic.view.special_types, diagnostic.no_sell_available,
        diagnostic.no_sell, diagnostic.decision_attempted
                                  ? decision_reason_name(diagnostic.decision) : "not_evaluated",
        diagnostic.action_require_ok, diagnostic.action_require_code,
        diagnostic.action_interface ? 1 : 0, diagnostic.action_sell_fn ? 1 : 0,
        diagnostic.sell_called ? 1 : 0,
        diagnostic.sell_status < 0 ? "not_called" : sell_status_name(diagnostic.sell_status),
        diagnostic.sell_status);
}

struct PhysicalScanCtx {
    const autosell::Config* cfg;
    ScanStats* stats;
};

bool scan_gates_ok(ScanGateReason* out_reason) {
    if (out_reason != nullptr) *out_reason = ScanGateReason::kReady;
    int32_t world_state = -1;
    if (!qol::features::autosell::read_only::world_state(&world_state)) {
        if (out_reason != nullptr) *out_reason = ScanGateReason::kWorldUnavailable;
        return false;
    }
    if (world_state != 5) {
        if (out_reason != nullptr) *out_reason = ScanGateReason::kNotInWorld;
        return false;
    }
    int32_t state_gate = 0;
    if (!qol::features::autosell::read_only::game_state_gate(&state_gate)) {
        if (out_reason != nullptr) *out_reason = ScanGateReason::kGameStateUnavailable;
        return false;
    }
    if (state_gate == 0) {
        if (out_reason != nullptr) *out_reason = ScanGateReason::kGameStateBlocked;
        return false;
    }
    const ExtensionApiLookup lookup = extension_bag_api_lookup();
    const auto* eb = lookup.api;
    if (eb != nullptr && eb->module_view_installed != nullptr && eb->module_view_installed()) {
        if (out_reason != nullptr) *out_reason = ScanGateReason::kModuleViewInstalled;
        return false;
    }
    return true;
}

void process_ref(const qol::features::autosell::read_only::InventoryItemRef& ref,
                 const autosell::Config& cfg, ScanStats* stats,
                 CandidateDiagnostics* diagnostics) {
    autosell::ItemView view;
    const AutosellViewBuildReason view_reason =
        autosell_build_view(ref, &view, diagnostics == nullptr ? nullptr : &diagnostics->view);
    if (diagnostics != nullptr) diagnostics->view_reason = view_reason;
    if (view_reason != AutosellViewBuildReason::kBuilt) return;

    const auto decision = autosell::should_sell_with_reason(view, cfg);
    if (diagnostics != nullptr) {
        diagnostics->decision = decision;
        diagnostics->decision_attempted = true;
        int32_t no_sell_value = 0;
        diagnostics->no_sell_available =
            qol::features::autosell::read_only::item_is_no_sell(ref.category, &no_sell_value) ? 1 : 0;
        diagnostics->no_sell = diagnostics->no_sell_available != 0 ? no_sell_value : -1;
    }
    if (decision != autosell::DecisionReason::kRuleMatch) return;

    int32_t no_sell = 0;
    if (qol::features::autosell::read_only::item_is_no_sell(ref.category, &no_sell) &&
        no_sell != 0) {
        return;
    }

    const InventoryActionLookup action_lookup = inventory_action_api_lookup();
    if (diagnostics != nullptr) {
        diagnostics->action_require_ok = action_lookup.result.ok;
        diagnostics->action_require_code = action_lookup.result.code;
        diagnostics->action_interface = action_lookup.api != nullptr;
        diagnostics->action_sell_fn = action_lookup.api != nullptr &&
                                      action_lookup.api->sell != nullptr;
    }
    const auto* actions = action_lookup.api;
    if (actions == nullptr || actions->sell == nullptr) return;
    if (diagnostics != nullptr) diagnostics->sell_called = true;
    const qol::contracts::InventorySellResult result =
        actions->sell(ref.bag, ref.slot, ref.item, "autosell");
    if (diagnostics != nullptr) diagnostics->sell_status = static_cast<int>(result.status);
    if (result.status == qol::contracts::InventorySellStatus::kOk) {
        ++stats->sold;
        return;
    }
    if (result.status == qol::contracts::InventorySellStatus::kStale ||
        result.status == qol::contracts::InventorySellStatus::kEmptySlot ||
        result.status == qol::contracts::InventorySellStatus::kNoItemHandle) {
        return;
    }
    ++stats->failed;
    AUTOSELL_LOG_WARN("sell failed bag=%d slot=%d category=%d status=%d", ref.bag, ref.slot,
                      ref.category, static_cast<int>(result.status));
}

void scan_body(const autosell::Config& cfg, int64_t frame) {
    if (g_reset_diagnostic_cache.exchange(false, std::memory_order_relaxed)) {
        g_last_candidate_diagnostics = {};
    }
    ScanStats stats;
    PhysicalScanCtx physical{&cfg, &stats};
    (void)qol::features::autosell::read_only::for_each_bag_slot(
        [](const qol::features::autosell::read_only::InventoryItemRef* source, void* ctx) -> int32_t {
            if (source == nullptr || source->bag > kLastPhysicalBag) return 0;
            auto* scan = static_cast<PhysicalScanCtx*>(ctx);
            auto ref = *source;
            qol::features::autosell::read_only::refresh_item_count(&ref);
            process_ref(ref, *scan->cfg, scan->stats, nullptr);
            return 0;
        },
        &physical);

    const ExtensionApiLookup eb_lookup = extension_bag_api_lookup();
    const auto* eb = eb_lookup.api;
    for (int bag = kLogicalBagScanFirst; bag < kLogicalBagScanFirst + kLogicalBagScanCount;
         ++bag) {
        int logical_bag = -1;
        const bool logical_callback = eb != nullptr && eb->is_logical_bag != nullptr;
        if (eb != nullptr && eb->is_logical_bag != nullptr) {
            logical_bag = eb->is_logical_bag(bag) ? 1 : 0;
        }
        for (int slot = 0; slot < kSlotCount; ++slot) {
            CandidateDiagnostics diagnostic{};
            diagnostic.eb_require_ok = eb_lookup.result.ok;
            diagnostic.eb_require_code = eb_lookup.result.code;
            diagnostic.eb_interface = eb != nullptr;
            diagnostic.logical_callback = logical_callback;
            diagnostic.logical_bag = logical_bag;

            if (logical_bag == 1) {
                qol::features::autosell::read_only::InventoryItemRef ref{};
                const QolResult result =
                    qol::features::autosell::read_only::inventory_item_ref_at(bag, slot, &ref);
                diagnostic.ref_attempted = true;
                diagnostic.ref_ok = result.ok;
                diagnostic.ref_code = result.code;
                if (result.ok == kQolOk) {
                    diagnostic.kind = ref.kind;
                    diagnostic.ref_bag = ref.bag;
                    diagnostic.ref_slot = ref.slot;
                    diagnostic.category = ref.category;
                    diagnostic.handle_valid = ref.item.slot != 0 && ref.item.generation != 0;
                    const bool provider_ref = ref.kind == static_cast<int32_t>(
                        qol::contracts::QolInventoryItemKind::kProvider);
                    if (provider_ref && diagnostic.handle_valid && ref.category > 0) {
                        qol::features::autosell::read_only::refresh_item_count(&ref);
                        process_ref(ref, cfg, &stats, &diagnostic);
                    }
                }
            }
            emit_candidate_diagnostic(frame, bag, slot, cfg, diagnostic);
        }
    }

    autosell_note_scan(frame, stats.sold, stats.failed);
    if (stats.sold > 0 || stats.failed > 0) {
        AUTOSELL_LOG_INFO("scan frame=%lld sold=%d failed=%d", static_cast<long long>(frame),
                          stats.sold, stats.failed);
    }
}

int32_t autosell_tick(int64_t frame, void*) {
    update_diagnostic_debug_state();
    if (!g_global_enabled.load(std::memory_order_acquire)) {
        log_blocked_gate_once(frame, ScanGateReason::kGlobalDisabled);
        return 1;
    }
    const int slot = autosell_active_save_slot();
    if (!autosell_scan_session_active(slot)) {
        log_blocked_gate_once(frame, ScanGateReason::kSessionInactive);
        return 1;
    }
    const autosell::Config cfg = autosell_get_runtime_config();
    if (!cfg.enabled) {
        log_blocked_gate_once(frame, ScanGateReason::kConfigDisabled);
        return 1;
    }
    ScanGateReason gate_reason = ScanGateReason::kReady;
    if (!scan_gates_ok(&gate_reason)) {
        log_blocked_gate_once(frame, gate_reason);
        return 1;
    }
    g_last_blocked_gate.store(-1, std::memory_order_relaxed);
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
    if (!autosell_get_runtime_config().enabled) {
        log_blocked_gate_once(-1, ScanGateReason::kConfigDisabled);
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
    if (!autosell_scan_save_enter(slot)) {
        AUTOSELL_LOG_WARN("save enter rejected slot mismatch slot=%d", slot);
        remove_task_if_any();
        return;
    }
    sync_task();
    AUTOSELL_LOG_INFO("save enter slot=%d; sync task", slot);
}

void autosell_on_save_exit(void*, const void*) {
    autosell_scan_session_closed(-1, 0);
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
