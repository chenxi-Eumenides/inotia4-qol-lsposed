#include "debug_components.h"

#if defined(QOL_DEBUG_COMPONENTS) && QOL_DEBUG_COMPONENTS

#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "../../../contracts/http.h"
#include "../../../contracts/panels.h"
#include "../../../contracts/ui.h"
#include "../../../contracts/ui_theme.h"
#include "../ui_components.h"
#include "stack.h"

namespace qol::features::core_ui {
namespace {

constexpr const char* kPanelId = "debug_components";
constexpr const char* kOwner = "core.custom_ui";
constexpr int64_t kFrameTopMargin = 18;
constexpr int64_t kFrameBottomMargin = 18;
constexpr int64_t kFrameRightMargin = 10;
constexpr int64_t kFrameLeftMargin = contracts::ui_theme::kNavBoxAbsX +
    contracts::ui_theme::kNavBoxW + 8;
constexpr int64_t kPaneInset = 8;
constexpr int64_t kPaneGap = 12;
constexpr int64_t kLeftPaneWidth = 200;
constexpr int64_t kLeftCategoryX = 16;
constexpr int64_t kLeftCategoryY = 68;
constexpr int64_t kLeftCategoryW = 168;
constexpr int64_t kLeftCategoryH = 48;
constexpr int64_t kLeftCategoryGap = 8;
constexpr uint32_t kStyleControlCount = 6;
constexpr uint32_t kOpacityStep = 10;
constexpr uint32_t kOpacityMax = 100;
constexpr uint32_t kCategoryCount = 8;
constexpr uint32_t kCategoryBox = 0;
constexpr uint32_t kCategoryText = 1;
constexpr uint32_t kCategoryArrow = 2;
constexpr uint32_t kCategoryIcon = 3;
constexpr uint32_t kCategoryButton = 4;
constexpr uint32_t kCategoryBackNav = 5;
constexpr uint32_t kCategoryRestricted = 6;
constexpr uint32_t kCategoryPalette = 7;
constexpr uint32_t kMaxPageHandles = 20;

// 配色页预览值：正式把选中行迁移到 #695031/90%（kRowSelectedBgRgb565/kRowSelectedAlpha）之前，
// 先用 ABGR 直绘在本页目视验收；迁移完成后再以统一 token 替换。
constexpr uint32_t kPaletteRowSelectedAbgr = 0xE5315069u;  // RGB(105,80,49) / 90%
// 半透明色块先铺一层中灰底，便于在深色面板上直接观察 alpha 叠加结果。
constexpr uint32_t kPaletteSwatchBase = 0xFF808080u;

struct FillColorOption {
    const char* name;
    uint32_t rgb565;
};

struct DebugPanelRequest {
    bool open = false;
    bool has_fill_rgb565 = false;
    uint32_t fill_rgb565 = 0;
    bool has_alpha = false;
    uint32_t alpha = 0;
    bool has_page = false;
    uint32_t page = 0;
    bool has_text_subpage = false;
    uint32_t text_subpage = 0;
};

struct PendingStyleUpdate {
    bool valid = false;
    bool has_fill_rgb565 = false;
    uint32_t fill_rgb565 = 0;
    bool has_alpha = false;
    uint32_t alpha = 0;
    bool has_page = false;
    uint32_t page = 0;
    bool has_text_subpage = false;
    uint32_t text_subpage = 0;
};

constexpr uint32_t kCommandPending = 1u << 31;
constexpr uint32_t kCommandOpen = 1u << 0;
constexpr uint32_t kCommandHasFill = 1u << 1;
constexpr uint32_t kCommandHasAlpha = 1u << 2;
constexpr uint32_t kCommandFillShift = 3;
constexpr uint32_t kCommandAlphaShift = 19;
constexpr uint64_t kCommandHasPage = 1ull << 29;
constexpr uint64_t kCommandPageShift = 26;
constexpr uint64_t kCommandPageMask = 0x7u;
constexpr uint64_t kCommandHasTextSubpage = 1ull << 32;
constexpr uint64_t kCommandTextSubpageShift = 33;
constexpr uint64_t kCommandTextSubpageMask = 0x1u;

uint64_t pack_debug_panel_request(const DebugPanelRequest& request) {
    uint64_t packed = kCommandPending | (request.open ? kCommandOpen : 0u);
    if (request.has_fill_rgb565) packed |= kCommandHasFill | (request.fill_rgb565 << kCommandFillShift);
    if (request.has_alpha) packed |= kCommandHasAlpha | (request.alpha << kCommandAlphaShift);
    if (request.has_page) packed |= kCommandHasPage | (static_cast<uint64_t>(request.page) << kCommandPageShift);
    if (request.has_text_subpage) {
        packed |= kCommandHasTextSubpage |
            (static_cast<uint64_t>(request.text_subpage) << kCommandTextSubpageShift);
    }
    return packed;
}

DebugPanelRequest unpack_debug_panel_request(uint64_t packed) {
    DebugPanelRequest request{};
    request.open = (packed & kCommandOpen) != 0;
    request.has_fill_rgb565 = (packed & kCommandHasFill) != 0;
    request.fill_rgb565 = (packed >> kCommandFillShift) & 0xffffu;
    request.has_alpha = (packed & kCommandHasAlpha) != 0;
    request.alpha = (packed >> kCommandAlphaShift) & 0x7fu;
    request.has_page = (packed & kCommandHasPage) != 0;
    request.page = static_cast<uint32_t>((packed >> kCommandPageShift) & kCommandPageMask);
    request.has_text_subpage = (packed & kCommandHasTextSubpage) != 0;
    request.text_subpage = static_cast<uint32_t>(
        (packed >> kCommandTextSubpageShift) & kCommandTextSubpageMask);
    return request;
}

constexpr FillColorOption kFillColors[] = {
    {"黑色", 0x0000}, {"灰色", 0x8410}, {"白色", 0xFFFF}, {"红色", 0xF800}};
constexpr uint32_t kFillColorCount = sizeof(kFillColors) / sizeof(kFillColors[0]);
constexpr const char* kCategoryNames[kCategoryCount] = {
    "方框", "文本按钮", "箭头按钮", "图标按钮", "普通控件按钮", "返回/导航", "列表/主菜单",
    "配色"};
constexpr size_t kStyledContainerApiSize =
    offsetof(contracts::QolUiComponents, draw_container_styled) +
    sizeof(((contracts::QolUiComponents*)nullptr)->draw_container_styled);
constexpr size_t kFlatButtonApiSize =
    offsetof(contracts::QolUiComponents, flat_text_button_create) +
    sizeof(((contracts::QolUiComponents*)nullptr)->flat_text_button_create);

contracts::UiRect g_panel_rect{0, 0, 960, 640};
contracts::UiRect g_panel_area_rect{};
contracts::UiRect g_left_panel_rect{};
contracts::UiRect g_right_panel_rect{};
contracts::UiRect g_preview_rect{};
contracts::UiRect g_panel_nav_rect{};
const contracts::QolHostSdk* g_sdk = nullptr;
std::atomic<bool> g_enabled{false};
uint64_t g_apply_task = 0;
bool g_active = false;
contracts::QolUiHandle g_root = nullptr;
contracts::QolUiHandle g_panel_nav = nullptr;
contracts::QolUiHandle g_touch_root = nullptr;
contracts::QolUiHandle g_category_controls[kCategoryCount]{};
contracts::QolUiHandle g_style_controls[kStyleControlCount]{};
struct PageHandle {
    contracts::QolUiHandle handle = nullptr;
    uint8_t kind = 0;  // 0 text/flat, 1 arrow, 2 icon, 3 native button, 4 back, 5 nav, 6 back preview.
    contracts::UiRect rect{};
};
PageHandle g_page_handles[kMaxPageHandles]{};
uint32_t g_page_handle_count = 0;
int32_t g_built_category = -1;
int32_t g_pending_category = -1;
bool g_page_rebuild_requested = false;
bool g_page_error = false;
uint32_t g_text_size_preset = 1;  // Default 60 high: leaves label room for decorated.
uint32_t g_arrow_size_preset = 0;
uint32_t g_icon_size_preset = 0;
bool g_text_enabled = true;
bool g_text_active = false;
bool g_text_long = false;
uint32_t g_text_style = 0;
uint32_t g_text_width_preset = 1;
uint32_t g_text_subpage = 0;
uint32_t g_back_size_preset = 1;
int64_t g_text_widths[3] = {204, 300, 360};
char g_text_width_labels[3][24] = {"204", "300", "最大 360"};
int32_t g_text_factory_error[5] = {0, 0, 0, 0, 0};
int32_t g_text_draw_error = 0;
int32_t g_native_pressed[3] = {-1, -1, -1};
uint32_t g_native_pressed_count = 0;
bool g_arrow_enabled = true;
bool g_icon_enabled = true;
uint32_t g_selected_category = kCategoryBox;
uint32_t g_category_control_count = 0;
uint32_t g_fill_rgb565 = 0;
uint32_t g_fill_opacity = 60;
uint32_t g_style_control_count = 0;
bool g_styled_container_available = false;
bool g_style_applied = false;
int32_t g_last_style_draw_error = 0;
int32_t g_last_shell_draw_error = 0;
std::atomic<uint64_t> g_requested_command{0};
PendingStyleUpdate g_pending_open_style{};

void debug_log(const char* message) {
    if (g_sdk == nullptr || g_sdk->log.write == nullptr || message == nullptr) return;
    (void)g_sdk->log.write(1, 12, "core.custom_ui/debug_components", message,
                           static_cast<uint32_t>(std::strlen(message)));
}

void log_result(const char* action, QolResult result) {
    char message[96]{};
    std::snprintf(message, sizeof(message), "%s result=%d", action, result.code);
    debug_log(message);
}

int32_t color_channel(uint32_t color, uint32_t shift) {
    return static_cast<int32_t>((color >> shift) & 0xffu);
}

const char* fill_color_name(uint32_t rgb565) {
    for (const auto& color : kFillColors) {
        if (color.rgb565 == rgb565) return color.name;
    }
    return "自定义";
}

void apply_style_update(const DebugPanelRequest& request) {
    if (request.has_fill_rgb565) g_fill_rgb565 = request.fill_rgb565;
    if (request.has_alpha) g_fill_opacity = request.alpha;
    if (request.has_fill_rgb565 || request.has_alpha) g_style_applied = false;
}

void apply_pending_open_style() {
    if (!g_pending_open_style.valid) return;
    const DebugPanelRequest request{true, g_pending_open_style.has_fill_rgb565,
        g_pending_open_style.fill_rgb565, g_pending_open_style.has_alpha,
        g_pending_open_style.alpha, g_pending_open_style.has_page,
        g_pending_open_style.page, g_pending_open_style.has_text_subpage,
        g_pending_open_style.text_subpage};
    g_pending_open_style = {};
    apply_style_update(request);
    if (request.has_page && request.page < kCategoryCount) {
        g_pending_category = static_cast<int32_t>(request.page);
    }
    if (request.has_text_subpage) {
        g_text_subpage = request.text_subpage & 1u;
    }
}

bool styled_container_api_available(const contracts::QolUiComponents* components) {
    return components != nullptr && components->major == contracts::kUiContractMajor &&
        components->minor >= 23 &&
        components->size >= kStyledContainerApiSize && components->draw_container_styled != nullptr;
}

enum class StyleControlAction : uintptr_t {
    kOpacityDown = kFillColorCount,
    kOpacityUp,
};

void style_control_clicked(void* user) {
    if (g_selected_category != kCategoryBox) return;
    const uintptr_t action = reinterpret_cast<uintptr_t>(user);
    if (action < kFillColorCount) {
        g_fill_rgb565 = kFillColors[action].rgb565;
    } else if (action == static_cast<uintptr_t>(StyleControlAction::kOpacityDown)) {
        g_fill_opacity = g_fill_opacity >= kOpacityStep ? g_fill_opacity - kOpacityStep : 0;
    } else if (action == static_cast<uintptr_t>(StyleControlAction::kOpacityUp)) {
        g_fill_opacity = g_fill_opacity <= kOpacityMax - kOpacityStep
            ? g_fill_opacity + kOpacityStep : kOpacityMax;
    } else {
        return;
    }
    g_style_applied = false;
}

enum class PendingPageAction : uint8_t {
    kNone = 0,
    kSizePreset,
    kTextEnabled,
    kTextActive,
    kArrowEnabled,
    kIconEnabled,
    kNavEnabled,
    kTextLength,
    kTextWidth,
    kTextStyle,
    kTextSubpage,
    kBackSize,
};

PendingPageAction g_pending_action = PendingPageAction::kNone;
uint32_t g_pending_action_value = 0;
bool g_nav_enabled = false;

void category_clicked(void* user) {
    const uintptr_t index = reinterpret_cast<uintptr_t>(user);
    if (index >= kCategoryCount) return;
    g_pending_category = static_cast<int32_t>(index);
}

bool create_category_controls(const contracts::QolUiComponents* components) {
    if (components == nullptr || components->minor < 16 ||
        components->size < kFlatButtonApiSize ||
        components->flat_text_button_create == nullptr || components->text_button_draw == nullptr) {
        debug_log("debug category controls unavailable: flat text button API missing");
        return false;
    }
    for (uint32_t i = 0; i < kCategoryCount; ++i) {
        const contracts::UiButtonDesc desc{
            {g_left_panel_rect.x + kLeftCategoryX,
             g_left_panel_rect.y + kLeftCategoryY +
                 static_cast<int64_t>(i) * (kLeftCategoryH + kLeftCategoryGap),
             kLeftCategoryW, kLeftCategoryH},
            kCategoryNames[i], 0, &category_clicked,
            reinterpret_cast<void*>(static_cast<uintptr_t>(i))};
        const QolResult result = components->flat_text_button_create(
            &desc, g_root, &g_category_controls[i]);
        if (!contracts::succeeded(result) || g_category_controls[i] == nullptr) {
            log_result("create debug category button", result);
            return false;
        }
        g_category_control_count = i + 1;
    }
    return true;
}

void size_preset_clicked(void* user) {
    g_pending_action = PendingPageAction::kSizePreset;
    g_pending_action_value = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user));
}

