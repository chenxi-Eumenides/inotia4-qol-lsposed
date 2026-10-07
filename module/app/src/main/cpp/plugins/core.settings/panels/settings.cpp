#include "settings.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../../../contracts/panels.h"
#include "../../../contracts/settings.h"
#include "../../../contracts/settings_config.h"
#include "../../../contracts/ui.h"
#include "../../../contracts/ui_theme.h"
#include "log.h"
#include "settings_binding.h"
#include "settings_model.h"

// 顶层 include：settings_binding.h 在全局作用域打开 qol::features::core_ui；若在下方
// 匿名 namespace 内 include，其 namespace 声明会被相对解析成错误的嵌套命名空间。
// 结构：help_page 风格两栏（左类别列表 / 右选项列表），全部原生事件树输入模型 +
// 组件能力绘制；数据来自 settings_catalog 快照与 settings_config 持久配置桥。
#define SETTINGS_LOG(...) CORE_UI_LOG_INFO(__VA_ARGS__)

namespace qol::features::core_ui {
namespace {

using contracts::CatalogView;
using contracts::QolUiHandle;
using contracts::SettingsCatalogToken;
using contracts::SettingsCategoryView;
using contracts::SettingsConfigApplyState;
using contracts::SettingsConfigRequestState;
using contracts::SettingsConfigRequestStatus;
using contracts::SettingsItemType;
using contracts::SettingsItemView;
using contracts::UiRect;

constexpr const char* kMoreGamesStateEntry = "F_PANEL_UNK1_ENTER";

// 逻辑布局（0-960 × 0-640，root 由 screen inset 居中；触摸坐标同空间）。
constexpr int64_t kPanelW = 960;
constexpr int64_t kPanelH = 640;
// 两栏容器外扩 8、右栏另留 16 给原生滚动条；左框右缘(348)与右框左缘(356)保持间距。
constexpr int64_t kLeftX = 24, kLeftY = 100, kLeftW = 300, kLeftH = 444;
constexpr int64_t kRightX = 364, kRightY = 100, kRightW = 572, kRightH = 444;
constexpr int32_t kLeftRowH = 73;
int32_t g_right_row_height = settings_model::kMinimumRightRowHeight;
// Row geometry and GRPX_FillRect arguments are integer logical UI units. The native scale-to-
// physical-pixel ratio is not established by this path; two logical units reduce subpixel risk,
// but visibility still requires real-device confirmation.
// 返回按钮沿用旧设置页的左上绝对逻辑 rect；创建时按 root 原点转成相对 rect。
// 盒取三页统一的导航标准盒（contracts/ui_theme.h kNavBox*：左上 (8,8) 78×86，原生
// X/返回按钮贴图盒），使 X 与返回在同一盒内共用同款金属框；不再按 74×81 自行缩放。
constexpr int64_t kBackAbsX = contracts::ui_theme::kNavBoxAbsX;
constexpr int64_t kBackAbsY = contracts::ui_theme::kNavBoxAbsY;
constexpr int64_t kBackW = contracts::ui_theme::kNavBoxW;
constexpr int64_t kBackH = contracts::ui_theme::kNavBoxH;

// 背景：借用游戏主菜单共享图集 0x4f/0x50（左/右半图），绘制参数与覆盖顺序沿用旧设置页
// （居中，画在两栏容器之前）。共享图集只 ensure/load，不 unload。
constexpr int32_t kBackgroundLeftUnit = 0x4f;
constexpr int32_t kBackgroundRightUnit = 0x50;
constexpr int32_t kBackgroundLoc = 0x0;
constexpr int64_t kBackgroundHalfOffset = 0x12c;  // 左右半图相对面板中心的水平偏移

constexpr uint32_t kMaxCats = contracts::kSettingsMaxCategories;
constexpr uint32_t kMaxItems = contracts::kSettingsMaxItems;
// 轮询/回读限频：每帧调用 query_request/read_bool 无必要，按帧计数节流。
constexpr uint64_t kRefreshIntervalFrames = 30;

constexpr uint32_t kGold = contracts::ui_theme::kTextGold;
constexpr uint32_t kRowSelected = contracts::ui_theme::kRowSelectedBg;
constexpr uint32_t kDivider = contracts::ui_theme::kFrameGold;
constexpr uint32_t kErrorText = contracts::ui_theme::kError;

std::mutex g_mtx;
bool g_panel_active = false;
int32_t g_state_id = -1;

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolUiAdapter* g_ui = nullptr;
const contracts::QolUiComponents* g_components = nullptr;
const contracts::QolSettingsCatalogApi* g_catalog = nullptr;
const contracts::QolSettingsConfigApi* g_config = nullptr;

QolUiHandle g_root = nullptr;
QolUiHandle g_left = nullptr;
QolUiHandle g_right = nullptr;
QolUiHandle g_back = nullptr;
QolUiHandle g_pressed_list = nullptr;
bool g_back_pressed = false;
int64_t g_inset_x = 0;  // root 相对屏幕的逻辑原点，用于把绝对逻辑 rect 转成 root 相对 rect
int64_t g_inset_y = 0;
char g_module_version[64] = {};  // 运行时模块版本（BuildConfig.VERSION_NAME），空表示未知
bool g_module_version_fallback_logged = false;

// 快照深拷贝：目录 metadata 与 generation 在 enter 时复制，随后立即释放快照 token。
SettingsCategoryView g_categories[kMaxCats];
SettingsItemView g_items[kMaxItems];
uint32_t g_category_count = 0;
uint32_t g_item_count = 0;
uint64_t g_generation = 0;

int g_selected_category = 0;   // 当前右栏对应类别下标
int g_pending_category = -1;   // on_select 回调只置此标记，下一安全点重建右栏
int g_right_rows = 0;
int g_right_indices[kMaxItems] = {};
struct ItemTextLayout {
    settings_model::WrappedText description;
    settings_model::RowTextLayout row{};
};
ItemTextLayout g_item_text_layout[kMaxItems];

enum class Pending : uint8_t { kNone = 0, kQueued = 1, kDonePending = 2, kFailed = 3 };
struct ItemState {
    int32_t confirmed = 0;
    bool confirmed_valid = false;
    uint64_t request_id = 0;
    Pending pending = Pending::kNone;
    char message[192] = {};
};
ItemState g_item_state[kMaxItems];

struct ToggleContext {
    uint32_t item_index = 0;
    uint64_t generation = 0;
};
struct Availability {
    contracts::SettingsItemAvailability value{};
    bool valid = false;
};
ToggleContext g_toggle_contexts[kMaxItems];
Availability g_availability[kMaxItems];
int32_t g_desired_toggle[kMaxItems];
bool g_toggle_requested[kMaxItems];
QolUiHandle g_toggle_handles[kMaxItems]{};
bool g_toggle_enabled_cache[kMaxItems]{};
bool g_toggle_enabled_cache_valid[kMaxItems]{};
QolUiHandle g_action_handles[kMaxItems]{};
bool g_action_enabled_cache[kMaxItems]{};
bool g_action_enabled_cache_valid[kMaxItems]{};

int g_pending_action_item = -1;
char g_status[256] = {};
bool g_status_is_availability = false;
uint64_t g_frame = 0;
uint64_t g_last_poll_frame = 0;
uint64_t g_last_refresh_frame = 0;

// status_json 只读取以下摘要（g_mtx 保护），不触碰主线程控件句柄。
int g_summary_categories = 0;
int g_summary_items = 0;
int g_summary_selected = 0;
uint64_t g_summary_generation = 0;
char g_summary_category[contracts::kSettingsIdCapacity] = {};

// ---- 基础助手 ----

bool main_thread() {
    return g_ui != nullptr && g_ui->is_main_thread != nullptr && g_ui->is_main_thread() != 0;
}

void set_status(const char* text) {
    std::snprintf(g_status, sizeof(g_status), "%s", text == nullptr ? "" : text);
    g_status_is_availability = false;
}

void set_availability_status(const char* text) {
    if (!g_status_is_availability && g_status[0] != '\0') return;
    std::snprintf(g_status, sizeof(g_status), "%s", text == nullptr ? "" : text);
    g_status_is_availability = g_status[0] != '\0';
}

// 标题版本：取运行时模块身份（Kotlin BuildConfig.VERSION_NAME 启动期下发）。
// 未就绪时不伪造，保持无版本标题并只在首次落一条日志；成功拿到即缓存。
void resolve_module_version() {
    if (g_module_version[0] != '\0') return;
    const char* version = contracts::host_sdk_module_version(g_sdk);
    if (version != nullptr && version[0] != '\0') {
        std::snprintf(g_module_version, sizeof(g_module_version), "%s", version);
        return;
    }
    if (!g_module_version_fallback_logged) {
        SETTINGS_LOG("settings module version unavailable; title shown without version");
        g_module_version_fallback_logged = true;
    }
}

void set_item_message(uint32_t index, Pending pending, const char* text) {
    if (index >= kMaxItems) return;
    ItemState& state = g_item_state[index];
    state.pending = pending;
    std::snprintf(state.message, sizeof(state.message), "%s", text == nullptr ? "" : text);
}

void update_summary() {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_summary_categories = static_cast<int>(g_category_count);
    g_summary_items = static_cast<int>(g_item_count);
    g_summary_selected = g_selected_category;
    g_summary_generation = g_generation;
    if (g_selected_category >= 0 && static_cast<uint32_t>(g_selected_category) < g_category_count) {
        std::snprintf(g_summary_category, sizeof(g_summary_category), "%s",
                      g_categories[g_selected_category].id);
    } else {
        g_summary_category[0] = '\0';
    }
}

bool toggle_enabled(uint32_t index) {
    if (index >= g_item_count || g_items[index].type != SettingsItemType::kToggle) return false;
    const Availability& availability = g_availability[index];
    const ItemState& state = g_item_state[index];
    return settings_model::toggle_enabled(
        g_toggle_handles[index] != nullptr, state.confirmed_valid, availability.valid,
        availability.value.operable != 0, g_toggle_requested[index], state.request_id,
        state.pending == Pending::kQueued);
}

void sync_toggle_enabled(uint32_t index) {
    if (index >= g_item_count || g_items[index].type != SettingsItemType::kToggle ||
        g_toggle_handles[index] == nullptr || g_components == nullptr ||
        g_components->native_toggle_set_enabled == nullptr) return;
    const bool enabled = toggle_enabled(index);
    if (g_toggle_enabled_cache_valid[index] && g_toggle_enabled_cache[index] == enabled) return;
    if (contracts::succeeded(g_components->native_toggle_set_enabled(
            g_toggle_handles[index], static_cast<uint8_t>(enabled)))) {
        g_toggle_enabled_cache[index] = enabled;
        g_toggle_enabled_cache_valid[index] = true;
    }
}

void sync_all_toggle_enabled() {
    for (uint32_t i = 0; i < g_item_count; ++i) sync_toggle_enabled(i);
}

bool action_enabled(uint32_t index) {
    return index < g_item_count && g_items[index].type == SettingsItemType::kAction &&
        g_availability[index].valid && g_availability[index].value.operable != 0;
}

void sync_action_enabled(uint32_t index) {
    if (index >= g_item_count || g_items[index].type != SettingsItemType::kAction ||
        g_action_handles[index] == nullptr || g_components == nullptr ||
        g_components->text_button_set_enabled == nullptr) return;
    const bool enabled = action_enabled(index);
    if (g_action_enabled_cache_valid[index] && g_action_enabled_cache[index] == enabled) return;
    const QolResult result = g_components->text_button_set_enabled(
        g_action_handles[index], static_cast<uint8_t>(enabled));
    if (contracts::succeeded(result)) {
        g_action_enabled_cache[index] = enabled;
        g_action_enabled_cache_valid[index] = true;
        return;
    }
    char message[256] = {};
    std::snprintf(message, sizeof(message), "%s：操作按钮状态更新失败（%d）",
                  g_items[index].label, result.code);
    set_status(message);
}

void sync_all_action_enabled() {
    for (uint32_t i = 0; i < g_item_count; ++i) sync_action_enabled(i);
}

void refresh_item_value(uint32_t index) {
    if (index >= g_item_count) return;
    if (g_items[index].type != SettingsItemType::kToggle) return;
    ItemState& state = g_item_state[index];
    if (state.request_id != 0 || state.pending == Pending::kQueued) return;
    const bool had_confirmed_value = state.confirmed_valid;
    if (g_config == nullptr) {
        state.confirmed_valid = false;
        sync_toggle_enabled(index);
        return;
    }
    int32_t value = 0;
    if (contracts::succeeded(g_config->read_bool(g_items[index].config_key, &value))) {
        state.confirmed = value != 0 ? 1 : 0;
        state.confirmed_valid = true;
        if (g_toggle_handles[index] != nullptr && g_components != nullptr &&
            g_components->native_toggle_set_value != nullptr) {
            (void)g_components->native_toggle_set_value(g_toggle_handles[index], state.confirmed);
        }
    } else {
        state.confirmed_valid = false;
        if (g_toggle_handles[index] != nullptr && had_confirmed_value && g_components != nullptr &&
            g_components->native_toggle_set_value != nullptr) {
            (void)g_components->native_toggle_set_value(g_toggle_handles[index], state.confirmed);
        }
    }
    sync_toggle_enabled(index);
}

void refresh_all_values() {
    for (uint32_t i = 0; i < g_item_count; ++i) refresh_item_value(i);
}

// ---- 目录快照 ----

bool load_catalog() {
    g_category_count = 0;
    g_item_count = 0;
    g_generation = 0;
    if (g_catalog == nullptr) return false;
    const CatalogView* view = nullptr;
    SettingsCatalogToken token = 0;
    const QolResult acquired = g_catalog->snapshot_acquire(&view, &token);
    if (!contracts::succeeded(acquired) || view == nullptr) return false;
    bool ok = true;
    const uint32_t category_count = view->category_count < kMaxCats ? view->category_count : kMaxCats;
    const uint32_t item_count = view->item_count < kMaxItems ? view->item_count : kMaxItems;
    if (category_count != 0 && view->categories == nullptr) ok = false;
    if (item_count != 0 && view->items == nullptr) ok = false;
    if (ok) {
        for (uint32_t i = 0; i < category_count; ++i) g_categories[i] = view->categories[i];
        for (uint32_t i = 0; i < item_count; ++i) g_items[i] = view->items[i];
        g_category_count = category_count;
        g_item_count = item_count;
        g_generation = view->generation;
    }
    (void)g_catalog->snapshot_release(token);
    return ok;
}

// ---- 列表绘制回调（原生事件树；draw_item 在 clip 内自绘，不写控件内联文本）----

void draw_row_background(QolUiHandle row, int32_t width, int32_t height, uint8_t selected) {
    if (g_ui == nullptr || g_ui->fill_rect == nullptr) return;
    int64_t x = 0, y = 0;
    if (!contracts::succeeded(g_ui->control_absolute_origin(row, &x, &y))) return;
    const settings_model::RowPaintPlan paint = settings_model::row_paint_plan(
        x, y, width, height, selected != 0);
    for (uint8_t i = 0; i < paint.operation_count; ++i) {
        if (paint.operations[i] == settings_model::RowPaintPlan::Operation::kSelectedFill) {
            (void)g_ui->fill_rect({x, y, width, height}, kRowSelected);
        } else {
            (void)g_ui->fill_rect(paint.divider, kDivider);
        }
    }
}

void draw_left_row(void*, uint32_t index, QolUiHandle row, uint8_t selected) {
    if (g_ui == nullptr || g_components == nullptr || index >= g_category_count) return;
    draw_row_background(row, static_cast<int32_t>(kLeftW), kLeftRowH, selected);
    (void)g_components->draw_control_text(row, g_categories[index].title, 16, 18,
                                          contracts::ui_theme::kTextGold, 0, 1);
}

void draw_right_row(void*, uint32_t index, QolUiHandle row, uint8_t selected) {
    if (g_ui == nullptr || g_components == nullptr || index >= static_cast<uint32_t>(g_right_rows)) {
        return;
    }
    const int item_index = g_right_indices[index];
    if (item_index < 0 || static_cast<uint32_t>(item_index) >= g_item_count) return;
    const SettingsItemView& item = g_items[item_index];
    const Availability& availability = g_availability[item_index];
    const bool operable = item.type == SettingsItemType::kToggle
        ? toggle_enabled(static_cast<uint32_t>(item_index))
        : availability.valid && availability.value.operable != 0;
    draw_row_background(row, static_cast<int32_t>(kRightW), g_right_row_height, selected);
    const ItemTextLayout& text_layout = g_item_text_layout[item_index];
    (void)g_components->draw_control_text(row, item.label, 16, text_layout.row.title_y,
                                          operable ? contracts::ui_theme::kTextGold :
                                          contracts::ui_theme::kTextDisabled,
                                          0, 1);
    if (item.type == SettingsItemType::kToggle) {
        const QolUiHandle toggle = g_toggle_handles[item_index];
        if (toggle != nullptr && g_components->native_toggle_draw != nullptr) {
            (void)g_components->native_toggle_draw(toggle);
        }
    } else {
        const QolUiHandle action = g_action_handles[item_index];
        if (action != nullptr && g_components->text_button_draw != nullptr) {
            (void)g_components->text_button_draw(action);
        }
    }
    if (text_layout.row.has_description) {
        for (size_t line = 0; line < text_layout.description.lines.size(); ++line) {
            (void)g_components->draw_control_text(row,
                text_layout.description.lines[line].c_str(), 16,
                text_layout.row.description_y +
                    static_cast<int32_t>(line) * settings_model::kTextLineAdvance,
                contracts::ui_theme::kTextNormal, 0, 1);
        }
    }
}

void on_left_select(void*, uint32_t index) {
    if (static_cast<int>(index) != g_selected_category) {
        g_pending_category = static_cast<int>(index);
    }
}

void queue_toggle_item(uint32_t index, int32_t desired) {
    if (index >= g_item_count) return;
    if (g_item_state[index].pending == Pending::kQueued || g_item_state[index].request_id != 0) return;
    if (g_config == nullptr || g_items[index].config_key[0] == '\0') {
        set_item_message(index, Pending::kFailed, "配置桥不可用");
        set_status(g_item_state[index].message);
        return;
    }
    ItemState& state = g_item_state[index];
    if (!state.confirmed_valid) {
        set_item_message(index, Pending::kFailed, "读取当前设置失败");
        set_status(state.message);
        return;
    }
    uint64_t request = 0;
    desired = desired != 0 ? 1 : 0;
    if (state.confirmed == desired) {
        set_item_message(index, Pending::kNone, "");
        return;
    }
    const QolResult result = g_config->request_set_bool(g_items[index].config_key, desired, &request);
    if (!contracts::succeeded(result) || request == 0) {
        g_item_state[index].request_id = 0;
        set_item_message(index, Pending::kFailed, settings_model::request_error_text(result.code));
        set_status(g_item_state[index].message);
        return;
    }
    state.request_id = request;
    set_item_message(index, Pending::kQueued, "保存中…");
}

void native_toggle_changed(void* user, int32_t desired) {
    auto* context = static_cast<ToggleContext*>(user);
    if (context == nullptr || context->generation != g_generation ||
        context->item_index >= g_item_count) return;
    const uint32_t index = context->item_index;
    if (!toggle_enabled(index)) return;
    g_desired_toggle[index] = desired != 0 ? 1 : 0;
    g_toggle_requested[index] = true;
    sync_toggle_enabled(index);
}

void action_button_clicked(void* user) {
    auto* context = static_cast<ToggleContext*>(user);
    if (context == nullptr || context->generation != g_generation ||
        context->item_index >= g_item_count ||
        g_items[context->item_index].type != SettingsItemType::kAction) return;
    const uint32_t index = context->item_index;
    if (!g_availability[index].valid || g_availability[index].value.operable == 0) return;
    // 动作只由独立按钮的 ExecuteProc 请求，并在后续帧安全点执行。
    g_pending_action_item = static_cast<int>(index);
}

// ---- 列表生命周期（只允许在非回调、非绘制安全点调用）----

void destroy_right_list() {
    for (uint32_t i = 0; i < kMaxItems; ++i) {
        g_toggle_handles[i] = nullptr;
        g_toggle_enabled_cache_valid[i] = false;
        g_action_handles[i] = nullptr;
        g_action_enabled_cache_valid[i] = false;
    }
    if (g_right != nullptr && g_components != nullptr) (void)g_components->destroy(g_right);
    g_right = nullptr;
    g_right_rows = 0;
}

bool build_right_list(int category_index) {
    destroy_right_list();
    if (category_index < 0 || static_cast<uint32_t>(category_index) >= g_category_count) return true;
    const char* category_id = g_categories[category_index].id;
    const int rows = settings_model::category_item_count(g_items, g_item_count, category_id);
    g_right_rows = rows > static_cast<int>(kMaxItems) ? static_cast<int>(kMaxItems) : rows;
    int written = 0;
    for (uint32_t i = 0; i < g_item_count && written < g_right_rows; ++i) {
        if (std::strcmp(g_items[i].category_id, category_id) == 0) {
            const int item_index = static_cast<int>(i);
            g_right_indices[written++] = item_index;
            ItemTextLayout& layout = g_item_text_layout[item_index];
            const SettingsItemView& item = g_items[item_index];
            const int32_t toggle_left = settings_model::right_control_left(
                static_cast<int32_t>(kRightW));
            const int32_t text_width = settings_model::description_width_for_toggle(toggle_left);
            layout.description = settings_model::truncate_text(
                settings_model::row_description(item), text_width);
        }
    }
    g_right_row_height = settings_model::right_row_height(1);
    for (int row = 0; row < g_right_rows; ++row) {
        ItemTextLayout& layout = g_item_text_layout[g_right_indices[row]];
        layout.row = settings_model::row_text_layout(
            !layout.description.lines.empty(), g_right_row_height);
    }
    if (g_right_rows == 0 || g_components == nullptr || g_root == nullptr) return true;
    const contracts::UiScrollListDesc desc{{kRightX, kRightY, kRightW, kRightH}, g_right_row_height,
                                           static_cast<uint32_t>(g_right_rows), -1, &draw_right_row,
                                           nullptr, nullptr};
    if (!contracts::succeeded(g_components->scroll_list_create(&desc, g_root, &g_right))) {
        g_right = nullptr;
        return false;
    }
    if (g_components->scroll_list_set_selection_enabled == nullptr ||
        !contracts::succeeded(g_components->scroll_list_set_selection_enabled(g_right, 0))) {
        destroy_right_list();
        return false;
    }
    if (g_components->native_toggle_create == nullptr || g_components->scroll_list_get_row == nullptr ||
        g_components->text_button_create == nullptr ||
        g_components->text_button_set_enabled == nullptr) {
        destroy_right_list();
        return false;
    }
    for (int row = 0; row < g_right_rows; ++row) {
        const uint32_t item_index = static_cast<uint32_t>(g_right_indices[row]);
        const SettingsItemView& item = g_items[item_index];
        if (item.type != SettingsItemType::kToggle && item.type != SettingsItemType::kAction) continue;
        QolUiHandle row_handle = nullptr;
        if (!contracts::succeeded(g_components->scroll_list_get_row(g_right,
                                                                      static_cast<uint32_t>(row),
                                                                      &row_handle))) {
            destroy_right_list();
            return false;
        }
        g_toggle_contexts[item_index] = {item_index, g_generation};
        if (item.type == SettingsItemType::kAction) {
            const contracts::UiButtonDesc action_desc{
                {settings_model::right_control_left(static_cast<int32_t>(kRightW)),
                  settings_model::toggle_top(g_right_row_height),
                  settings_model::kToggleWidth, settings_model::kToggleHeight},
                "打开", contracts::kUiTextButtonFontLarger, &action_button_clicked,
                &g_toggle_contexts[item_index]};
            if (!contracts::succeeded(g_components->text_button_create(
                    &action_desc, row_handle, &g_action_handles[item_index]))) {
                destroy_right_list();
                return false;
            }
            sync_action_enabled(item_index);
            continue;
        }
        const ItemState& state = g_item_state[item_index];
        const Availability& availability = g_availability[item_index];
        const bool operable = settings_model::toggle_enabled(
            true, state.confirmed_valid, availability.valid, availability.value.operable != 0,
            g_toggle_requested[item_index], state.request_id,
            state.pending == Pending::kQueued);
        const contracts::UiNativeToggleDesc toggle_desc{
            {settings_model::right_control_left(static_cast<int32_t>(kRightW)),
             settings_model::toggle_top(g_right_row_height),
             settings_model::kToggleWidth, settings_model::kToggleHeight},
            state.confirmed_valid ? state.confirmed : 0,
            static_cast<uint8_t>(operable ? 1 : 0), &native_toggle_changed,
            &g_toggle_contexts[item_index]};
        if (!contracts::succeeded(g_components->native_toggle_create(&toggle_desc, row_handle,
                                                                       &g_toggle_handles[item_index]))) {
            destroy_right_list();
            return false;
        }
        g_toggle_enabled_cache[item_index] = operable;
        g_toggle_enabled_cache_valid[item_index] = true;
    }
    return true;
}

void refresh_availability() {
    const char* unavailable_reason = nullptr;
    char reason[256] = {};
    for (uint32_t i = 0; i < g_item_count; ++i) {
        Availability& cached = g_availability[i];
        cached.valid = false;
        if (g_catalog == nullptr || g_catalog->query_item_availability == nullptr) {
            if (unavailable_reason == nullptr) {
                std::snprintf(reason, sizeof(reason), "%s：设置目录可用性查询不可用",
                              g_items[i].label);
                unavailable_reason = reason;
            }
            continue;
        }
        contracts::SettingsItemAvailability availability{};
        availability.size = sizeof(availability);
        const QolResult result = g_catalog->query_item_availability(
            g_items[i].id, g_generation, &availability);
        if (contracts::succeeded(result)) {
            cached.value = availability;
            cached.valid = true;
        } else {
            cached.valid = false;
            if (unavailable_reason == nullptr) {
                std::snprintf(reason, sizeof(reason), "%s：%s", g_items[i].label,
                              result.message != nullptr ? result.message : "所属功能状态未知");
                unavailable_reason = reason;
            }
        }
    }
    sync_all_toggle_enabled();
    sync_all_action_enabled();
    set_availability_status(unavailable_reason);
}

void process_pending_toggles() {
    for (uint32_t i = 0; i < g_item_count; ++i) {
        if (!g_toggle_requested[i]) continue;
        const int32_t desired = g_desired_toggle[i];
        g_toggle_requested[i] = false;
        if (g_toggle_contexts[i].generation != g_generation ||
            g_items[i].type != SettingsItemType::kToggle ||
            !toggle_enabled(i)) {
            sync_toggle_enabled(i);
            continue;
        }
        queue_toggle_item(i, desired);
        sync_toggle_enabled(i);
    }
}

bool build_left_list() {
    if (g_category_count == 0 || g_components == nullptr || g_root == nullptr) return true;
    const contracts::UiScrollListDesc desc{
        {kLeftX, kLeftY, kLeftW, kLeftH}, kLeftRowH, g_category_count,
        g_selected_category < static_cast<int>(g_category_count) ? g_selected_category : -1,
        &draw_left_row, &on_left_select, nullptr};
    return contracts::succeeded(g_components->scroll_list_create(&desc, g_root, &g_left));
}

void destroy_page() {
    if (g_components != nullptr) {
        destroy_right_list();
        if (g_left != nullptr) (void)g_components->destroy(g_left);
        g_left = nullptr;
        // nav 组件是注册在 core.custom_ui 组件表里的独立组件（native 父为裸 g_root），
        // 须先于裸 root 删除，否则组件表残留悬垂条目。
        if (g_back != nullptr) (void)g_components->destroy(g_back);
        g_back = nullptr;
    }
    if (g_ui != nullptr && g_root != nullptr) (void)g_ui->delete_control(g_root);
    g_root = nullptr;
    g_pressed_list = nullptr;
    g_back_pressed = false;
    for (uint32_t i = 0; i < kMaxItems; ++i) {
        g_action_handles[i] = nullptr;
        g_action_enabled_cache_valid[i] = false;
    }
    g_pending_action_item = -1;
    g_pending_category = -1;
    g_selected_category = 0;
    for (auto& state : g_item_state) state = ItemState{};
    for (auto& availability : g_availability) availability = Availability{};
    for (auto& context : g_toggle_contexts) context = ToggleContext{};
    for (uint32_t i = 0; i < kMaxItems; ++i) {
        g_toggle_requested[i] = false;
        g_desired_toggle[i] = 0;
    }
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_panel_active = false;
    }
}

// ---- 帧安全点推进 ----

void process_pending_category() {
    if (g_pending_category < 0 || g_pending_category == g_selected_category) {
        g_pending_category = -1;
        return;
    }
    if (build_right_list(g_pending_category)) {
        g_selected_category = g_pending_category;
        update_summary();
    }
    g_pending_category = -1;
}

void process_pending_action() {
    if (g_pending_action_item < 0) return;
    const int item_index = g_pending_action_item;
    g_pending_action_item = -1;
    if (static_cast<uint32_t>(item_index) >= g_item_count) return;
    if (!g_availability[item_index].valid || g_availability[item_index].value.operable == 0) return;
    if (g_catalog == nullptr) {
        set_status("设置目录不可用");
        return;
    }
    const QolResult result = g_catalog->invoke_action(g_items[item_index].id, g_generation);
    if (contracts::succeeded(result)) {
        set_status("");
        return;
    }
    char message[256] = {};
    std::snprintf(message, sizeof(message), "%s：%s", g_items[item_index].label,
                  settings_model::action_error_text(result.code));
    set_status(message);
}

void poll_requests() {
    if (g_config == nullptr) return;
    for (uint32_t i = 0; i < g_item_count; ++i) {
        ItemState& state = g_item_state[i];
        if (state.request_id == 0 || state.pending != Pending::kQueued) continue;
        SettingsConfigRequestStatus status{};
        status.size = sizeof(status);
        const QolResult queried = g_config->query_request(state.request_id, &status);
        if (!contracts::succeeded(queried)) {
            // Unknown/evicted receipts cannot confirm persistence; keep the last confirmed value.
            state.request_id = 0;
            set_item_message(i, Pending::kFailed, "保存状态未知，请核对当前设置");
            char message[256] = {};
            std::snprintf(message, sizeof(message), "%s：%s", g_items[i].label,
                          state.message);
            set_status(message);
            sync_toggle_enabled(i);
            continue;
        }
        if (status.state == SettingsConfigRequestState::kQueued) continue;
        if (status.state != SettingsConfigRequestState::kSucceeded &&
            status.state != SettingsConfigRequestState::kFailed) {
            state.request_id = 0;
            set_item_message(i, Pending::kFailed, "保存状态未知，请核对当前设置");
            char message[256] = {};
            std::snprintf(message, sizeof(message), "%s：%s", g_items[i].label,
                          state.message);
            set_status(message);
            sync_toggle_enabled(i);
            continue;
        }
        state.request_id = 0;
        state.confirmed = status.configured_value != 0 ? 1 : 0;
        state.confirmed_valid = true;
        if (g_toggle_handles[i] != nullptr && g_components != nullptr &&
            g_components->native_toggle_set_value != nullptr) {
            (void)g_components->native_toggle_set_value(g_toggle_handles[i], state.confirmed);
        }
        if (status.state == SettingsConfigRequestState::kFailed) {
            const char* error = status.error[0] != '\0'
                                    ? status.error
                                    : (status.apply_message[0] != '\0' ? status.apply_message
                                                                       : "保存失败");
            set_item_message(i, Pending::kFailed, error);
            char message[256] = {};
            std::snprintf(message, sizeof(message), "%s：%s", g_items[i].label, error);
            set_status(message);
            sync_toggle_enabled(i);
            continue;
        }
        // configured_value is authoritative at receipt completion; do not read a lagging mirror.
        const char* pending_text = settings_model::apply_pending_text(status.apply_state);
        if (pending_text[0] != '\0') {
            set_item_message(i, Pending::kDonePending, pending_text);
            if (status.apply_state == SettingsConfigApplyState::kRejected) {
                char message[256] = {};
                std::snprintf(message, sizeof(message), "%s：%s", g_items[i].label,
                              pending_text);
                set_status(message);
            }
        } else {
            set_item_message(i, Pending::kNone, "");
        }
        sync_toggle_enabled(i);
    }
}

// ---- 面板回调 ----

void settings_panel_enter() {
    if (!main_thread() || g_ui == nullptr || g_components == nullptr) return;
    if (g_root != nullptr) {
        // 嵌套子页返回后的重入：控件仍在，只需回读配置镜像；版本若首次未就绪则再试一次。
        refresh_all_values();
        resolve_module_version();
        return;
    }
    g_pending_action_item = -1;
    g_pending_category = -1;
    g_selected_category = 0;
    set_status("");
    for (auto& state : g_item_state) state = ItemState{};
    if (load_catalog()) {
        refresh_all_values();
        refresh_availability();
    } else {
        set_status("设置目录不可用");
    }

    if (g_ui->create_control == nullptr || g_ui->set_rect == nullptr ||
        g_ui->delete_control == nullptr || g_ui->draw_custom_button == nullptr ||
        g_ui->begin_frame == nullptr || g_ui->end_frame == nullptr ||
        g_ui->control_absolute_origin == nullptr) {
        SETTINGS_LOG("settings panel enter: ui adapter incomplete");
        return;
    }
    int32_t inset_x = 0;
    int32_t inset_y = 0;
    if (g_ui->screen_inset_width == nullptr || g_ui->screen_inset_height == nullptr ||
        !contracts::succeeded(g_ui->screen_inset_width(&inset_x)) ||
        !contracts::succeeded(g_ui->screen_inset_height(&inset_y))) {
        inset_x = 0;
        inset_y = 0;
    }
    QolResult result = g_ui->create_control(0, nullptr, &g_root);
    if (!contracts::succeeded(result) || g_root == nullptr) {
        g_root = nullptr;
        SETTINGS_LOG("settings panel enter: root create failed");
        return;
    }
    g_inset_x = inset_x;
    g_inset_y = inset_y;
    (void)g_ui->set_rect(g_root, {inset_x, inset_y, kPanelW, kPanelH});
    // 导航按钮：绝对逻辑 (8,8) 换算成 root 相对 rect（root 原点为 inset）；auto 模式按
    // 面板栈深绘制 X 或返回箭头，点击统一延迟请求宿主弹栈顶（原生事件树 ExecuteProc 触发）。
    if (g_components->nav_button_create != nullptr) {
        const contracts::UiNavButtonDesc nav_desc{
            {kBackAbsX - inset_x, kBackAbsY - inset_y, kBackW, kBackH},
            contracts::kUiNavButtonAuto, nullptr, nullptr};
        const QolResult nav_result = g_components->nav_button_create(&nav_desc, g_root, &g_back);
        if (!contracts::succeeded(nav_result)) {
            g_back = nullptr;
            SETTINGS_LOG("settings nav create failed code=%d", nav_result.code);
        }
    } else {
        SETTINGS_LOG("settings nav create unavailable");
    }
    // 借用游戏主菜单共享背景图（ensure 加载幂等；共享图集不 unload）。
    if (g_ui->load_image_unit != nullptr) {
        (void)g_ui->load_image_unit(kBackgroundLeftUnit);
        (void)g_ui->load_image_unit(kBackgroundRightUnit);
        (void)g_ui->load_image_unit(0x0f);
    }
    resolve_module_version();
    (void)build_left_list();
    if (g_category_count != 0) (void)build_right_list(g_selected_category);
    update_summary();
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_panel_active = true;
    }
    SETTINGS_LOG("settings panel enter: root=%p cat=%u items=%u gen=%llu", g_root,
                 static_cast<unsigned>(g_category_count), static_cast<unsigned>(g_item_count),
                 static_cast<unsigned long long>(g_generation));
}

