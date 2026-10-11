#include <cassert>
#include <cstdint>
#include <cstring>

#include "plugins/move_merge/plugin.cpp"

namespace qol::features::move_merge {
namespace {

using namespace qol;
using namespace qol::contracts;

QolHostSdk sdk{};
QolUiAdapter ui{};
QolMoveMergePort move_port{};
QolCharacterExtrasPort character_port{};
ItemFactsApi facts{};
InventoryMergeProjectionApi projection{};
InventoryRefreshApi refresh{};
MoveMergeControlApi control{};
QolItemHandle source_item{1, 1};
QolItemHandle destination_item{2, 1};
int original_calls = 0;
int move_calls = 0;
int refresh_calls = 0;
int sync_slot_calls = 0;
int sync_bag_calls = 0;
int cursor_calls = 0;
int moved_result = 1;
int facts_available = 1;
int projection_available = 1;
int refresh_available = 1;
int extension_source_item = 1;
int32_t destination_count_value = 20;
uint32_t count_limit_value = 99;
int apply_allowed_value = 0;
int count_supported_value = 1;
QolItemCountEncoding source_encoding_value = QolItemCountEncoding::kEncoded;
QolItemCountEncoding destination_encoding_value = QolItemCountEncoding::kEncoded;
int source_identity_available = 1;
int destination_identity_available = 1;
int source_control_available = 1;
int source_native = 1;
int destination_native = 1;
int source_slot_value = 1;
int destination_slot_value = 2;
int blocked_original_input = 0;
int tab_drop_allowed = 0;
MoveMergeProjectionDropResult projection_drop_result = MoveMergeProjectionDropResult::kNotHandled;
int64_t source_control_value = 0;
int64_t destination_control_value = 0;

QolResult ok() { return contracts::ok(); }
QolResult require(const char* id, QolVersionRange, const void** out) {
    *out = nullptr;
    if (std::strcmp(id, kItemFactsCapabilityId) == 0 && facts_available) *out = &facts;
    if (std::strcmp(id, kInventoryMergeProjectionCapabilityId) == 0 && projection_available)
        *out = &projection;
    if (std::strcmp(id, kInventoryRefreshCapabilityId) == 0 && refresh_available)
        *out = &refresh;
    return *out != nullptr ? ok() : error(kQolErrNotReady, "unavailable", 11);
}
QolResult call_original(void*, uint64_t, void*, void*, uint64_t* out) {
    ++original_calls;
    *out = 77;
    return ok();
}
QolResult source_control(void*, uint64_t, const void*, QolUiHandle* out) {
    if (!source_control_available) return error(kQolErrNotReady, "source unavailable", 18);
    *out = &source_control_value;
    return ok();
}
QolResult identity(void* control_handle, QolItemHandle* out) {
    if ((control_handle == &source_control_value && !source_identity_available) ||
        (control_handle != &source_control_value && !destination_identity_available)) {
        return error(kQolErrNotReady, "identity unavailable", 20);
    }
    *out = control_handle == &source_control_value ? source_item : destination_item;
    return ok();
}
QolResult slot(void* control_handle, int32_t* out) {
    *out = control_handle == &source_control_value ? source_slot_value : destination_slot_value;
    return ok();
}
QolResult cursor(void*, void*) { ++cursor_calls; return ok(); }
QolResult root(QolUiRootKind, QolUiHandle* out) { *out = &destination_control_value; return ok(); }
QolResult item_view(QolItemHandle handle, QolItemInventoryView* out) {
    out->count_supported = count_supported_value;
    out->category = 42;
    out->effective_count = handle.slot == 1 ? 10 : destination_count_value;
    return ok();
}
QolResult category(QolItemHandle, int32_t* out) { *out = 42; return ok(); }
QolResult count_encoding(QolItemHandle item, QolItemCountEncoding* out) {
    *out = item.slot == source_item.slot ? source_encoding_value : destination_encoding_value;
    if (*out == QolItemCountEncoding::kUnknown) return error(kQolErrUnsupported, "unknown", 7);
    return ok();
}
QolResult cumulate(QolItemHandle handle, int32_t* out) {
    *out = handle.slot == 1 ? 10 : destination_count_value;
    return ok();
}
QolResult count_limit(QolItemHandle, uint32_t* out) { *out = count_limit_value; return ok(); }
QolResult can_apply(QolItemHandle, QolItemHandle, int32_t* out) {
    *out = apply_allowed_value;
    return ok();
}
QolResult current_bag(int32_t* out) { *out = 0; return ok(); }
QolResult validate_native(QolItemHandle item) {
    if ((item.slot == source_item.slot && source_native) ||
        (item.slot == destination_item.slot && destination_native)) return ok();
    return error(kQolErrInvalidArgument, "foreign item", 12);
}
QolResult move_item(QolItemHandle handle, int32_t count, int32_t bag, int32_t dest,
                    int32_t* out) {
    ++move_calls;
    assert(handle.slot == source_item.slot && count == 10 && bag == 0 && dest == 2);
    *out = moved_result;
    return ok();
}
MoveMergeProjectionDropResult projection_drop(void*, void*) { return projection_drop_result; }
bool input_blocked() { return blocked_original_input != 0; }
bool allow_tab_drop(void*, void*, uint64_t caller) {
    assert(caller == 0);
    return tab_drop_allowed != 0;
}
QolResult identify_extension(QolItemHandle, int32_t* bag, int32_t* item_slot) {
    if (!extension_source_item) return error(kQolErrNotFound, "not extension", 12);
    *bag = 6;
    *item_slot = 0;
    return ok();
}
uint64_t observe_pre(void*, uint64_t, void*, void*) { return 9; }
void observe_source(uint64_t, void*, uint64_t, void*, void*, void*) {}
void observe_post(uint64_t token, void*, uint64_t, void*, void*, uint64_t) { assert(token == 9); }
bool module_view() { return true; }
bool sync_slot(int32_t bag, int32_t dest) {
    assert(bag == 0 && (dest == 1 || dest == 2));
    ++sync_slot_calls;
    return true;
}
bool sync_bag() { ++sync_bag_calls; return true; }
bool call_refresh() { ++refresh_calls; return true; }
int inflight = 0;
QolResult in_flight_enter(InFlightHandle handle) { assert(std::strcmp(handle.resource_id, kX1Resource) == 0); ++inflight; return ok(); }
QolResult in_flight_leave(InFlightHandle) { --inflight; return ok(); }

void reset() {
    original_calls = move_calls = refresh_calls = sync_slot_calls = sync_bag_calls = cursor_calls = 0;
    moved_result = 1;
    facts_available = projection_available = refresh_available = 1;
    destination_count_value = 20;
    count_limit_value = 99;
    apply_allowed_value = 0;
    count_supported_value = 1;
    source_encoding_value = destination_encoding_value = QolItemCountEncoding::kEncoded;
    source_identity_available = destination_identity_available = 1;
    source_control_available = 1;
    source_native = destination_native = 1;
    source_slot_value = 1;
    destination_slot_value = 2;
    blocked_original_input = tab_drop_allowed = 0;
    extension_source_item = 0;
    projection_drop_result = MoveMergeProjectionDropResult::kNotHandled;
}

void setup() {
    sdk = {};
    sdk.abi = {sizeof(QolHostSdk), kHostSdkMajor, kHostSdkMinor};
    sdk.hooks.call_original_control_proc = &call_original;
    sdk.capabilities.require = &require;
    sdk.ui = &ui;
    sdk.in_flight = {&in_flight_enter, &in_flight_leave};
    ui = {};
    ui.item_proc_source_control = &source_control;
    ui.control_item_identity = &identity;
    ui.control_item_slot = &slot;
    ui.set_touch_cursor = &cursor;
    ui.find_root = &root;
    move_port = {kQolMoveMergePortVersion, &move_item, &current_bag, &validate_native};
    character_port = {kQolPortCharacterExtrasVersion, {}};
    character_port.read.struct_size = sizeof(character_port.read);
    character_port.read.can_apply_stuff_items = &can_apply;
    facts = {};
    facts.struct_size = sizeof(facts);
    facts.view = &item_view;
    facts.category = &category;
    facts.cumulate_count = &cumulate;
    facts.count_limit = &count_limit;
    facts.count_encoding = &count_encoding;
    projection = {sizeof(projection), kInventoryMergeProjectionVersionMajor,
                  kInventoryMergeProjectionVersionMinor, &projection_drop, &input_blocked,
                  &allow_tab_drop, &identify_extension, &observe_pre, &observe_source,
                  &observe_post, &module_view, &sync_slot, &sync_bag};
    refresh = {&call_refresh};
    g_sdk = &sdk;
    g_move_port = &move_port;
    g_character_port = &character_port;
    // 门禁等价插件活跃态：默认不活跃，需要合并语义的用例显式置 true。
    g_active.store(false);
    inflight = 0;
}

void test_backup_once_and_projection_sync() {
    reset();
    setup();
    const auto result = ui_equip_inven_item_proc_wrapper(&destination_control_value, 4, nullptr,
                                                         &source_control_value);
    assert(result == 77 && original_calls == 1 && move_calls == 0);
    assert(sync_bag_calls == 1 && inflight == 0);
    g_active.store(true);
    reset();
    const auto merged = ui_equip_inven_item_proc_wrapper(&destination_control_value, 4, nullptr,
                                                         &source_control_value);
    assert(merged == 0 && move_calls == 1 && original_calls == 0);
    assert(sync_slot_calls == 2 && refresh_calls == 1 && cursor_calls == 1);
    assert(sync_bag_calls == 0 && inflight == 0);
}

void test_drop_tristate_and_protection() {
    reset();
    setup();
    projection_drop_result = MoveMergeProjectionDropResult::kHandled;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 2, nullptr,
                                           &source_control_value) == 1);
    projection_drop_result = MoveMergeProjectionDropResult::kRejected;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 2, nullptr,
                                           &source_control_value) == 0);
    projection_drop_result = MoveMergeProjectionDropResult::kNotHandled;
    source_native = 0;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 2, nullptr,
                                           &source_control_value) == 0);
    assert(original_calls == 0 && move_calls == 0);
    source_native = 1;
    blocked_original_input = 1;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 4, nullptr,
                                           &source_control_value) == 0);
    tab_drop_allowed = 1;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 4, nullptr,
                                           &source_control_value) == 77);
    assert(original_calls == 1);
    source_native = 0;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 4, nullptr,
                                           &source_control_value) == 0);
    assert(original_calls == 1 && move_calls == 0);
}