void text_enable_clicked(void*) { g_pending_action = PendingPageAction::kTextEnabled; }
void text_active_clicked(void*) { g_pending_action = PendingPageAction::kTextActive; }
void text_length_clicked(void*) { g_pending_action = PendingPageAction::kTextLength; }
void text_width_clicked(void* user) {
    g_pending_action = PendingPageAction::kTextWidth;
    g_pending_action_value = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user));
}
void text_style_clicked(void* user) {
    g_pending_action = PendingPageAction::kTextStyle;
    g_pending_action_value = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user));
}
void text_subpage_clicked(void*) { g_pending_action = PendingPageAction::kTextSubpage; }
void back_size_clicked(void* user) {
    g_pending_action = PendingPageAction::kBackSize;
    g_pending_action_value = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user));
}
void arrow_enable_clicked(void*) { g_pending_action = PendingPageAction::kArrowEnabled; }
void icon_enable_clicked(void*) { g_pending_action = PendingPageAction::kIconEnabled; }
void nav_enable_clicked(void*) { g_pending_action = PendingPageAction::kNavEnabled; }
void native_button_clicked(void*) {}

void add_page_handle(const contracts::QolUiComponents* components,
                     contracts::QolUiHandle handle, uint8_t kind) {
    if (components == nullptr || handle == nullptr || g_page_handle_count >= kMaxPageHandles) return;
    g_page_handles[g_page_handle_count++] = {handle, kind};
}

bool add_page_button(const contracts::QolUiComponents* components, contracts::UiRect rect,
                     const char* text, contracts::QolUiClickFn on_click, void* user) {
    contracts::UiButtonDesc desc{rect, text, 0, on_click, user};
    contracts::QolUiHandle handle = nullptr;
    const QolResult result = components->flat_text_button_create(&desc, g_root, &handle);
    if (!contracts::succeeded(result) || handle == nullptr) return false;
    add_page_handle(components, handle, 0);
    return true;
}

constexpr int64_t kTextSizeHeights[3] = {48, 60, 72};
constexpr int64_t kArrowSizes[3] = {48, 64, 96};
constexpr int64_t kIconSizes[3] = {48, 78, 120};
struct BackNavSize { int64_t w; int64_t h; };
constexpr BackNavSize kBackNavSizes[3] = {{64, 72}, {78, 86}, {96, 104}};

bool build_box_page(const contracts::QolUiComponents* components) {
    const int64_t left = g_right_panel_rect.x + 32;
    const int64_t top = g_right_panel_rect.y + 416;
    static constexpr const char* kColorButtonLabels[] = {
        "黑色 0000", "灰色 8410", "白色 FFFF", "红色 F800"};
    for (uint32_t i = 0; i < kFillColorCount; ++i) {
        const contracts::UiButtonDesc desc{{left + static_cast<int64_t>(i % 2) * 124,
            top + static_cast<int64_t>(i / 2) * 52, 112, 44},
            kColorButtonLabels[i], 0, &style_control_clicked,
            reinterpret_cast<void*>(static_cast<uintptr_t>(i))};
        if (!contracts::succeeded(components->flat_text_button_create(
                &desc, g_root, &g_style_controls[i])) || g_style_controls[i] == nullptr) {
            return false;
        }
        add_page_handle(components, g_style_controls[i], 0);
        g_style_control_count = i + 1;
    }
    const int64_t opacity_left = left + 252;
    const contracts::UiButtonDesc opacity_buttons[2] = {
        {{opacity_left, top, 80, 44}, "-10", 0, &style_control_clicked,
         reinterpret_cast<void*>(static_cast<uintptr_t>(StyleControlAction::kOpacityDown))},
        {{opacity_left + 92, top, 80, 44}, "+10", 0, &style_control_clicked,
         reinterpret_cast<void*>(static_cast<uintptr_t>(StyleControlAction::kOpacityUp))}};
    for (uint32_t i = 0; i < 2; ++i) {
        if (!contracts::succeeded(components->flat_text_button_create(
                &opacity_buttons[i], g_root, &g_style_controls[kFillColorCount + i])) ||
            g_style_controls[kFillColorCount + i] == nullptr) {
            return false;
        }
        add_page_handle(components, g_style_controls[kFillColorCount + i], 0);
        g_style_control_count = kFillColorCount + i + 1;
    }
    return true;
}

void apply_text_states(const contracts::QolUiComponents* components) {
    for (uint32_t i = 0; i < g_page_handle_count && i < 5; ++i) {
        if (g_page_handles[i].handle == nullptr) continue;
        (void)components->text_button_set_enabled(g_page_handles[i].handle,
                                                  g_text_enabled ? 1 : 0);
        (void)components->text_button_set_active(g_page_handles[i].handle,
                                                 g_text_active ? 1 : 0);
    }
}

int32_t adapter_major() {
    const contracts::QolUiAdapter* adapter = ui_adapter();
    return adapter == nullptr ? -1 : static_cast<int32_t>(adapter->major);
}

int32_t adapter_minor() {
    const contracts::QolUiAdapter* adapter = ui_adapter();
    return adapter == nullptr ? -1 : static_cast<int32_t>(adapter->minor);
}

void compute_text_widths() {
    int64_t max_width = g_right_panel_rect.w - 168;
    if (max_width < 204) max_width = 204;
    const int64_t mid_width = max_width > 300 ? 300 : max_width;
    g_text_widths[0] = 204;
    g_text_widths[1] = mid_width;
    g_text_widths[2] = max_width;
    std::snprintf(g_text_width_labels[0], sizeof(g_text_width_labels[0]), "%lld",
                  static_cast<long long>(g_text_widths[0]));
    std::snprintf(g_text_width_labels[1], sizeof(g_text_width_labels[1]), "%lld",
                  static_cast<long long>(g_text_widths[1]));
    std::snprintf(g_text_width_labels[2], sizeof(g_text_width_labels[2]), "最大 %lld",
                  static_cast<long long>(g_text_widths[2]));
}