void settings_panel_process() {
    if (!main_thread()) return;
    if (!g_panel_active || g_root == nullptr || g_ui == nullptr || g_components == nullptr) return;
    ++g_frame;
    process_pending_category();
    process_pending_action();
    if (g_frame != g_last_poll_frame) {
        g_last_poll_frame = g_frame;
        poll_requests();
    }
    if (g_frame - g_last_refresh_frame >= kRefreshIntervalFrames) {
        g_last_refresh_frame = g_frame;
        refresh_all_values();
        refresh_availability();
    }
    process_pending_toggles();
    if (g_left != nullptr) (void)g_components->scroll_list_process(g_left);
    if (g_right != nullptr) (void)g_components->scroll_list_process(g_right);

    if (!contracts::succeeded(g_ui->begin_frame())) return;
    int64_t origin_x = 0;
    int64_t origin_y = 0;
    const bool have_origin =
        contracts::succeeded(g_ui->control_absolute_origin(g_root, &origin_x, &origin_y));
    // 背景：游戏主菜单共享图 0x4f/0x50 左/右半图，居中绘制在两栏容器之前；整页不再压黑蒙版。
    if (have_origin && g_ui->draw_image_part != nullptr) {
        const int64_t center_x = origin_x + kPanelW / 2;
        const int64_t center_y = origin_y + kPanelH / 2;
        (void)g_ui->draw_image_part(kBackgroundLeftUnit, kBackgroundLoc,
                                    static_cast<int32_t>(center_x - kBackgroundHalfOffset),
                                    static_cast<int32_t>(center_y), 2, 1);
        (void)g_ui->draw_image_part(kBackgroundRightUnit, kBackgroundLoc,
                                    static_cast<int32_t>(center_x + kBackgroundHalfOffset),
                                    static_cast<int32_t>(center_y), 2, 1);
    }
    // 标题：文本经组件自绘入口，不写 native 控件内联文本（32B 上限）。
    char title[128] = {};
    if (g_module_version[0] != '\0') {
        std::snprintf(title, sizeof(title), "QoL 模块设置 v%s", g_module_version);
    } else {
        std::snprintf(title, sizeof(title), "QoL 模块设置");
    }
    (void)g_components->draw_control_text(g_root, title, static_cast<int32_t>(kPanelW / 2), 24, kGold,
                                          2, 1);
    // 两栏先铺深灰，再绘原生金框；行本身保持透明，仅选中态填充。
    if (have_origin) {
        if (g_ui->fill_rect != nullptr) {
            (void)g_ui->fill_rect({origin_x + kLeftX - 8, origin_y + kLeftY - 8,
                                   kLeftW + 32, kLeftH + 16},
                                  contracts::ui_theme::kPanelBg);
            (void)g_ui->fill_rect({origin_x + kRightX - 8, origin_y + kRightY - 8,
                                   kRightW + 32, kRightH + 16},
                                  contracts::ui_theme::kPanelBg);
        }
        (void)g_components->draw_container(
            {origin_x + kLeftX - 8, origin_y + kLeftY - 8, kLeftW + 32, kLeftH + 16});
        (void)g_components->draw_container(
            {origin_x + kRightX - 8, origin_y + kRightY - 8, kRightW + 32, kRightH + 16});
    }
    if (g_left != nullptr) (void)g_components->scroll_list_draw(g_left);
    if (g_right != nullptr) (void)g_components->scroll_list_draw(g_right);

    // 状态行仅异常时出现（持久化/入口失败等），正常闲置不显示统计或占位。
    if (g_status[0] != '\0') {
        (void)g_components->draw_control_text(g_root, g_status, 24,
                                              static_cast<int32_t>(kPanelH - 72), kErrorText, 0, 1);
    }
    if (g_back != nullptr && g_components->nav_button_draw != nullptr) {
        (void)g_components->nav_button_draw(g_back);
    }
    (void)g_ui->end_frame();
}

