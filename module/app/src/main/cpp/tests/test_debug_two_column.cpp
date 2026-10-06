#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "contracts/ui.h"
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
const PanelProviderSpec* g_provider = nullptr;
HttpCommandHandlerFn g_command = nullptr;
// fake 原生 TouchHandle：按下置态、移出取消、松开仍落于导航按钮内才 ExecuteProc。
Control* g_nav_control = nullptr;
bool g_nav_pressed = false;

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
    const bool inside = hit_test(g_nav_control, point[0], point[1], {0, 0, 48, 48}) > 0;
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
QolResult fill_rect_alpha(UiRect, uint32_t, uint32_t) { return ok(); }
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
QolResult inset(int32_t* out) { *out = 0; return ok(); }
QolResult draw_native_toggle(UiRect, int32_t, uint8_t) { ++g_native_toggle_draws; return ok(); }
QolResult draw_native_text_button_background(UiRect, uint8_t) { ++g_text_button_draws; return ok(); }
QolResult draw_native_text_button_label(UiRect, const char*, uint32_t, int32_t) { return ok(); }
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
    ui.screen_inset_width = &inset;
    ui.screen_inset_height = &inset;
    ui.draw_native_toggle = &draw_native_toggle;
    ui.draw_native_text_button_background = &draw_native_text_button_background;
    ui.draw_native_text_button_label = &draw_native_text_button_label;
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
QolResult register_command(HttpMethod, const char*, const char* owner,
                           HttpCommandHandlerFn command, void*) {
    assert(std::strcmp(owner, "core.custom_ui") == 0);
    g_command = command;
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

    // 双栏布局常量（与 debug_components.cpp 保持一致）：左/右 viewport 扩框不重叠，
    // 面板关闭导航按钮位于右栏内且不与分类控件行重叠。
    const UiRect left{16, 16, 204, 608};
    const UiRect right{260, 16, 684, 608};
    const UiRect left_frame{left.x - 8, left.y - 8, left.w + 32, left.h + 16};
    const UiRect right_frame{right.x - 8, right.y - 8, right.w + 16, right.h + 16};
    CHECK(left_frame.x + left_frame.w <= right_frame.x);
    CHECK(right_frame.x + right_frame.w <= 960 && right_frame.y + right_frame.h <= 640);
    const UiRect panel_nav{888, 24, 48, 48};
    const UiRect text_button_rects[3] = {{280, 128, 220, 48}, {504, 128, 220, 48},
                                         {280, 180, 220, 48}};
    // 导航演示盒取原生 X/返回尺寸 78×86（与设置页共用同款金属框），间距 88 不重叠。
    const UiRect nav_demo_rects[3] = {{280, 128, 78, 86}, {368, 128, 78, 86},
                                      {456, 128, 78, 86}};
    const auto overlaps = [](UiRect a, UiRect b) {
        return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
    };
    CHECK(!overlaps(panel_nav, text_button_rects[1]));
    for (const auto& rect : nav_demo_rects) CHECK(!overlaps(panel_nav, rect));

    auto ui = make_adapter();
    CHECK(ui_components_bind(&ui));
    const QolUiComponents* api = ui_components_api();
    CHECK(api->minor == kUiContractMinor && api->nav_button_create != nullptr);

    QolHostSdk sdk{};
    sdk.ui = &ui;
    sdk.panels.register_provider = &register_provider;
    sdk.panels.unregister_owner = &unregister_owner;
    sdk.http_dispatch.register_command = &register_command;
    sdk.http_dispatch.unregister_owner = &unregister_owner;
    sdk.scheduler.add = &add_task;
    sdk.scheduler.remove = &remove_task;
    CHECK(debug_components_register(&sdk));
    CHECK(g_provider != nullptr && g_provider->panel_id != nullptr &&
          std::strcmp(g_provider->panel_id, "debug_components") == 0);
    debug_components_set_enabled(true);
    CHECK(g_provider->open());
    g_provider->callbacks->enter();
    CHECK(g_provider->active());

    // 结构：root = [双栏页, 面板导航按钮, 分类0 三枚文本按钮]。
    Control* root = root_control();
    CHECK(root != nullptr && root->kind == 0 && root->children.size() == 5);
    Control* columns = root->children[0].get();
    Control* panel_nav_control = root->children[1].get();
    g_nav_control = panel_nav_control;
    CHECK(columns->kind == 3);
    CHECK(panel_nav_control->kind == 1 && panel_nav_control->click != nullptr);
    CHECK(panel_nav_control->rect.x == panel_nav.x &&
          panel_nav_control->rect.y == panel_nav.y &&
          panel_nav_control->rect.w == panel_nav.w && panel_nav_control->rect.h == panel_nav.h);
    for (size_t i = 0; i < 3; ++i) {
        Control* text_button = root->children[2 + i].get();
        CHECK(text_button->kind == 1 && text_button->rect.x == text_button_rects[i].x &&
              text_button->rect.w == text_button_rects[i].w);
    }
    // 双栏页内部：左列表（原生 scroll）5 行，行高 56。
    CHECK(columns->children.size() == 1);
    Control* list = columns->children[0].get();
    CHECK(list->kind == 4 && list->rect.x == left.x && list->rect.w == left.w);
    CHECK(list->children.size() == 1);
    Control* group = list->children[0].get();
    CHECK(group->children.size() == 5);
    for (size_t i = 0; i < 5; ++i) {
        CHECK(group->children[i]->rect.y == static_cast<int64_t>(i) * 56 &&
              group->children[i]->rect.h == 56 && group->children[i]->click != nullptr);
    }

    // 首帧 process：绘制双栏（框架 + detail 文本 + 左列表）与自动导航按钮（depth 1 → X）。
    g_provider->callbacks->process();
    CHECK(g_border_draws >= 3);       // 外框 + 左栏框 + 右栏框
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
        CHECK(demo->kind == 1 && demo->rect.x == nav_demo_rects[i].x);
    }
    CHECK(g_string_draws > 5);

    // 切到「开关胶囊」分类（纯绘制，无交互控件）→ 右栏不再有按钮。
    CHECK(api->help_page_set_selected(columns, 1).ok == kQolOk);
    g_provider->callbacks->process();
    CHECK(root->children.size() == 2);  // 双栏页 + 面板导航
    CHECK(g_native_toggle_draws >= 6);

    // 面板导航按钮点击（自适应组件）→ 请求弹栈顶。
    g_back_requested = false;
    panel_nav_control->click(panel_nav_control->user);
    CHECK(g_back_requested);

    // 新语义：按下只派发/置原生态并消费，松开仍命中导航按钮才经原生树 ExecuteProc 触发。
    int64_t nav_hit[] = {900, 40};
    const uint64_t nav_hit_param = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(nav_hit));
    g_back_requested = false;
    g_nav_pressed = false;
    CHECK(g_provider->callbacks->event(0x17, nav_hit_param, 0x10) == 1);
    CHECK(!g_back_requested);  // 按下不触发
    CHECK(g_provider->callbacks->event(0x18, nav_hit_param, 0x10) == 1);
    CHECK(g_back_requested);   // 松开触发恰一次

    // 移出后松开：原生取消，不触发。
    int64_t elsewhere[] = {16, 16};
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

    // 关闭：组件侧树释放，native 由宿主删除。
    g_provider->close();
    CHECK(!g_provider->active());
    CHECK(api->help_page_draw(columns).code == kQolErrNotFound);
    remove_control(root);
    CHECK(g_live == 0);

    debug_components_unregister(&sdk);
    ui_components_unbind();
    std::printf("debug_two_column checks: %d\n", checks);
    return 0;
#undef CHECK
}
