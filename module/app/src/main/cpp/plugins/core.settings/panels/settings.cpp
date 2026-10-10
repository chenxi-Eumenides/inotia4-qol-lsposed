#include "settings.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
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
constexpr int32_t kLeftRowH = 73, kRightRowH = 224;
constexpr int64_t kBackAbsX = contracts::ui_theme::kNavBoxAbsX;
constexpr int64_t kBackAbsY = contracts::ui_theme::kNavBoxAbsY;
constexpr int64_t kBackW = contracts::ui_theme::kNavBoxW;
constexpr int64_t kBackH = contracts::ui_theme::kNavBoxH;
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
std::vector<FeatureCategory> g_categories;
std::vector<feature_model::Item> g_items;
std::vector<uint32_t> g_right_indices;
struct ItemState {
    bool valid = false;
    uint8_t confirmed = 0;
    bool desired_valid = false;
    uint8_t any_feature = 0;
    uint64_t request_id = 0;
    bool gesture = false;
    int32_t desired = 0;
    QolUiHandle toggle = nullptr, action = nullptr;
    char message[128]{};
};
std::vector<ItemState> g_states;
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
bool toggle_enabled(uint32_t i) {
    const auto& s = g_states[i];
    return feature_model::toggle_enabled(s.valid, s.request_id, s.gesture);
}
bool action_enabled(uint32_t i) {
    const auto& s = g_states[i];
    return feature_model::entry_enabled(g_items[i], s.valid, s.confirmed != 0,
        s.request_id, s.gesture);
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
    if (g_control == nullptr) { s.valid = false; s.desired_valid = false; return; }
    uint8_t desired = 0, any_feature = 0;
    s.desired_valid = contracts::succeeded(g_control->read_desired(
        g_control->context, item.plugin_id, &desired, &any_feature));
    if (s.desired_valid) s.any_feature = any_feature;
    if (s.request_id == 0 && !s.gesture) {
        uint8_t value = 0;
        s.valid = item.desired ? s.desired_valid : contracts::succeeded(g_control->read_feature(
            g_control->context, item.plugin_id, item.feature_id, &value));
        if (s.valid) s.confirmed = item.desired ? desired : value;
    }
    sync_controls(i);
}
void refresh_all_values() {
    for (uint32_t i = 0; i < g_items.size(); ++i) refresh_item_value(i);
}
bool load_catalog() {
    g_items.clear(); g_categories.clear(); g_states.clear(); g_contexts.clear();
    ++g_generation;
    const contracts::QolFeatureCatalogPlugin* plugins = nullptr;
    uint32_t count = 0;
    if (g_control == nullptr || !contracts::succeeded(
        g_control->read_catalog(g_control->context, &plugins, &count)) ||
        !feature_model::build(plugins, count, kShowHidden, g_items)) return false;
    for (const auto category : {FeatureCategory::kGeneral, FeatureCategory::kGameplay,
                               FeatureCategory::kTools, FeatureCategory::kHidden}) {
        for (const auto& item : g_items) {
            if (item.category == category) { g_categories.push_back(category); break; }
        }
    }
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
    (void)g_components->draw_control_text(row, feature_model::category_title(g_categories[index]),
        16, 18, contracts::ui_theme::kTextGold, 0, 1);
}
void draw_right_row(void*, uint32_t row_index, QolUiHandle row, uint8_t selected) {
    if (row_index >= g_right_indices.size()) return;
    const auto i = g_right_indices[row_index];
    const auto& item = g_items[i];
    const auto& s = g_states[i];
    draw_row_background(row, kRightW, kRightRowH, selected);
    const auto title = settings_model::truncate_text(item.label, kRightW - 32);
    if (!title.lines.empty()) (void)g_components->draw_control_text(row, title.lines[0].c_str(),
        16, 8, contracts::ui_theme::kTextGold, 0, 1);
    const auto description = settings_model::truncate_text(item.description, kRightW - 32);
    if (!description.lines.empty()) (void)g_components->draw_control_text(row,
        description.lines[0].c_str(), 16, 42, contracts::ui_theme::kTextNormal, 0, 1);
    char detail[192]{};
    if (s.message[0] != '\0') std::snprintf(detail, sizeof(detail), "%s", s.message);
    else if (!s.valid) std::snprintf(detail, sizeof(detail), "当前状态读取失败");
    else if (item.desired) std::snprintf(detail, sizeof(detail), "期望：%s · 功能：%s",
        s.confirmed ? "启用" : "停用", s.any_feature ? "有已开启项" : "全部关闭");
    else std::snprintf(detail, sizeof(detail), "%s · %s", item.plugin_id,
        item.type == FeatureType::kEntry ? "入口由宿主检查运行状态" : "开关状态不代表实际运行");
    const auto status = settings_model::truncate_text(detail, kRightW - 32);
    if (!status.lines.empty()) (void)g_components->draw_control_text(row, status.lines[0].c_str(),
        16, 78, contracts::ui_theme::kTextNormal, 0, 1);
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
        context->item_index >= g_items.size() || !action_enabled(context->item_index)) return;
    g_pending_action_item = static_cast<int>(context->item_index);
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
        if (g_items[i].category == g_categories[category]) g_right_indices.push_back(i);
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
        const contracts::UiNativeToggleDesc toggle{{16, 120, 220, 80}, g_states[i].confirmed,
            static_cast<uint8_t>(toggle_enabled(i)), &native_toggle_changed, &g_contexts[i]};
        if (!contracts::succeeded(g_components->native_toggle_create(&toggle, handle,
            &g_states[i].toggle))) { destroy_right_list(); return false; }
        if (!g_items[i].desired && g_items[i].type == FeatureType::kEntry) {
            const contracts::UiButtonDesc button{{332, 120, 220, 80}, "打开",
                contracts::kUiTextButtonFontLarger, &action_button_clicked, &g_contexts[i]};
            if (!contracts::succeeded(g_components->text_button_create(&button, handle,
                &g_states[i].action))) { destroy_right_list(); return false; }
        }
        sync_controls(i);
    }
    return true;
}
void destroy_page() {
    { std::lock_guard<std::mutex> lock(g_mtx); g_panel_active = false;
      g_summary_categories = 0; g_summary_items = 0; }
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
        const auto result = item.desired ? g_control->request_desired(g_control->context,
            item.plugin_id, s.desired, &receipt) : g_control->request_set_feature(g_control->context,
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
    if (!action_enabled(i)) { item_error(i, "请先开启入口开关"); return; }
    const auto result = g_control->invoke_entry(g_control->context,
        g_items[i].plugin_id, g_items[i].feature_id);
    if (!contracts::succeeded(result)) item_error(i, settings_model::action_error_text(result.code));
    else set_status("");
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
    process_pending_action(); poll_requests();
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
void* more_games_btn() {
    void* button = nullptr;
    if (g_ui && g_ui->find_main_menu_more_games && contracts::succeeded(
        g_ui->find_main_menu_more_games(&button))) return button;
    return nullptr;
}
void more_games_clicked(void*) {
    if (plugin_enabled()) (void)settings_panels_open("settings");
}
bool g_more_games_injected = false;
uint64_t g_more_games_binding_token = 0;
std::atomic<int32_t> g_more_games_bind{0};
std::atomic<bool> g_more_games_error{false};
class RuntimeMoreGamesBindOps final : public MoreGamesBindOps {
public:
    bool proc_matches() override {
        void* button = more_games_btn(); int32_t matches = 0;
        return g_more_games_binding_token && button && g_ui->execute_proc_matches &&
            contracts::succeeded(g_ui->execute_proc_matches(button, &more_games_clicked, &matches)) && matches;
    }
    void release() override { settings_release_more_games(); }
    bool bind() override {
        uint64_t token = 0;
        if (!more_games_btn() || !g_ui->bind_main_menu_more_games || !contracts::succeeded(
            g_ui->bind_main_menu_more_games(&more_games_clicked, &token)) || !token) return false;
        g_more_games_binding_token = token; g_more_games_injected = true; return true;
    }
    bool btn_resolvable() override { return more_games_btn() != nullptr; }
    void mark_live() override { g_more_games_bind.store(1); g_more_games_error.store(false); }
    void mark_failed(bool resolvable) override { g_more_games_bind.store(0); g_more_games_error.store(resolvable); }
};
}  // namespace

void settings_bind_sdk(const contracts::QolHostSdk* sdk, contracts::QolOwnerToken) {
    // Teardown uses the previous adapter before installing a new lifecycle context.
    if (g_root != nullptr) destroy_page();
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
    RuntimeMoreGamesBindOps ops; if (!ops.proc_matches()) { ops.release(); (void)ops.bind(); }
    return result_ok();
}
std::string data_settings_ui_status_json() {
    std::lock_guard<std::mutex> lock(g_mtx); char buffer[256]{};
    std::snprintf(buffer, sizeof(buffer), "{\"panel_active\":%s,\"categories\":%d,\"items\":%d}",
        g_panel_active ? "true" : "false", g_summary_categories, g_summary_items);
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
    if (g_more_games_binding_token && g_ui && g_ui->unbind_main_menu_more_games)
        (void)g_ui->unbind_main_menu_more_games(g_more_games_binding_token);
    g_more_games_binding_token = 0; g_more_games_injected = false;
    g_more_games_bind.store(0); g_more_games_error.store(false);
}
const char* settings_more_games_bind_state() {
    if (g_more_games_error.load()) return "error";
    return g_more_games_bind.load() ? "live" : "waiting";
}
void settings_more_games_tick() {
    RuntimeMoreGamesBindOps ops;
    more_games_bind_tick(std::strcmp(ui_screen_name(), "main_menu") == 0, ops);
}
}  // namespace qol::features::core_ui