void settings_panel_f3() {
    if (!main_thread()) return;
    destroy_page();
    SETTINGS_LOG("settings panel f3 (terminate)");
}

void settings_panel_f4() {}

uint32_t settings_panel_event(uint64_t event, uint64_t param, uint64_t param2) {
    if (!main_thread() || !g_panel_active || g_root == nullptr || g_components == nullptr) return 1;
    // 只处理 0x17 按下 / 0x18 松开 / 0x19 移动；0x8000000X 等 bit63 系统子事件伴随每次
    // 按下到达，不参与手势：直接消费，不转发也不得改变本地按下镜像（g_back_pressed 若
    // 在此清零，0x18 抬起会被下方门控拒绝，原生树收不到抬起，返回手势无法完成）。
    if (event != 0x17 && event != 0x18 && event != 0x19) return 1;
    // 0x19 移动按按下落点镜像路由：列表手势跟随 g_pressed_list；返回按钮手势（未落列表）
    // 跟随 root，移出取消交由原生 TouchHandle 按释放落点判定，模块侧不自行吞事件。
    if (event == 0x19) {
        uint32_t consumed = 0;
        const QolUiHandle event_root = g_pressed_list != nullptr ? g_pressed_list : g_root;
        const QolResult result = g_components->dispatch_native_event(
            event_root, event, param, param2, &consumed);
        if (!contracts::succeeded(result)) {
            SETTINGS_LOG("settings event dispatch failed code=%d", result.code);
        }
        return consumed;
    }
    if (param == 0 || g_ui == nullptr || g_ui->touch_coordinates == nullptr) return 1;
    int64_t x = 0;
    int64_t y = 0;
    if (!contracts::succeeded(g_ui->touch_coordinates(param, &x, &y))) return 1;

    // 返回按钮挂 root 的原生 ExecuteProc：按下/抬起各投递一次到原生事件树，由控件自身
    // 在释放落于按钮内时触发 back；不再调用 back_button_touch（原生树返回 Unsupported）。
    if (g_back != nullptr && g_ui->hit_test != nullptr) {
        const bool inside = g_ui->hit_test(g_back, x, y, {0, 0, kBackW, kBackH}) > 0;
        if ((event == 0x17 && inside) || (event == 0x18 && g_back_pressed)) {
            g_back_pressed = event == 0x17;
            uint32_t consumed = 0;
            const QolResult result = g_components->dispatch_native_event(
                g_root, event, param, param2, &consumed);
            if (!contracts::succeeded(result)) {
                SETTINGS_LOG("settings back event dispatch failed code=%d", result.code);
            }
            return consumed;
        }
    }

    if (event == 0x17) {
        g_pressed_list = g_ui->hit_test(g_right, x, y, {0, 0, kRightW, kRightH}) > 0
            ? g_right
            : g_ui->hit_test(g_left, x, y, {0, 0, kLeftW, kLeftH}) > 0 ? g_left : nullptr;
    }
    if (g_pressed_list == nullptr) return 1;
    uint32_t consumed = 0;
    const QolResult result = g_components->dispatch_native_event(
        g_pressed_list, event, param, param2, &consumed);
    if (!contracts::succeeded(result)) {
        SETTINGS_LOG("settings list event dispatch failed code=%d", result.code);
    }
    if (event == 0x18) g_pressed_list = nullptr;
    return 1;
}

