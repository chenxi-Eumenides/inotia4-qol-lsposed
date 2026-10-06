// Stateful UI fake around the real settings.cpp toggle callbacks and request polling.
#include "../plugins/core.settings/panels/settings.cpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace qol;
using namespace qol::features::core_ui;
using contracts::SettingsConfigRequestState;
using contracts::SettingsConfigApplyState;
using ::QolResult;

struct FakeWidget {
    int32_t value = 0;
    bool enabled = false;
    int set_value_calls = 0;
    int set_enabled_calls = 0;
    UiRect rect{};
    std::string text;
    int draw_calls = 0;
    contracts::QolUiChangedFn on_changed = nullptr;
    contracts::QolUiClickFn on_click = nullptr;
    void* user = nullptr;
};

FakeWidget g_widget;
FakeWidget g_action_widget;
int g_creates = 0;
int g_destroys = 0;
int g_list_creates = 0;
int g_list_destroys = 0;
int g_action_creates = 0;
int g_action_destroys = 0;
bool g_action_live = false;
uint32_t g_fake_row_count = 2;
int g_selection_callbacks = 0;
int g_native_event_dispatches = 0;
contracts::QolUiHandle g_native_event_root = nullptr;
uint64_t g_native_event_code = 0;
uint64_t g_native_event_param = 0;
uint64_t g_native_event_param2 = 0;
int g_back_requests = 0;
bool g_back_close_pending = false;
// fake 原生 TouchHandle：按下落于按钮内置态，移出取消，松开仍落于按钮内才 ExecuteProc。
bool g_fake_native_pressed = false;
bool g_right_selection_enabled = true;
int g_receipt_queries = 0;
int32_t g_persisted_value = 0;
uint64_t g_request_id = 0;
uint64_t g_next_request = 1;
SettingsConfigRequestState g_request_state = SettingsConfigRequestState::kQueued;
SettingsConfigApplyState g_apply_state = SettingsConfigApplyState::kApplied;
bool g_read_fails = false;
bool g_admit_fails = false;
bool g_query_fails = false;
bool g_owner_enabled = true;
bool g_owner_operable = true;
bool g_action_operable = true;
bool g_action_availability_fails = false;
bool g_availability_fails = false;
contracts::SettingsItemAvailability g_item_availability{};
contracts::SettingsCategoryView g_category{};
contracts::SettingsItemView g_items_fixture[2]{};
contracts::CatalogView g_catalog_view{};
contracts::QolUiComponents g_fake_components{};
contracts::QolUiAdapter g_fake_ui{};
contracts::QolSettingsCatalogApi g_fake_catalog{};
contracts::QolSettingsConfigApi g_fake_config{};
contracts::QolUiHandle g_fake_root = reinterpret_cast<void*>(0x1000);
struct DrawRecord {
    std::string text;
    int32_t x = 0;
    int32_t y = 0;
    int32_t align = -1;
};
std::vector<DrawRecord> g_draw_records;