bool build_text_page(const contracts::QolUiComponents* components) {
    compute_text_widths();
    for (auto& error : g_text_factory_error) error = 0;
    static int64_t logged_width = -1;
    static int64_t logged_height = -1;
    static uint32_t logged_style = UINT32_MAX;
    static uint32_t logged_subpage = UINT32_MAX;
    const int64_t configured_width =
        g_text_widths[g_text_width_preset < 3 ? g_text_width_preset : 0];
    const int64_t configured_height =
        kTextSizeHeights[g_text_size_preset < 3 ? g_text_size_preset : 0];
    if (configured_width != logged_width || configured_height != logged_height ||
        g_text_style != logged_style || g_text_subpage != logged_subpage) {
        logged_width = configured_width;
        logged_height = configured_height;
        logged_style = g_text_style;
        logged_subpage = g_text_subpage;
        char message[96]{};
        std::snprintf(message, sizeof(message),
                      "text page width=%lld height=%lld style=%u subpage=%u",
                      static_cast<long long>(configured_width),
                      static_cast<long long>(configured_height), g_text_style,
                      g_text_subpage);
        debug_log(message);
    }
    const int64_t height = kTextSizeHeights[g_text_size_preset < 3 ? g_text_size_preset : 0];
    const int64_t width = g_text_widths[g_text_width_preset < 3 ? g_text_width_preset : 0];
    const int64_t base = g_right_panel_rect.y + 84;
    const int64_t left = g_right_panel_rect.x + 32;
    const int64_t sample_x = g_right_panel_rect.x + 136;
    const char* sample_text = g_text_long ? "较长示例文本" : "示例";
    const uint32_t sample_count = g_text_subpage == 0 ? 3 : 2;
    const uint32_t first = g_text_subpage == 0 ? 0 : 3;
    for (uint32_t n = 0; n < sample_count; ++n) {
        const uint32_t i = first + n;
        const int64_t y = base + static_cast<int64_t>(n) * (height + 8);
        const contracts::UiButtonDesc desc{{sample_x, y, width, height}, sample_text,
            g_text_style, &native_button_clicked, nullptr};
        contracts::QolUiHandle handle = nullptr;
        QolResult result = contracts::error(kQolErrNotReady, "factory", 0);
        switch (i) {
            case 0: result = components->capsule_text_button_create(&desc, g_root, &handle); break;
            case 1: result = components->text_button_create(&desc, g_root, &handle); break;
            case 2: result = components->decorated_text_button_create(&desc, g_root, &handle); break;
            case 3: result = components->compact_capsule_text_button_create(&desc, g_root, &handle); break;
            default: result = components->flat_text_button_create(&desc, g_root, &handle); break;
        }
        if (!contracts::succeeded(result) || handle == nullptr) {
            g_text_factory_error[i] = contracts::succeeded(result) ? kQolErrNotReady : result.code;
            char message[96]{};
            std::snprintf(message, sizeof(message),
                          "text factory i=%u code=%d adapter=%u.%u components=%u.%u",
                          i, g_text_factory_error[i], adapter_major(), adapter_minor(),
                          components->major, components->minor);
            debug_log(message);
            continue;
        }
        add_page_handle(components, handle, 0);
    }
    apply_text_states(components);
    const int64_t controls = g_right_panel_rect.y + 330;
    for (uint32_t i = 0; i < 3; ++i) {
        if (!add_page_button(components, {left + static_cast<int64_t>(i) * 122, controls, 110, 42},
                g_text_width_labels[i], &text_width_clicked,
                reinterpret_cast<void*>(static_cast<uintptr_t>(i)))) return false;
    }
    static constexpr const char* kHeightLabels[3] = {"高 48", "高 60", "高 72"};
    for (uint32_t i = 0; i < 3; ++i) {
        if (!add_page_button(components, {left + static_cast<int64_t>(i) * 122, controls + 48,
                110, 42}, kHeightLabels[i], &size_preset_clicked,
                reinterpret_cast<void*>(static_cast<uintptr_t>(i)))) return false;
    }
    static constexpr const char* kStyleLabels[2] = {"字级 0", "字级 1"};
    for (uint32_t i = 0; i < 2; ++i) {
        if (!add_page_button(components, {left + static_cast<int64_t>(i) * 122, controls + 96,
                110, 42}, kStyleLabels[i], &text_style_clicked,
                reinterpret_cast<void*>(static_cast<uintptr_t>(i)))) return false;
    }
    if (!add_page_button(components, {g_right_panel_rect.x + g_right_panel_rect.w - 152,
            g_right_panel_rect.y + 18, 120, 42},
            g_text_subpage == 0 ? "下一页" : "上一页", &text_subpage_clicked, nullptr)) return false;
    if (!add_page_button(components, {left, controls + 140, 130, 44},
            g_text_enabled ? "禁用" : "启用", &text_enable_clicked, nullptr)) return false;
    if (!add_page_button(components, {left + 146, controls + 140, 130, 44},
            g_text_active ? "取消选中" : "设为选中", &text_active_clicked, nullptr)) return false;
    if (!add_page_button(components, {left + 292, controls + 140, 150, 44},
            g_text_long ? "恢复短文本" : "切换长文本", &text_length_clicked, nullptr)) return false;
    return true;
}

bool build_arrow_page(const contracts::QolUiComponents* components) {
    const int64_t size = kArrowSizes[g_arrow_size_preset < 3 ? g_arrow_size_preset : 0];
    const int64_t base = g_right_panel_rect.y + 80;
    const int64_t left = g_right_panel_rect.x + 32;
    for (uint32_t i = 0; i < 2; ++i) {
        const contracts::UiArrowButtonDesc desc{{left + static_cast<int64_t>(i) * 228, base,
            size, size}, i == 0 ? contracts::kUiArrowLeft : contracts::kUiArrowRight,
            static_cast<uint8_t>(g_arrow_enabled ? 1 : 0), &native_button_clicked, nullptr};
        contracts::QolUiHandle handle = nullptr;
        if (!contracts::succeeded(components->arrow_button_create(&desc, g_root, &handle)) ||
            handle == nullptr) return false;
        add_page_handle(components, handle, 1);
    }
    static constexpr const char* kSizeLabels[3] = {"48×48", "64×64", "96×96"};
    const int64_t controls = base + size + 70;
    for (uint32_t i = 0; i < 3; ++i) {
        if (!add_page_button(components, {left + static_cast<int64_t>(i) * 122, controls, 110, 42},
                kSizeLabels[i], &size_preset_clicked,
                reinterpret_cast<void*>(static_cast<uintptr_t>(i)))) return false;
    }
    if (!add_page_button(components, {left, controls + 52, 130, 44},
            g_arrow_enabled ? "禁用箭头" : "启用箭头", &arrow_enable_clicked, nullptr)) return false;
    return true;
}

bool build_icon_page(const contracts::QolUiComponents* components) {
    const int64_t size = kIconSizes[g_icon_size_preset < 3 ? g_icon_size_preset : 0];
    const int64_t base = g_right_panel_rect.y + 80;
    const int64_t left = g_right_panel_rect.x + 32;
    for (uint32_t i = 0; i < 2; ++i) {
        const contracts::UiIconButtonDesc desc{{left + static_cast<int64_t>(i) * 228, base,
            size, size}, 0x59, 0x03, 1, static_cast<uint8_t>(g_icon_enabled ? 1 : 0),
            "图标不可用", &native_button_clicked, nullptr, static_cast<uint8_t>(i == 0 ? 1 : 0)};
        contracts::QolUiHandle handle = nullptr;
        if (!contracts::succeeded(components->icon_button_create(&desc, g_root, &handle)) ||
            handle == nullptr) return false;
        add_page_handle(components, handle, 2);
    }
    static constexpr const char* kSizeLabels[3] = {"48×48", "78×78", "120×120"};
    const int64_t controls = base + size + 70;
    for (uint32_t i = 0; i < 3; ++i) {
        if (!add_page_button(components, {left + static_cast<int64_t>(i) * 122, controls, 110, 42},
                kSizeLabels[i], &size_preset_clicked,
                reinterpret_cast<void*>(static_cast<uintptr_t>(i)))) return false;
    }
    if (!add_page_button(components, {left, controls + 52, 130, 44},
            g_icon_enabled ? "禁用图标" : "启用图标", &icon_enable_clicked, nullptr)) return false;
    return true;
}

bool build_native_button_page(const contracts::QolUiComponents* components) {
    const int64_t left = g_right_panel_rect.x + 32;
    const int64_t max_width = g_right_panel_rect.w - 64;
    const contracts::UiRect rects[3] = {
        {left, g_right_panel_rect.y + 92, 204, 48},
        {left, g_right_panel_rect.y + 168, max_width, 60},
        {left, g_right_panel_rect.y + 256, 300, 80}};
    for (uint32_t i = 0; i < 3; ++i) {
        const contracts::UiButtonDesc desc{rects[i], "原生按钮", 0,
            &native_button_clicked, nullptr};
        contracts::QolUiHandle handle = nullptr;
        if (!contracts::succeeded(components->button_create(&desc, g_root, &handle)) ||
            handle == nullptr) return false;
        add_page_handle(components, handle, 3);
        if (g_page_handle_count > 0) g_page_handles[g_page_handle_count - 1].rect = rects[i];
    }
    return true;
}

bool build_back_nav_page(const contracts::QolUiComponents* components) {
    const BackNavSize size = kBackNavSizes[g_back_size_preset < 3 ? g_back_size_preset : 1];
    const int64_t left = g_right_panel_rect.x + 32;
    const int64_t top = g_right_panel_rect.y + 84;
    // Uses a real native control with a no-op ExecuteProc so the shared event root stays
    // dispatchable. Visuals come from adapter->draw_back_button; the helper path is avoided
    // because its component button has no ExecuteProc and blocks root event dispatch.
    const contracts::UiButtonDesc back_desc{{left, top, size.w, size.h}, "返回", 0,
        &native_button_clicked, nullptr};
    contracts::QolUiHandle back = nullptr;
    if (!contracts::succeeded(components->button_create(&back_desc, g_root, &back)) ||
        back == nullptr) return false;
    add_page_handle(components, back, 6);
    const int64_t step = size.w + 16;
    for (uint32_t i = 0; i < 3; ++i) {
        const contracts::UiNavButtonDesc desc{{left + step * static_cast<int64_t>(i + 1), top,
            size.w, size.h},
            i == 0 ? contracts::kUiNavButtonAuto :
            (i == 1 ? contracts::kUiNavButtonForceClose : contracts::kUiNavButtonForceBack),
            nullptr, nullptr};
        contracts::QolUiHandle handle = nullptr;
        if (!contracts::succeeded(components->nav_button_create(&desc, g_root, &handle)) ||
            handle == nullptr) return false;
        (void)components->nav_button_set_enabled(handle, g_nav_enabled ? 1 : 0);
        add_page_handle(components, handle, 5);
    }
    static constexpr const char* kSizeLabels[3] = {"小 64×72", "标准 78×86", "大 96×104"};
    for (uint32_t i = 0; i < 3; ++i) {
        if (!add_page_button(components, {left + static_cast<int64_t>(i) * 122,
                g_right_panel_rect.y + 222, 110, 42}, kSizeLabels[i], &back_size_clicked,
                reinterpret_cast<void*>(static_cast<uintptr_t>(i)))) return false;
    }
    if (!add_page_button(components, {left, g_right_panel_rect.y + 274, 150, 44},
            g_nav_enabled ? "禁用导航点击" : "启用导航点击", &nav_enable_clicked, nullptr)) return false;
    return true;
}

