#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "contracts/ui.h"
#include "contracts/ui_choice.h"
#include "contracts/ui_confirmation.h"
#include "contracts/ui_theme.h"
#include "plugins/core.custom_ui/panels/debug_components.h"
#include "plugins/core.custom_ui/ui_components.h"

using namespace qol::contracts;

namespace qol::features::core_ui {
bool g_back_requested = false;
int g_open_calls = 0;
int g_close_calls = 0;
bool ui_panels_open(const char*) {
    ++g_open_calls;
    return true;
}
void ui_panels_close(const char*) { ++g_close_calls; }
bool ui_panels_active(const char*) { return true; }
void ui_panels_request_back() { g_back_requested = true; }
int ui_panels_depth() { return 1; }
}  // namespace qol::features::core_ui

namespace {

struct Control {
    uint32_t kind = 0;
    Control* parent = nullptr;
    std::vector<std::unique_ptr<Control>> children;
    UiRect rect{};
    QolUiDrawFn draw = nullptr;
    QolUiNativeScrollDrawFn content_draw = nullptr;
    QolUiClickFn click = nullptr;
    void* user = nullptr;
    int64_t offset = 0;
    bool enabled = true;
};

std::vector<std::unique_ptr<Control>> g_native_roots;
bool g_main_thread = true;
int g_live = 0;
int g_deletes = 0;
int g_string_draws = 0;
int g_native_toggle_draws = 0;
int g_text_button_draws = 0;
int g_close_draws = 0;
int g_back_draws = 0;
int g_border_draws = 0;
int g_fill_draws = 0;
int g_alpha_fill_draws = 0;
// 记录开关胶囊绘制 rect（屏幕绝对），验证右栏内容不与文本行重叠、不越 viewport。
std::vector<UiRect> g_toggle_rects;
const PanelProviderSpec* g_provider = nullptr;
// fake 原生 TouchHandle：按下置态、移出取消、松开仍落于导航按钮内才 ExecuteProc。
Control* g_nav_control = nullptr;
bool g_nav_pressed = false;
QolResult ok();
bool g_choice_active = false;
int g_choice_open_count = 0;
int g_choice_close_count = 0;
UiChoiceSpec g_last_choice{};
bool g_confirmation_active = false;
uint64_t g_next_confirmation_token = 41;
uint64_t g_last_closed_confirmation_token = 0;
UiConfirmationSpec g_last_confirmation{};
UiConfirmationSpecText g_last_confirmation_text{};
int g_confirmation_open_count = 0;

bool demo_choice_open(const UiChoiceSpec& spec) {
    if (g_choice_active) return false;
    g_choice_active = true;
    ++g_choice_open_count;
    g_last_choice = spec;
    return true;
}
bool demo_choice_is_active() { return g_choice_active; }
void demo_choice_close() {
    ++g_choice_close_count;
    g_choice_active = false;
}
const UiChoiceApi kDemoChoiceApi{&demo_choice_open, &demo_choice_is_active, &demo_choice_close};

QolResult demo_confirmation_open(const UiConfirmationSpec* spec, uint64_t* out_token) {
    if (spec == nullptr || out_token == nullptr || g_confirmation_active) {
        return error(kQolErrBusy, "busy", 4);
    }
    g_confirmation_active = true;
    ++g_confirmation_open_count;
    g_last_confirmation = *spec;
    *out_token = g_next_confirmation_token++;
    return ok();
}
QolResult demo_confirmation_active(uint64_t token, uint8_t* out_active) {
    if (token + 1 != g_next_confirmation_token || out_active == nullptr) {
        return error(kQolErrNotFound, "stale", 5);
    }
    *out_active = g_confirmation_active ? 1 : 0;
    return ok();
}
QolResult demo_confirmation_close(uint64_t token) {
    if (token + 1 != g_next_confirmation_token) return error(kQolErrNotFound, "stale", 5);
    g_last_closed_confirmation_token = token;
    g_confirmation_active = false;
    return ok();
}
QolResult demo_confirmation_open_text(const UiConfirmationSpecText* spec, uint64_t* out_token) {
    if (spec == nullptr || out_token == nullptr ||
        spec->size < sizeof(UiConfirmationSpecText) || g_confirmation_active) {
        return error(kQolErrBusy, "busy", 4);
    }
    g_confirmation_active = true;
    ++g_confirmation_open_count;
    g_last_confirmation_text = *spec;
    *out_token = g_next_confirmation_token++;
    return ok();
}
const UiConfirmationApi kDemoConfirmationApi{
    &demo_confirmation_open, &demo_confirmation_active, &demo_confirmation_close,
    &demo_confirmation_open_text};

QolResult require_debug_capability(const char* id, QolVersionRange range, const void** out) {
    if (id == nullptr || out == nullptr) return error(kQolErrInvalidArgument, "invalid", 7);
    *out = nullptr;
    if (std::strcmp(id, kUiChoiceCapabilityId) == 0 && range.major == 1 && range.min_minor == 1) {
        *out = &kDemoChoiceApi;
    } else if (std::strcmp(id, kUiConfirmationCapabilityId) == 0 &&
               range.major == 1 && range.min_minor == kUiConfirmationVersionMinor) {
        *out = &kDemoConfirmationApi;
    }
    return *out == nullptr ? error(kQolErrNotFound, "missing", 7) : ok();
}

QolResult ok() { return qol::contracts::ok(); }
int32_t main_thread() { return g_main_thread ? 1 : 0; }

QolResult create(uint32_t kind, QolUiHandle parent, QolUiHandle* out) {
    auto node = std::make_unique<Control>();
    node->kind = kind;
    node->parent = static_cast<Control*>(parent);
    *out = node.get();
    if (parent != nullptr) static_cast<Control*>(parent)->children.push_back(std::move(node));
    else g_native_roots.push_back(std::move(node));
    ++g_live;
    return ok();
}
QolResult set_rect(QolUiHandle handle, UiRect rect) {
    if (handle == nullptr) return error(kQolErrInvalidArgument, "invalid", 7);
    static_cast<Control*>(handle)->rect = rect;
    return ok();
}
QolResult get_rect(QolUiHandle handle, UiRect* out) {
    *out = static_cast<Control*>(handle)->rect;
    return ok();
}
QolResult set_text(QolUiHandle, const char*) { return ok(); }
QolResult set_draw(QolUiHandle handle, QolUiDrawFn fn) {
    static_cast<Control*>(handle)->draw = fn;
    return ok();
}
QolResult set_click(QolUiHandle handle, QolUiClickFn fn, void* user) {
    static_cast<Control*>(handle)->click = fn;
    static_cast<Control*>(handle)->user = user;
    return ok();
}
QolResult remove_control(QolUiHandle handle) {
    auto* node = static_cast<Control*>(handle);
    auto& siblings = node->parent == nullptr ? g_native_roots : node->parent->children;
    const auto it = std::find_if(siblings.begin(), siblings.end(),
        [node](const auto& item) { return item.get() == node; });
    assert(it != siblings.end());
    const auto count_subtree = [](const Control* subtree, const auto& self) -> int {
        int total = 1;
        for (const auto& child : subtree->children) total += self(child.get(), self);
        return total;
    };
    g_live -= count_subtree(node, count_subtree);
    siblings.erase(it);
    ++g_deletes;
    return ok();
}
QolResult set_button_enabled(QolUiHandle handle, uint8_t enabled) {
    if (handle == nullptr || enabled > 1) return error(kQolErrInvalidArgument, "invalid", 7);
    static_cast<Control*>(handle)->enabled = enabled != 0;
    return ok();
}
int32_t hit_test(QolUiHandle handle, int64_t x, int64_t y, UiRect rect) {
    auto* node = static_cast<Control*>(handle);
    int64_t ax = 0, ay = 0;
    for (auto* current = node; current != nullptr; current = current->parent) {
        ax += current->rect.x;
        ay += current->rect.y;
    }
    return x >= ax + rect.x && x < ax + rect.x + rect.w &&
        y >= ay + rect.y && y < ay + rect.y + rect.h;
}
QolResult origin(QolUiHandle handle, int64_t* x, int64_t* y) {
    *x = 0;
    *y = 0;
    for (auto* node = static_cast<Control*>(handle); node != nullptr; node = node->parent) {
        *x += node->rect.x;
        *y += node->rect.y;
    }
    return ok();
}
QolResult pressed(QolUiHandle, int32_t* out) { *out = 0; return ok(); }
QolResult scroll_create(QolUiHandle parent, const UiNativeScrollDesc* desc,
                        QolUiNativeScrollDrawFn draw, QolUiHandle* out) {
    const QolResult result = create(4, parent, out);
    if (!succeeded(result)) return result;
    auto* scroll = static_cast<Control*>(*out);
    scroll->rect = desc->viewport;
    scroll->content_draw = draw;
    return ok();
}
QolResult scroll_target(QolUiHandle, QolUiHandle) { return ok(); }
QolResult button_scroll(QolUiHandle, QolUiHandle) { return ok(); }
QolResult scroll_process(QolUiHandle) { return ok(); }
QolResult scroll_draw(QolUiHandle handle) {
    auto* scroll = static_cast<Control*>(handle);
    if (scroll->content_draw != nullptr) scroll->content_draw(handle, -scroll->offset);
    return ok();
}
QolResult scroll_get_offset(QolUiHandle handle, int64_t* out) {
    *out = static_cast<Control*>(handle)->offset;
    return ok();
}
QolResult scroll_set_offset(QolUiHandle handle, int64_t offset) {
    static_cast<Control*>(handle)->offset = offset;
    return ok();
}
QolResult event(QolUiHandle root, uint64_t ev, uint64_t param, uint64_t, uint32_t* consumed) {
    *consumed = 1;
    // 模拟游戏原生 TouchHandle：仅导航按钮手势需要判定，其它控件不触发返回。
    if (g_nav_control == nullptr || root == nullptr || param == 0) return ok();
    const auto* point = reinterpret_cast<const int64_t*>(static_cast<uintptr_t>(param));
    // 命中盒是相对控件的局部盒 {0,0,w,h}（hit_test 内部累加父链绝对原点）；
    // 尺寸取控件自身 rect（面板关闭按钮 78×86），与实现读 desc->rect 的行为一致。
    const bool inside = hit_test(g_nav_control, point[0], point[1],
                                 {0, 0, g_nav_control->rect.w, g_nav_control->rect.h}) > 0;
    if (ev == 0x17) {
        g_nav_pressed = inside;
    } else if (ev == 0x19) {
        if (!inside) g_nav_pressed = false;
    } else if (ev == 0x18) {
        const bool pressed = g_nav_pressed;
        g_nav_pressed = false;
        if (pressed && inside && g_nav_control->click != nullptr) {
            g_nav_control->click(g_nav_control->user);
        }
    }
    return ok();
}
QolResult draw_control(QolUiHandle) { return ok(); }
QolResult no_op() { return ok(); }
QolResult fill_rect(UiRect, uint32_t) { ++g_fill_draws; return ok(); }
QolResult fill_rect_alpha(UiRect, uint32_t, uint32_t) { ++g_alpha_fill_draws; return ok(); }
QolResult border_rect(int32_t, int32_t, int32_t, int32_t) { ++g_border_draws; return ok(); }
QolResult custom_button(QolUiHandle, UiRect, uint32_t, uint32_t, int32_t, const char*, uint32_t,
                        int32_t, int32_t) {
    return ok();
}
QolResult draw_text(QolUiHandle, int32_t, int32_t, uint32_t) { return ok(); }
QolResult string_with_font(const char*, int32_t, int32_t, int32_t, int32_t) {
    ++g_string_draws;
    return ok();
}
QolResult font_color_rgb(int32_t, int32_t, int32_t) { return ok(); }
// 非 0 inset：逻辑屏幕仍由 960×640 加两侧 inset 得出，全屏 root 原点为 (0,0)。
int32_t g_fake_inset_x = 24;
int32_t g_fake_inset_y = 16;
QolResult inset_width(int32_t* out) { *out = g_fake_inset_x; return ok(); }
QolResult inset_height(int32_t* out) { *out = g_fake_inset_y; return ok(); }
QolResult draw_native_toggle(UiRect rect, int32_t, uint8_t) {
    ++g_native_toggle_draws;
    g_toggle_rects.push_back(rect);
    return ok();
}
QolResult draw_native_text_button_background(UiRect, uint8_t) { ++g_text_button_draws; return ok(); }
QolResult draw_native_text_button_label(UiRect, const char*, uint32_t, int32_t) { return ok(); }
// 显式外观文本按钮的绘画接口：布局原样返回 visual_rect，绘画只计数。
QolResult layout_native_text_button(const UiTextButtonPaintDesc* desc,
                                    UiTextButtonPaintLayout* out) {
    if (desc != nullptr && out != nullptr) {
        out->normal_bounds = desc->visual_rect;
        out->pressed_bounds = desc->visual_rect;
        out->label_bounds = desc->visual_rect;
    }
    return ok();
}
QolResult paint_native_text_button(const UiTextButtonPaintDesc*, const char*, uint32_t, uint8_t) {
    ++g_text_button_draws;
    return ok();
}
QolResult draw_native_close_button(UiRect, uint8_t) { ++g_close_draws; return ok(); }
QolResult draw_back_button(QolUiHandle, uint8_t) { ++g_back_draws; return ok(); }
QolResult load_image_unit(int32_t) { return ok(); }
QolResult image_group(int32_t, QolUiHandle* out) {
    static int dummy = 0;
    *out = &dummy;
    return ok();
}
QolResult image_loc(int32_t, int32_t, QolUiHandle* out) {
    static int dummy = 0;
    *out = &dummy;
    return ok();
}
QolResult image_loc_size(int32_t, int32_t, int32_t* width, int32_t* height) {
    *width = 16;
    *height = 16;
    return ok();
}
QolResult draw_part(QolUiHandle, int32_t, int32_t, QolUiHandle, int32_t, int32_t) { return ok(); }
QolResult touch_coordinates(uint64_t param, int64_t* x, int64_t* y) {
    if (param == 0) {
        *x = 0;
        *y = 0;
        return ok();
    }
    const auto* point = reinterpret_cast<const int64_t*>(static_cast<uintptr_t>(param));
    *x = point[0];
    *y = point[1];
    return ok();
}

QolUiAdapter make_adapter() {
    QolUiAdapter ui{};
    ui.size = sizeof(ui);
    ui.major = kUiAdapterMajor;
    ui.minor = kUiAdapterMinor;
    ui.create_control = &create;
    ui.set_rect = &set_rect;
    ui.get_rect = &get_rect;
    ui.set_text = &set_text;
    ui.set_draw_proc = &set_draw;
    ui.set_click_proc = &set_click;
    ui.delete_control = &remove_control;
    ui.is_main_thread = &main_thread;
    ui.hit_test = &hit_test;
    ui.control_absolute_origin = &origin;
    ui.control_pressed = &pressed;
    ui.scroll_create = &scroll_create;
    ui.scroll_set_target = &scroll_target;
    ui.button_set_scroll = &button_scroll;
    ui.scroll_process = &scroll_process;
    ui.scroll_draw = &scroll_draw;
    ui.scroll_get_offset = &scroll_get_offset;
    ui.scroll_set_offset = &scroll_set_offset;
    ui.control_tree_event = &event;
    ui.draw_control = &draw_control;
    ui.begin_frame = &no_op;
    ui.end_frame = &no_op;
    ui.fill_rect = &fill_rect;
    ui.fill_rect_alpha = &fill_rect_alpha;
    ui.draw_border_rect = &border_rect;
    ui.draw_custom_button = &custom_button;
    ui.draw_text = &draw_text;
    ui.draw_string_with_font = &string_with_font;
    ui.set_font_color_rgb = &font_color_rgb;
    ui.screen_inset_width = &inset_width;
    ui.screen_inset_height = &inset_height;
    ui.draw_native_toggle = &draw_native_toggle;
    ui.draw_native_text_button_background = &draw_native_text_button_background;
    ui.draw_native_text_button_label = &draw_native_text_button_label;
    ui.layout_native_text_button = &layout_native_text_button;
    ui.draw_native_text_button = &paint_native_text_button;
    ui.set_button_enabled = &set_button_enabled;
    ui.draw_native_close_button = &draw_native_close_button;
    ui.draw_back_button = &draw_back_button;
    ui.image_unit_load = &load_image_unit;
    ui.image_group = &image_group;
    ui.image_loc = &image_loc;
    ui.image_loc_size = &image_loc_size;
    ui.draw_part = &draw_part;
    ui.touch_coordinates = &touch_coordinates;
    return ui;
}
QolResult register_provider(const PanelProviderSpec* provider) {
    g_provider = provider;
    return ok();
}
QolResult unregister_owner(const char*) { return ok(); }
QolResult add_task(int32_t, QolFrameHandler, void*, int32_t, int32_t, uint64_t* out) {
    *out = 7;
    return ok();
}
QolResult remove_task(uint64_t) { return ok(); }

Control* root_control() { return g_native_roots.empty() ? nullptr : g_native_roots.front().get(); }

}  // namespace