QolResult ui_ok() { return contracts::ok(); }
QolResult fake_list_create(const contracts::UiScrollListDesc* desc,
                           contracts::QolUiHandle, contracts::QolUiHandle* out) {
    assert(desc != nullptr && desc->item_count == g_fake_row_count);
    ++g_list_creates;
    *out = reinterpret_cast<void*>(0x2000);
    return contracts::ok();
}
QolResult fake_list_get_row(contracts::QolUiHandle, uint32_t row,
                            contracts::QolUiHandle* out) {
    assert(row < g_fake_row_count);
    *out = reinterpret_cast<void*>(0x3000);
    return contracts::ok();
}
QolResult fake_list_selection_enabled(contracts::QolUiHandle, uint8_t enabled) {
    g_right_selection_enabled = enabled != 0;
    return contracts::ok();
}
QolResult fake_text_button_create(const contracts::UiButtonDesc* desc,
                                  contracts::QolUiHandle row,
                                  contracts::QolUiHandle* out) {
    assert(desc != nullptr && row == reinterpret_cast<void*>(0x3000));
    assert(std::strcmp(desc->text, "打开") == 0 && desc->on_click != nullptr);
    assert(desc->style == contracts::kUiTextButtonFontLarger);
    g_action_widget.rect = desc->rect;
    g_action_widget.text = desc->text;
    g_action_widget.on_click = desc->on_click;
    g_action_widget.user = desc->user;
    g_action_widget.enabled = true;
    ++g_action_creates;
    g_action_live = true;
    *out = &g_action_widget;
    return contracts::ok();
}
QolResult fake_text_button_set_enabled(contracts::QolUiHandle handle, uint8_t enabled) {
    assert(handle == &g_action_widget && enabled <= 1);
    ++g_action_widget.set_enabled_calls;
    g_action_widget.enabled = enabled != 0;
    return contracts::ok();
}
QolResult fake_text_button_draw(contracts::QolUiHandle handle) {
    assert(handle == &g_action_widget && g_action_widget.text == "打开");
    ++g_action_widget.draw_calls;
    return contracts::ok();
}
QolResult fake_back_request();
QolResult fake_dispatch_native_event(contracts::QolUiHandle root, uint64_t event,
                                     uint64_t param, uint64_t param2, uint32_t* consumed) {
    ++g_native_event_dispatches;
    g_native_event_root = root;
    g_native_event_code = event;
    g_native_event_param = param;
    g_native_event_param2 = param2;
    *consumed = 7;
    int64_t x = 0;
    int64_t y = 0;
    if (param != 0) {
        const auto* point = reinterpret_cast<const int64_t*>(static_cast<uintptr_t>(param));
        x = point[0];
        y = point[1];
    }
    // 模拟游戏原生 TouchHandle：仅 nav 手势根（g_root）需要判定。
    if (root == g_root && g_ui != nullptr && g_ui->hit_test != nullptr) {
        const bool inside = g_ui->hit_test(g_back, x, y, {0, 0, 74, 81}) > 0;
        if (event == 0x17) {
            g_fake_native_pressed = inside;
        } else if (event == 0x19) {
            if (!inside) g_fake_native_pressed = false;
        } else if (event == 0x18) {
            const bool pressed = g_fake_native_pressed;
            g_fake_native_pressed = false;
            if (pressed && inside) (void)fake_back_request();
        }
    }
    return contracts::ok();
}
QolResult fake_back_request() {
    ++g_back_requests;
    g_back_close_pending = true;
    return contracts::ok();
}
int32_t fake_main_thread() { return 1; }
QolResult fake_touch_coordinates(uint64_t param, int64_t* x, int64_t* y) {
    if (param == 0 || x == nullptr || y == nullptr)
        return contracts::error(kQolErrInvalidArgument, "touch", 5);
    const auto* point = reinterpret_cast<const int64_t*>(static_cast<uintptr_t>(param));
    *x = point[0];
    *y = point[1];
    return contracts::ok();
}
int32_t fake_hit_test(contracts::QolUiHandle handle, int64_t x, int64_t y, contracts::UiRect) {
    if (handle == reinterpret_cast<void*>(0x2000)) return 1;
    return handle == reinterpret_cast<void*>(0x4000) && x >= 0 && x < 74 && y >= 0 && y < 81
        ? 1 : 0;
}
QolResult fake_toggle_create(const contracts::UiNativeToggleDesc* desc,
                             contracts::QolUiHandle, contracts::QolUiHandle* out) {
    assert(desc != nullptr && *out == nullptr);
    ++g_creates;
    g_widget.value = desc->value;
    g_widget.rect = desc->rect;
    g_widget.enabled = desc->enabled != 0;
    g_widget.on_changed = desc->on_changed;
    g_widget.user = desc->user;
    *out = &g_widget;
    return contracts::ok();
}
QolResult fake_toggle_set_value(contracts::QolUiHandle handle, int32_t value) {
    assert(handle == &g_widget);
    ++g_widget.set_value_calls;
    g_widget.value = value != 0 ? 1 : 0;
    return contracts::ok();
}
QolResult fake_toggle_set_enabled(contracts::QolUiHandle handle, uint8_t enabled) {
    assert(handle == &g_widget);
    ++g_widget.set_enabled_calls;
    g_widget.enabled = enabled != 0;
    return contracts::ok();
}
QolResult fake_draw_control_text(contracts::QolUiHandle, const char* text, int32_t x,
                                 int32_t y, uint32_t, int32_t align, int32_t) {
    g_draw_records.push_back({text == nullptr ? "" : text, x, y, align});
    return contracts::ok();
}
QolResult fake_destroy(contracts::QolUiHandle handle) {
    if (handle == &g_widget) ++g_destroys;
    else if (handle == &g_action_widget) ++g_destroys;
    else if (handle == reinterpret_cast<void*>(0x2000)) {
        ++g_list_destroys;
        if (g_action_live) {
            ++g_action_destroys;
            g_action_live = false;
        }
    }
    return contracts::ok();
}

