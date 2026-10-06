#include "debug_components.h"

#if defined(QOL_DEBUG_COMPONENTS) && QOL_DEBUG_COMPONENTS

#include <atomic>
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
constexpr const char* kRoute = "/api/debug/custom-ui/components";

// 双栏页：面板撑满 960×640；左右 viewport 相对 panel，扩框/裁剪由 help_page 处理。
constexpr contracts::UiRect kPanelRect{0, 0, 960, 640};
constexpr contracts::UiRect kLeftViewport{16, 16, 204, 608};
constexpr contracts::UiRect kRightViewport{260, 16, 684, 608};
constexpr int32_t kRowHeight = 56;
// 面板关闭入口：自适应导航按钮，位于右栏顶部，不与分类控件行重叠（x 888 >= 504+220）。
constexpr contracts::UiRect kPanelNavRect{888, 24, 48, 48};

// 分类清单：新增调试内容 = 加一条名称，并在 build_category_controls 与
// draw_category_detail 各加一个分支；两类内容互不冲突。
constexpr const char* kCategoryNames[] = {
    "文本按钮", "开关胶囊", "X/导航按钮", "主题调色板", "框架边框"};
constexpr uint32_t kCategoryCount = sizeof(kCategoryNames) / sizeof(kCategoryNames[0]);

// 右栏交互控件：text_button 走手工命中绘制，nav_button 走自适应绘制。
struct CategoryControl {
    contracts::QolUiHandle handle = nullptr;
    uint8_t is_nav = 0;  // 1 = nav_button，0 = text_button。
};

constexpr uint32_t kMaxCategoryControls = 8;
CategoryControl g_category_controls[kMaxCategoryControls]{};
uint32_t g_category_control_count = 0;
int32_t g_selected_category = -1;

const contracts::QolHostSdk* g_sdk = nullptr;
std::atomic<int32_t> g_requested_state{-1};
std::atomic<bool> g_enabled{false};
uint64_t g_apply_task = 0;
bool g_active = false;
contracts::QolUiHandle g_root = nullptr;
contracts::QolUiHandle g_page = nullptr;
contracts::QolUiHandle g_panel_nav = nullptr;
// 单手势根：双栏页交互控件都在 g_root 下，0x17 统一以 g_root 为根派发，0x18 后清空。
contracts::QolUiHandle g_touch_root = nullptr;
uint32_t g_demo_click_count = 0;

int32_t color_channel(uint32_t color, uint32_t shift) {
    return static_cast<int32_t>((color >> shift) & 0xffu);
}

void set_text_color(const contracts::QolUiAdapter* adapter, uint32_t color) {
    if (adapter == nullptr || adapter->set_font_color_rgb == nullptr) return;
    (void)adapter->set_font_color_rgb(color_channel(color, 0), color_channel(color, 8),
                                      color_channel(color, 16));
}

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

// 首枚文本按钮演示点击计数；仅第 0 枚写入文本，其余按钮点击忽略。
void demo_button_clicked(void* user) {
    if (reinterpret_cast<uintptr_t>(user) != 0) return;
    ++g_demo_click_count;
    char text[32]{};
    std::snprintf(text, sizeof(text), "点击 %u", g_demo_click_count);
    const contracts::QolUiComponents* components = ui_components_api();
    if (components != nullptr && components->button_set_text != nullptr &&
        g_category_control_count > 0 && g_category_controls[0].handle != nullptr) {
        (void)components->button_set_text(g_category_controls[0].handle, text);
    }
}

// —— 左栏分类列表行：背景/边框走绘制原语，文本经 draw_control_text 不写内联缓冲。 ——
void draw_category_row(void*, uint32_t index, contracts::QolUiHandle row, uint8_t selected) {
    const contracts::QolUiAdapter* adapter = ui_adapter();
    const contracts::QolUiComponents* components = ui_components_api();
    if (adapter == nullptr || components == nullptr || index >= kCategoryCount) return;
    if (adapter->draw_custom_button != nullptr) {
        (void)adapter->draw_custom_button(row, {0, 0, kLeftViewport.w, kRowHeight},
            selected ? 0xFF51412Cu : 0xFF211D1Au, selected ? 0xFFE6C989u : 0xFF5B4C39u,
            selected ? 2 : 1, nullptr, 0, 0, 0);
    }
    (void)components->draw_control_text(row, kCategoryNames[index], 16, 16,
        selected ? 0xFFFFE9B4u : 0xFFF2E8D5u, 0, 1);
}

