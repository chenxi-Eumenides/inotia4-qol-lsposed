#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "plugins/autosell/config.h"
#include "plugins/autosell/read_only.h"
#include "plugins/autosell/scan.h"
#include "plugins/autosell/view.h"

#include <cstdio>

namespace {

int g_pass = 0;
int g_fail = 0;
#define CHECK(condition) do { \
    if (condition) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } \
} while (false)

qol::contracts::QolHostSdk g_sdk{};
bool g_store_session_ready = false;
int g_scheduler_adds = 0;
int g_scheduler_removes = 0;
uint64_t g_last_task_id = 0;

QolResult fake_scheduler_add(int32_t, qol::contracts::QolFrameHandler handler, void*, int32_t interval,
                             int32_t, uint64_t* out_id) {
    if (handler == nullptr || out_id == nullptr || interval != kAutoSellScanIntervalFrames)
        return qol::contracts::error(kQolErrInvalidArgument, "invalid autosell task", 20);
    ++g_scheduler_adds;
    g_last_task_id = static_cast<uint64_t>(100 + g_scheduler_adds);
    *out_id = g_last_task_id;
    return qol::contracts::ok();
}

QolResult fake_scheduler_remove(uint64_t id) {
    if (id != g_last_task_id)
        return qol::contracts::error(kQolErrNotFound, "task not found", 14);
    ++g_scheduler_removes;
    return qol::contracts::ok();
}

}  // namespace

namespace qol::features::autosell {

const contracts::QolHostSdk* host_sdk() { return &g_sdk; }
const contracts::UiStateApi* ui_state() { return nullptr; }

}  // namespace qol::features::autosell

namespace qol::features::autosell::read_only {

bool item_category(contracts::QolItemHandle, int32_t* out) {
    if (out == nullptr) return false;
    *out = 0;
    return true;
}
bool item_type(contracts::QolItemHandle, contracts::QolItemType* out) {
    if (out == nullptr) return false;
    *out = contracts::QolItemType::kOther;
    return true;
}
bool item_rarity(contracts::QolItemHandle, int32_t* out) {
    if (out == nullptr) return false;
    *out = 0;
    return true;
}
bool item_enchant_remaining(contracts::QolItemHandle, int32_t* out) {
    if (out == nullptr) return false;
    *out = 0;
    return true;
}
bool item_socket_filled(contracts::QolItemHandle, int32_t* out) {
    if (out == nullptr) return false;
    *out = 0;
    return true;
}
bool item_socket_total(contracts::QolItemHandle, int32_t* out) {
    if (out == nullptr) return false;
    *out = 0;
    return true;
}
bool item_is_backpack(int32_t, int32_t* out) {
    if (out == nullptr) return false;
    *out = 0;
    return true;
}
bool item_is_dice(int32_t, int32_t* out) {
    if (out == nullptr) return false;
    *out = 0;
    return true;
}
bool item_is_no_sell(int32_t, int32_t* out) {
    if (out == nullptr) return false;
    *out = 0;
    return true;
}
bool item_jewel_value(contracts::QolItemHandle, int32_t* out) {
    if (out == nullptr) return false;
    *out = 0;
    return true;
}
bool item_jewel_attribute_type(contracts::QolItemHandle, int32_t* out) {
    if (out == nullptr) return false;
    *out = 0;
    return true;
}
bool item_inventory_view(contracts::QolItemHandle, contracts::QolItemInventoryView* out) {
    if (out == nullptr) return false;
    *out = {};
    return true;
}
bool world_state(int32_t* out) {
    if (out == nullptr) return false;
    *out = 5;
    return true;
}
bool game_state_gate(int32_t* out) {
    if (out == nullptr) return false;
    *out = 1;
    return true;
}
QolResult inventory_item_ref_at(int32_t, int32_t, InventoryItemRef* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "null ref", 8);
    *out = {};
    return qol::contracts::ok();
}
QolResult for_each_bag_slot(BagSlotFn, void*) { return qol::contracts::ok(); }
QolResult for_each_space_slot(int32_t, BagSlotFn, void*) { return qol::contracts::ok(); }
void refresh_item_count(InventoryItemRef*) {}

}  // namespace qol::features::autosell::read_only

AutosellViewBuildReason autosell_build_view(
    const qol::features::autosell::read_only::InventoryItemRef&, autosell::ItemView* out,
    AutosellViewDiagnostics* diagnostics, AttributeQualityApiProvider) {
    if (diagnostics != nullptr) *diagnostics = {};
    if (out != nullptr) *out = {};
    return out == nullptr ? AutosellViewBuildReason::kNullOutput
                          : AutosellViewBuildReason::kBuilt;
}

bool autosell_store_session_matches(int slot) {
    return g_store_session_ready && slot == 0;
}

uint64_t autosell_store_session_epoch(int slot) {
    return autosell_store_session_matches(slot) ? 1 : 0;
}

void autosell_store_reject_session() {
    g_store_session_ready = false;
    autosell_scan_session_closed(-1, 0);
}

int main() {
    g_sdk.scheduler.add = &fake_scheduler_add;
    g_sdk.scheduler.remove = &fake_scheduler_remove;

    autosell::Config disabled{};
    autosell_apply_config(disabled);
    autosell_set_global_enabled(false);
    CHECK(g_scheduler_adds == 0);

    autosell_set_global_enabled(true);
    CHECK(g_scheduler_adds == 0);  // Session is inactive.

    g_store_session_ready = true;
    autosell_scan_session_loaded(0, 1);
    CHECK(g_scheduler_adds == 0);  // Config is disabled.

    autosell::Config enabled{};
    enabled.enabled = true;
    autosell_apply_config(enabled);
    CHECK(g_scheduler_adds == 1);
    autosell_scan_sync_after_session_change();
    CHECK(g_scheduler_adds == 1);  // Repeated sync does not register twice.

    autosell_apply_config(disabled);
    CHECK(g_scheduler_removes == 1);
    autosell_apply_config(disabled);
    autosell_scan_sync_after_session_change();
    CHECK(g_scheduler_removes == 1);  // Repeated disable does not remove twice.

    autosell_apply_config(enabled);
    CHECK(g_scheduler_adds == 2);
    const int removes_before_reset = g_scheduler_removes;
    const int adds_before_reset = g_scheduler_adds;
    autosell_reset_scan_timer();
    CHECK(g_scheduler_removes == removes_before_reset + 1);
    CHECK(g_scheduler_adds == adds_before_reset + 1);
    autosell_set_global_enabled(false);
    CHECK(g_scheduler_removes == removes_before_reset + 2);
    autosell_set_global_enabled(false);
    CHECK(g_scheduler_removes == removes_before_reset + 2);  // Repeated global disable does not remove twice.

    std::printf("autosell_scan_task_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