QolResult fake_snapshot(const contracts::CatalogView** out, uint64_t* token) {
    *out = &g_catalog_view;
    *token = 1;
    return contracts::ok();
}
QolResult fake_snapshot_release(uint64_t token) {
    assert(token == 1);
    return contracts::ok();
}
QolResult fake_query_availability(const char* id, uint64_t generation,
                                  contracts::SettingsItemAvailability* out) {
    assert(generation == 1);
    if (std::strcmp(id, "test.action") == 0) {
        if (g_action_availability_fails)
            return contracts::error(kQolErrNotReady, "action availability unknown", 26);
        *out = g_item_availability;
        out->operable = g_owner_enabled && g_action_operable ? 1 : 0;
        std::snprintf(out->reason, sizeof(out->reason), "%s",
                      out->operable != 0 ? "" : "settings action owner is unavailable");
        return contracts::ok();
    }
    assert(std::strcmp(id, "test.toggle") == 0);
    if (g_availability_fails)
        return contracts::error(kQolErrNotReady, "owner availability unknown", 27);
    *out = g_item_availability;
    out->operable = g_owner_operable ? 1 : 0;
    return contracts::ok();
}
QolResult fake_read_bool(const char* key, int32_t* out) {
    assert(std::strcmp(key, "testBool") == 0);
    if (g_read_fails) return contracts::error(kQolErrNotReady, "read failed", 11);
    *out = g_persisted_value;
    return contracts::ok();
}
QolResult fake_request_bool(const char* key, int32_t desired, uint64_t* out) {
    assert(std::strcmp(key, "testBool") == 0 && (desired == 0 || desired == 1));
    if (g_admit_fails) return contracts::error(kQolErrBlocked, "request rejected", 16);
    if (desired == g_persisted_value) {
        *out = 0;
        return contracts::ok();
    }
    g_request_id = g_next_request++;
    g_request_state = SettingsConfigRequestState::kQueued;
    *out = g_request_id;
    return contracts::ok();
}
QolResult fake_query_request(uint64_t id, contracts::SettingsConfigRequestStatus* out) {
    assert(id == g_request_id);
    ++g_receipt_queries;
    if (g_query_fails) return contracts::error(kQolErrNotFound, "unknown receipt", 15);
    out->state = g_request_state;
    out->request_id = id;
    out->persisted = g_request_state == SettingsConfigRequestState::kSucceeded ? 1 : 0;
    out->configured_value = g_persisted_value;
    out->apply_state = g_apply_state;
    return contracts::ok();
}