// —— 分类 0：文本按钮（交互）。三枚按钮：点击计数 / 禁用 / 长文本。 ——
bool build_text_buttons(const contracts::QolUiComponents* components,
                        contracts::QolUiHandle parent, CategoryControl* out, uint32_t* out_count) {
    if (components->text_button_create == nullptr || components->text_button_set_enabled == nullptr) {
        return false;
    }
    const contracts::UiRect rects[3] = {{280, 128, 220, 48}, {504, 128, 220, 48},
                                        {280, 180, 220, 48}};
    const char* labels[3] = {"打开", "不可用", "较长的 UTF-8 按钮文字示例"};
    for (uint32_t i = 0; i < 3; ++i) {
        const contracts::UiButtonDesc desc{rects[i], labels[i], 0, &demo_button_clicked,
            reinterpret_cast<void*>(static_cast<uintptr_t>(i))};
        contracts::QolUiHandle handle = nullptr;
        const QolResult result = components->text_button_create(&desc, parent, &handle);
        if (!contracts::succeeded(result) || handle == nullptr) return false;
        out[i] = {handle, 0};
        *out_count = i + 1;
    }
    // 第二枚演示禁用态：仍绘制，但点击不再触发。
    (void)components->text_button_set_enabled(out[1].handle, 0);
    return true;
}

// —— 分类 2：X/导航按钮演示（交互）。auto / 强制 X / 强制返回箭头。 ——
bool build_nav_demos(const contracts::QolUiComponents* components,
                     contracts::QolUiHandle parent, CategoryControl* out, uint32_t* out_count) {
    if (components->nav_button_create == nullptr) return false;
    // 盒尺寸取原生 78×86（与设置页一致），间距 88 使三枚金属框不重叠。
    const contracts::UiRect rects[3] = {{280, 128, 78, 86}, {368, 128, 78, 86},
                                        {456, 128, 78, 86}};
    const uint32_t modes[3] = {contracts::kUiNavButtonAuto, contracts::kUiNavButtonForceClose,
                               contracts::kUiNavButtonForceBack};
    for (uint32_t i = 0; i < 3; ++i) {
        const contracts::UiNavButtonDesc desc{rects[i], modes[i], nullptr, nullptr};
        contracts::QolUiHandle handle = nullptr;
        const QolResult result = components->nav_button_create(&desc, parent, &handle);
        if (!contracts::succeeded(result) || handle == nullptr) return false;
        out[i] = {handle, 1};
        *out_count = i + 1;
    }
    return true;
}

void clear_category_controls() {
    const contracts::QolUiComponents* components = ui_components_api();
    if (components != nullptr && components->destroy != nullptr) {
        for (uint32_t i = 0; i < g_category_control_count; ++i) {
            if (g_category_controls[i].handle == nullptr) continue;
            (void)components->destroy(g_category_controls[i].handle);
        }
    }
    for (auto& control : g_category_controls) control = {};
    g_category_control_count = 0;
}

// 只在 panel_process 帧安全点调用：销毁旧分类交互控件并创建新分类控件。
bool build_category_controls(int32_t index) {
    clear_category_controls();
    g_selected_category = index;
    const contracts::QolUiComponents* components = ui_components_api();
    if (components == nullptr || g_root == nullptr) return false;
    if (index < 0 || static_cast<uint32_t>(index) >= kCategoryCount) return true;
    uint32_t count = 0;
    bool ok = true;
    switch (index) {
        case 0:
            ok = build_text_buttons(components, g_root, g_category_controls, &count);
            break;
        case 2:
            ok = build_nav_demos(components, g_root, g_category_controls, &count);
            break;
        default:  // 分类 1/3/4 纯绘制，无交互控件。
            break;
    }
    g_category_control_count = count;
    return ok;
}