int main() {
    using namespace qol::features::core_ui;
    int checks = 0;
#define CHECK(expr) do { assert(expr); ++checks; } while (false)

    // 当前测试 inset 下逻辑屏幕为 1008×672；原生返回框保持 (8,8) 78×86，
    // 左右各留按钮宽度，两栏垂直边距 10px，间距先压至 0 后宽度等比缩放至 183/613。
    const UiRect left{94, 18, 183, 636};
    const UiRect right{309, 18, 613, 636};
    const UiRect left_frame{left.x - 8, left.y - 8, left.w + 32, left.h + 16};
    const UiRect right_frame{right.x - 8, right.y - 8, right.w + 16, right.h + 16};
    CHECK(left_frame.x == 86 && left_frame.y == 10);
    CHECK(left_frame.x + left_frame.w == right_frame.x);
    CHECK(right_frame.x + right_frame.w == 930 && right_frame.y == 10);
    CHECK(left_frame.y + left_frame.h == 662 && right_frame.y + right_frame.h == 662);
    CHECK(left.w == 183 && right.w == 613);
    // 返回按钮完全恢复原版屏幕绝对位置、尺寸与命中框。
    const UiRect panel_nav{qol::contracts::ui_theme::kNavBoxAbsX,
                           qol::contracts::ui_theme::kNavBoxAbsY,
                           qol::contracts::ui_theme::kNavBoxW, qol::contracts::ui_theme::kNavBoxH};
    const UiRect nav_abs{panel_nav.x, panel_nav.y,
                         qol::contracts::ui_theme::kNavBoxW, qol::contracts::ui_theme::kNavBoxH};
    // 文本按钮/导航演示盒 rect = 右 viewport 原点 + 局部偏移（与实现同基准）。
    const UiRect text_button_rects[6] = {{right.x + 20, right.y + 120, 220, 48},
        {right.x + 264, right.y + 120, 220, 48}, {right.x + 20, right.y + 176, 220, 48},
        {right.x + 264, right.y + 176, 220, 48}, {right.x + 20, right.y + 232, 220, 48},
        {right.x + 264, right.y + 232, 220, 48}};
    // 导航演示盒取原生 X/返回尺寸 78×86（与设置页共用同款金属框），间距 88 不重叠。
    const UiRect nav_demo_rects[3] = {{right.x + 20, right.y + 112, 78, 86},
        {right.x + 108, right.y + 112, 78, 86}, {right.x + 196, right.y + 112, 78, 86}};
    const auto overlaps = [](UiRect a, UiRect b) {
        return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
    };
    CHECK(!overlaps(nav_abs, left));  // 不覆盖左栏列表命中区
    CHECK(nav_abs.x + nav_abs.w == left_frame.x && nav_abs.y == 8 && nav_abs.h == 86);
    CHECK(!overlaps(panel_nav, text_button_rects[1]));
    for (const auto& rect : nav_demo_rects) CHECK(!overlaps(nav_abs, rect));
    // 右栏说明与控件不重叠：控件 rect 全部落在右 viewport 内，且与 detail 文本行
    // （标题 y+32 / 说明 y+84 / 脚注 y+242，行高按 20 保守估计）带状区域分离。
    const auto text_band = [&right](int32_t local_y) {
        return UiRect{right.x, right.y + local_y, right.w, 20};
    };
    const UiRect text_button_bands[3] = {text_band(32), text_band(84), text_band(296)};
    const auto within = [](UiRect rect, UiRect viewport) {
        return rect.x >= viewport.x && rect.y >= viewport.y &&
               rect.x + rect.w <= viewport.x + viewport.w &&
               rect.y + rect.h <= viewport.y + viewport.h;
    };
    for (const auto& rect : text_button_rects) {
        CHECK(within(rect, right));
        for (const auto& band : text_button_bands) CHECK(!overlaps(rect, band));
    }
    const UiRect nav_label_band = text_band(224);
    for (const auto& rect : nav_demo_rects) {
        CHECK(within(rect, right));
        CHECK(!overlaps(rect, text_band(32)) && !overlaps(rect, nav_label_band));
    }

    auto ui = make_adapter();
    CHECK(ui_components_bind(&ui));
    const QolUiComponents* api = ui_components_api();
    CHECK(api->minor == kUiContractMinor && api->nav_button_create != nullptr);

    QolHostSdk sdk{};
    sdk.ui = &ui;
    sdk.panels.register_provider = &register_provider;
    sdk.panels.unregister_owner = &unregister_owner;
    sdk.scheduler.add = &add_task;
    sdk.scheduler.remove = &remove_task;
    sdk.capabilities.require = &require_debug_capability;
    CHECK(debug_components_register(&sdk));
    CHECK(g_provider != nullptr && g_provider->panel_id != nullptr &&
          std::strcmp(g_provider->panel_id, "debug_components") == 0);
    debug_components_set_enabled(true);
    CHECK(g_provider->open());
    g_provider->callbacks->enter();
    CHECK(g_provider->active());

    // 结构：root = [双栏页, 面板导航按钮, 分类0 八枚文本按钮（胶囊/装饰各三态 + 选中态两枚）]。
    Control* root = root_control();
    CHECK(root != nullptr && root->kind == 0 && root->children.size() == 10);
    CHECK(root->rect.x == 0 && root->rect.y == 0 && root->rect.w == 1008 && root->rect.h == 672);
    Control* columns = root->children[0].get();
    CHECK(columns->rect.x == 0 && columns->rect.y == 0 &&
          columns->rect.w == 1008 && columns->rect.h == 672);
    Control* panel_nav_control = root->children[1].get();
    g_nav_control = panel_nav_control;
    CHECK(columns->kind == 3);
    CHECK(panel_nav_control->kind == 1 && panel_nav_control->click != nullptr);
    CHECK(panel_nav_control->rect.x == panel_nav.x &&
          panel_nav_control->rect.y == panel_nav.y &&
          panel_nav_control->rect.w == panel_nav.w && panel_nav_control->rect.h == panel_nav.h);
    for (size_t i = 0; i < 6; ++i) {
        Control* text_button = root->children[2 + i].get();
        CHECK(text_button->kind == 1 && text_button->rect.x == text_button_rects[i].x &&
              text_button->rect.y == text_button_rects[i].y &&
              text_button->rect.w == text_button_rects[i].w &&
              text_button->rect.h == text_button_rects[i].h);
    }
    // 双栏页内部：左列表（原生 scroll）9 行，行高 56。
    CHECK(columns->children.size() == 1);
    Control* list = columns->children[0].get();
    CHECK(list->kind == 4 && list->rect.x == left.x && list->rect.w == left.w);
    CHECK(list->children.size() == 1);
    Control* group = list->children[0].get();
    CHECK(group->children.size() == 9);
    for (size_t i = 0; i < 9; ++i) {
        CHECK(group->children[i]->rect.y == static_cast<int64_t>(i) * 56 &&
              group->children[i]->rect.h == 56 && group->children[i]->click != nullptr);
    }

    // 首帧 process：绘制双栏（框架 + detail 文本 + 左列表）与自动导航按钮（depth 1 → X）。
    g_provider->callbacks->process();
    CHECK(g_border_draws == 2);       // 只绘制左右栏自身边框
    CHECK(g_string_draws >= 5);       // 左列表 5 个分类名
    CHECK(g_close_draws >= 1);        // 面板导航按钮（depth 1 → X）
    g_provider->callbacks->process();
    CHECK(g_native_toggle_draws == 0);  // 开关胶囊分类未选中时不绘制

    // 切换到「X/导航按钮」分类：帧安全点重建右栏控件（文本按钮销毁、导航演示创建）。
    CHECK(api->help_page_set_selected(columns, 2).ok == kQolOk);
    const int deletes_before = g_deletes;
    g_provider->callbacks->process();
    CHECK(g_deletes > deletes_before);  // 旧分类控件在 process 内销毁
    CHECK(root->children.size() == 5);  // 双栏页 + 面板导航 + 三枚导航演示
    CHECK(root->children[0].get() == columns && root->children[1].get() == panel_nav_control);
    for (size_t i = 0; i < 3; ++i) {
        Control* demo = root->children[2 + i].get();
        CHECK(demo->kind == 1 && demo->rect.x == nav_demo_rects[i].x &&
              demo->rect.y == nav_demo_rects[i].y);
    }
    CHECK(g_string_draws > 5);

    // 切到「开关胶囊」分类（纯绘制，无交互控件）→ 右栏不再有按钮。
    CHECK(api->help_page_set_selected(columns, 1).ok == kQolOk);
    g_provider->callbacks->process();
    CHECK(root->children.size() == 2);  // 双栏页 + 面板导航
    CHECK(g_native_toggle_draws >= 6);
    // 开关胶囊 rect（屏幕绝对，detail 回调收到 root 原点平移后的 viewport）全部
    // 落在右 viewport 内，且与标题行（y+32）与说明行（y+296）文本带分离。
    CHECK(g_toggle_rects.size() >= 6);
    const UiRect right_abs{right.x + root->rect.x, right.y + root->rect.y, right.w, right.h};
    const auto abs_text_band = [&right_abs](int32_t local_y) {
        return UiRect{right_abs.x, right_abs.y + local_y, right_abs.w, 20};
    };
    const int64_t switch_group_x = (right_abs.w - 282) / 2;
    const int32_t switch_widths[6] = {116, 116, 94, 94, 150, 150};
    const int32_t switch_y[6] = {76, 76, 144, 144, 204, 204};
    CHECK(g_toggle_rects.size() == 6);
    for (size_t i = 0; i < g_toggle_rects.size(); ++i) {
        const auto& rect = g_toggle_rects[i];
        CHECK(rect.x == right_abs.x + switch_group_x + static_cast<int64_t>(i % 2) * 132);
        CHECK(rect.y == right_abs.y + switch_y[i] && rect.w == switch_widths[i]);
        CHECK(within(rect, right_abs));
        CHECK(!overlaps(rect, abs_text_band(32)) && !overlaps(rect, abs_text_band(296)));
    }

    // 面板导航按钮点击（自适应组件）→ 请求弹栈顶。
    g_back_requested = false;
    panel_nav_control->click(panel_nav_control->user);
    CHECK(g_back_requested);

    // 新语义：按下只派发/置原生态并消费，松开仍命中导航按钮才经原生树 ExecuteProc 触发。
    // root 覆盖逻辑全屏，事件坐标与右栏标题带中的 nav rect 同为屏幕坐标。
    int64_t nav_hit[] = {nav_abs.x + nav_abs.w / 2, nav_abs.y + nav_abs.h / 2};
    const uint64_t nav_hit_param = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(nav_hit));
    g_back_requested = false;
    g_nav_pressed = false;
    CHECK(g_provider->callbacks->event(0x17, nav_hit_param, 0x10) == 1);
    CHECK(!g_back_requested);  // 按下不触发
    CHECK(g_provider->callbacks->event(0x18, nav_hit_param, 0x10) == 1);
    CHECK(g_back_requested);   // 松开触发恰一次

    // 移出后松开：原生取消，不触发（点在 nav 盒外）。
    int64_t elsewhere[] = {480, 400};
    const uint64_t elsewhere_param = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(elsewhere));
    g_back_requested = false;
    g_nav_pressed = false;
    CHECK(g_provider->callbacks->event(0x17, nav_hit_param, 0x10) == 1);
    CHECK(g_provider->callbacks->event(0x19, elsewhere_param, 0x10) == 1);
    CHECK(g_provider->callbacks->event(0x18, nav_hit_param, 0x10) == 1);
    CHECK(!g_back_requested);

    // bit63 系统子事件只消费、不派发、不改手势态；随后松开仍正常触发。
    g_back_requested = false;
    g_nav_pressed = false;
    CHECK(g_provider->callbacks->event(0x17, nav_hit_param, 0x10) == 1);
    CHECK(g_provider->callbacks->event(0x80000001ULL, nav_hit_param, 0x10) == 1);
    CHECK(!g_back_requested);
    CHECK(g_provider->callbacks->event(0x18, nav_hit_param, 0x10) == 1);
    CHECK(g_back_requested);

    // 其它控件（双栏页文本区）按下/抬起不触发导航返回。
    g_back_requested = false;
    CHECK(g_provider->callbacks->event(0x17, elsewhere_param, 0x10) == 1);
    CHECK(g_provider->callbacks->event(0x18, elsewhere_param, 0x10) == 1);
    CHECK(!g_back_requested);

    // 基础形状类别保留静态绘制示例，形状绘制原语在 Debug 面板帧中被实际调用。
    CHECK(api->help_page_set_selected(columns, 7).ok == kQolOk);
    const int shape_draws_before = g_alpha_fill_draws;
    g_provider->callbacks->process();
    CHECK(g_alpha_fill_draws > shape_draws_before + 1);

    // 原生确认入口由三个独立根子项触发，不挂在 HelpPage 的 native scroll 子树内。
    CHECK(api->help_page_set_selected(columns, 8).ok == kQolOk);
    g_provider->callbacks->process();
    CHECK(root->children.size() == 5);  // 双栏页、导航、3 个 Debug-only 弹窗入口。
    Control* choice_demo = root->children[2].get();
    Control* no_price_demo = root->children[3].get();
    Control* price_demo = root->children[4].get();
    choice_demo->click(choice_demo->user);
    CHECK(g_choice_open_count == 1 && g_choice_active);
    CHECK(g_last_choice.count == 3 && g_last_choice.close_index == 2 &&
          std::strcmp(g_last_choice.dialog_type_id, "core.custom_ui.debug.choice") == 0);
    g_last_choice.on_select(1, g_last_choice.user);
    g_choice_active = false;
    no_price_demo->click(no_price_demo->user);
    CHECK(g_confirmation_open_count == 1 && g_confirmation_active);
    CHECK(g_last_confirmation_text.display_type == 0 && g_last_confirmation_text.price == 0);
    CHECK(std::strcmp(g_last_confirmation_text.message, "确认执行该操作？") == 0);
    g_confirmation_active = false;
    g_last_confirmation_text.on_confirm(g_last_confirmation_text.user);
    price_demo->click(price_demo->user);
    CHECK(g_confirmation_open_count == 2 && g_confirmation_active &&
          g_last_confirmation_text.price == 100);
    CHECK(std::strcmp(g_last_confirmation_text.message, "是否出售该物品？") == 0);
    g_confirmation_active = false;
    g_last_confirmation_text.on_cancel(g_last_confirmation_text.user);
    price_demo->click(price_demo->user);
    CHECK(g_confirmation_open_count == 3 && g_confirmation_active &&
          g_last_confirmation_text.price == 100);
    const uint64_t owned_demo_token = g_next_confirmation_token - 1;
    choice_demo->click(choice_demo->user);
    CHECK(g_choice_open_count == 2 && g_choice_active);

    // 关闭：组件侧树释放，native 由宿主删除。
    g_provider->close();
    CHECK(!g_provider->active());
    CHECK(!g_confirmation_active && g_last_closed_confirmation_token == owned_demo_token);
    CHECK(!g_choice_active && g_choice_close_count == 1);
    CHECK(api->help_page_draw(columns).code == kQolErrNotFound);
    remove_control(root);
    CHECK(g_live == 0);

    debug_components_set_enabled(false);
    CHECK(g_choice_close_count == 1);  // Panel/disable/unregister cleanup is idempotent.
    g_choice_active = true;  // A different owner's choice must not be closed by stale demo state.
    debug_components_unregister(&sdk);
    CHECK(g_choice_active && g_choice_close_count == 1);
    g_choice_active = false;
    ui_components_unbind();
    std::printf("debug_two_column checks: %d\n", checks);
    return 0;
#undef CHECK
}