bool build_palette_page(const contracts::QolUiComponents*) {
    // 配色页全部为静态绘制（draw_palette_page），不创建任何交互控件。
    return true;
}

void destroy_page_handles() {
    const contracts::QolUiComponents* components = ui_components_api();
    if (components != nullptr && components->destroy != nullptr) {
        for (uint32_t i = 0; i < g_page_handle_count; ++i) {
            if (g_page_handles[i].handle != nullptr) {
                (void)components->destroy(g_page_handles[i].handle);
            }
        }
    }
    for (auto& entry : g_page_handles) entry = {};
    g_page_handle_count = 0;
    for (auto& control : g_style_controls) control = nullptr;
    g_style_control_count = 0;
}

bool rebuild_page(int32_t category) {
    destroy_page_handles();
    g_page_error = false;
    g_built_category = category;
    const contracts::QolUiComponents* components = ui_components_api();
    if (components == nullptr) {
        g_page_error = true;
        return false;
    }
    bool ok = true;
    switch (category) {
        case static_cast<int32_t>(kCategoryBox): ok = build_box_page(components); break;
        case static_cast<int32_t>(kCategoryText): ok = build_text_page(components); break;
        case static_cast<int32_t>(kCategoryArrow): ok = build_arrow_page(components); break;
        case static_cast<int32_t>(kCategoryIcon): ok = build_icon_page(components); break;
        case static_cast<int32_t>(kCategoryButton): ok = build_native_button_page(components); break;
        case static_cast<int32_t>(kCategoryBackNav): ok = build_back_nav_page(components); break;
        case static_cast<int32_t>(kCategoryPalette): ok = build_palette_page(components); break;
        default: ok = true; break;
    }
    if (!ok) {
        destroy_page_handles();
        g_page_error = true;
        log_result("build debug page", contracts::error(kQolErrInternal, "page build failed", 0));
    }
    return ok;
}

void apply_pending_page_action() {
    if (g_pending_action == PendingPageAction::kNone) return;
    bool rebuild = false;
    switch (g_pending_action) {
        case PendingPageAction::kSizePreset:
            if (g_selected_category == kCategoryText) g_text_size_preset = g_pending_action_value;
            else if (g_selected_category == kCategoryArrow) g_arrow_size_preset = g_pending_action_value;
            else if (g_selected_category == kCategoryIcon) g_icon_size_preset = g_pending_action_value;
            rebuild = true;
            break;
        case PendingPageAction::kTextEnabled: g_text_enabled = !g_text_enabled; rebuild = true; break;
        case PendingPageAction::kTextActive: g_text_active = !g_text_active; rebuild = true; break;
        case PendingPageAction::kTextLength: g_text_long = !g_text_long; rebuild = true; break;
        case PendingPageAction::kTextWidth:
            g_text_width_preset = g_pending_action_value < 3 ? g_pending_action_value : 0;
            rebuild = true;
            break;
        case PendingPageAction::kTextStyle:
            g_text_style = g_pending_action_value < 2 ? g_pending_action_value : 0;
            rebuild = true;
            break;
        case PendingPageAction::kTextSubpage: g_text_subpage ^= 1u; rebuild = true; break;
        case PendingPageAction::kBackSize:
            g_back_size_preset = g_pending_action_value < 3 ? g_pending_action_value : 1;
            rebuild = true;
            break;
        case PendingPageAction::kArrowEnabled: g_arrow_enabled = !g_arrow_enabled; rebuild = true; break;
        case PendingPageAction::kIconEnabled: g_icon_enabled = !g_icon_enabled; rebuild = true; break;
        case PendingPageAction::kNavEnabled: g_nav_enabled = !g_nav_enabled; rebuild = true; break;
        default: break;
    }
    g_pending_action = PendingPageAction::kNone;
    g_pending_action_value = 0;
    if (rebuild) rebuild_page(g_built_category);
}

QolResult draw_styled_container(const contracts::QolUiComponents* components,
                                contracts::UiRect rect, uint32_t rgb565, uint32_t opacity) {
    if (components == nullptr || components->draw_container_styled == nullptr) {
        return contracts::error(kQolErrNotReady, "styled container unavailable", 28);
    }
    const contracts::UiContainerStyleDesc style{rect, rgb565, opacity, 1};
    return components->draw_container_styled(&style);
}

// 帮助页选项行的原生封装：背景/按下效果全部由 adapter 的 draw_help_option_row 绘制。
// 标题区域用 selected=1、选项/次级区域用 selected=0；按钮按下时 pressed=1。
void palette_native_option_row(const contracts::QolUiAdapter* adapter,
                               contracts::UiRect rect, bool selected, bool pressed,
                               const char* text) {
    if (adapter->draw_help_option_row != nullptr) {
        (void)adapter->draw_help_option_row(rect, selected ? 1u : 0u, pressed ? 1u : 0u);
    }
    if (adapter->set_font_color_rgb != nullptr && adapter->draw_string_with_font != nullptr) {
        (void)adapter->set_font_color_rgb(color_channel(contracts::ui_theme::kTextGold, 0),
                                          color_channel(contracts::ui_theme::kTextGold, 8),
                                          color_channel(contracts::ui_theme::kTextGold, 16));
        (void)adapter->draw_string_with_font(
            text, rect.x + 16, rect.y + (rect.h > 22 ? (rect.h - 22) / 2 : 0), 0, 0);
    }
}

void draw_palette_page(const contracts::QolUiAdapter* adapter,
                       const contracts::QolUiComponents* components) {
    const auto set_color = [adapter](uint32_t color) {
        (void)adapter->set_font_color_rgb(color_channel(color, 0), color_channel(color, 8),
                                          color_channel(color, 16));
    };
    const auto draw_text = [adapter](const char* string, int32_t x, int32_t y) {
        (void)adapter->draw_string_with_font(string, x, y, 0, 0);
    };
    const bool can_fill = adapter->fill_rect != nullptr;
    const auto fill = [&](contracts::UiRect rect, uint32_t color) {
        if (can_fill) (void)adapter->fill_rect(rect, color);
    };
    const int32_t x = static_cast<int32_t>(g_right_panel_rect.x + 32);
    const int32_t y = static_cast<int32_t>(g_right_panel_rect.y);
    constexpr int32_t kBoxW = 480;
    constexpr int32_t kBoxH = 160;
    const int32_t px = x + kBoxW + 8;  // 右侧色板列，宽 76

    // —— 面板/方框：160x80 放大到 480x160，三层 60/84/93% 叠层 ——
    set_color(contracts::ui_theme::kTextGold);
    draw_text("方框 3x / 2x（60/84/93%）", x, y + 52);
    if (g_styled_container_available && components != nullptr) {
        (void)draw_styled_container(components, {x, y + 74, kBoxW, kBoxH},
                                    contracts::ui_theme::kPanelBgRgb565,
                                    contracts::ui_theme::kPanelAlpha);
        (void)draw_styled_container(components, {x + 16, y + 86, kBoxW - 32, kBoxH - 24},
                                    contracts::ui_theme::kPanelBgRgb565,
                                    contracts::ui_theme::kPanelAlpha);
        (void)draw_styled_container(components, {x + 32, y + 98, kBoxW - 64, kBoxH - 48},
                                    contracts::ui_theme::kPanelBgRgb565,
                                    contracts::ui_theme::kPanelAlpha);
    } else {
        draw_text("容器接口不可用", x, y + 74);
    }

    // —— 区域铺在面板之上：标题区域用帮助页选中项；次级/选项区域用非选中项；均静态 ——
    palette_native_option_row(adapter, {x + 42, y + 104, 396, 36}, true, false,
                              "标题区域（帮助页选中项 · 静态）");
    palette_native_option_row(adapter, {x + 42, y + 146, 396, 28}, false, false,
                              "次级标题区域（非选中项 · 静态）");
    palette_native_option_row(adapter, {x + 42, y + 178, 396, 28}, false, false,
                              "选项区域（非选中项 · 静态）");

    // —— 按钮：独立于面板/区域，帮助页选中项复用为可选/未选组件；按下态走原生 0x10 贴片 ——
    set_color(contracts::ui_theme::kTextGold);
    draw_text("按钮组件（帮助页选项 · 原生按下）", x, y + 244);
    constexpr int32_t kHalf = (480 - 8) / 2;
    palette_native_option_row(adapter, {x, y + 268, kHalf, 48}, false, true,
                              "未选中 · 按下");
    palette_native_option_row(adapter, {x + kHalf + 8, y + 268, kHalf, 48}, true, true,
                              "选中 · 按下");

    // —— 正文与次级说明：正文白色，次级灰；按钮下方两行 ——
    set_color(contracts::ui_theme::kTextBody);
    draw_text("正文 #FFFFFF", x, y + 330);
    set_color(contracts::ui_theme::kTextNormal);
    draw_text("次级 #C1C7C8；禁用 #A7AAA8。", x, y + 354);

    // —— 色板：右侧窄列，16 项；当前高度足够，不需要滚动 ——
    struct PaletteEntry {
        const char* name;
        uint32_t color;
    };
    constexpr PaletteEntry kEntries[] = {
        {"标题", 0xE5315069u}, {"次级", contracts::ui_theme::kRowBg},
        {"金线", contracts::ui_theme::kFrameGold}, {"正文", contracts::ui_theme::kTextBody},
        {"金", contracts::ui_theme::kTextGold}, {"次灰", contracts::ui_theme::kTextNormal},
        {"禁用", contracts::ui_theme::kTextDisabled}, {"错误", contracts::ui_theme::kError},
        {"成功", contracts::ui_theme::kOk}, {"按下", contracts::ui_theme::kRowPressedBg},
        {"面板", contracts::ui_theme::kPanelBg}, {"按钮", contracts::ui_theme::kButtonBg},
        {"按按", contracts::ui_theme::kButtonBgPressed}, {"关", contracts::ui_theme::kSwitchTrackOff},
        {"开", contracts::ui_theme::kSwitchTrackOn}, {"遮罩", 0x4C000000u},
    };
    set_color(contracts::ui_theme::kTextGold);
    draw_text("色板", px, y + 52);
    int32_t py = y + 76;
    for (const auto& entry : kEntries) {
        fill({px, py + 4, 12, 12}, 0xFF808080u);
        fill({px, py + 4, 12, 12}, entry.color);
        set_color(contracts::ui_theme::kTextNormal);
        draw_text(entry.name, px + 16, py + 2);
        py += 24;
    }
    set_color(contracts::ui_theme::kTextNormal);
    draw_text("ABGR", px, y + 472);
    draw_text("RGB565", px, y + 496);
}