// —— 分类静态内容：detail 回调绘制右栏，接收绝对 viewport，不做创建/销毁。 ——
void draw_switches_detail(const contracts::QolUiAdapter* adapter, contracts::UiRect right) {
    if (adapter == nullptr || adapter->draw_native_toggle == nullptr) return;
    if (adapter->draw_string_with_font != nullptr) {
        set_text_color(adapter, contracts::ui_theme::kTextGold);
        (void)adapter->draw_string_with_font("原生 Options 胶囊（可用范围，纯绘制）",
            static_cast<int32_t>(right.x + 20), static_cast<int32_t>(right.y + 32), 0, 0);
        set_text_color(adapter, contracts::ui_theme::kTextNormal);
        (void)adapter->draw_string_with_font("开 / 关 / 三种尺寸 · 点击语义由消费者决定",
            static_cast<int32_t>(right.x + 20), static_cast<int32_t>(right.y + 262), 0, 0);
    }
    constexpr uint32_t kSwitchDemoCount = 6;
    const contracts::UiRect local[kSwitchDemoCount] = {
        {380, 70, 116, 50}, {512, 70, 116, 50},
        {380, 138, 94, 42}, {512, 138, 94, 42},
        {380, 202, 150, 66}, {512, 202, 150, 66},
    };
    for (uint32_t i = 0; i < kSwitchDemoCount; ++i) {
        (void)adapter->draw_native_toggle(
            {right.x + local[i].x, right.y + local[i].y, local[i].w, local[i].h},
            i % 2 == 0 ? 1 : 0, 0);
    }
}

void draw_nav_detail(const contracts::QolUiAdapter* adapter, contracts::UiRect right) {
    if (adapter == nullptr || adapter->draw_string_with_font == nullptr) return;
    const int32_t x = static_cast<int32_t>(right.x);
    const int32_t y = static_cast<int32_t>(right.y);
    set_text_color(adapter, contracts::ui_theme::kTextGold);
    (void)adapter->draw_string_with_font("自适应导航按钮：点击请求弹栈顶", x + 20, y + 32, 0, 0);
    set_text_color(adapter, contracts::ui_theme::kTextNormal);
    (void)adapter->draw_string_with_font("自动（按栈深）", x + 20, y + 232, 0, 0);
    (void)adapter->draw_string_with_font("强制 X", x + 108, y + 232, 0, 0);
    (void)adapter->draw_string_with_font("强制返回箭头", x + 196, y + 232, 0, 0);
}

void draw_palette_detail(const contracts::QolUiAdapter* adapter, contracts::UiRect right) {
    if (adapter == nullptr || adapter->fill_rect == nullptr ||
        adapter->draw_string_with_font == nullptr || adapter->set_font_color_rgb == nullptr) {
        return;
    }
    struct Swatch {
        const char* name;
        uint32_t color;
    };
    const Swatch palette[] = {
        {"kPanelBg", contracts::ui_theme::kPanelBg},
        {"kFrameGold", contracts::ui_theme::kFrameGold},
        {"kDividerGold", contracts::ui_theme::kDividerGold},
        {"kTextGold", contracts::ui_theme::kTextGold},
        {"kTextNormal", contracts::ui_theme::kTextNormal},
        {"kTextDisabled", contracts::ui_theme::kTextDisabled},
        {"kRowSelectedBg", contracts::ui_theme::kRowSelectedBg},
        {"kError", contracts::ui_theme::kError},
        {"kOk", contracts::ui_theme::kOk},
    };
    constexpr uint32_t kPaletteCount = sizeof(palette) / sizeof(palette[0]);
    const int32_t x = static_cast<int32_t>(right.x);
    const int32_t y = static_cast<int32_t>(right.y);
    set_text_color(adapter, contracts::ui_theme::kTextGold);
    (void)adapter->draw_string_with_font("调色板 token（推荐值，可覆盖）", x + 20, y + 32, 0, 0);
    const uint32_t border = contracts::ui_theme::kTextDisabled;
    for (uint32_t i = 0; i < kPaletteCount; ++i) {
        const int64_t sx = right.x + 20 + static_cast<int64_t>(i % 2) * 300;
        const int64_t sy = right.y + 70 + static_cast<int64_t>(i / 2) * 44;
        (void)adapter->fill_rect({sx, sy, 40, 28}, palette[i].color);
        (void)adapter->fill_rect({sx, sy, 40, 1}, border);
        (void)adapter->fill_rect({sx, sy + 27, 40, 1}, border);
        (void)adapter->fill_rect({sx, sy, 1, 28}, border);
        (void)adapter->fill_rect({sx + 39, sy, 1, 28}, border);
        set_text_color(adapter, contracts::ui_theme::kTextNormal);
        (void)adapter->draw_string_with_font(palette[i].name,
            static_cast<int32_t>(sx + 46), static_cast<int32_t>(sy + 6), 0, 0);
    }
}