void setup() {
    g_widget = {};
    g_action_widget = {};
    g_draw_records.clear();
    g_creates = g_destroys = g_list_creates = g_list_destroys = 0;
    g_action_creates = g_action_destroys = 0;
    g_action_live = false;
    g_fake_row_count = 2;
    g_selection_callbacks = 0;
    g_native_event_dispatches = 0;
    g_native_event_root = nullptr;
    g_native_event_code = 0;
    g_native_event_param = 0;
    g_native_event_param2 = 0;
    g_back_requests = 0;
    g_back_close_pending = false;
    g_fake_native_pressed = false;
    g_right_selection_enabled = true;
    g_receipt_queries = 0;
    g_persisted_value = 0;
    g_request_id = 0;
    g_next_request = 1;
    g_request_state = SettingsConfigRequestState::kQueued;
    g_apply_state = SettingsConfigApplyState::kApplied;
    g_read_fails = g_admit_fails = g_query_fails = g_availability_fails = false;
    g_owner_enabled = g_owner_operable = true;
    g_action_operable = true;
    g_action_availability_fails = false;
    g_catalog_view = {};
    g_category = {};
    g_items_fixture[0] = {};
    g_items_fixture[1] = {};
    std::snprintf(g_category.id, sizeof(g_category.id), "%s", "test");
    std::snprintf(g_items_fixture[0].id, sizeof(g_items_fixture[0].id), "%s", "test.toggle");
    std::snprintf(g_items_fixture[0].category_id, sizeof(g_items_fixture[0].category_id), "%s", "test");
    std::snprintf(g_items_fixture[0].config_key, sizeof(g_items_fixture[0].config_key), "%s", "testBool");
    g_items_fixture[0].type = contracts::SettingsItemType::kToggle;
    g_items_fixture[0].owner_enable = g_owner_enabled ? 1 : 0;
    std::snprintf(g_items_fixture[1].id, sizeof(g_items_fixture[1].id), "%s", "test.action");
    std::snprintf(g_items_fixture[1].category_id, sizeof(g_items_fixture[1].category_id), "%s", "test");
    g_items_fixture[1].type = contracts::SettingsItemType::kAction;
    g_catalog_view.generation = 1;
    g_catalog_view.category_count = 1;
    g_catalog_view.item_count = 2;
    g_catalog_view.categories = &g_category;
    g_catalog_view.items = g_items_fixture;
    g_item_availability = {};
    g_item_availability.size = sizeof(g_item_availability);
    g_item_availability.operable = 1;

    g_fake_components = {};
    g_fake_ui = {};
    g_fake_components.size = sizeof(g_fake_components);
    g_fake_components.destroy = &fake_destroy;
    g_fake_components.scroll_list_create = &fake_list_create;
    g_fake_components.scroll_list_get_row = &fake_list_get_row;
    g_fake_components.scroll_list_set_selection_enabled = &fake_list_selection_enabled;
    g_fake_components.text_button_create = &fake_text_button_create;
    g_fake_components.text_button_set_enabled = &fake_text_button_set_enabled;
    g_fake_components.text_button_draw = &fake_text_button_draw;
    g_fake_components.dispatch_native_event = &fake_dispatch_native_event;
    g_fake_components.back_request = &fake_back_request;
    g_fake_components.native_toggle_create = &fake_toggle_create;
    g_fake_components.native_toggle_set_value = &fake_toggle_set_value;
    g_fake_components.native_toggle_set_enabled = &fake_toggle_set_enabled;
    g_fake_components.draw_control_text = &fake_draw_control_text;
    g_fake_components.scroll_list_process = nullptr;
    g_components = &g_fake_components;
    g_fake_ui.is_main_thread = &fake_main_thread;
    g_fake_ui.touch_coordinates = &fake_touch_coordinates;
    g_fake_ui.hit_test = &fake_hit_test;
    g_ui = &g_fake_ui;
    g_root = g_fake_root;
    g_back = reinterpret_cast<void*>(0x4000);
    g_panel_active = false;
    g_touch_root = nullptr;

    g_fake_catalog = {};
    g_fake_catalog.size = sizeof(g_fake_catalog);
    g_fake_catalog.major = contracts::kSettingsCatalogMajor;
    g_fake_catalog.minor = contracts::kSettingsCatalogMinor;
    g_fake_catalog.snapshot_acquire = &fake_snapshot;
    g_fake_catalog.snapshot_release = &fake_snapshot_release;
    g_fake_catalog.query_item_availability = &fake_query_availability;
    g_catalog = &g_fake_catalog;
    g_fake_config = {};
    g_fake_config.size = sizeof(g_fake_config);
    g_fake_config.major = contracts::kSettingsConfigMajor;
    g_fake_config.minor = contracts::kSettingsConfigMinor;
    g_fake_config.read_bool = &fake_read_bool;
    g_fake_config.request_set_bool = &fake_request_bool;
    g_fake_config.query_request = &fake_query_request;
    g_config = &g_fake_config;
    g_item_count = 2;
    g_category_count = 1;
    g_generation = 1;
    g_items[0] = g_items_fixture[0];
    g_items[1] = g_items_fixture[1];
    g_categories[0] = g_category;
    g_item_state[0] = {};
    g_item_state[1] = {};
    g_availability[0] = {{}, true};
    g_availability[0].value = g_item_availability;
    g_availability[1] = {{}, true};
    g_availability[1].value = g_item_availability;
    g_toggle_requested[0] = false;
    g_desired_toggle[0] = 0;
    g_toggle_handles[0] = nullptr;
    g_toggle_enabled_cache_valid[0] = false;
    g_pending_action_item = -1;
    g_status[0] = '\0';
    assert(build_right_list(0));
    refresh_item_value(0);
    refresh_availability();
    assert(g_widget.enabled);
    assert(!g_right_selection_enabled);
}