void update_native_button_states(const contracts::QolUiAdapter* adapter) {
    g_native_pressed_count = 0;
    for (auto& state : g_native_pressed) state = -1;
    if (adapter == nullptr || adapter->control_pressed == nullptr) return;
    for (uint32_t i = 0; i < g_page_handle_count && g_native_pressed_count < 3; ++i) {
        if (g_page_handles[i].kind != 3 || g_page_handles[i].handle == nullptr) continue;
        int32_t pressed = 0;
        g_native_pressed[g_native_pressed_count] =
            contracts::succeeded(adapter->control_pressed(g_page_handles[i].handle, &pressed))
            ? (pressed != 0 ? 1 : 0) : -1;
        ++g_native_pressed_count;
    }
}

void draw_page_handles(const contracts::QolUiAdapter* adapter,
                       const contracts::QolUiComponents* components) {
    for (uint32_t i = 0; i < g_page_handle_count; ++i) {
        const PageHandle& entry = g_page_handles[i];
        if (entry.handle == nullptr) continue;
        switch (entry.kind) {
            case 1: if (components->arrow_button_draw) (void)components->arrow_button_draw(entry.handle); break;
            case 2: if (components->icon_button_draw) (void)components->icon_button_draw(entry.handle); break;
            case 3: {
                // The native control has no DrawProc; draw_control intentionally produces no
                // default skin. The outline below is debug-only hitbox visualization.
                if (adapter->draw_control) (void)adapter->draw_control(entry.handle);
                if (adapter->fill_rect != nullptr) {
                    static uint32_t native_index = 0;
                    // Reset per frame: first kind-3 handle seen starts at index 0.
                    if (i == 0 || g_page_handles[i - 1].kind != 3) native_index = 0;
                    const int32_t pressed = native_index < g_native_pressed_count
                        ? g_native_pressed[native_index] : -1;
                    const uint32_t color = pressed < 0 ? contracts::ui_theme::kTextDisabled :
                        (pressed != 0 ? contracts::ui_theme::kOk : contracts::ui_theme::kFrameGold);
                    const contracts::UiRect& r = entry.rect;
                    (void)adapter->fill_rect({r.x, r.y, r.w, 1}, color);
                    (void)adapter->fill_rect({r.x, r.y + r.h - 1, r.w, 1}, color);
                    (void)adapter->fill_rect({r.x, r.y, 1, r.h}, color);
                    (void)adapter->fill_rect({r.x + r.w - 1, r.y, 1, r.h}, color);
                    ++native_index;
                }
                break;
            }
            case 4: if (components->back_button_draw) (void)components->back_button_draw(entry.handle); break;
            case 6:
                if (adapter->control_pressed != nullptr && adapter->draw_back_button != nullptr) {
                    int32_t pressed = 0;
                    if (contracts::succeeded(adapter->control_pressed(entry.handle, &pressed))) {
                        (void)adapter->draw_back_button(entry.handle,
                                                        static_cast<uint8_t>(pressed != 0));
                    }
                }
                break;
            case 5: if (components->nav_button_draw) (void)components->nav_button_draw(entry.handle); break;
            default:
                if (components->text_button_draw) {
                    const QolResult draw_result = components->text_button_draw(entry.handle);
                    if (!contracts::succeeded(draw_result)) {
                        if (g_text_draw_error != draw_result.code) {
                            char message[80]{};
                            std::snprintf(message, sizeof(message),
                                          "text button draw failed code=%d", draw_result.code);
                            debug_log(message);
                            g_text_draw_error = draw_result.code;
                        }
                    } else {
                        g_text_draw_error = 0;
                    }
                }
                break;
        }
    }
}