// ---- 结果 JSON 助手（JNI/调试端点沿用）----

std::string result_ok() {
    char buffer[64] = {};
    uint32_t size = 0;
    if (g_sdk != nullptr && g_sdk->result.write_ok != nullptr &&
        contracts::succeeded(g_sdk->result.write_ok(buffer, sizeof(buffer), &size))) {
        return std::string(buffer, size);
    }
    return "{\"ok\":false,\"error\":\"result unavailable\"}";
}

std::string result_error(const char* message) {
    char buffer[256] = {};
    uint32_t size = 0;
    if (g_sdk != nullptr && g_sdk->result.write_error != nullptr && message != nullptr &&
        contracts::succeeded(g_sdk->result.write_error(message, buffer, sizeof(buffer), &size))) {
        return std::string(buffer, size);
    }
    return "{\"ok\":false,\"error\":\"result unavailable\"}";
}

const char* ui_screen_name() {
    const char* name = nullptr;
    if (g_ui != nullptr && g_ui->screen_name != nullptr &&
        contracts::succeeded(g_ui->screen_name(&name)) && name != nullptr) {
        return name;
    }
    return "";
}

// ---- 「更多游戏」按钮绑定（帧任务驱动；运行时控件实例槽）----

void* more_games_btn() {
    if (g_ui == nullptr || g_ui->find_main_menu_more_games == nullptr) return nullptr;
    void* button = nullptr;
    return contracts::succeeded(g_ui->find_main_menu_more_games(&button)) ? button : nullptr;
}