void test_production_draw_uses_cached_description_geometry() {
    setup();
    std::snprintf(g_items[0].label, sizeof(g_items[0].label), "%s", "自动出售");
    std::snprintf(g_items[0].description, sizeof(g_items[0].description), "%s",
                  "控制自动出售扫描及背包买入，受保护装备、已穿戴、已强化、已镶嵌物品都会跳过，不改变其它规则。");
    std::snprintf(g_items[1].label, sizeof(g_items[1].label), "%s", "打开规则页");
    std::snprintf(g_items[1].description, sizeof(g_items[1].description), "%s",
                  "打开详细规则并查看各项设置。");
    assert(build_right_list(0));
    assert(g_right_row_height == settings_model::kMinimumRightRowHeight);
    assert(g_item_text_layout[0].description.lines.size() == 1);
    assert(g_item_text_layout[0].description.lines[0].compare(
        g_item_text_layout[0].description.lines[0].size() - 3, 3, "…") == 0);

    g_draw_records.clear();
    draw_right_row(nullptr, 0, reinterpret_cast<void*>(0x3000), 1);
    assert(g_draw_records.size() == 2);
    assert(g_draw_records[0].text == "自动出售");
    assert(g_draw_records[0].y == 8);
    assert(g_draw_records[0].x == 16);
    assert(g_draw_records[1].x == 16 && g_draw_records[1].y == settings_model::kDescriptionTop);
    assert(g_draw_records[1].text.compare(g_draw_records[1].text.size() - 3, 3, "…") == 0);

    g_draw_records.clear();
    g_action_widget.draw_calls = 0;
    draw_right_row(nullptr, 1, reinterpret_cast<void*>(0x3000), 0);
    bool saw_action_label = false;
    bool saw_old_hint = false;
    for (const DrawRecord& record : g_draw_records) {
        if (record.text == "打开") saw_action_label = true;
        if (record.text == "打开 ›") saw_old_hint = true;
    }
    assert(!saw_action_label && !saw_old_hint);
    assert(g_action_widget.draw_calls == 1 && g_action_widget.text == "打开");
    assert(g_action_widget.rect.x == g_widget.rect.x &&
           g_action_widget.rect.y == g_widget.rect.y &&
           g_action_widget.rect.w == g_widget.rect.w &&
           g_action_widget.rect.h == g_widget.rect.h);
    assert(g_action_widget.rect.x == settings_model::right_control_left(static_cast<int32_t>(kRightW)));
    assert(g_action_widget.rect.y == settings_model::toggle_top(g_right_row_height));
    assert(g_action_widget.rect.w == settings_model::kToggleWidth &&
           g_action_widget.rect.h == settings_model::kToggleHeight);
}

void finish_request(bool success, int32_t value, SettingsConfigApplyState apply) {
    g_persisted_value = value;
    g_request_state = success ? SettingsConfigRequestState::kSucceeded
                              : SettingsConfigRequestState::kFailed;
    g_apply_state = apply;
    poll_requests();
}

void click(int32_t value) {
    assert(g_widget.enabled && g_widget.on_changed != nullptr);
    g_widget.on_changed(g_widget.user, value);
    process_pending_toggles();
}

void test_repeated_toggle_receipts_same_widget() {
    setup();
    FakeWidget* const handle = &g_widget;
    const int initial_enabled_calls = g_widget.set_enabled_calls;

    click(1);
    assert(g_item_state[0].pending == Pending::kQueued && !g_widget.enabled);
    assert(g_widget.value == 0);
    g_persisted_value = 1;  // Mirror publishes before receipt; in-flight state must stay authoritative.
    refresh_item_value(0);
    assert(g_item_state[0].confirmed == 0 && g_widget.value == 0);
    const int queries_before = g_receipt_queries;
    poll_requests();
    assert(g_receipt_queries == queries_before + 1);
    assert(g_widget.value == 0 && !g_widget.enabled);
    finish_request(true, 1, SettingsConfigApplyState::kApplied);
    assert(g_item_state[0].confirmed == 1 && g_widget.value == 1 && g_widget.enabled);
    click(0);
    assert(g_item_state[0].pending == Pending::kQueued && !g_widget.enabled);
    finish_request(true, 0, SettingsConfigApplyState::kApplied);
    assert(g_item_state[0].confirmed == 0 && g_widget.value == 0 && g_widget.enabled);
    click(1);
    finish_request(true, 1, SettingsConfigApplyState::kApplied);
    assert(g_item_state[0].confirmed == 1 && g_widget.value == 1 && g_widget.enabled);

    assert(handle == &g_widget && g_creates == 1 && g_destroys == 0);
    assert(g_list_creates == 1 && g_list_destroys == 0);
    assert(g_widget.set_enabled_calls >= initial_enabled_calls + 6);
}