void draw_right_page_text(const contracts::QolUiAdapter* adapter,
                          const contracts::QolUiComponents* components) {
    const auto set_color = [adapter](uint32_t color) {
        (void)adapter->set_font_color_rgb(color_channel(color, 0), color_channel(color, 8),
                                          color_channel(color, 16));
    };
    const int32_t right_x = static_cast<int32_t>(g_right_panel_rect.x + 32);
    const int32_t right_y = static_cast<int32_t>(g_right_panel_rect.y);
    const int32_t right_w = static_cast<int32_t>(g_right_panel_rect.w);
    const int32_t line_wrap_70 = right_w - 64;
    (void)line_wrap_70;

    set_color(contracts::ui_theme::kTextGold);
    (void)adapter->draw_string_with_font(kCategoryNames[g_selected_category],
        right_x, right_y + 28, 0, 0);

    if (g_selected_category == kCategoryBox) {
        set_color(contracts::ui_theme::kTextNormal);
        char preview_status[96]{};
        std::snprintf(preview_status, sizeof(preview_status), "%s：%s 0x%04X · 不透明度 %u%%",
                      g_style_applied ? "已应用" : "未应用",
                      fill_color_name(g_fill_rgb565), g_fill_rgb565, g_fill_opacity);
        (void)adapter->draw_string_with_font(preview_status, right_x, right_y + 340, 0, 0);
        if (!g_styled_container_available) {
            (void)adapter->draw_string_with_font("样式接口不可用，未绘制", right_x, right_y + 364, 0, 0);
        } else if (!g_style_applied) {
            char failure[72]{};
            std::snprintf(failure, sizeof(failure), "示意未应用（绘制错误 %d）", g_last_style_draw_error);
            (void)adapter->draw_string_with_font(failure, right_x, right_y + 364, 0, 0);
        } else {
            char preview_size[64]{};
            std::snprintf(preview_size, sizeof(preview_size), "示意 %lld × %lld",
                          static_cast<long long>(g_preview_rect.w),
                          static_cast<long long>(g_preview_rect.h));
            (void)adapter->draw_string_with_font(preview_size, right_x, right_y + 364, 0, 0);
        }
        (void)adapter->draw_string_with_font("底色 RGB565", right_x, right_y + 396, 0, 0);
        const int64_t opacity_left = g_right_panel_rect.x + 32 + 252;
        char opacity_status[64]{};
        std::snprintf(opacity_status, sizeof(opacity_status), "不透明度 %u%%", g_fill_opacity);
        (void)adapter->draw_string_with_font(opacity_status, static_cast<int32_t>(opacity_left),
                                             right_y + 396, 0, 0);
        (void)adapter->draw_string_with_font("0 全透明 · 100 全不透明",
            static_cast<int32_t>(opacity_left), right_y + 476, 0, 0);
        char color_status[80]{};
        std::snprintf(color_status, sizeof(color_status), "当前 %s 0x%04X",
                      fill_color_name(g_fill_rgb565), g_fill_rgb565);
        (void)adapter->draw_string_with_font(color_status, right_x, right_y + 528, 0, 0);
        (void)adapter->draw_string_with_font("仅影响右侧示例，两个外壳固定黑色 60%",
                                             right_x, right_y + 556, 0, 0);
    } else if (g_selected_category == kCategoryText) {
        set_color(contracts::ui_theme::kTextNormal);
        const int64_t height = kTextSizeHeights[g_text_size_preset < 3 ? g_text_size_preset : 0];
        const int64_t width = g_text_widths[g_text_width_preset < 3 ? g_text_width_preset : 0];
        const int64_t base = right_y + 84;
        const uint32_t first = g_text_subpage == 0 ? 0 : 3;
        const uint32_t count = g_text_subpage == 0 ? 3 : 2;
        const char* labels[5] = {"胶囊", "遗留", "装饰", "紧凑胶囊", "平面"};
        for (uint32_t n = 0; n < count; ++n) {
            const uint32_t i = first + n;
            const int64_t y = base + static_cast<int64_t>(n) * (height + 8);
            (void)adapter->draw_string_with_font(labels[i], right_x,
                static_cast<int32_t>(y + (height - 20) / 2), 0, 0);
            if (g_text_factory_error[i] != 0) {
                char missing[64]{};
                std::snprintf(missing, sizeof(missing), "工厂不可用 code %d",
                              g_text_factory_error[i]);
                (void)adapter->draw_string_with_font(missing,
                    static_cast<int32_t>(g_right_panel_rect.x + 136),
                    static_cast<int32_t>(y + (height - 20) / 2), 0, 0);
            }
        }
        char page_state[128]{};
        std::snprintf(page_state, sizeof(page_state), "%u/2 %s · %lld×%lld · 字级%u",
                      g_text_subpage + 1, g_text_subpage == 0 ? "纹理" : "紧凑/平面",
                      static_cast<long long>(width), static_cast<long long>(height),
                      g_text_style);
        (void)adapter->draw_string_with_font(page_state, right_x, right_y + 62, 0, 0);
        const int64_t controls = right_y + 330;
        char toggle_state[128]{};
        std::snprintf(toggle_state, sizeof(toggle_state), "%s · %s · %s",
                      g_text_enabled ? "可用" : "禁用",
                      g_text_active ? "选中 active" : "未选中",
                      g_text_long ? "长文本" : "短文本");
        (void)adapter->draw_string_with_font(toggle_state, right_x,
            static_cast<int32_t>(controls + 188), 0, 0);
        // Rows below the toggle row: state 544, variant 568, limits 592 at 640h;
        // last row ends at 612 inside the pane bottom 614.
        if (g_text_subpage == 0) {
            (void)adapter->draw_string_with_font("胶囊/遗留：横向三片，只横向拉伸，不纵向拉长。",
                right_x, static_cast<int32_t>(controls + 212), 0, 0);
            (void)adapter->draw_string_with_font("装饰：中段约 10px 竖向平铺，四角不拉伸。",
                right_x, static_cast<int32_t>(controls + 236), 0, 0);
        } else {
            (void)adapter->draw_string_with_font("紧凑胶囊仍横向三片；平面 80×42 起，2px 锐边。",
                right_x, static_cast<int32_t>(controls + 212), 0, 0);
            (void)adapter->draw_string_with_font("字级 0/1 只切原生字号；颜色不提供伪装。",
                right_x, static_cast<int32_t>(controls + 236), 0, 0);
        }
    } else if (g_selected_category == kCategoryArrow) {
        set_color(contracts::ui_theme::kTextNormal);
        const int64_t size = kArrowSizes[g_arrow_size_preset < 3 ? g_arrow_size_preset : 0];
        const int64_t base = right_y + 80;
        (void)adapter->draw_string_with_font("左", static_cast<int32_t>(right_x + size / 2),
                                             static_cast<int32_t>(base + size + 6), 0, 0);
        (void)adapter->draw_string_with_font("右", static_cast<int32_t>(right_x + 228 + size / 2),
                                             static_cast<int32_t>(base + size + 6), 0, 0);
        (void)adapter->draw_string_with_font(
            g_arrow_enabled ? "可用：按下切换原生按压图。点击无业务回调。" :
                              "禁用：不响应触摸，仍绘制固定外观。",
            right_x, static_cast<int32_t>(base + size + 196), 0, 0);
        (void)adapter->draw_string_with_font("尺寸按短边等比，可选 48/64/96；无颜色配置。",
            right_x, static_cast<int32_t>(base + size + 222), 0, 0);
        (void)adapter->draw_string_with_font("矩形宽高须为正，过小会退化不可读。",
            right_x, static_cast<int32_t>(base + size + 248), 0, 0);
    } else if (g_selected_category == kCategoryIcon) {
        set_color(contracts::ui_theme::kTextNormal);
        const int64_t size = kIconSizes[g_icon_size_preset < 3 ? g_icon_size_preset : 0];
        const int64_t base = right_y + 80;
        (void)adapter->draw_string_with_font("带底框", static_cast<int32_t>(right_x),
                                             static_cast<int32_t>(base + size + 6), 0, 0);
        (void)adapter->draw_string_with_font("无底框", static_cast<int32_t>(right_x + 228),
                                             static_cast<int32_t>(base + size + 6), 0, 0);
        (void)adapter->draw_string_with_font(
            g_icon_enabled ? "可用：按下使用固定底色，不提供颜色参数。" :
                             "禁用：不响应触摸，仍显示图标。",
            right_x, static_cast<int32_t>(base + size + 196), 0, 0);
        (void)adapter->draw_string_with_font("1–400% 等比；底框开关按样本分别展示。",
            right_x, static_cast<int32_t>(base + size + 222), 0, 0);
        (void)adapter->draw_string_with_font("图集缺失时显示 fallback 文本。",
            right_x, static_cast<int32_t>(base + size + 248), 0, 0);
    } else if (g_selected_category == kCategoryButton) {
        set_color(contracts::ui_theme::kTextNormal);
        const int64_t max_width = g_right_panel_rect.w - 64;
        char wide[64]{};
        std::snprintf(wide, sizeof(wide), "宽 %lld×60 命中框", static_cast<long long>(max_width));
        (void)adapter->draw_string_with_font("窄 204×48 命中框", right_x, right_y + 72, 0, 0);
        (void)adapter->draw_string_with_font(wide, right_x, right_y + 148, 0, 0);
        (void)adapter->draw_string_with_font("高 300×80 命中框", right_x, right_y + 236, 0, 0);
        (void)adapter->draw_string_with_font("描边为调试命中框，仅作诊断。",
            right_x, right_y + 352, 0, 0);
        (void)adapter->draw_string_with_font("button_create 本身不设置 DrawProc/默认外观。",
            right_x, right_y + 376, 0, 0);
        const char* pressed_text = "真实按下：未按下";
        bool unavailable = false;
        bool pressed_any = false;
        for (uint32_t i = 0; i < g_native_pressed_count; ++i) {
            if (g_native_pressed[i] < 0) unavailable = true;
            else if (g_native_pressed[i] != 0) pressed_any = true;
        }
        if (g_native_pressed_count == 0 || unavailable) pressed_text = "真实按下：状态不可读（缺少接口）";
        else if (pressed_any) pressed_text = "真实按下：按下";
        (void)adapter->draw_string_with_font(pressed_text, right_x, right_y + 400, 0, 0);
        (void)adapter->draw_string_with_font("点击为真实 no-op 回调，不退出页面。",
            right_x, right_y + 426, 0, 0);
        (void)adapter->draw_string_with_font("无 enabled/active/颜色接口，不能禁用或改色。",
            right_x, right_y + 452, 0, 0);
        (void)adapter->draw_string_with_font("文本写入原生内联缓冲，超过 31 字节有风险。",
            right_x, right_y + 478, 0, 0);
    } else if (g_selected_category == kCategoryBackNav) {
        set_color(contracts::ui_theme::kTextNormal);
        const BackNavSize size = kBackNavSizes[g_back_size_preset < 3 ? g_back_size_preset : 1];
        const int64_t top = right_y + 84;
        const int64_t step = size.w + 16;
        const int64_t label_y = top + size.h + 8;
        (void)adapter->draw_string_with_font("返回", right_x, static_cast<int32_t>(label_y), 0, 0);
        (void)adapter->draw_string_with_font("自动", right_x + static_cast<int32_t>(step),
            static_cast<int32_t>(label_y), 0, 0);
        (void)adapter->draw_string_with_font("强制 X", right_x + static_cast<int32_t>(step * 2),
            static_cast<int32_t>(label_y), 0, 0);
        (void)adapter->draw_string_with_font("强制返回", right_x + static_cast<int32_t>(step * 3),
            static_cast<int32_t>(label_y), 0, 0);
        (void)adapter->draw_string_with_font("实际命中框随预设变化；图标纹理固定，不随 rect 缩放。",
            right_x, right_y + 336, 0, 0);
        (void)adapter->draw_string_with_font("返回为原生外观预览，读取真实按下态；未绑定返回动作。",
            right_x, right_y + 362, 0, 0);
        (void)adapter->draw_string_with_font("导航启用后点击会退出本页；禁用不响应触摸。",
            right_x, right_y + 388, 0, 0);
        (void)adapter->draw_string_with_font("Auto / 强制 X / 强制返回 三种视觉模式保留。",
            right_x, right_y + 414, 0, 0);
    } else if (g_selected_category == kCategoryPalette) {
        draw_palette_page(adapter, components);
    } else {
        set_color(contracts::ui_theme::kTextNormal);
        (void)adapter->draw_string_with_font("主菜单入口、锚定按钮与列表行按钮", right_x, right_y + 80, 0, 0);
        (void)adapter->draw_string_with_font("需要游戏原生父控件或列表行，不能建在独立 Debug root。",
            right_x, right_y + 108, 0, 0);
        (void)adapter->draw_string_with_font("本页不创建控件，也不伪造预览；请在对应游戏界面验证。",
            right_x, right_y + 136, 0, 0);
    }

    if (g_page_error) {
        char error[64]{};
        std::snprintf(error, sizeof(error), "页面控件创建失败，已清理。");
        (void)adapter->draw_string_with_font(error,
            static_cast<int32_t>(g_left_panel_rect.x + 32),
            static_cast<int32_t>(g_left_panel_rect.y + 548), 0, 0);
    }
    if (g_last_shell_draw_error != 0) {
        char shell_error[64]{};
        std::snprintf(shell_error, sizeof(shell_error), "面板外框绘制错误 %d", g_last_shell_draw_error);
        (void)adapter->draw_string_with_font(shell_error,
            static_cast<int32_t>(g_left_panel_rect.x + 32),
            static_cast<int32_t>(g_left_panel_rect.y + 520), 0, 0);
    }
}

void draw_debug_panels(const contracts::QolUiAdapter* adapter,
                       const contracts::QolUiComponents* components) {
    if (adapter == nullptr || components == nullptr) return;
    if (g_styled_container_available) {
        const QolResult left_result = draw_styled_container(
            components, g_left_panel_rect, 0x0000, 60);
        const QolResult right_result = draw_styled_container(
            components, g_right_panel_rect, 0x0000, 60);
        if (!contracts::succeeded(left_result) || !contracts::succeeded(right_result)) {
            const QolResult failure = !contracts::succeeded(left_result) ? left_result : right_result;
            if (g_last_shell_draw_error != failure.code) {
                char message[96]{};
                std::snprintf(message, sizeof(message), "debug pane shell draw failed code=%d",
                              failure.code);
                debug_log(message);
                g_last_shell_draw_error = failure.code;
            }
        } else {
            g_last_shell_draw_error = 0;
        }
        if (g_selected_category == kCategoryBox) {
            const QolResult preview_result = draw_styled_container(
                components, g_preview_rect, g_fill_rgb565, g_fill_opacity);
            if (!contracts::succeeded(preview_result)) {
                g_style_applied = false;
                if (g_last_style_draw_error != preview_result.code) {
                    char message[96]{};
                    std::snprintf(message, sizeof(message), "debug preview draw failed code=%d",
                                  preview_result.code);
                    debug_log(message);
                    g_last_style_draw_error = preview_result.code;
                }
            } else {
                g_style_applied = true;
                g_last_style_draw_error = 0;
            }
        }
    }

    if (g_selected_category == kCategoryButton) update_native_button_states(adapter);
    if (adapter->draw_string_with_font == nullptr || adapter->set_font_color_rgb == nullptr) return;
    const auto set_color = [adapter](uint32_t color) {
        (void)adapter->set_font_color_rgb(color_channel(color, 0), color_channel(color, 8),
                                          color_channel(color, 16));
    };
    const int32_t left_x = static_cast<int32_t>(g_left_panel_rect.x + 32);
    const int32_t left_y = static_cast<int32_t>(g_left_panel_rect.y);
    set_color(contracts::ui_theme::kTextGold);
    (void)adapter->draw_string_with_font("选项", left_x, left_y + 28, 0, 0);

    if (g_selected_category < 0 || g_selected_category >= kCategoryCount) return;
    const int64_t marker_y = g_left_panel_rect.y + kLeftCategoryY +
        static_cast<int64_t>(g_selected_category) * (kLeftCategoryH + kLeftCategoryGap);
    if (adapter->fill_rect != nullptr) {
        (void)adapter->fill_rect({g_left_panel_rect.x + 20, marker_y + 8, 4, 32},
                                 contracts::ui_theme::kTextGold);
    }
    draw_right_page_text(adapter, components);
}