void more_games_clicked(void*) {
    SETTINGS_LOG("more games clicked -> open settings panel");
    if (!settings_panels_open("settings")) {
        SETTINGS_LOG("settings panel open failed from more games button");
    }
}

bool g_more_games_injected = false;
uint64_t g_more_games_binding_token = 0;
std::atomic<int32_t> g_more_games_bind{0};
std::atomic<bool> g_more_games_error{false};

class RuntimeMoreGamesBindOps final : public MoreGamesBindOps {
public:
    bool proc_matches() override {
        if (g_more_games_binding_token == 0) return false;
        void* button = more_games_btn();
        if (button == nullptr) return false;
        if (g_ui == nullptr || g_ui->execute_proc_matches == nullptr) return false;
        int32_t matches = 0;
        return contracts::succeeded(
                   g_ui->execute_proc_matches(button, &more_games_clicked, &matches)) &&
               matches != 0;
    }

    void release() override { settings_release_more_games(); }

    bool bind() override {
        void* button = more_games_btn();
        if (button == nullptr) return false;
        if (g_ui == nullptr || g_ui->bind_main_menu_more_games == nullptr) return false;
        uint64_t token = 0;
        if (!contracts::succeeded(g_ui->bind_main_menu_more_games(&more_games_clicked, &token)) ||
            token == 0) {
            return false;
        }
        g_more_games_binding_token = token;
        g_more_games_injected = true;
        SETTINGS_LOG("more games ExecuteProc bound: ctrl=%p", button);
        return true;
    }

