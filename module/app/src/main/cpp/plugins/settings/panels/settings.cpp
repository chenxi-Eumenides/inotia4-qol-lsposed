#include "settings.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "../../../contracts/panels.h"
#include "../../../contracts/ui.h"
#include "../../../contracts/ui_theme.h"
#include "log.h"
#include "settings_binding.h"
#include "settings_model.h"
#include "feature_model.h"
#include "../feature.h"

namespace qol::features::core_ui {
namespace {
using contracts::QolUiHandle;
using contracts::UiRect;
using contracts::FeatureCategory;
using contracts::FeatureType;
constexpr int64_t kPanelW = 960, kPanelH = 640;
constexpr int64_t kLeftX = 24, kLeftY = 100, kLeftW = 300, kLeftH = 444;
constexpr int64_t kRightX = 364, kRightY = 100, kRightW = 572, kRightH = 444;
constexpr int32_t kLeftRowH = 73;
constexpr int32_t kRightRowH = settings_model::kMinimumRightRowHeight;
constexpr int64_t kBackAbsX = contracts::ui_theme::kNavBoxAbsX;
constexpr int64_t kBackAbsY = contracts::ui_theme::kNavBoxAbsY;
constexpr int64_t kBackW = contracts::ui_theme::kNavBoxW;
constexpr int64_t kBackH = contracts::ui_theme::kNavBoxH;
constexpr int32_t kNativeControlActive = 0x20;
constexpr int32_t kNativeControlInactive = 0x21;
constexpr size_t kFeatureControlReadActualSize =
    offsetof(contracts::QolFeatureControlApi, read_actual) +
    sizeof(((contracts::QolFeatureControlApi*)nullptr)->read_actual);
// Android CMake Debug omits NDEBUG; optimized configurations define it.
#if defined(NDEBUG)
constexpr bool kShowHidden = false;
#else
constexpr bool kShowHidden = true;
#endif

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolUiAdapter* g_ui = nullptr;
const contracts::QolUiComponents* g_components = nullptr;
const contracts::QolFeatureControlApi* g_control = nullptr;
QolUiHandle g_root = nullptr, g_left = nullptr, g_right = nullptr, g_back = nullptr;
QolUiHandle g_pressed_list = nullptr;
bool g_back_pressed = false;
std::mutex g_mtx;
bool g_panel_active = false;
uint64_t g_generation = 0, g_frame = 0;
int g_selected_category = 0, g_pending_category = -1, g_pending_action_item = -1;
std::vector<contracts::FeatureCategoryDefinition> g_categories;
std::vector<feature_model::Item> g_items;
std::vector<uint32_t> g_right_indices;
struct ItemState {
    bool valid = false;
    uint8_t confirmed = 0;
    uint64_t request_id = 0;
    bool gesture = false;
    int32_t desired = 0;
    QolUiHandle toggle = nullptr, action = nullptr;
    char message[128]{};
};
std::vector<ItemState> g_states;
struct EntryContinuation {
    bool active = false;
    std::string plugin_id;
    std::string feature_id;
    uint64_t request_id = 0;
    uint64_t page_generation = 0;
    uint32_t process_ticks = 0;
    bool persisted = false;
};
constexpr uint32_t kEntryWaitMaxProcessTicks = 300;
EntryContinuation g_entry_continuation;
struct ToggleContext { uint32_t item_index; uint64_t generation; };
std::vector<ToggleContext> g_contexts;
char g_status[256]{};
char g_module_version[64]{};
int g_summary_categories = 0, g_summary_items = 0;

bool main_thread() {
    return g_ui != nullptr && g_ui->is_main_thread != nullptr && g_ui->is_main_thread() != 0;
}
void set_status(const char* text) {
    std::snprintf(g_status, sizeof(g_status), "%s", text == nullptr ? "" : text);
}
void item_error(uint32_t i, const char* text) {
    std::snprintf(g_states[i].message, sizeof(g_states[i].message), "%s", text);
    std::snprintf(g_status, sizeof(g_status), "%s：%s", g_items[i].label, text);
}
void clear_entry_continuation() { g_entry_continuation = EntryContinuation{}; }
bool feature_control_has_read_actual() {
    return g_control != nullptr && g_control->struct_size >= kFeatureControlReadActualSize &&
        g_control->read_actual != nullptr;
}
bool entry_continuation_matches(const feature_model::Item& item) {
    return g_entry_continuation.active &&
        g_entry_continuation.page_generation == g_generation &&
        g_entry_continuation.plugin_id == item.plugin_id &&
        g_entry_continuation.feature_id == item.feature_id;
}
bool toggle_enabled(uint32_t i) {
    const auto& s = g_states[i];
    return feature_model::toggle_enabled(s.valid, s.request_id, s.gesture);
}
bool action_enabled(uint32_t i) {
    const auto& s = g_states[i];
    const bool request_pending = s.request_id != 0 || g_pending_action_item == static_cast<int>(i) ||
        g_entry_continuation.active;
    return feature_model::entry_enabled(g_items[i], s.valid, request_pending, s.gesture);
}
void sync_controls(uint32_t i) {
    auto& s = g_states[i];
    if (g_components == nullptr) return;
    if (s.toggle != nullptr) {
        const auto value = g_components->native_toggle_set_value(s.toggle, s.confirmed);
        const auto enabled = g_components->native_toggle_set_enabled(s.toggle, toggle_enabled(i));
        if (!contracts::succeeded(value) || !contracts::succeeded(enabled))
            set_status("开关状态更新失败");
    }
    if (s.action != nullptr && !contracts::succeeded(
            g_components->text_button_set_enabled(s.action, action_enabled(i))))
        set_status("入口按钮状态更新失败");
}
void refresh_item_value(uint32_t i) {
    auto& s = g_states[i];
    const auto& item = g_items[i];
    if (g_control == nullptr) { s.valid = false; return; }
    if (s.request_id == 0 && !s.gesture) {
        uint8_t value = 0;
        s.valid = contracts::succeeded(g_control->read_feature(
            g_control->context, item.plugin_id, item.feature_id, &value));
        if (s.valid) s.confirmed = value;
    }
    sync_controls(i);
}
void refresh_all_values() {
    for (uint32_t i = 0; i < g_items.size(); ++i) refresh_item_value(i);
}
bool load_catalog() {
    clear_entry_continuation();
    g_items.clear(); g_categories.clear(); g_states.clear(); g_contexts.clear();
    ++g_generation;
    g_pending_action_item = -1;
    const contracts::QolFeatureCatalogPlugin* plugins = nullptr;
    uint32_t count = 0;
    const contracts::FeatureCategoryDefinition* categories = nullptr;
    uint32_t category_count = 0;
    if (g_control == nullptr || !contracts::succeeded(
        g_control->read_catalog(g_control->context, &plugins, &count)) ||
        !contracts::succeeded(g_control->read_categories(
            g_control->context, &categories, &category_count)) ||
        !feature_model::build(plugins, count, kShowHidden, g_items) ||
        !feature_model::build_categories(categories, category_count, kShowHidden,
                                         g_items, g_categories)) return false;
    g_states.resize(g_items.size());
    for (uint32_t i = 0; i < g_items.size(); ++i) g_contexts.push_back({i, g_generation});
    std::lock_guard<std::mutex> lock(g_mtx);
    g_summary_categories = static_cast<int>(g_categories.size());
    g_summary_items = static_cast<int>(g_items.size());
    return true;
}
void draw_row_background(QolUiHandle row, int32_t width, int32_t height, uint8_t selected) {
    int64_t x = 0, y = 0;
    if (!contracts::succeeded(g_ui->control_absolute_origin(row, &x, &y))) return;
    if (selected) (void)g_ui->fill_rect({x, y, width, height}, contracts::ui_theme::kRowSelectedBg);
    (void)g_ui->fill_rect({x, y + height - 2, width, 2}, contracts::ui_theme::kFrameGold);
}
void draw_left_row(void*, uint32_t index, QolUiHandle row, uint8_t selected) {
    if (index >= g_categories.size()) return;
    draw_row_background(row, kLeftW, kLeftRowH, selected);
    (void)g_components->draw_control_text(row, g_categories[index].name,
        16, 18, contracts::ui_theme::kTextGold, 0, 1);
}
void draw_right_row(void*, uint32_t row_index, QolUiHandle row, uint8_t selected) {
    if (row_index >= g_right_indices.size()) return;
    const auto i = g_right_indices[row_index];
    const auto& item = g_items[i];
    const auto& s = g_states[i];
    draw_row_background(row, kRightW, kRightRowH, selected);
    const int32_t control_left = settings_model::right_control_left(kRightW);
    const int32_t text_right = control_left;
    const int32_t text_width = settings_model::description_width_for_toggle(text_right);
    const auto layout = settings_model::row_text_layout(
        settings_model::use_item_description(item.description), kRightRowH);
    const auto title = settings_model::truncate_text(item.label, text_width);
    if (!title.lines.empty()) (void)g_components->draw_control_text(row, title.lines[0].c_str(),
        16, layout.title_y, toggle_enabled(i) ? contracts::ui_theme::kTextGold :
        contracts::ui_theme::kTextDisabled, 0, 1);
    const auto description = settings_model::truncate_text(item.description, text_width);
    if (!description.lines.empty()) (void)g_components->draw_control_text(row,
        description.lines[0].c_str(), 16, layout.description_y,
        contracts::ui_theme::kTextNormal, 0, 1);
    if (s.toggle != nullptr) (void)g_components->native_toggle_draw(s.toggle);
    if (s.action != nullptr) (void)g_components->text_button_draw(s.action);
}
void on_left_select(void*, uint32_t i) { g_pending_category = static_cast<int>(i); }
void native_toggle_changed(void* user, int32_t desired) {
    const auto* context = static_cast<const ToggleContext*>(user);
    if (!g_panel_active || context == nullptr || context->generation != g_generation ||
        context->item_index >= g_items.size()) return;
    const auto i = context->item_index;
    if (!toggle_enabled(i)) return;
    g_states[i].desired = desired != 0;
    g_states[i].gesture = true;
    sync_controls(i);
}
void action_button_clicked(void* user) {
    const auto* context = static_cast<const ToggleContext*>(user);
    if (!g_panel_active || context == nullptr || context->generation != g_generation ||
        context->item_index >= g_items.size() || g_pending_action_item >= 0 ||
        !action_enabled(context->item_index)) return;
    g_pending_action_item = static_cast<int>(context->item_index);
    sync_controls(context->item_index);
}
void destroy_right_list() {
    for (auto& s : g_states) { s.toggle = nullptr; s.action = nullptr; }
    if (g_right != nullptr && g_components != nullptr) (void)g_components->destroy(g_right);
    g_right = nullptr; g_right_indices.clear();
}
bool build_right_list(int category) {
    destroy_right_list();
    if (category < 0 || static_cast<size_t>(category) >= g_categories.size()) return false;
    for (uint32_t i = 0; i < g_items.size(); ++i)
        if (g_items[i].category == g_categories[category].category) g_right_indices.push_back(i);
    const contracts::UiScrollListDesc list{{kRightX, kRightY, kRightW, kRightH}, kRightRowH,
        static_cast<uint32_t>(g_right_indices.size()), -1, &draw_right_row, nullptr, nullptr};
    if (!contracts::succeeded(g_components->scroll_list_create(&list, g_root, &g_right))) return false;
    if (!contracts::succeeded(g_components->scroll_list_set_selection_enabled(g_right, 0))) {
        destroy_right_list(); return false;
    }
    for (uint32_t row = 0; row < g_right_indices.size(); ++row) {
        const auto i = g_right_indices[row];
        QolUiHandle handle = nullptr;
        if (!contracts::succeeded(g_components->scroll_list_get_row(g_right, row, &handle))) {
            destroy_right_list(); return false;
        }
        const int32_t control_left = settings_model::right_control_left(kRightW);
        const int32_t control_top = settings_model::toggle_top(kRightRowH);
        if (g_items[i].type == FeatureType::kEntry) {
            const contracts::UiButtonDesc button{{control_left, control_top,
                settings_model::kToggleWidth, settings_model::kToggleHeight},
                g_items[i].action_label, contracts::kUiTextButtonFontLarger,
                &action_button_clicked, &g_contexts[i]};
            if (!contracts::succeeded(g_components->text_button_create(&button, handle,
                &g_states[i].action))) { destroy_right_list(); return false; }
        } else {
            const contracts::UiNativeToggleDesc toggle{{control_left, control_top,
                settings_model::kToggleWidth, settings_model::kToggleHeight}, g_states[i].confirmed,
                static_cast<uint8_t>(toggle_enabled(i)), &native_toggle_changed, &g_contexts[i]};
            if (!contracts::succeeded(g_components->native_toggle_create(&toggle, handle,
                &g_states[i].toggle))) { destroy_right_list(); return false; }
        }
        sync_controls(i);
    }
    return true;
}
void destroy_page() {
    { std::lock_guard<std::mutex> lock(g_mtx); g_panel_active = false;
      g_summary_categories = 0; g_summary_items = 0; }
    clear_entry_continuation();
    ++g_generation;
    g_pending_category = -1; g_pending_action_item = -1;
    destroy_right_list();
    if (g_components != nullptr) {
        if (g_left != nullptr) (void)g_components->destroy(g_left);
        if (g_back != nullptr) (void)g_components->destroy(g_back);
    }
    if (g_ui != nullptr && g_root != nullptr) (void)g_ui->delete_control(g_root);
    g_root = nullptr; g_left = nullptr; g_back = nullptr; g_pressed_list = nullptr;
    g_back_pressed = false; g_selected_category = 0;
    g_states.clear(); g_contexts.clear(); g_items.clear(); g_categories.clear();
    g_frame = 0; set_status("");
}
void process_pending_toggles() {
    for (uint32_t i = 0; i < g_items.size(); ++i) {
        auto& s = g_states[i];
        if (!s.gesture) continue;
        s.gesture = false;
        if (!toggle_enabled(i) || g_control == nullptr) { sync_controls(i); continue; }
        if (s.confirmed == s.desired) { sync_controls(i); continue; }
        const auto& item = g_items[i];
        contracts::FeatureReceipt receipt{};
        const auto result = g_control->request_set_feature(g_control->context,
            item.plugin_id, item.feature_id, s.desired, &receipt);
        if (!contracts::succeeded(result) || receipt.admission != contracts::FeatureAdmission::kAdmitted ||
            receipt.request_id == 0) item_error(i, settings_model::request_error_text(result.code));
        else { s.request_id = receipt.request_id;
               std::snprintf(s.message, sizeof(s.message), "保存中…"); }
        sync_controls(i);
    }
}
void poll_requests() {
    if (g_control == nullptr) return;
    for (uint32_t i = 0; i < g_items.size(); ++i) {
        auto& s = g_states[i];
        if (s.request_id == 0) continue;
        contracts::FeatureReceipt receipt{};
        const auto result = g_control->receipt(g_control->context, s.request_id, &receipt);
        if (!contracts::succeeded(result)) {
            s.request_id = 0; item_error(i, "保存状态未知，请核对当前设置");
        } else if (receipt.persistence == contracts::FeaturePersistence::kPending) continue;
        else if (receipt.persistence == contracts::FeaturePersistence::kFailed ||
                 receipt.admission != contracts::FeatureAdmission::kAdmitted) {
            s.request_id = 0; item_error(i, "保存失败，设置未更改");
        } else {
            s.request_id = 0;
            std::snprintf(s.message, sizeof(s.message), "已保存；运行状态由宿主更新");
        }
        // Receipt confirms persistence, not the latest value or lifecycle completion.
        refresh_item_value(i);
    }
}
void process_pending_action() {
    const int i = g_pending_action_item; g_pending_action_item = -1;
    if (i < 0 || static_cast<size_t>(i) >= g_items.size() || g_control == nullptr) return;
    refresh_item_value(i);
    auto& state = g_states[static_cast<size_t>(i)];
    const auto& item = g_items[static_cast<size_t>(i)];
    if (!action_enabled(static_cast<uint32_t>(i))) {
        item_error(static_cast<uint32_t>(i), settings_model::action_error_text(kQolErrBusy));
        sync_controls(static_cast<uint32_t>(i));
        return;
    }
    if (state.confirmed != 0) {
        if (!feature_control_has_read_actual()) {
            item_error(static_cast<uint32_t>(i),
                       settings_model::action_error_text(kQolErrNotReady));
            sync_controls(static_cast<uint32_t>(i));
            return;
        }
        int32_t ignored_host_state = 0;
        uint8_t actual_enabled = 0;
        const auto actual_result = g_control->read_actual(g_control->context,
            item.plugin_id, &ignored_host_state, &actual_enabled);
        const int32_t actual_error = !contracts::succeeded(actual_result) ?
            actual_result.code : kQolErrNotReady;
        if (!contracts::succeeded(actual_result) || actual_enabled != 1) {
            item_error(static_cast<uint32_t>(i),
                       settings_model::action_error_text(actual_error));
            sync_controls(static_cast<uint32_t>(i));
            return;
        }
        if (g_control->invoke_entry == nullptr) {
            item_error(static_cast<uint32_t>(i),
                       settings_model::action_error_text(kQolErrNotReady));
        } else {
            const auto result = g_control->invoke_entry(
                g_control->context, item.plugin_id, item.feature_id);
            if (!contracts::succeeded(result))
                item_error(static_cast<uint32_t>(i),
                           settings_model::action_error_text(result.code));
            else
                set_status("");
        }
        sync_controls(static_cast<uint32_t>(i));
        return;
    }

    if (!feature_control_has_read_actual() ||
        g_control->request_set_feature == nullptr || g_control->receipt == nullptr) {
        item_error(static_cast<uint32_t>(i),
                   settings_model::action_error_text(kQolErrNotReady));
        sync_controls(static_cast<uint32_t>(i));
        return;
    }

    contracts::FeatureReceipt receipt{};
    const auto result = g_control->request_set_feature(
        g_control->context, item.plugin_id, item.feature_id, 1, &receipt);
    if (!contracts::succeeded(result) ||
        receipt.admission != contracts::FeatureAdmission::kAdmitted || receipt.request_id == 0) {
        const int32_t error = !contracts::succeeded(result) ? result.code :
            (receipt.error_code != 0 ? receipt.error_code : kQolErrBlocked);
        item_error(static_cast<uint32_t>(i), settings_model::request_error_text(error));
        sync_controls(static_cast<uint32_t>(i));
        return;
    }

    g_entry_continuation.active = true;
    g_entry_continuation.plugin_id = item.plugin_id;
    g_entry_continuation.feature_id = item.feature_id;
    g_entry_continuation.request_id = receipt.request_id;
    g_entry_continuation.page_generation = g_generation;
    g_entry_continuation.process_ticks = 0;
    g_entry_continuation.persisted = false;
    set_status("");
    sync_controls(static_cast<uint32_t>(i));
}
int entry_continuation_item_index() {
    if (!g_entry_continuation.active ||
        g_entry_continuation.page_generation != g_generation || !g_panel_active) return -1;
    for (uint32_t i = 0; i < g_items.size(); ++i) {
        if (entry_continuation_matches(g_items[i])) return static_cast<int>(i);
    }
    return -1;
}
void finish_entry_continuation(uint32_t item_index, const char* error) {
    clear_entry_continuation();
    if (item_index >= g_items.size()) return;
    if (error != nullptr) item_error(item_index, error);
    sync_controls(item_index);
}
void poll_entry_continuation() {
    if (!g_entry_continuation.active) return;
    const int item_index = entry_continuation_item_index();
    if (item_index < 0) {
        clear_entry_continuation();
        return;
    }
    const auto index = static_cast<uint32_t>(item_index);
    if (g_control == nullptr || g_control->receipt == nullptr ||
        g_control->read_feature == nullptr || g_control->invoke_entry == nullptr ||
        !feature_control_has_read_actual()) {
        finish_entry_continuation(index, settings_model::action_error_text(kQolErrNotReady));
        return;
    }
    if (++g_entry_continuation.process_ticks > kEntryWaitMaxProcessTicks) {
        finish_entry_continuation(index, settings_model::action_error_text(kQolErrTimeout));
        return;
    }

    if (!g_entry_continuation.persisted) {
        contracts::FeatureReceipt receipt{};
        const auto result = g_control->receipt(
            g_control->context, g_entry_continuation.request_id, &receipt);
        if (!contracts::succeeded(result)) {
            finish_entry_continuation(index, settings_model::action_error_text(result.code));
            return;
        }
        if (receipt.request_id != g_entry_continuation.request_id ||
            receipt.admission != contracts::FeatureAdmission::kAdmitted) {
            finish_entry_continuation(index,
                settings_model::request_error_text(receipt.error_code != 0 ?
                    receipt.error_code : kQolErrInternal));
            return;
        }
        if (receipt.persistence == contracts::FeaturePersistence::kPending) {
            if (g_entry_continuation.process_ticks >= kEntryWaitMaxProcessTicks)
                finish_entry_continuation(index,
                    settings_model::action_error_text(kQolErrTimeout));
            return;
        }
        if (receipt.persistence != contracts::FeaturePersistence::kPersisted) {
            finish_entry_continuation(index,
                settings_model::request_error_text(receipt.error_code != 0 ?
                    receipt.error_code : kQolErrInternal));
            return;
        }
        g_entry_continuation.persisted = true;
    }

    uint8_t feature_enabled = 0;
    const auto feature_result = g_control->read_feature(g_control->context,
        g_entry_continuation.plugin_id.c_str(), g_entry_continuation.feature_id.c_str(),
        &feature_enabled);
    if (!contracts::succeeded(feature_result)) {
        finish_entry_continuation(index,
            settings_model::action_error_text(feature_result.code));
        return;
    }
    if (feature_enabled != 1) {
        finish_entry_continuation(index,
            settings_model::action_error_text(kQolErrNotReady));
        return;
    }
    g_states[index].valid = true;
    g_states[index].confirmed = feature_enabled;

    int32_t ignored_host_state = 0;
    uint8_t actual_enabled = 0;
    const auto actual_result = g_control->read_actual(g_control->context,
        g_entry_continuation.plugin_id.c_str(), &ignored_host_state, &actual_enabled);
    if (actual_result.code == kQolErrNotReady) {
        if (g_entry_continuation.process_ticks >= kEntryWaitMaxProcessTicks)
            finish_entry_continuation(index,
                settings_model::action_error_text(kQolErrTimeout));
        return;
    }
    if (!contracts::succeeded(actual_result)) {
        finish_entry_continuation(index,
            settings_model::action_error_text(actual_result.code));
        return;
    }
    if (actual_enabled != 1) {
        if (g_entry_continuation.process_ticks >= kEntryWaitMaxProcessTicks)
            finish_entry_continuation(index,
                settings_model::action_error_text(kQolErrTimeout));
        return;
    }

    if (!g_panel_active || g_entry_continuation.page_generation != g_generation) {
        clear_entry_continuation();
        return;
    }
    const auto invoke_result = g_control->invoke_entry(g_control->context,
        g_entry_continuation.plugin_id.c_str(), g_entry_continuation.feature_id.c_str());
    finish_entry_continuation(index, contracts::succeeded(invoke_result) ? nullptr :
        settings_model::action_error_text(invoke_result.code));
    if (contracts::succeeded(invoke_result)) set_status("");
}
bool components_complete() {
    return g_components != nullptr && g_components->scroll_list_create &&
        g_components->scroll_list_get_row && g_components->scroll_list_set_selection_enabled &&
        g_components->scroll_list_process && g_components->scroll_list_draw &&
        g_components->native_toggle_create && g_components->native_toggle_set_value &&
        g_components->native_toggle_set_enabled && g_components->native_toggle_draw &&
        g_components->text_button_create && g_components->text_button_set_enabled &&
        g_components->text_button_draw && g_components->destroy &&
        g_components->draw_control_text && g_components->draw_container &&
        g_components->dispatch_native_event;
}
void settings_panel_enter() {
    if (!main_thread() || !components_complete() || g_control == nullptr) return;
    if (g_root != nullptr) { refresh_all_values(); return; }
    if (!g_ui->create_control || !g_ui->set_rect || !g_ui->delete_control ||
        !g_ui->control_absolute_origin || !g_ui->fill_rect || !g_ui->begin_frame ||
        !g_ui->end_frame || !g_ui->hit_test) return;
    if (!load_catalog()) { set_status("功能目录读取失败"); return; }
    refresh_all_values();
    int32_t x = 0, y = 0;
    if (g_ui->screen_inset_width) (void)g_ui->screen_inset_width(&x);
    if (g_ui->screen_inset_height) (void)g_ui->screen_inset_height(&y);
    if (!contracts::succeeded(g_ui->create_control(0, nullptr, &g_root)) || g_root == nullptr) return;
    (void)g_ui->set_rect(g_root, {x, y, kPanelW, kPanelH});
    if (g_components->nav_button_create) {
        const contracts::UiNavButtonDesc nav{{kBackAbsX - x, kBackAbsY - y, kBackW, kBackH},
            contracts::kUiNavButtonAuto, nullptr, nullptr};
        (void)g_components->nav_button_create(&nav, g_root, &g_back);
    }
    if (g_ui->load_image_unit) {
        (void)g_ui->load_image_unit(0x4f); (void)g_ui->load_image_unit(0x50);
        (void)g_ui->load_image_unit(0x0f);
    }
    const char* version = contracts::host_sdk_module_version(g_sdk);
    std::snprintf(g_module_version, sizeof(g_module_version), "%s", version == nullptr ? "" : version);
    if (!g_categories.empty()) {
        const contracts::UiScrollListDesc list{{kLeftX, kLeftY, kLeftW, kLeftH}, kLeftRowH,
            static_cast<uint32_t>(g_categories.size()), 0, &draw_left_row, &on_left_select, nullptr};
        if (!contracts::succeeded(g_components->scroll_list_create(&list, g_root, &g_left)) ||
            !build_right_list(0)) { destroy_page(); return; }
    }
    std::lock_guard<std::mutex> lock(g_mtx); g_panel_active = true;
}
void settings_panel_process() {
    if (!main_thread() || !g_panel_active || g_root == nullptr) return;
    ++g_frame;
    if (g_pending_category >= 0) {
        if (g_pending_category != g_selected_category && build_right_list(g_pending_category))
            g_selected_category = g_pending_category;
        g_pending_category = -1;
    }
    poll_entry_continuation(); process_pending_action(); poll_requests();
    if (g_frame % 30 == 0) refresh_all_values();
    process_pending_toggles();
    if (g_left) (void)g_components->scroll_list_process(g_left);
    if (g_right) (void)g_components->scroll_list_process(g_right);
    if (!contracts::succeeded(g_ui->begin_frame())) return;
    int64_t x = 0, y = 0;
    if (contracts::succeeded(g_ui->control_absolute_origin(g_root, &x, &y))) {
        if (g_ui->draw_image_part) {
            (void)g_ui->draw_image_part(0x4f, 0, x + kPanelW / 2 - 300, y + kPanelH / 2, 2, 1);
            (void)g_ui->draw_image_part(0x50, 0, x + kPanelW / 2 + 300, y + kPanelH / 2, 2, 1);
        }
        for (const auto& rect : {UiRect{x + kLeftX - 8, y + kLeftY - 8, kLeftW + 32, kLeftH + 16},
             UiRect{x + kRightX - 8, y + kRightY - 8, kRightW + 32, kRightH + 16}}) {
            (void)g_ui->fill_rect(rect, contracts::ui_theme::kPanelBg);
            (void)g_components->draw_container(rect);
        }
    }
    char title[128]{};
    if (g_module_version[0]) std::snprintf(title, sizeof(title), "QoL 模块设置 v%s", g_module_version);
    else std::snprintf(title, sizeof(title), "QoL 模块设置");
    (void)g_components->draw_control_text(g_root, title, kPanelW / 2, 24,
        contracts::ui_theme::kTextGold, 2, 1);
    if (g_left) (void)g_components->scroll_list_draw(g_left);
    if (g_right) (void)g_components->scroll_list_draw(g_right);
    const auto status = settings_model::truncate_text(g_status, kPanelW - 48);
    if (!status.lines.empty()) (void)g_components->draw_control_text(g_root, status.lines[0].c_str(),
        24, kPanelH - 72, contracts::ui_theme::kError, 0, 1);
    if (g_back && g_components->nav_button_draw) (void)g_components->nav_button_draw(g_back);
    (void)g_ui->end_frame();
}
void settings_panel_f3() { if (main_thread()) destroy_page(); }
void settings_panel_f4() {}
uint32_t settings_panel_event(uint64_t event, uint64_t param, uint64_t param2) {
    if (!main_thread() || !g_panel_active || g_root == nullptr) return 1;
    if (event != 0x17 && event != 0x18 && event != 0x19) return 1;
    if (event == 0x19) {
        uint32_t consumed = 0;
        (void)g_components->dispatch_native_event(g_pressed_list ? g_pressed_list : g_root,
            event, param, param2, &consumed);
        return consumed;
    }
    int64_t x = 0, y = 0;
    if (!param || !g_ui->touch_coordinates || !contracts::succeeded(
        g_ui->touch_coordinates(param, &x, &y))) return 1;
    if (g_back) {
        const bool inside = g_ui->hit_test(g_back, x, y, {0, 0, kBackW, kBackH}) > 0;
        if ((event == 0x17 && inside) || (event == 0x18 && g_back_pressed)) {
            g_back_pressed = event == 0x17;
            uint32_t consumed = 0;
            (void)g_components->dispatch_native_event(g_root, event, param, param2, &consumed);
            return consumed;
        }
    }
    if (event == 0x17) g_pressed_list = g_right &&
        g_ui->hit_test(g_right, x, y, {0, 0, kRightW, kRightH}) > 0 ? g_right :
        g_left && g_ui->hit_test(g_left, x, y, {0, 0, kLeftW, kLeftH}) > 0 ? g_left : nullptr;
    if (!g_pressed_list) return 1;
    uint32_t consumed = 0;
    (void)g_components->dispatch_native_event(g_pressed_list, event, param, param2, &consumed);
    if (event == 0x18) g_pressed_list = nullptr;
    return 1;
}
std::string result_ok() { return "{\"ok\":true}"; }
std::string result_error(const char* message) {
    char buffer[256]{}; uint32_t size = 0;
    if (g_sdk && g_sdk->result.write_error && contracts::succeeded(
        g_sdk->result.write_error(message, buffer, sizeof(buffer), &size))) return std::string(buffer, size);
    return "{\"ok\":false,\"error\":\"settings unavailable\"}";
}
const char* ui_screen_name() {
    const char* name = nullptr;
    if (g_ui && g_ui->screen_name && contracts::succeeded(g_ui->screen_name(&name)) && name) return name;
    return "";
}
void more_games_clicked(void*) {
    if (plugin_enabled()) (void)settings_panels_open("settings");
}
QolUiHandle g_more_games_original = nullptr;
QolUiHandle g_more_games_replacement = nullptr;
bool g_more_games_original_active = false;
bool g_more_games_original_active_valid = false;
bool g_more_games_binding_ready = false;
std::atomic<int32_t> g_more_games_bind{0};
std::atomic<bool> g_more_games_error{false};
bool release_more_games_runtime();

class RuntimeMoreGamesOriginalStateOps final : public MoreGamesOriginalStateOps {
public:
    explicit RuntimeMoreGamesOriginalStateOps(QolUiHandle control) : control_(control) {}