void destroy_components() {
    const contracts::QolUiComponents* components = ui_components_api();
    if (g_root != nullptr && components != nullptr && components->destroy != nullptr) {
        (void)components->destroy(g_root);
    }
    g_root = nullptr;
    g_panel_nav = nullptr;
    g_touch_root = nullptr;
    for (auto& control : g_category_controls) control = nullptr;
    destroy_page_handles();
    g_category_control_count = 0;
    g_selected_category = kCategoryBox;
    g_built_category = -1;
    g_pending_category = -1;
    g_pending_action = PendingPageAction::kNone;
    g_page_error = false;
    g_styled_container_available = false;
    g_style_applied = false;
    g_last_style_draw_error = 0;
    g_last_shell_draw_error = 0;
    g_pending_open_style = {};
    g_active = false;
}

QolResult create_components() {
    const contracts::QolUiComponents* components = ui_components_api();
    const contracts::QolUiAdapter* adapter = ui_adapter();
    if (components == nullptr || adapter == nullptr || components->panel_create == nullptr ||
        components->nav_button_create == nullptr || components->nav_button_draw == nullptr ||
        components->destroy == nullptr) {
        return contracts::error(kQolErrNotReady, "debug ui components unavailable", 30);
    }
    g_fill_rgb565 = 0;
    g_fill_opacity = 60;
    g_selected_category = kCategoryBox;
    g_style_applied = false;
    g_last_style_draw_error = 0;
    apply_pending_open_style();
    g_styled_container_available = styled_container_api_available(components);
    if (!g_styled_container_available) {
        debug_log("styled container API unavailable; no legacy fallback or style controls");
    }
    if (adapter->screen_inset_width == nullptr || adapter->screen_inset_height == nullptr) {
        return contracts::error(kQolErrNotReady, "inset unavailable", 16);
    }
    int32_t inset_width = 0;
    int32_t inset_height = 0;
    if (!contracts::succeeded(adapter->screen_inset_width(&inset_width)) ||
        !contracts::succeeded(adapter->screen_inset_height(&inset_height)) ||
        inset_width < 0 || inset_height < 0) {
        return contracts::error(kQolErrInvalidArgument, "bad inset", 9);
    }
    const int64_t screen_width = 960 + static_cast<int64_t>(inset_width) * 2;
    const int64_t screen_height = 640 + static_cast<int64_t>(inset_height) * 2;
    if (screen_width < 960 || screen_height < 640) {
        return contracts::error(kQolErrInvalidArgument, "screen too small", 16);
    }
    g_panel_rect = {0, 0, screen_width, screen_height};
    g_panel_area_rect = {kFrameLeftMargin, kFrameTopMargin,
        screen_width - kFrameLeftMargin - kFrameRightMargin,
        screen_height - kFrameTopMargin - kFrameBottomMargin};
    const int64_t pane_height = g_panel_area_rect.h - 2 * kPaneInset;
    const int64_t right_pane_width = g_panel_area_rect.w -
        2 * kPaneInset - kLeftPaneWidth - kPaneGap;
    if (pane_height < 48 || right_pane_width < 48) {
        return contracts::error(kQolErrInvalidArgument, "debug panes too small", 21);
    }
    g_left_panel_rect = {g_panel_area_rect.x + kPaneInset,
        g_panel_area_rect.y + kPaneInset, kLeftPaneWidth, pane_height};
    g_right_panel_rect = {g_left_panel_rect.x + kLeftPaneWidth + kPaneGap,
        g_left_panel_rect.y, right_pane_width, pane_height};
    g_preview_rect = {g_right_panel_rect.x + 32, g_right_panel_rect.y + 76,
        g_right_panel_rect.w - 64, 240};
    g_panel_nav_rect = {contracts::ui_theme::kNavBoxAbsX, contracts::ui_theme::kNavBoxAbsY,
                        contracts::ui_theme::kNavBoxW, contracts::ui_theme::kNavBoxH};

    const contracts::UiPanelDesc panel_desc{g_panel_rect, g_panel_rect, 0, 0};
    QolResult result = components->panel_create(&panel_desc, nullptr, &g_root);
    if (!contracts::succeeded(result) || g_root == nullptr) {
        destroy_components();
        return contracts::succeeded(result)
            ? contracts::error(kQolErrNotReady, "panel create returned null", 26) : result;
    }

    const contracts::UiNavButtonDesc nav_desc{
        g_panel_nav_rect, contracts::kUiNavButtonAuto, nullptr, nullptr};
    result = components->nav_button_create(&nav_desc, g_root, &g_panel_nav);
    if (!contracts::succeeded(result)) {
        destroy_components();
        return result;
    }
    if (adapter->image_unit_load != nullptr) {
        (void)adapter->image_unit_load(0x4f);
        (void)adapter->image_unit_load(0x50);
        (void)adapter->image_unit_load(0x0f);
        (void)adapter->image_unit_load(0x59);
    }
    if (!create_category_controls(components)) {
        destroy_components();
        return contracts::error(kQolErrNotReady, "debug category controls unavailable", 31);
    }
    if (!rebuild_page(static_cast<int32_t>(kCategoryBox))) {
        destroy_components();
        return contracts::error(kQolErrNotReady, "debug page controls unavailable", 32);
    }
    g_active = true;
    return contracts::ok();
}

void panel_enter() {
    debug_log("panel enter");
    if (g_root == nullptr) {
        const QolResult result = create_components();
        if (!contracts::succeeded(result)) log_result("create", result);
    } else {
        apply_pending_open_style();
        g_active = true;
    }
}

void panel_process() {
    if (!g_active || g_root == nullptr) return;
    const contracts::QolUiAdapter* adapter = ui_adapter();
    const contracts::QolUiComponents* components = ui_components_api();
    if (adapter == nullptr || components == nullptr || adapter->begin_frame == nullptr ||
        adapter->end_frame == nullptr) {
        return;
    }
    if (g_pending_category >= 0) {
        if (g_pending_category != g_built_category) {
            g_selected_category = static_cast<uint32_t>(g_pending_category);
            rebuild_page(g_pending_category);
            if (g_pending_category == static_cast<int32_t>(kCategoryText)) {
                g_page_rebuild_requested = false;
            }
        }
        g_pending_category = -1;
    }
    apply_pending_page_action();
    if (g_page_rebuild_requested) {
        g_page_rebuild_requested = false;
        if (g_selected_category == kCategoryText) rebuild_page(kCategoryText);
    }
    if (!contracts::succeeded(adapter->begin_frame())) return;
    int64_t root_x = 0;
    int64_t root_y = 0;
    if (adapter->control_absolute_origin != nullptr && adapter->draw_image_part != nullptr &&
        contracts::succeeded(adapter->control_absolute_origin(g_root, &root_x, &root_y))) {
        const int64_t panel_center_x = root_x + g_panel_rect.x + g_panel_rect.w / 2;
        const int64_t panel_center_y = root_y + g_panel_rect.y + g_panel_rect.h / 2;
        (void)adapter->draw_image_part(0x4f, 0,
            static_cast<int32_t>(panel_center_x - 300),
            static_cast<int32_t>(panel_center_y), 2, 1);
        (void)adapter->draw_image_part(0x50, 0,
            static_cast<int32_t>(panel_center_x + 300),
            static_cast<int32_t>(panel_center_y), 2, 1);
    }
    draw_debug_panels(adapter, components);
    if (components->text_button_draw != nullptr) {
        for (uint32_t i = 0; i < g_category_control_count; ++i) {
            if (g_category_controls[i] != nullptr) {
                (void)components->text_button_draw(g_category_controls[i]);
            }
        }
    }
    draw_page_handles(adapter, components);
    if (g_panel_nav != nullptr && components->nav_button_draw != nullptr) {
        (void)components->nav_button_draw(g_panel_nav);
    }
    if (!contracts::succeeded(adapter->end_frame())) debug_log("end frame failed");
}

void panel_close() { destroy_components(); }
void panel_f3() { (void)ui_components_api()->back_request(); }
void panel_f4() { (void)ui_components_api()->back_request(); }

uint32_t panel_event(uint64_t event, uint64_t param, uint64_t param2) {
    if (!g_active || g_root == nullptr) return 0;
    const contracts::QolUiComponents* components = ui_components_api();
    if (components == nullptr || components->dispatch_native_event == nullptr) return 1;
    if (event != 0x17 && event != 0x18 && event != 0x19) return 1;
    if (event == 0x17) g_touch_root = g_root;
    if (g_touch_root == nullptr) return 1;
    uint32_t consumed = 0;
    (void)components->dispatch_native_event(g_touch_root, event, param, param2, &consumed);
    if (event == 0x18) g_touch_root = nullptr;
    return 1;
}

const contracts::PanelCallbacks kCallbacks{
    &panel_enter, &panel_process, &panel_f3, &panel_f4, &panel_event};

bool provider_open() { return g_sdk != nullptr; }
void provider_close() { panel_close(); }
bool provider_active() { return g_active; }
const char* provider_block_reason() { return g_active ? "ui occupied: debug components" : nullptr; }