    bool btn_resolvable() override { return more_games_btn() != nullptr; }

    void mark_live() override {
        g_more_games_bind.store(1, std::memory_order_relaxed);
        g_more_games_error.store(false, std::memory_order_relaxed);
    }

    void mark_failed(bool btn_resolvable) override {
        g_more_games_error.store(btn_resolvable, std::memory_order_relaxed);
        g_more_games_bind.store(0, std::memory_order_relaxed);
    }
};

bool settings_try_bind_more_games() {
    RuntimeMoreGamesBindOps ops;
    if (ops.proc_matches()) return true;
    if (more_games_btn() == nullptr) return false;
    ops.release();
    return ops.bind();
}

}  // namespace

// ---- 公开绑定与 PanelProviderSpec ----

void settings_bind_sdk(const contracts::QolHostSdk* sdk) {
    g_sdk = sdk;
    g_ui = sdk != nullptr ? sdk->ui : nullptr;
    g_catalog = contracts::settings_catalog_if_available(sdk);
    g_config = contracts::settings_config_if_available(sdk);
    g_components = nullptr;
    if (sdk != nullptr && sdk->capabilities.require != nullptr) {
        const void* iface = nullptr;
        if (contracts::succeeded(sdk->capabilities.require(
                contracts::kUiComponentsCapabilityId,
                {contracts::kUiContractMajor, contracts::kUiContractMinor}, &iface))) {
            g_components = static_cast<const contracts::QolUiComponents*>(iface);
        }
    }
}