void draw_frames_detail(const contracts::QolUiAdapter* adapter,
                        const contracts::QolUiComponents* components, contracts::UiRect right) {
    if (adapter == nullptr || components == nullptr) return;
    const int32_t x = static_cast<int32_t>(right.x);
    const int32_t y = static_cast<int32_t>(right.y);
    if (adapter->draw_string_with_font != nullptr) {
        set_text_color(adapter, contracts::ui_theme::kTextGold);
        (void)adapter->draw_string_with_font("框架边框与原生控件", x + 20, y + 32, 0, 0);
        set_text_color(adapter, contracts::ui_theme::kTextNormal);
        (void)adapter->draw_string_with_font("普通容器 220×100", x + 42, y + 92, 0, 0);
        (void)adapter->draw_string_with_font("带角容器 220×100", x + 302, y + 92, 0, 0);
        (void)adapter->draw_string_with_font("原生 X 关闭贴图", x + 542, y + 92, 0, 0);
    }
    if (components->draw_container != nullptr) {
        (void)components->draw_container({right.x + 20, right.y + 120, 220, 100});
    }
    if (components->draw_container_ornamented != nullptr) {
        (void)components->draw_container_ornamented({right.x + 280, right.y + 120, 220, 100});
    }
    if (adapter->draw_native_close_button != nullptr) {
        (void)adapter->draw_native_close_button({right.x + 540, right.y + 120, 56, 56}, 0);
    }
    if (adapter->draw_string_with_font != nullptr) {
        (void)adapter->draw_string_with_font("label / button / toggle / select / scroll_view",
            x + 20, y + 268, 0, 0);
    }
}

// help_page detail 回调：user 存分类序号，只做静态绘制。
void draw_category_detail(void* user, contracts::UiRect right) {
    const int32_t index = static_cast<int32_t>(reinterpret_cast<uintptr_t>(user));
    const contracts::QolUiAdapter* adapter = ui_adapter();
    const contracts::QolUiComponents* components = ui_components_api();
    if (adapter == nullptr || components == nullptr) return;
    if (index < 0 || static_cast<uint32_t>(index) >= kCategoryCount) return;
    switch (index) {
        case 0:
            if (adapter->draw_string_with_font != nullptr) {
                set_text_color(adapter, contracts::ui_theme::kTextGold);
                (void)adapter->draw_string_with_font("原生文本按钮：点击计数 / 禁用 / 长文本",
                    static_cast<int32_t>(right.x + 20), static_cast<int32_t>(right.y + 32), 0, 0);
                set_text_color(adapter, contracts::ui_theme::kTextNormal);
                (void)adapter->draw_string_with_font(
                    "按钮文本经原生标签绘制，不写控件内联缓冲。",
                    static_cast<int32_t>(right.x + 20), static_cast<int32_t>(right.y + 84), 0, 0);
                (void)adapter->draw_string_with_font(
                    "点击经原生事件树派发，不混用手工命中。",
                    static_cast<int32_t>(right.x + 20), static_cast<int32_t>(right.y + 242), 0, 0);
            }
            break;
        case 1:
            draw_switches_detail(adapter, right);
            break;
        case 2:
            draw_nav_detail(adapter, right);
            break;
        case 3:
            draw_palette_detail(adapter, right);
            break;
        case 4:
            draw_frames_detail(adapter, components, right);
            break;
        default:
            break;
    }
}