const contracts::PanelProviderSpec kProvider{
    kPanelId, kOwner, &provider_open, &provider_close, &provider_active,
    &provider_block_reason, &kCallbacks};

QolResult write_response(contracts::HttpRes* response, const char* body, uint16_t status = 200) {
    if (response == nullptr || response->body == nullptr || body == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "bad request", 11);
    }
    const uint32_t size = static_cast<uint32_t>(std::strlen(body));
    if (response->body_capacity < size) {
        response->status = 500;
        return contracts::error(kQolErrInternal, "response buffer too small", 25);
    }
    std::memcpy(response->body, body, size);
    response->body_size = size;
    response->status = status;
    return contracts::ok();
}

void skip_json_whitespace(std::string_view* json) {
    const size_t start = json->find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) {
        json->remove_prefix(json->size());
    } else if (start != 0) {
        json->remove_prefix(start);
    }
}

bool take_json_token(std::string_view* json, std::string_view token) {
    skip_json_whitespace(json);
    if (json->substr(0, token.size()) != token) return false;
    json->remove_prefix(token.size());
    return true;
}

bool take_json_key(std::string_view* json, std::string_view* key) {
    skip_json_whitespace(json);
    if (json->empty() || json->front() != '"') return false;
    json->remove_prefix(1);
    const size_t end = json->find('"');
    if (end == std::string_view::npos) return false;
    const std::string_view parsed_key = json->substr(0, end);
    if (parsed_key.find('\\') != std::string_view::npos) return false;
    for (const unsigned char ch : parsed_key) {
        if (ch < 0x20) return false;
    }
    *key = parsed_key;
    json->remove_prefix(end + 1);
    return true;
}

bool take_json_uint(std::string_view* json, uint32_t maximum, uint32_t* out) {
    skip_json_whitespace(json);
    size_t digits = 0;
    while (digits < json->size() && (*json)[digits] >= '0' && (*json)[digits] <= '9') ++digits;
    if (digits == 0 || (digits > 1 && json->front() == '0')) return false;
    uint32_t parsed = 0;
    const auto result = std::from_chars(json->data(), json->data() + digits, parsed);
    if (result.ec != std::errc{} || result.ptr != json->data() + digits || parsed > maximum) {
        return false;
    }
    json->remove_prefix(digits);
    skip_json_whitespace(json);
    if (!json->empty() && json->front() != ',' && json->front() != '}') return false;
    *out = parsed;
    return true;
}

bool parse_debug_panel_request(std::string_view json, DebugPanelRequest* out,
                               const char** error_message) {
    if (out == nullptr || error_message == nullptr) return false;
    *out = {};
    *error_message = "invalid JSON object";
    if (!take_json_token(&json, "{")) return false;

    DebugPanelRequest request{};
    bool has_open = false;
    if (!take_json_token(&json, "}")) {
        while (true) {
            std::string_view key;
            if (!take_json_key(&json, &key) || !take_json_token(&json, ":")) return false;
            if (key == "open") {
                if (has_open) {
                    *error_message = "duplicate field";
                    return false;
                }
                has_open = true;
                if (take_json_token(&json, "true")) request.open = true;
                else if (take_json_token(&json, "false")) request.open = false;
                else {
                    *error_message = "open must be boolean";
                    return false;
                }
            } else if (key == "fill_rgb565") {
                if (request.has_fill_rgb565) {
                    *error_message = "duplicate field";
                    return false;
                }
                if (!take_json_uint(&json, 65535, &request.fill_rgb565)) {
                    *error_message = "fill_rgb565 must be an integer from 0 to 65535";
                    return false;
                }
                request.has_fill_rgb565 = true;
            } else if (key == "alpha") {
                if (request.has_alpha) {
                    *error_message = "duplicate field";
                    return false;
                }
                if (!take_json_uint(&json, 100, &request.alpha)) {
                    *error_message = "alpha must be an integer from 0 to 100";
                    return false;
                }
                request.has_alpha = true;
            } else if (key == "page") {
                if (request.has_page) {
                    *error_message = "duplicate field";
                    return false;
                }
                if (!take_json_uint(&json, kCategoryCount - 1, &request.page)) {
                    *error_message = "page must be an integer from 0 to 7";
                    return false;
                }
                request.has_page = true;
            } else if (key == "text_subpage") {
                if (request.has_text_subpage) {
                    *error_message = "duplicate field";
                    return false;
                }
                if (!take_json_uint(&json, 1, &request.text_subpage)) {
                    *error_message = "text_subpage must be 0 or 1";
                    return false;
                }
                request.has_text_subpage = true;
            } else {
                *error_message = "unknown field";
                return false;
            }

            if (take_json_token(&json, "}")) break;
            if (!take_json_token(&json, ",")) return false;
        }
    }
    skip_json_whitespace(&json);
    if (!json.empty()) return false;
    if (!has_open) {
        *error_message = "open boolean required";
        return false;
    }
    if (!request.open && (request.has_fill_rgb565 || request.has_alpha || request.has_page ||
                          request.has_text_subpage)) {
        *error_message = "style/page fields require open:true";
        return false;
    }
    *out = request;
    return true;
}

QolResult command_handler(const contracts::HttpReq* request, contracts::HttpRes* response, void*) {
    if (!g_enabled.load(std::memory_order_acquire)) {
        return write_response(response, "{\"ok\":false,\"error\":\"custom ui disabled\"}", 503);
    }
    if (request == nullptr || request->body == nullptr) {
        return write_response(response, "{\"ok\":false,\"error\":\"open boolean required\"}", 400);
    }
    DebugPanelRequest parsed{};
    const char* error_message = nullptr;
    if (!parse_debug_panel_request(
            std::string_view(reinterpret_cast<const char*>(request->body), request->body_len),
            &parsed, &error_message)) {
        char error_body[160]{};
        std::snprintf(error_body, sizeof(error_body), "{\"ok\":false,\"error\":\"%s\"}",
                      error_message == nullptr ? "invalid request" : error_message);
        return write_response(response, error_body, 400);
    }
    const QolResult response_result =
        write_response(response, "{\"ok\":true,\"queued\":true}");
    if (!contracts::succeeded(response_result)) return response_result;
    g_requested_command.store(pack_debug_panel_request(parsed), std::memory_order_release);
    return response_result;
}

int32_t apply_request(int64_t, void*) {
    const contracts::QolUiAdapter* adapter = ui_adapter();
    if (adapter != nullptr && adapter->is_main_thread != nullptr &&
        adapter->is_main_thread() == 0) return 1;
    const uint64_t packed = g_requested_command.exchange(0, std::memory_order_acq_rel);
    if ((packed & kCommandPending) == 0) return 1;
    const DebugPanelRequest request = unpack_debug_panel_request(packed);
    if (!g_enabled.load(std::memory_order_acquire) || !request.open) {
        g_pending_open_style = {};
        ui_panels_close(kPanelId);
        char message[96]{};
        std::snprintf(message, sizeof(message), "apply requested=close active=%d",
                      ui_panels_active(kPanelId) ? 1 : 0);
        debug_log(message);
        return 1;
    }
    const bool opened = ui_panels_open(kPanelId);
    if (opened) {
        const bool has_style = request.has_fill_rgb565 || request.has_alpha;
        if (g_active) {
            if (has_style) apply_style_update(request);
            if (request.has_page) {
                g_pending_category = static_cast<int32_t>(request.page);
            }
            if (request.has_text_subpage) {
                g_text_subpage = request.text_subpage & 1u;
                g_page_rebuild_requested = true;
            }
        } else if (has_style || request.has_page || request.has_text_subpage) {
            g_pending_open_style = {true, request.has_fill_rgb565, request.fill_rgb565,
                                    request.has_alpha, request.alpha, request.has_page,
                                    request.page, request.has_text_subpage,
                                    request.text_subpage};
        }
        // No fields: keep any queued style/page and the current values; repeated open must not reset.
    }
    char message[96]{};
    std::snprintf(message, sizeof(message),
                  "apply requested=open page=%d sub=%d result=%d active=%d",
                  request.has_page ? static_cast<int>(request.page) : -1,
                  request.has_text_subpage ? static_cast<int>(request.text_subpage) : -1,
                  opened ? 1 : 0, ui_panels_active(kPanelId) ? 1 : 0);
    debug_log(message);
    return 1;
}

}  // namespace

QolResult debug_components_http_command(const contracts::HttpReq* request,
                                        contracts::HttpRes* response, void* ctx) {
    return command_handler(request, response, ctx);
}

bool debug_components_register(const contracts::QolHostSdk* sdk) {
    if (sdk == nullptr || sdk->panels.register_provider == nullptr || sdk->scheduler.add == nullptr) {
        return false;
    }
    g_sdk = sdk;
    if (!contracts::succeeded(sdk->panels.register_provider(&kProvider))) return false;
    if (!contracts::succeeded(sdk->scheduler.add(1, &apply_request, nullptr, 1, 0,
                                                 &g_apply_task)) || g_apply_task == 0) {
        if (sdk->panels.unregister_owner != nullptr) (void)sdk->panels.unregister_owner(kOwner);
        g_sdk = nullptr;
        return false;
    }
    return true;
}

void debug_components_unregister(const contracts::QolHostSdk* sdk) {
    ui_panels_close(kPanelId);
    if (sdk != nullptr) {
        if (g_apply_task != 0 && sdk->scheduler.remove != nullptr) {
            (void)sdk->scheduler.remove(g_apply_task);
        }
        if (sdk->panels.unregister_owner != nullptr) (void)sdk->panels.unregister_owner(kOwner);
    }
    g_apply_task = 0;
    g_requested_command.store(0, std::memory_order_release);
    g_enabled.store(false, std::memory_order_release);
    g_sdk = nullptr;
}

void debug_components_set_enabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    if (!enabled) {
        g_requested_command.store(0, std::memory_order_release);
        ui_panels_close(kPanelId);
    }
}

}  // namespace qol::features::core_ui

#else

namespace qol::features::core_ui {
bool debug_components_register(const contracts::QolHostSdk*) { return true; }
void debug_components_unregister(const contracts::QolHostSdk*) {}
void debug_components_set_enabled(bool) {}
}  // namespace qol::features::core_ui

#endif