void test_foreign_failure_and_missing_facts_fallthrough() {
    reset();
    setup();
    g_active.store(true);
    moved_result = 0;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 4, nullptr,
                                           &source_control_value) == 77);
    assert(move_calls == 1 && original_calls == 1);
    reset();
    extension_source_item = 0;
    facts_available = 0;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 4, nullptr,
                                           &source_control_value) == 77);
    assert(move_calls == 0 && original_calls == 1);
    reset();
    setup();
    g_active.store(true);
    count_supported_value = 0;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 4, nullptr,
                                           &source_control_value) == 77);
    assert(move_calls == 0 && original_calls == 1);
    reset();
    setup();
    g_active.store(true);
    destination_slot_value = 16;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 4, nullptr,
                                           &source_control_value) == 77);
    assert(move_calls == 0 && original_calls == 1);
}

void test_single_owner_and_feature_lifecycle() {
    reset();
    setup();
    const auto& descriptor = plugin_descriptor();
    assert(std::strcmp(descriptor.id, "move_merge") == 0);
    assert(descriptor.resources_count == 1);
    assert(std::strcmp(descriptor.resources[0].id, "ptrslot:MOVE_MERGE_GOT") == 0);
    assert(std::strcmp(descriptor.resources[0].owner, "move_merge") == 0);
    assert(descriptor.resources[0].gate == nullptr);
    assert(descriptor.provides_count == 1);
    assert(std::strcmp(descriptor.provides[0].id, kMoveMergeControlCapabilityId) == 0);
    // 单一 feature、无配置/事件订阅：门禁即插件活跃态，由宿主 lifecycle 驱动。
    assert(descriptor.feature_count == 1);
    assert(std::strcmp(descriptor.features[0].id, "move_merge.drag_merge") == 0);
    assert(descriptor.features[0].default_enabled == 1);
    assert(descriptor.features[0].category == FeatureCategory::kBasicOptimization);

    const void* ports[] = {&move_port, &character_port};
    PluginContext ctx{sizeof(PluginContext), kHostSdkMajor, kHostSdkMinor, &sdk, nullptr,
                      "move_merge", ports, 2};
    assert(plugin_install(ctx) == 0);
    assert(control_enabled());
    plugin_on_disabled(ctx);
    assert(!control_enabled());
    plugin_on_enabled(ctx);
    assert(control_enabled());
    plugin_deactivate(ctx);
    assert(!control_enabled());
}