    bool set_show(bool shown) override {
        return g_ui != nullptr && g_ui->set_control_show != nullptr &&
            contracts::succeeded(g_ui->set_control_show(control_, shown ? 1 : 0));
    }
    bool set_enabled(bool enabled) override {
        return g_ui != nullptr && g_ui->set_button_enabled != nullptr &&
            contracts::succeeded(g_ui->set_button_enabled(control_, enabled ? 1 : 0));
    }
    bool set_active(bool active) override {
        return g_ui != nullptr && g_ui->main_menu_more_games_set_active != nullptr &&
            contracts::succeeded(g_ui->main_menu_more_games_set_active(
                control_, active ? 1 : 0));
    }

private:
    QolUiHandle control_;
};

class RuntimeMoreGamesBindOps final : public MoreGamesBindOps {
public:
    MoreGamesBindingState binding_state() override {
        if (!g_more_games_binding_ready || g_more_games_original == nullptr ||
            g_more_games_replacement == nullptr ||
            g_ui == nullptr || g_ui->main_menu_more_games_snapshot == nullptr ||
            g_components == nullptr || g_components->main_menu_text_button_is_live == nullptr) {
            return MoreGamesBindingState::kNeedsReplace;
        }
        QolUiHandle current = nullptr;
        QolUiHandle parent = nullptr;
        UiRect rect{};
        const QolResult snapshot = g_ui->main_menu_more_games_snapshot(&current, &parent, &rect);
        if (!contracts::succeeded(snapshot)) return MoreGamesBindingState::kUnknown;
        if (current != g_more_games_original) {
            return MoreGamesBindingState::kNeedsReplace;
        }
        uint8_t live = 0;
        const QolResult result = g_components->main_menu_text_button_is_live(
            g_more_games_replacement, &live);
        if (result.code == kQolErrNotFound) return MoreGamesBindingState::kNeedsReplace;
        if (!contracts::succeeded(result)) return MoreGamesBindingState::kUnknown;
        return live != 0 ? MoreGamesBindingState::kLive : MoreGamesBindingState::kNeedsReplace;
    }
    bool release() override { return release_more_games_runtime(); }
    bool bind() override {
        if (g_components == nullptr || g_components->main_menu_text_button_create == nullptr) {
            return false;
        }
        // Gate on the adapter ABI before touching any tail member or native control.
        constexpr size_t kActiveAdapterSize =
            offsetof(contracts::QolUiAdapter, main_menu_more_games_set_active) +
            sizeof(((contracts::QolUiAdapter*)nullptr)->main_menu_more_games_set_active);
        if (g_ui == nullptr || g_ui->minor < contracts::kUiAdapterMinor ||
            g_ui->size < kActiveAdapterSize || g_ui->main_menu_more_games_snapshot == nullptr ||
            g_ui->control_active == nullptr || g_ui->set_control_show == nullptr ||
            g_ui->set_button_enabled == nullptr ||
            g_ui->main_menu_more_games_set_active == nullptr) {
            return false;
        }
        QolUiHandle snapshot_original = nullptr;
        QolUiHandle snapshot_parent = nullptr;
        UiRect snapshot_rect{};
        const QolResult snapshot = g_ui->main_menu_more_games_snapshot(
            &snapshot_original, &snapshot_parent, &snapshot_rect);
        if (!contracts::succeeded(snapshot) || snapshot_original == nullptr ||
            snapshot_parent == nullptr) return false;
        int32_t raw_active = 0;
        if (!contracts::succeeded(g_ui->control_active(snapshot_original, &raw_active)) ||
            (raw_active != kNativeControlActive && raw_active != kNativeControlInactive)) {
            return false;
        }
        const bool original_active = raw_active == kNativeControlActive;
        const contracts::UiMainMenuTextButtonDesc desc{
            "模块设置", contracts::kUiTextButtonFontDefault, &more_games_clicked, nullptr};
        QolUiHandle original = nullptr;
        QolUiHandle replacement = nullptr;
        const QolResult created = g_components->main_menu_text_button_create(
            &desc, &original, &replacement);
        if (!contracts::succeeded(created) || original == nullptr || replacement == nullptr) {
            return false;
        }
        g_more_games_original = original;
        g_more_games_replacement = replacement;
        g_more_games_original_active = original_active;
        g_more_games_original_active_valid = original == snapshot_original;
        g_more_games_binding_ready = false;
        if (original != snapshot_original) {
            (void)release_more_games_runtime();
            return false;
        }
        RuntimeMoreGamesOriginalStateOps state_ops(original);
        if (!more_games_disable_original(state_ops, original_active)) {
            (void)release_more_games_runtime();
            return false;
        }
        g_more_games_binding_ready = true;
        return true;
    }
    bool btn_resolvable() override {
        QolUiHandle current = nullptr;
        QolUiHandle parent = nullptr;
        UiRect rect{};
        return g_ui != nullptr && g_ui->main_menu_more_games_snapshot != nullptr &&
            contracts::succeeded(g_ui->main_menu_more_games_snapshot(&current, &parent, &rect)) &&
            current != nullptr && parent != nullptr;
    }
    void mark_live() override { g_more_games_bind.store(1); g_more_games_error.store(false); }
    void mark_failed(bool resolvable) override {
        g_more_games_bind.store(0);
        g_more_games_error.store(resolvable);
    }
    void mark_waiting() override {
        g_more_games_bind.store(0);
        g_more_games_error.store(false);
    }
};

class RuntimeMoreGamesDrawOps final : public MoreGamesDrawOps {
public:
    RuntimeMoreGamesDrawOps(void* group, int32_t x, int32_t y, void* loc,
                            int32_t type, int32_t flip)
        : group_(group), x_(x), y_(y), loc_(loc), type_(type), flip_(flip) {}