void test_failures_and_apply_pending_release_guard() {
    setup();
    click(0);
    assert(g_item_state[0].request_id == 0 && g_widget.value == 0 && g_widget.enabled);

    click(1);
    g_request_state = SettingsConfigRequestState::kFailed;
    g_apply_state = SettingsConfigApplyState::kRejected;
    poll_requests();
    assert(g_widget.value == 0 && g_widget.enabled);

    g_admit_fails = true;
    click(1);
    assert(g_item_state[0].request_id == 0 && g_widget.value == 0 && g_widget.enabled);
    g_admit_fails = false;
    click(1);
    finish_request(true, 1, SettingsConfigApplyState::kPendingFrame);
    assert(g_item_state[0].pending == Pending::kDonePending && g_widget.enabled);
    click(0);
    finish_request(true, 0, SettingsConfigApplyState::kPlatformPending);
    assert(g_item_state[0].pending == Pending::kDonePending && g_widget.enabled);
    click(1);
    finish_request(true, 1, SettingsConfigApplyState::kUnchanged);
    assert(g_widget.enabled);

    g_query_fails = true;
    click(0);
    poll_requests();
    assert(g_item_state[0].request_id == 0 && g_item_state[0].pending == Pending::kFailed);
    assert(g_widget.enabled && g_widget.value == 1);
}

void test_owner_and_mirror_gates() {
    setup();
    g_owner_enabled = false;
    g_items[0].owner_enable = 0;
    g_owner_operable = true;
    g_action_operable = true;
    refresh_availability();
    assert(g_widget.enabled);
    click(1);
    assert(g_item_state[0].pending == Pending::kQueued && !g_widget.enabled);
    finish_request(true, 1, SettingsConfigApplyState::kApplied);
    assert(g_widget.enabled && g_widget.value == 1);
    // Row selection cannot launch actions; settings rows have no selection callback.
    assert(g_pending_action_item == -1);

    g_owner_operable = false;
    g_action_operable = false;
    g_pending_action_item = -1;
    refresh_availability();
    assert(!g_widget.enabled);
    assert(g_status[0] == '\0');
    // Unavailable actions remain inert because only their child button can queue invocation.
    assert(g_pending_action_item == -1);
    g_widget.on_changed(g_widget.user, 0);
    process_pending_toggles();
    assert(g_item_state[0].request_id == 0 && g_persisted_value == 1);

    g_owner_operable = true;
    g_availability_fails = true;
    g_action_availability_fails = true;
    g_pending_action_item = -1;
    refresh_availability();
    assert(!g_widget.enabled);
    assert(g_status[0] != '\0');
    // Availability failure also leaves row text inert.
    assert(g_pending_action_item == -1);
    g_availability_fails = false;
    g_action_availability_fails = false;
    g_read_fails = true;
    refresh_item_value(0);
    assert(!g_widget.enabled && g_widget.value == 1);
}

void test_action_requires_independent_button_and_status_is_error_only() {
    setup();
    assert(!g_right_selection_enabled);
    g_items[1].type = contracts::SettingsItemType::kAction;
    std::snprintf(g_items[1].label, sizeof(g_items[1].label), "%s", "打开规则页");
    assert(build_right_list(0));
    assert(g_action_widget.on_click != nullptr);
    assert(g_action_widget.text == "打开" && g_action_widget.enabled);
    g_pending_action_item = -1;
    // Selecting/text hit on the row does not dispatch an action; only its child button does.
    g_components->scroll_list_set_selection_enabled(g_right, 0);
    assert(g_pending_action_item == -1);
    g_action_widget.on_click(g_action_widget.user);
    assert(g_pending_action_item == 1);

    click(1);
    assert(g_status[0] == '\0');
    assert(g_widget.value == 0 && !g_widget.enabled);
    g_request_state = SettingsConfigRequestState::kSucceeded;
    g_persisted_value = 1;
    poll_requests();
    assert(g_widget.value == 1 && g_widget.enabled);
    assert(g_action_widget.enabled);
    for (const DrawRecord& record : g_draw_records) {
        assert(record.text.find("保存中") == std::string::npos);
        assert(record.text.find("已保存") == std::string::npos);
    }
}