void test_full_limit_apply_and_optional_refresh() {
    reset();
    setup();
    g_active.store(true);
    destination_count_value = 99;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 4, nullptr,
                                           &source_control_value) == 77);
    assert(move_calls == 0 && original_calls == 1);
    reset();
    setup();
    g_active.store(true);
    count_limit_value = 999;
    destination_count_value = 998;
    refresh_available = 0;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 4, nullptr,
                                           &source_control_value) == 77);
    assert(move_calls == 0 && original_calls == 1 && refresh_calls == 0);
    reset();
    setup();
    g_active.store(true);
    apply_allowed_value = 1;
    extension_source_item = 0;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 4, nullptr,
                                           &source_control_value) == 77);
    assert(move_calls == 0 && original_calls == 1);
}

void test_foreign_source_protected_without_projection_or_gate() {
    reset();
    setup();
    g_active.store(true);
    extension_source_item = 1;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 4, nullptr,
                                           &source_control_value) == 0);
    assert(move_calls == 0 && original_calls == 0);

    reset();
    setup();
    source_native = 0;
    projection_available = 0;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 4, nullptr,
                                           &source_control_value) == 0);
    assert(move_calls == 0 && original_calls == 0);

    reset();
    setup();
    source_native = 0;
    extension_source_item = 0;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 4, nullptr,
                                           &source_control_value) == 0);
    assert(move_calls == 0 && original_calls == 0);

    reset();
    setup();
    source_identity_available = 0;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 4, nullptr,
                                           &source_control_value) == 0);
    assert(move_calls == 0 && original_calls == 0);

    reset();
    setup();
    source_control_available = 0;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 4, nullptr,
                                           &source_control_value) == 0);
    assert(move_calls == 0 && original_calls == 0);
}