    bool replacement_live() override {
        if (g_more_games_original == nullptr || g_more_games_replacement == nullptr ||
            g_components == nullptr || g_components->main_menu_text_button_is_live == nullptr) {
            return false;
        }
        uint8_t live = 0;
        const QolResult result = g_components->main_menu_text_button_is_live(
            g_more_games_replacement, &live);
        return contracts::succeeded(result) && live != 0;
    }

    bool draw_replacement() override {
        if (g_components == nullptr || g_components->text_button_draw == nullptr) return false;
        const QolResult result = g_components->text_button_draw(g_more_games_replacement);
        return contracts::succeeded(result);
    }

    void draw_original() override {
        // This callsite's final GRPX_DrawPart argument is zero. Adapter draw_part calls the
        // resolved function directly, not the patched callsite, so fallback cannot recurse.
        if (g_ui == nullptr || g_ui->draw_part == nullptr) return;
        (void)g_ui->draw_part(group_, x_, y_, loc_, type_, flip_);
    }

private:
    void* group_;
    int32_t x_;
    int32_t y_;
    void* loc_;
    int32_t type_;
    int32_t flip_;
};

}  // namespace

void settings_bind_sdk(const contracts::QolHostSdk* sdk, contracts::QolOwnerToken) {
    // Teardown uses the previous adapter before installing a new lifecycle context.
    if (g_root != nullptr) destroy_page();
    else clear_entry_continuation();
    g_sdk = sdk; g_ui = sdk ? sdk->ui : nullptr;
    g_control = contracts::feature_control_if_available(sdk); g_components = nullptr;
    if (sdk && sdk->capabilities.require) {
        const void* iface = nullptr;
        if (contracts::succeeded(sdk->capabilities.require(contracts::kUiComponentsCapabilityId,
            {contracts::kUiContractMajor, contracts::kUiContractMinor}, &iface)))
            g_components = static_cast<const contracts::QolUiComponents*>(iface);
    }
    g_module_version[0] = '\0';
}
bool settings_provider_open() { return plugin_enabled() && g_control != nullptr && components_complete(); }
bool settings_provider_active() { std::lock_guard<std::mutex> lock(g_mtx); return g_panel_active; }
void settings_provider_close() { if (settings_provider_active()) settings_ui_close_panel(); }
const char* settings_provider_block_reason() { return settings_provider_active() ? "ui occupied: settings panel" : nullptr; }
const contracts::PanelCallbacks* settings_panel_callbacks() {
    static const contracts::PanelCallbacks callbacks{&settings_panel_enter, &settings_panel_process,
        &settings_panel_f3, &settings_panel_f4, &settings_panel_event};
    return &callbacks;
}
void settings_ui_close_panel() { settings_panel_f3(); }
void settings_register_config_bridge(void*, void*) {}
std::string data_settings_ui_inject() {
    if (!g_sdk) return result_error("settings unavailable");
    settings_more_games_tick();
    return result_ok();
}
std::string data_settings_ui_status_json() {
    std::lock_guard<std::mutex> lock(g_mtx); char buffer[256]{};
    std::snprintf(buffer, sizeof(buffer),
        "{\"panel_active\":%s,\"categories\":%d,\"items\":%d,\"more_games_bind\":\"%s\"}",
        g_panel_active ? "true" : "false", g_summary_categories, g_summary_items,
        settings_more_games_bind_state());
    return buffer;
}
std::string data_settings_ui_restore() {
    if (settings_provider_active()) settings_panels_close("settings");
    settings_release_more_games(); return result_ok();
}
std::string data_settings_ui_open_panel() {
    return settings_provider_open() && settings_panels_open("settings") ? result_ok() : result_error("settings unavailable");
}
std::string data_settings_ui_open_option() {
    contracts::QolPopupStateEntry entry{};
    if (!g_sdk || !g_sdk->hooks.popup_resolve || !g_ui || !g_ui->set_popup_process_info ||
        !contracts::succeeded(g_sdk->hooks.popup_resolve("F_PANEL_OPTIONS_ENTER", &entry)) ||
        !contracts::succeeded(g_ui->set_popup_process_info(1, entry.state_id))) return result_error("option state unavailable");
    return result_ok();
}
void settings_release_more_games() {
    if (!release_more_games_runtime()) {
        g_more_games_bind.store(0);
        g_more_games_error.store(false);
    }
}
namespace {
bool release_more_games_runtime() {
    if (g_more_games_replacement != nullptr) {
        if (g_components == nullptr || g_components->destroy == nullptr) return false;
        const QolResult destroyed = g_components->destroy(g_more_games_replacement);
        if (!contracts::succeeded(destroyed) && destroyed.code != kQolErrNotFound) return false;
    }
    if (g_more_games_original != nullptr) {
        if (g_ui == nullptr || g_ui->main_menu_more_games_snapshot == nullptr) return false;
        QolUiHandle current = nullptr;
        QolUiHandle parent = nullptr;
        UiRect rect{};
        const QolResult snapshot = g_ui->main_menu_more_games_snapshot(&current, &parent, &rect);
        if (!contracts::succeeded(snapshot)) return false;
        // A verified non-current original gets no native reads or writes.
        if (current == g_more_games_original) {
            RuntimeMoreGamesOriginalStateOps ops(current);
            if (g_more_games_original_active_valid) {
                if (!more_games_restore_original(true, g_more_games_original_active, ops)) {
                    return false;
                }
            } else if (!ops.set_show(true) || !ops.set_enabled(true)) {
                return false;
            }
        }
    }
    g_more_games_original = nullptr;
    g_more_games_replacement = nullptr;
    g_more_games_original_active = false;
    g_more_games_original_active_valid = false;
    g_more_games_binding_ready = false;
    g_more_games_bind.store(0);
    g_more_games_error.store(false);
    return true;
}
}  // namespace
const char* settings_more_games_bind_state() {
    if (g_more_games_error.load()) return "error";
    return g_more_games_bind.load() ? "live" : "waiting";
}
void settings_more_games_tick() {
    RuntimeMoreGamesBindOps ops;
    const bool on_main_menu = std::strcmp(ui_screen_name(), "main_menu") == 0;
    if (!plugin_enabled()) {
        if (on_main_menu) ops.release();
        ops.mark_waiting();
        return;
    }
    more_games_bind_tick(on_main_menu, ops);
}

void settings_main_menu_more_games_draw(void* group, int32_t x, int32_t y, void* loc,
                                        int32_t type, int32_t flip, int32_t unknown) {
    (void)unknown;
    RuntimeMoreGamesDrawOps ops(group, x, y, loc, type, flip);
    (void)more_games_draw(plugin_enabled(), ops);
}
}  // namespace qol::features::core_ui