void test_action_control_tracks_availability() {
    setup();
    assert(g_action_widget.enabled);
    g_action_operable = false;
    refresh_availability();
    assert(!g_action_widget.enabled && g_availability[1].valid &&
           std::strcmp(g_availability[1].value.reason,
                       "settings action owner is unavailable") == 0);
    g_pending_action_item = -1;
    g_action_widget.on_click(g_action_widget.user);
    assert(g_pending_action_item == -1);

    g_action_operable = true;
    g_action_availability_fails = true;
    refresh_availability();
    assert(!g_action_widget.enabled && g_status[0] != '\0');
    g_pending_action_item = -1;
    g_action_widget.on_click(g_action_widget.user);
    assert(g_pending_action_item == -1);

    g_action_availability_fails = false;
    refresh_availability();
    assert(g_action_widget.enabled);
}

void test_settings_event_uses_one_scroll_list_dispatch() {
    setup();
    g_panel_active = true;
    int64_t point[] = {500, 200};
    const uint64_t param = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(point));
    assert(settings_panel_event(0x17, param, 11) == 1);
    assert(g_native_event_dispatches == 1 && g_native_event_root == g_right &&
           g_native_event_code == 0x17 && g_native_event_param == param &&
           g_native_event_param2 == 11);
    assert(settings_panel_event(0x19, param, 12) == 1);
    assert(g_native_event_dispatches == 2 && g_native_event_root == g_right &&
           g_native_event_code == 0x19 && g_native_event_param2 == 12);
    assert(settings_panel_event(0x18, param, 13) == 1);
    assert(g_native_event_dispatches == 3 && g_native_event_root == g_right &&
           g_native_event_code == 0x18 && g_native_event_param2 == 13);
    g_panel_active = false;
}

void test_back_button_requests_on_release() {
    setup();
    g_panel_active = true;
    g_back = reinterpret_cast<void*>(0x4000);
    int64_t inside[] = {10, 10};
    const uint64_t inside_param = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(inside));

    // 按下命中 nav：只置原生态并派发/消费，不请求返回（松开才触发）。
    assert(settings_panel_event(0x17, inside_param, 21) == 1);
    assert(g_back_requests == 0 && !g_back_close_pending);
    assert(g_native_event_dispatches == 1 && g_native_event_root == g_root &&
           g_native_event_code == 0x17);

    // 松开仍落于按钮内 → 原生 ExecuteProc 触发恰一次 back_request，并清手势根。
    assert(settings_panel_event(0x18, inside_param, 22) == 1);
    assert(g_back_requests == 1 && g_back_close_pending);
    assert(g_native_event_dispatches == 2 && g_native_event_root == g_root &&
           g_native_event_code == 0x18);
    // 手势根已清：后续 0x18 不再派发、不再请求。
    assert(settings_panel_event(0x18, inside_param, 23) == 1);
    assert(g_native_event_dispatches == 2 && g_back_requests == 1);

    // 按下后移出再松开：原生 TouchHandle 取消，不触发。
    g_back_requests = 0;
    g_back_close_pending = false;
    int64_t outside[] = {400, 300};
    const uint64_t outside_param = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(outside));
    assert(settings_panel_event(0x17, inside_param, 31) == 1);
    assert(settings_panel_event(0x19, outside_param, 32) == 1);
    assert(settings_panel_event(0x18, inside_param, 33) == 1);
    assert(g_back_requests == 0 && !g_back_close_pending);

    // nav 外按下落到右列表（g_right），松开不请求返回。
    assert(settings_panel_event(0x17, outside_param, 41) == 1);
    assert(g_native_event_dispatches > 0 && g_native_event_root == g_right);
    assert(settings_panel_event(0x18, outside_param, 42) == 1);
    assert(g_back_requests == 0);

    // bit63 系统子事件：不派发、不改手势态、消费返回 1；且不打断已开始的手势。
    assert(settings_panel_event(0x17, inside_param, 51) == 1);
    g_back_requests = 0;
    g_back_close_pending = false;
    const int dispatches_before = g_native_event_dispatches;
    constexpr uint64_t kSystemCancel = 0x80000001ULL;
    assert(settings_panel_event(kSystemCancel, inside_param, 52) == 1);
    assert(g_native_event_dispatches == dispatches_before);
    assert(settings_panel_event(0x18, inside_param, 53) == 1);
    assert(g_back_requests == 1 && g_back_close_pending);
    g_panel_active = false;
}