bool settings_provider_open() { return g_sdk != nullptr; }

void settings_provider_close() {
    // 幂等：未激活时不触碰游戏 UI（避免栈对陈旧栈顶调用 close 时误关当前顶层面板）。
    if (!settings_provider_active()) return;
    settings_ui_close_panel();
}

bool settings_provider_active() {
    std::lock_guard<std::mutex> lock(g_mtx);
    return g_panel_active;
}

const char* settings_provider_block_reason() {
    std::lock_guard<std::mutex> lock(g_mtx);
    return g_panel_active ? "ui occupied: settings panel" : nullptr;
}

const contracts::PanelCallbacks* settings_panel_callbacks() {
    static const contracts::PanelCallbacks callbacks{
        &settings_panel_enter, &settings_panel_process, &settings_panel_f3, &settings_panel_f4,
        &settings_panel_event};
    return &callbacks;
}

void settings_ui_close_panel() {
    if (!main_thread()) return;
    settings_panel_f3();
}

// ---- 对外 API（原 data_settings_ui_*，名与签名保持）----

void settings_register_config_bridge(void* env, void* bridge_class) {
    (void)env;
    (void)bridge_class;
}

std::string data_settings_ui_inject() {
    if (g_sdk == nullptr || g_sdk->hooks.popup_resolve == nullptr) {
        return result_error("state entry inject failed");
    }
    if (!settings_try_bind_more_games()) {
        // 控件实例通常在启动期尚未创建：不视为失败，帧任务会在主菜单重试。
        SETTINGS_LOG("settings more games entry not ready; frame task will retry");
    }
    contracts::QolPopupStateEntry entry{};
    if (!contracts::succeeded(g_sdk->hooks.popup_resolve(kMoreGamesStateEntry, &entry))) {
        return result_error("state entry inject failed");
    }
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_state_id = entry.state_id;
    }
    return result_ok();
}