void destroy_components() {
    // root 是 panel 组件：destroy 归还 owned 子树（含 help_page/按钮），native root 由宿主删除。
    const contracts::QolUiComponents* components = ui_components_api();
    for (auto& control : g_category_controls) control = {};
    g_category_control_count = 0;
    if (g_root != nullptr && components != nullptr && components->destroy != nullptr) {
        (void)components->destroy(g_root);
    }
    g_root = nullptr;
    g_page = nullptr;
    g_panel_nav = nullptr;
    g_touch_root = nullptr;
    g_selected_category = -1;
    g_demo_click_count = 0;
    g_active = false;
}

QolResult create_components() {
    const contracts::QolUiComponents* components = ui_components_api();
    const contracts::QolUiAdapter* adapter = ui_adapter();
    if (components == nullptr || adapter == nullptr || components->panel_create == nullptr ||
        components->help_page_create == nullptr || components->help_page_process == nullptr ||
        components->help_page_draw == nullptr || components->help_page_get_selected == nullptr ||
        components->nav_button_create == nullptr || components->nav_button_draw == nullptr ||
        components->destroy == nullptr) {
        return contracts::error(kQolErrNotReady, "debug ui components unavailable", 30);
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
    if (screen_width < kPanelRect.w || screen_height < kPanelRect.h) {
        return contracts::error(kQolErrInvalidArgument, "screen too small", 16);
    }
    const contracts::UiRect rect{(screen_width - kPanelRect.w) / 2,
                                 (screen_height - kPanelRect.h) / 2,
                                 kPanelRect.w, kPanelRect.h};

    const contracts::UiPanelDesc panel_desc{rect, rect, 0, 0};
    QolResult result = components->panel_create(&panel_desc, nullptr, &g_root);
    if (!contracts::succeeded(result) || g_root == nullptr) {
        destroy_components();
        return contracts::succeeded(result)
            ? contracts::error(kQolErrNotReady, "panel create returned null", 26) : result;
    }

    contracts::UiHelpOption options[kCategoryCount]{};
    for (uint32_t i = 0; i < kCategoryCount; ++i) {
        options[i].user = reinterpret_cast<void*>(static_cast<uintptr_t>(i));
        options[i].draw_detail = &draw_category_detail;
    }
    const contracts::UiHelpPageDesc page_desc{kPanelRect, kLeftViewport, kRightViewport,
        kRowHeight, options, kCategoryCount, 0, &draw_category_row, nullptr};
    result = components->help_page_create(&page_desc, g_root, &g_page);
    if (!contracts::succeeded(result)) {
        destroy_components();
        return result;
    }

    const contracts::UiNavButtonDesc nav_desc{kPanelNavRect, contracts::kUiNavButtonAuto,
                                              nullptr, nullptr};
    result = components->nav_button_create(&nav_desc, g_root, &g_panel_nav);
    if (!contracts::succeeded(result)) {
        destroy_components();
        return result;
    }

    if (!build_category_controls(0)) {
        destroy_components();
        return contracts::error(kQolErrInternal, "category build failed", 22);
    }

    // 边框图集由消费者确保加载；关闭时不卸载（游戏共享）。
    if (adapter->image_unit_load != nullptr) (void)adapter->image_unit_load(0x0f);

    g_active = true;
    return contracts::ok();
}

void panel_enter() {
    debug_log("panel enter");
    if (g_root == nullptr) {
        const QolResult result = create_components();
        if (!contracts::succeeded(result)) log_result("create", result);
    } else {
        g_active = true;
    }
}

void panel_process() {
    if (!g_active || g_root == nullptr || g_page == nullptr) return;
    const contracts::QolUiAdapter* adapter = ui_adapter();
    const contracts::QolUiComponents* components = ui_components_api();
    if (adapter == nullptr || components == nullptr || adapter->begin_frame == nullptr ||
        adapter->end_frame == nullptr) {
        return;
    }

    // 帧安全点：轮询左栏选中，切换分类时销毁旧交互控件并重建，回调内不做创建/销毁。
    int32_t selected = -1;
    if (contracts::succeeded(components->help_page_get_selected(g_page, &selected)) &&
        selected != g_selected_category) {
        if (!build_category_controls(selected)) debug_log("category rebuild failed");
    }

    // 原生 scroll 显式 process / draw，不依赖挂树自动刷新。
    if (!contracts::succeeded(components->help_page_process(g_page))) {
        debug_log("help page process failed");
    }

    if (!contracts::succeeded(adapter->begin_frame())) return;

    contracts::UiRect root_rect{};
    if (adapter->get_rect != nullptr && adapter->fill_rect_alpha != nullptr &&
        contracts::succeeded(adapter->get_rect(g_root, &root_rect))) {
        (void)adapter->fill_rect_alpha(root_rect, contracts::ui_theme::kPanelBgRgb565, 94u);
    }
    if (adapter->draw_custom_button != nullptr) {
        (void)adapter->draw_custom_button(g_root, {0, 0, kPanelRect.w, kPanelRect.h},
            0xFF101820u, 0xFFB89B62u, 3, nullptr, 0, 0, 0);
    }

    (void)components->help_page_draw(g_page);

    // 当前分类交互控件：nav 走自适应绘制，text_button 走原生文本按钮绘制。
    for (uint32_t i = 0; i < g_category_control_count; ++i) {
        const CategoryControl& control = g_category_controls[i];
        if (control.handle == nullptr) continue;
        if (control.is_nav != 0) {
            if (components->nav_button_draw != nullptr) {
                (void)components->nav_button_draw(control.handle);
            }
        } else if (components->text_button_draw != nullptr) {
            (void)components->text_button_draw(control.handle);
        }
    }

    if (g_panel_nav != nullptr && components->nav_button_draw != nullptr) {
        (void)components->nav_button_draw(g_panel_nav);
    }

    if (!contracts::succeeded(adapter->end_frame())) debug_log("end frame failed");
}

void panel_close() { destroy_components(); }

void panel_f3() { (void)ui_components_api()->back_request(); }

void panel_f4() { (void)ui_components_api()->back_request(); }

uint32_t panel_event(uint64_t event, uint64_t param, uint64_t param2) {
    if (!g_active || g_root == nullptr) return 1;
    const contracts::QolUiComponents* components = ui_components_api();
    if (components == nullptr || components->dispatch_native_event == nullptr) return 1;
    // 只处理 0x17 按下 / 0x18 松开 / 0x19 移动；0x8000000X 等 bit63 系统子事件伴随按下
    // 到达，不参与手势，只消费且不得改变本地状态（模块不持有按下态）。
    if (event != 0x17 && event != 0x18 && event != 0x19) return 1;
    if (event == 0x17) {
        // 双栏页交互控件全部挂在 g_root 下，统一以 g_root 为手势根，交游戏原生 TouchHandle
        // 状态机处理按下置态/移出取消/松开命中才 ExecuteProc（nav_button_execute → request_host_back）。
        g_touch_root = g_root;
        debug_log("panel touch press root=root");
    }
    if (g_touch_root == nullptr) return 1;
    uint32_t consumed = 0;
    (void)components->dispatch_native_event(g_touch_root, event, param, param2, &consumed);
    if (event == 0x18) {
        debug_log("panel touch release root=root");
        g_touch_root = nullptr;
    }
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

QolResult write_response(contracts::HttpRes* response, const char* body) {
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
    response->status = 200;
    return contracts::ok();
}

QolResult command_handler(const contracts::HttpReq* request, contracts::HttpRes* response, void*) {
    debug_log("endpoint hit");
    if (!g_enabled.load(std::memory_order_acquire)) {
        debug_log("endpoint g_enabled=0");
        return write_response(response, "{\"ok\":false,\"error\":\"custom ui disabled\"}");
    }
    if (request == nullptr || request->body == nullptr || request->body_len == 0) {
        return write_response(response, "{\"ok\":false,\"error\":\"bad request\"}");
    }
    const char* body = reinterpret_cast<const char*>(request->body);
    const std::string_view json(body, request->body_len);
    const size_t key = json.find("\"open\"");
    if (key == std::string_view::npos) {
        return write_response(response, "{\"ok\":false,\"error\":\"open required\"}");
    }
    const size_t colon = json.find(':', key + 6);
    if (colon == std::string_view::npos) {
        return write_response(response, "{\"ok\":false,\"error\":\"bad request\"}");
    }
    const size_t value = json.find_first_not_of(" \t\r\n", colon + 1);
    if (value == std::string_view::npos ||
        (json.compare(value, 4, "true") != 0 && json.compare(value, 5, "false") != 0)) {
        return write_response(response, "{\"ok\":false,\"error\":\"open must be boolean\"}");
    }
    const size_t value_size = json.compare(value, 4, "true") == 0 ? 4 : 5;
    const size_t end = json.find_first_not_of(" \t\r\n", value + value_size);
    if (end != std::string_view::npos && json[end] != ',' && json[end] != '}') {
        return write_response(response, "{\"ok\":false,\"error\":\"open must be boolean\"}");
    }
    g_requested_state.store(value_size == 4 ? 1 : 0, std::memory_order_release);
    debug_log(value_size == 4 ? "endpoint requested=open" : "endpoint requested=close");
    return write_response(response, "{\"ok\":true,\"queued\":true}");
}

int32_t apply_request(int64_t, void*) {
    const int32_t requested = g_requested_state.exchange(-1, std::memory_order_acq_rel);
    if (requested == 1) {
        const bool opened = ui_panels_open(kPanelId);
        char message[96]{};
        std::snprintf(message, sizeof(message), "apply requested=open result=%d active=%d",
                      opened ? 1 : 0, ui_panels_active(kPanelId) ? 1 : 0);
        debug_log(message);
    } else if (requested == 0) {
        ui_panels_close(kPanelId);
        char message[96]{};
        std::snprintf(message, sizeof(message), "apply requested=close active=%d",
                      ui_panels_active(kPanelId) ? 1 : 0);
        debug_log(message);
    }
    return 1;
}

}  // namespace

bool debug_components_register(const contracts::QolHostSdk* sdk) {
    if (sdk == nullptr || sdk->panels.register_provider == nullptr ||
        sdk->http_dispatch.register_command == nullptr || sdk->scheduler.add == nullptr) {
        return false;
    }
    g_sdk = sdk;
    if (!contracts::succeeded(sdk->panels.register_provider(&kProvider))) return false;
    if (!contracts::succeeded(sdk->http_dispatch.register_command(
            contracts::HttpMethod::kPost, kRoute, kOwner, &command_handler, nullptr))) {
        if (sdk->panels.unregister_owner != nullptr) {
            (void)sdk->panels.unregister_owner(kOwner);
        }
        g_sdk = nullptr;
        return false;
    }
    if (!contracts::succeeded(sdk->scheduler.add(1, &apply_request, nullptr, 1, 0,
                                                 &g_apply_task)) || g_apply_task == 0) {
        (void)sdk->http_dispatch.unregister_owner(kOwner);
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
        if (sdk->http_dispatch.unregister_owner != nullptr) {
            (void)sdk->http_dispatch.unregister_owner(kOwner);
        }
        if (sdk->panels.unregister_owner != nullptr) (void)sdk->panels.unregister_owner(kOwner);
    }
    g_apply_task = 0;
    g_requested_state.store(-1, std::memory_order_release);
    g_enabled.store(false, std::memory_order_release);
    g_sdk = nullptr;
}

void debug_components_set_enabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    if (!enabled) {
        g_requested_state.store(-1, std::memory_order_release);
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