void test_event2_native_and_ordinary_fallback() {
    reset();
    setup();
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 2, nullptr,
                                           &source_control_value) == 77);
    assert(original_calls == 1 && move_calls == 0);

    reset();
    setup();
    projection_available = 0;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 2, nullptr,
                                           &source_control_value) == 77);
    assert(original_calls == 1 && move_calls == 0);

    reset();
    setup();
    source_control_available = 0;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 2, nullptr,
                                           nullptr) == 77);
    assert(original_calls == 1 && move_calls == 0);

    reset();
    setup();
    projection_drop_result = MoveMergeProjectionDropResult::kNotHandled;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 2, nullptr,
                                           &source_control_value) == 77);
    assert(original_calls == 1 && move_calls == 0);

    reset();
    setup();
    source_identity_available = 0;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 2, nullptr,
                                           &source_control_value) == 0);
    assert(original_calls == 0 && move_calls == 0);
}

void test_native_fallback_and_count_encoding_gate() {
    reset();
    setup();
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 4, nullptr,
                                           &source_control_value) == 77);
    assert(move_calls == 0 && original_calls == 1);

    reset();
    setup();
    g_active.store(true);
    source_encoding_value = QolItemCountEncoding::kNotEncoded;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 4, nullptr,
                                           &source_control_value) == 77);
    assert(move_calls == 0 && original_calls == 1);

    reset();
    setup();
    g_active.store(true);
    destination_encoding_value = QolItemCountEncoding::kUnknown;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 4, nullptr,
                                           &source_control_value) == 77);
    assert(move_calls == 0 && original_calls == 1);

    reset();
    setup();
    g_active.store(true);
    count_limit_value = 99;
    destination_count_value = 89;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 4, nullptr,
                                           &source_control_value) == 0);
    assert(move_calls == 1 && original_calls == 0);

    reset();
    setup();
    g_active.store(true);
    count_limit_value = 999;
    destination_count_value = 989;
    assert(ui_equip_inven_item_proc_wrapper(&destination_control_value, 4, nullptr,
                                           &source_control_value) == 0);
    assert(move_calls == 1 && original_calls == 0);
}

}  // namespace
}  // namespace qol::features::move_merge

int main() {
    qol::features::move_merge::test_backup_once_and_projection_sync();
    qol::features::move_merge::test_drop_tristate_and_protection();
    qol::features::move_merge::test_foreign_failure_and_missing_facts_fallthrough();
    qol::features::move_merge::test_single_owner_and_feature_lifecycle();
    qol::features::move_merge::test_full_limit_apply_and_optional_refresh();
    qol::features::move_merge::test_foreign_source_protected_without_projection_or_gate();
    qol::features::move_merge::test_event2_native_and_ordinary_fallback();
    qol::features::move_merge::test_native_fallback_and_count_encoding_gate();
    return 0;
}