// 返回控件缺失（nav 创建失败的等价前置：裸父节点被拒或组件不可用 → g_back 为 nullptr）时，
// 按下不得请求返回，且必须落回左右列表分支（保证 g_back != nullptr 前置条件真实生效）。
void test_back_button_absent_does_not_request() {
    setup();
    g_panel_active = true;
    g_back = nullptr;
    g_back_requests = 0;
    g_back_close_pending = false;
    int64_t inside[] = {10, 10};
    const uint64_t inside_param = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(inside));
    assert(settings_panel_event(0x17, inside_param, 51) == 1);
    assert(g_back_requests == 0 && !g_back_close_pending);
    assert(g_native_event_dispatches == 1 && g_native_event_root == g_right);
    g_panel_active = false;
}

void test_actions_use_shared_factory_across_categories() {
    setup();
    g_category_count = 2;
    std::snprintf(g_categories[1].id, sizeof(g_categories[1].id), "%s", "other");
    std::snprintf(g_categories[1].title, sizeof(g_categories[1].title), "%s", "其它");
    std::snprintf(g_items[1].category_id, sizeof(g_items[1].category_id), "%s", "other");
    g_fake_row_count = 1;

    const int initial_creates = g_action_creates;
    const int initial_destroys = g_action_destroys;
    assert(build_right_list(0));  // toggle-only category removes the old action child with its list.
    assert(!g_action_live && g_action_destroys == initial_destroys + 1);
    assert(build_right_list(1));  // action-only category recreates the same native text-button pattern.
    assert(g_action_live && g_action_creates == initial_creates + 1);
    assert(g_action_widget.text == "打开" && g_action_widget.enabled);
    g_pending_action_item = -1;
    g_action_widget.on_click(g_action_widget.user);
    assert(g_pending_action_item == 1);
    assert(build_right_list(0));
    assert(!g_action_live && g_action_destroys == initial_destroys + 2);
}

void run_controlled_legacy_latch_red_case() {
    setup();
    click(1);
    assert(!g_widget.enabled && g_item_state[0].pending == Pending::kQueued);

    // Reproduce the verified old mechanism only in the fixture: terminal persistence updates
    // the widget value but omits production poll_requests()' enabled-state resynchronization.
    g_persisted_value = 1;
    g_item_state[0].confirmed = 1;
    g_item_state[0].confirmed_valid = true;
    g_item_state[0].request_id = 0;
    set_item_message(0, Pending::kNone, "");
    (void)fake_toggle_set_value(&g_widget, 1);
    std::fprintf(stderr, "LEGACY RED: disabled latch after terminal receipt: enabled=%d, value=%d; "
                         "second click assertion follows\n",
                 g_widget.enabled ? 1 : 0, g_widget.value);
    click(0);
}

}  // namespace

namespace qol::features::core_ui {
bool settings_panels_open(const char*) { return false; }
void settings_panels_close(const char*) {}
void more_games_bind_tick(bool, MoreGamesBindOps&) {}
}  // namespace qol::features::core_ui

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--legacy-latch-red") == 0) {
        run_controlled_legacy_latch_red_case();
        return 0;
    }
    test_repeated_toggle_receipts_same_widget();
    test_failures_and_apply_pending_release_guard();
    test_owner_and_mirror_gates();
    test_action_requires_independent_button_and_status_is_error_only();
    test_action_control_tracks_availability();
    test_settings_event_uses_one_scroll_list_dispatch();
    test_back_button_requests_on_release();
    test_back_button_absent_does_not_request();
    test_actions_use_shared_factory_across_categories();
    test_production_draw_uses_cached_description_geometry();
    std::puts("settings_panel_widget_tests: production settings.cpp widget path passed");
}