std::string data_settings_ui_status_json() {
    int categories = 0;
    int items = 0;
    int selected = 0;
    int state_id = -1;
    unsigned long long generation = 0;
    bool active = false;
    char category[contracts::kSettingsIdCapacity] = {};
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        active = g_panel_active;
        categories = g_summary_categories;
        items = g_summary_items;
        selected = g_summary_selected;
        state_id = g_state_id;
        generation = static_cast<unsigned long long>(g_summary_generation);
        std::snprintf(category, sizeof(category), "%s", g_summary_category);
    }
    char buffer[768] = {};
    std::snprintf(buffer, sizeof(buffer),
                  "{\"panel_active\":%s,\"categories\":%d,\"items\":%d,\"generation\":%llu,"
                  "\"selected\":%d,\"category\":\"%s\",\"more_games_bind\":\"%s\","
                  "\"state_id\":%d,\"screen\":\"%s\"}",
                  active ? "true" : "false", categories, items, generation, selected, category,
                  settings_more_games_bind_state(), state_id, ui_screen_name());
    return std::string(buffer);
}

std::string data_settings_ui_restore() {
    if (settings_provider_active()) settings_panels_close("settings");
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_summary_categories = 0;
        g_summary_items = 0;
        g_summary_selected = 0;
        g_summary_generation = 0;
        g_summary_category[0] = '\0';
    }
    settings_release_more_games();
    return result_ok();
}

std::string data_settings_ui_open_panel() {
    if (settings_panels_open("settings")) return result_ok();
    return result_error("state not injected");
}

std::string data_settings_ui_open_option() {
    if (g_sdk == nullptr || g_sdk->hooks.popup_resolve == nullptr) {
        return result_error("hooks API unavailable");
    }
    contracts::QolPopupStateEntry entry{};
    if (!contracts::succeeded(g_sdk->hooks.popup_resolve("F_PANEL_OPTIONS_ENTER", &entry))) {
        return result_error("option state not found");
    }
    if (g_ui == nullptr || g_ui->set_popup_process_info == nullptr ||
        !contracts::succeeded(g_ui->set_popup_process_info(1, entry.state_id))) {
        return result_error("ui state unavailable");
    }
    SETTINGS_LOG("open option state id=%d", entry.state_id);
    return result_ok();
}

// ---- 「更多游戏」按钮绑定状态/推进（settings.h 对外声明）----

void settings_release_more_games() {
    if (g_more_games_binding_token != 0 && g_ui != nullptr &&
        g_ui->unbind_main_menu_more_games != nullptr) {
        (void)g_ui->unbind_main_menu_more_games(g_more_games_binding_token);
    }
    g_more_games_binding_token = 0;
    g_more_games_injected = false;
    g_more_games_bind.store(0, std::memory_order_relaxed);
    g_more_games_error.store(false, std::memory_order_relaxed);
}

const char* settings_more_games_bind_state() {
    if (g_more_games_error.load(std::memory_order_relaxed)) return "error";
    if (!g_more_games_injected) return "waiting";
    // 只读判断：主线程帧任务写入的原子标志 + 句柄；此处可能被 HTTP 线程调用，
    // 只读取已发布状态，不再解引用游戏内存。
    return g_more_games_bind.load(std::memory_order_relaxed) == 1 ? "live" : "stale";
}

void settings_more_games_tick() {
    const char* screen = ui_screen_name();
    const bool on_main_menu = screen != nullptr && std::strcmp(screen, "main_menu") == 0;
    RuntimeMoreGamesBindOps ops;
    more_games_bind_tick(on_main_menu, ops);
}

}  // namespace qol::features::core_ui
