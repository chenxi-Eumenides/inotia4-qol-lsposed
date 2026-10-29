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
std::vector<UiRect> g_border_rects;
int g_fill_draws = 0;
int g_alpha_fill_draws = 0;
std::vector<std::string> g_drawn_strings;
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
int g_help_detail_draws = 0;
void* g_help_last_detail_user = nullptr;
UiRect g_help_last_detail_viewport{};

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
QolResult border_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    ++g_border_draws;
    g_border_rects.push_back({x, y, w, h});
    return ok();
}
QolResult custom_button(QolUiHandle, UiRect, uint32_t, uint32_t, int32_t, const char*, uint32_t,
                        int32_t, int32_t) {
    return ok();
}
QolResult draw_text(QolUiHandle, int32_t, int32_t, uint32_t) { return ok(); }
QolResult string_with_font(const char* text, int32_t, int32_t, int32_t, int32_t) {
    ++g_string_draws;
    g_drawn_strings.emplace_back(text == nullptr ? "" : text);
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

void help_list_item_draw(void*, uint32_t, QolUiHandle, uint8_t) {}
void help_detail_draw(void* user, UiRect viewport) {
    ++g_help_detail_draws;
    g_help_last_detail_user = user;
    g_help_last_detail_viewport = viewport;
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

    const auto rect_matches = [](UiRect actual, UiRect expected) {
        return actual.x == expected.x && actual.y == expected.y &&
               actual.w == expected.w && actual.h == expected.h;
    };
    const UiRect panel_rect{0, 0, 1008, 672};
    const UiRect panel_nav{qol::contracts::ui_theme::kNavBoxAbsX,
                           qol::contracts::ui_theme::kNavBoxAbsY,
                           qol::contracts::ui_theme::kNavBoxW,
                           qol::contracts::ui_theme::kNavBoxH};
    const UiRect outer_frame{panel_nav.x + panel_nav.w + 8, 18,
                             panel_rect.w - panel_nav.x - panel_nav.w - 8 - 10,
                             panel_rect.h - 18 - 18};
    CHECK(rect_matches(outer_frame, {94, 18, 904, 636}));

    auto ui = make_adapter();
    CHECK(ui_components_bind(&ui));
    const QolUiComponents* api = ui_components_api();
    CHECK(api != nullptr && api->minor == kUiContractMinor &&
          api->nav_button_create != nullptr && api->draw_container_ornamented != nullptr);

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
    CHECK(!g_provider->active());
    debug_components_set_enabled(true);
    CHECK(g_provider->open());
    g_provider->callbacks->enter();
    CHECK(g_provider->active());
    g_provider->callbacks->enter();  // 重复 enter 不重复创建面板与导航控件。

    Control* root = root_control();
    CHECK(root != nullptr && root->kind == 0 && root->children.size() == 1);
    CHECK(rect_matches(root->rect, panel_rect));
    CHECK(g_live == 2);
    Control* panel_nav_control = root->children[0].get();
    g_nav_control = panel_nav_control;
    CHECK(panel_nav_control->kind == 1 && panel_nav_control->click != nullptr);
    CHECK(rect_matches(panel_nav_control->rect, panel_nav));

    // Debug Components 只绘制单一最大外框观察内容，不再创建分类页、形状样例或弹窗入口。
    g_provider->callbacks->process();
    CHECK(g_border_draws == 1 && g_border_rects.size() == 1);
    CHECK(rect_matches(g_border_rects.back(), outer_frame));
    CHECK(g_drawn_strings.size() == 4);
    CHECK(g_drawn_strings[0] == "最大外层面板样例");
    CHECK(g_drawn_strings[1] == "面板区域 1008 × 672");
    CHECK(g_drawn_strings[2].find("主题色参考 kPanelBg  #") == 0);
    CHECK(g_drawn_strings[3] == "容器外框 904 × 636");
    CHECK(g_fill_draws == 1 && g_alpha_fill_draws == 0 && g_native_toggle_draws == 0);
    CHECK(g_close_draws >= 1);
    CHECK(g_choice_open_count == 0 && !g_choice_active);
    CHECK(g_confirmation_open_count == 0 && !g_confirmation_active);

    // ornamented container 在 48×48 边界接受绘制，任一维度小于 48 时拒绝且不绘制。
    const size_t borders_before_minimum_checks = g_border_rects.size();
    CHECK(api->draw_container_ornamented({200, 200, 47, 48}).code == kQolErrInvalidArgument);
    CHECK(api->draw_container_ornamented({200, 200, 48, 47}).code == kQolErrInvalidArgument);
    CHECK(g_border_rects.size() == borders_before_minimum_checks);
    CHECK(api->draw_container_ornamented({200, 200, 48, 48}).code == kQolOk);
    CHECK(g_border_rects.size() == borders_before_minimum_checks + 1);
    CHECK(rect_matches(g_border_rects.back(), {200, 200, 48, 48}));

    // 返回按钮保留直接 back 请求及原生按下/移出/抬起命中生命周期。
    g_back_requested = false;
    panel_nav_control->click(panel_nav_control->user);
    CHECK(g_back_requested);
    int64_t nav_hit[] = {panel_nav.x + panel_nav.w / 2, panel_nav.y + panel_nav.h / 2};
    const uint64_t nav_hit_param = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(nav_hit));
    g_back_requested = false;
    g_nav_pressed = false;
    CHECK(g_provider->callbacks->event(0x17, nav_hit_param, 0x10) == 1);
    CHECK(!g_back_requested);
    CHECK(g_provider->callbacks->event(0x18, nav_hit_param, 0x10) == 1);
    CHECK(g_back_requested);

    int64_t elsewhere[] = {480, 400};
    const uint64_t elsewhere_param = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(elsewhere));
    g_back_requested = false;
    g_nav_pressed = false;
    CHECK(g_provider->callbacks->event(0x17, nav_hit_param, 0x10) == 1);
    CHECK(g_provider->callbacks->event(0x19, elsewhere_param, 0x10) == 1);
    CHECK(g_provider->callbacks->event(0x18, nav_hit_param, 0x10) == 1);
    CHECK(!g_back_requested);

    g_back_requested = false;
    g_nav_pressed = false;
    CHECK(g_provider->callbacks->event(0x17, nav_hit_param, 0x10) == 1);
    CHECK(g_provider->callbacks->event(0x80000001ULL, nav_hit_param, 0x10) == 1);
    CHECK(!g_back_requested);
    CHECK(g_provider->callbacks->event(0x18, nav_hit_param, 0x10) == 1);
    CHECK(g_back_requested);

    g_back_requested = false;
    CHECK(g_provider->callbacks->event(0x17, elsewhere_param, 0x10) == 1);
    CHECK(g_provider->callbacks->event(0x18, elsewhere_param, 0x10) == 1);
    CHECK(!g_back_requested);

    const size_t borders_before_close = g_border_rects.size();
    g_provider->close();
    CHECK(!g_provider->active());
    g_provider->callbacks->process();
    CHECK(g_border_rects.size() == borders_before_close);
    CHECK(g_choice_open_count == 0 && g_confirmation_open_count == 0);
    remove_control(root);  // 组件归还其句柄；原生树由宿主释放。
    CHECK(g_live == 0);

    // HelpPage 的选择、绘制回调及销毁仍作为独立组件契约回归，不属于 Debug Components 示例页。
    int help_option_users[2]{};
    const UiHelpOption help_options[] = {
        {&help_option_users[0], &help_detail_draw},
        {&help_option_users[1], &help_detail_draw},
    };
    const UiHelpPageDesc help_desc{
        panel_rect, {94, 18, 183, 636}, {309, 18, 613, 636}, 56,
        help_options, 2, 0, &help_list_item_draw, nullptr};
    QolUiHandle help_page = nullptr;
    CHECK(api->help_page_create(&help_desc, nullptr, &help_page).code == kQolOk);
    CHECK(help_page != nullptr);
    Control* help_native_root = root_control();
    CHECK(help_native_root != nullptr && help_native_root->kind == 0);
    int32_t selected = -1;
    CHECK(api->help_page_get_selected(help_page, &selected).code == kQolOk && selected == 0);
    CHECK(api->help_page_set_selected(help_page, 2).code == kQolErrInvalidArgument);
    CHECK(api->help_page_set_selected(help_page, 1).code == kQolOk);
    const size_t help_borders_before_draw = g_border_rects.size();
    CHECK(api->help_page_draw(help_page).code == kQolOk);
    CHECK(g_border_rects.size() == help_borders_before_draw + 2);
    CHECK(g_help_detail_draws == 1 && g_help_last_detail_user == &help_option_users[1]);
    CHECK(rect_matches(g_help_last_detail_viewport, {309, 18, 613, 636}));
    CHECK(api->help_page_set_selected(help_page, -1).code == kQolOk);
    CHECK(api->help_page_draw(help_page).code == kQolOk);
    CHECK(g_help_detail_draws == 1);
    CHECK(api->destroy(help_page).code == kQolOk);
    CHECK(api->help_page_draw(help_page).code == kQolErrNotFound);
    CHECK(g_live == 0);

    debug_components_set_enabled(false);
    CHECK(g_close_calls == 1);
    debug_components_unregister(&sdk);
    CHECK(g_close_calls == 2);
    ui_components_unbind();
    std::printf("debug_two_column checks: %d\n", checks);
    return 0;
#undef CHECK
}
