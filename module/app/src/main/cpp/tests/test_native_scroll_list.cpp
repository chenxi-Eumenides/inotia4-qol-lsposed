#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "contracts/ui.h"
#include "fake_shared_atlases.h"
#include "plugins/core.custom_ui/panels/debug_scroll.h"
#include "plugins/core.custom_ui/ui_components.h"

using namespace qol::contracts;
namespace {
struct Control {
    uint32_t kind = 0;
    Control* parent = nullptr;
    std::vector<std::unique_ptr<Control>> children;
    UiRect rect{};
    UiNativeScrollDesc desc{};
    Control* target = nullptr;
    Control* scroll = nullptr;
    QolUiDrawFn draw = nullptr;
    QolUiNativeScrollDrawFn content_draw = nullptr;
    QolUiClickFn click = nullptr;
    void* user = nullptr;
    int64_t offset = 0;
    // 模拟原生按钮内联文本缓冲 data+0x00..0x20；text_overflow 记录 strcpy 越界
    // （真实溢出会覆盖 data+0x20 的 ExecuteProc，点击时跳垃圾地址）。
    char text[0x20] = {};
    bool text_overflow = false;
    bool enabled = true;
};
std::vector<std::unique_ptr<Control>> g_native_roots;
bool g_main_thread = true;
// Native scroll driver: the entry clip is the guest viewport and the real
// driver resets clip after the content callback. Row content must therefore
// intersect the saved parent instead of relying on the driver's reset.
UiRect g_clip{-1, -1, -1, -1};
std::vector<UiRect> g_row_clips;
int g_calls = 0, g_fail_at = -1, g_deletes = 0, g_live = 0, g_processes = 0, g_scroll_draws = 0;
int g_events = 0, g_row_draws = 0, g_selections = 0;
int g_child_button_clicks = 0;
int g_text_button_clicks = 0;
int g_text_button_draws = 0;
int g_text_label_draws = 0;
int32_t g_text_label_font = -1;
UiRect g_text_background_rect{};
UiRect g_text_label_rect{};
std::string g_text_label;
bool g_cancel_next_release = false;
int g_native_toggle_calls = 0, g_native_toggle_value = -1;
UiRect g_native_toggle_rect{};
bool g_fail_delete = false;
Control* g_execute = nullptr;
uint64_t g_event = 0, g_param = 0, g_param2 = 0;
QolUiHandle g_event_root = nullptr, g_destroy_on_select = nullptr;
QolUiHandle g_reentrant_create = nullptr;
QolUiHandle g_destroy_on_draw = nullptr;
const QolUiComponents* g_api = nullptr;
std::vector<uint32_t> g_draw_order;
std::vector<uint8_t> g_draw_selected;
uint32_t g_selected = 0;
const PanelProviderSpec* g_provider = nullptr;
QolFrameHandler g_frame = nullptr;
int g_open_calls = 0, g_close_calls = 0, g_removed_tasks = 0;
bool g_back_requested = false;

QolResult fail() { return error(kQolErrNotReady, "injected", 8); }
QolResult step() { return ++g_calls == g_fail_at ? fail() : ok(); }
int32_t main_thread() { return g_main_thread ? 1 : 0; }

QolResult create(uint32_t kind, QolUiHandle parent, QolUiHandle* out) {
    const QolResult result = step();
    if (!succeeded(result)) return result;
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
    const QolResult result = step();
    if (succeeded(result)) static_cast<Control*>(handle)->rect = rect;
    return result;
}
QolResult get_rect(QolUiHandle handle, UiRect* rect) {
    *rect = static_cast<Control*>(handle)->rect;
    return ok();
}
QolResult set_text(QolUiHandle handle, const char* text) {
    auto* control = static_cast<Control*>(handle);
    assert(control->kind == 1 || control->kind == 2);  // group 没有按钮文本数据。
    const size_t length = std::strlen(text);
    // 模拟原生 ControlButton_SetText（F_CONTROL_BUTTON_SET_TEXT_VMA）的 0x20 内联缓冲：
    // 清区后 strcpy；含 NUL 超 0x20 即越界覆盖相邻 ExecuteProc 槽，fail-closed 并记录。
    control->text_overflow = length >= sizeof(control->text);
    if (control->text_overflow) return error(kQolErrBlocked, "text exceeds inline buffer", 28);
    std::memcpy(control->text, text, length + 1);
    return ok();
}
QolResult draw_text(QolUiHandle handle, int32_t, int32_t, uint32_t) {
    const auto* control = static_cast<Control*>(handle);
    assert((control->kind == 1 || control->kind == 2) && control->text[0] != 0);
    return ok();
}
QolResult set_click(QolUiHandle handle, QolUiClickFn fn, void* user) {
    assert(fn != nullptr);
    const QolResult result = step();
    if (succeeded(result)) {
        auto* node = static_cast<Control*>(handle);
        node->click = fn;
        node->user = user;
    }
    return result;
}
QolResult set_draw(QolUiHandle handle, QolUiDrawFn fn) {
    const QolResult result = step();
    if (succeeded(result)) static_cast<Control*>(handle)->draw = fn;
    return result;
}
void revoke(Control* node) {
    node->click = nullptr;
    node->user = nullptr;
    --g_live;
    for (const auto& child : node->children) revoke(child.get());
}
QolResult remove_control(QolUiHandle handle) {
    assert(g_main_thread);
    if (g_fail_delete) return fail();
    ++g_deletes;
    auto* node = static_cast<Control*>(handle);
    auto& siblings = node->parent == nullptr ? g_native_roots : node->parent->children;
    const auto it = std::find_if(siblings.begin(), siblings.end(),
        [node](const auto& item) { return item.get() == node; });
    assert(it != siblings.end());
    revoke(node);
    siblings.erase(it);
    return ok();
}
QolResult scroll_create(QolUiHandle parent, const UiNativeScrollDesc* desc,
                       QolUiNativeScrollDrawFn draw, QolUiHandle* out) {
    assert(g_main_thread && parent != nullptr && draw != nullptr);
    const QolResult result = create(4, parent, out);
    if (!succeeded(result)) return result;
    auto* scroll = static_cast<Control*>(*out);
    scroll->desc = *desc;
    scroll->rect = desc->viewport;
    scroll->content_draw = draw;
    return ok();
}
QolResult scroll_target(QolUiHandle scroll, QolUiHandle group) {
    const QolResult result = step();
    if (succeeded(result)) static_cast<Control*>(scroll)->target = static_cast<Control*>(group);
    return result;
}
QolResult button_scroll(QolUiHandle row, QolUiHandle scroll) {
    const QolResult result = step();
    if (succeeded(result)) static_cast<Control*>(row)->scroll = static_cast<Control*>(scroll);
    return result;
}
QolResult clip_get(UiRect* out);
QolResult clip_reset();
QolResult process(QolUiHandle) { assert(g_main_thread); ++g_processes; return ok(); }
QolResult draw_control(QolUiHandle handle) {
    auto* node = static_cast<Control*>(handle);
    assert(node->kind == 1 && node->draw != nullptr);  // 不允许用按钮绘制入口画 scroll。
    ++g_row_draws;
    node->draw(handle);
    return ok();
}
QolResult draw_scroll(QolUiHandle handle) {
    ++g_scroll_draws;
    auto* scroll = static_cast<Control*>(handle);
    g_clip = scroll->rect;
    scroll->content_draw(handle, -scroll->offset);
    clip_reset();
    return ok();
}
QolResult get_offset(QolUiHandle handle, int64_t* out) {
    assert(g_main_thread);
    *out = static_cast<Control*>(handle)->offset;
    return ok();
}
QolResult set_offset(QolUiHandle handle, int64_t offset) {
    assert(g_main_thread);
    auto* scroll = static_cast<Control*>(handle);
    if (offset < 0 || offset > std::max<int64_t>(0, scroll->desc.content_extent - scroll->rect.h)) {
        return error(kQolErrInvalidArgument, "offset", 6);
    }
    scroll->offset = offset;
    return ok();
}
QolResult event(QolUiHandle root, uint64_t evt, uint64_t param, uint64_t param2, uint32_t* consumed) {
    assert(g_main_thread);
    ++g_events;
    g_event_root = root; g_event = evt; g_param = param; g_param2 = param2;
    if (evt == 0x18 && g_execute != nullptr && !g_cancel_next_release) {
        assert(g_execute->click != nullptr);
        g_execute->click(g_execute->user);
    }
    if (evt == 0x18) g_cancel_next_release = false;
    *consumed = 7;
    return ok();
}
void draw_item(void* user, uint32_t index, QolUiHandle row, uint8_t selected) {
    assert(user == &g_selected && row != nullptr);
    g_draw_order.push_back(index);
    g_draw_selected.push_back(selected);
    g_row_clips.push_back(g_clip);
    if (g_destroy_on_draw != nullptr) {
        const int deletes = g_deletes;
        assert(succeeded(g_api->destroy(g_destroy_on_draw)));
        assert(g_deletes == deletes);
    }
}
void select(void* user, uint32_t index) {
    assert(user == &g_selected);
    g_selected = index;
    ++g_selections;
    if (g_destroy_on_select != nullptr) {
        const int deletes = g_deletes;
        assert(succeeded(g_api->destroy(g_destroy_on_select)));
        assert(g_deletes == deletes);
        UiScrollListDesc desc{{0, 0, 100, 100}, 50, 2, 0, &draw_item, &select, &g_selected};
        assert(g_api->scroll_list_create(&desc, g_event_root, &g_reentrant_create).code == kQolErrNotReady);
        assert(g_reentrant_create == nullptr);
    }
}
void child_button_click(void*) { ++g_child_button_clicks; }
void text_button_click(void*) { ++g_text_button_clicks; }
QolResult no_op() { return ok(); }
QolResult inset(int32_t* out) { *out = 0; return ok(); }

QolResult clip_get(UiRect* out) { *out = g_clip; return ok(); }
QolResult clip_set(int32_t x, int32_t y, int32_t w, int32_t h) { g_clip = {x, y, w, h}; return ok(); }
QolResult clip_reset() { g_clip = {-1, -1, -1, -1}; return ok(); }

// 游戏 IMGSYS fake：option 图集（0x59）UnitLoad 幂等（已加载不重载、不增引用）；
// UnitUnload 无引用计数、直接置失效并记录物理卸载次数。0x59 由游戏共享（设置页返回
// 按钮/切换开关也在用），模块页关闭后原生消费者仍会绘制；页面任何关闭/停用路径都不
// 得卸载，物理卸载次数必须为 0。
constexpr int32_t kOptionImageUnit = 0x59;
qol::tests::FakeSharedAtlases g_atlases;
// 模拟仍存活的原生消费者：记录其依赖的图集加载代次，模块页关闭后校验仍可绘制。
const auto g_native_option = g_atlases.native_consumer({kOptionImageUnit});
QolResult load_option_images() {
    g_atlases.load(kOptionImageUnit);
    return ok();
}
QolResult unload_option_images() {
    g_atlases.unload(kOptionImageUnit);
    return ok();
}

QolResult pressed(QolUiHandle, int32_t* out) { *out = 0; return ok(); }
QolResult draw_native_toggle(UiRect rect, int32_t value, uint8_t pressed_value) {
    g_native_toggle_rect = rect;
    g_native_toggle_value = value;
    assert(pressed_value <= 1);
    ++g_native_toggle_calls;
    return ok();
}
QolResult draw_native_text_button_background(UiRect rect, uint8_t pressed_value) {
    assert(pressed_value <= 1);
    g_text_background_rect = rect;
    ++g_text_button_draws;
    return ok();
}
QolResult draw_native_text_button_label(UiRect rect, const char* text, uint32_t, int32_t font) {
    assert(text != nullptr && (font == 0 || font == 1));
    g_text_label_rect = rect;
    g_text_label = text;
    g_text_label_font = font;
    ++g_text_label_draws;
    return ok();
}
QolResult set_button_enabled(QolUiHandle button, uint8_t enabled) {
    assert(button != nullptr && enabled <= 1);
    static_cast<Control*>(button)->enabled = enabled != 0;
    return ok();
}
QolResult container(int32_t, int32_t, int32_t, int32_t) { return ok(); }
QolResult container_fill(UiRect, uint32_t) { return ok(); }
QolResult origin(QolUiHandle handle, int64_t* x, int64_t* y) {
    *x = 0;
    *y = 0;
    for (auto* node = static_cast<Control*>(handle); node != nullptr; node = node->parent) {
        *x += node->rect.x;
        *y += node->rect.y;
        if (node->scroll != nullptr && node->scroll->target == node->parent) {
            *y -= node->scroll->offset;
        }
    }
    return ok();
}
QolResult font_color_rgb(int32_t, int32_t, int32_t) { return ok(); }
QolResult string_with_font(const char*, int32_t, int32_t, int32_t, int32_t) { return ok(); }
QolResult custom_button(QolUiHandle handle, UiRect, uint32_t, uint32_t, int32_t, const char* text,
                        uint32_t, int32_t, int32_t) {
    if (text != nullptr) {
        const auto* control = static_cast<Control*>(handle);
        assert(control->kind == 1 && std::strcmp(control->text, text) == 0);
    }
    return ok();
}
QolResult back(QolUiHandle, uint8_t) { return ok(); }

QolUiAdapter make_adapter() {
    QolUiAdapter ui{};
    ui.size = sizeof(ui); ui.major = kUiAdapterMajor; ui.minor = kUiAdapterMinor;
    ui.create_control = &create; ui.set_rect = &set_rect; ui.get_rect = &get_rect;
    ui.set_text = &set_text; ui.draw_text = &draw_text;
    ui.set_click_proc = &set_click; ui.set_draw_proc = &set_draw; ui.delete_control = &remove_control;
    ui.is_main_thread = &main_thread; ui.scroll_create = &scroll_create;
    ui.scroll_set_target = &scroll_target; ui.button_set_scroll = &button_scroll;
    ui.scroll_process = &process; ui.scroll_draw = &draw_scroll; ui.draw_control = &draw_control;
    ui.scroll_get_offset = &get_offset; ui.scroll_set_offset = &set_offset;
    ui.control_tree_event = &event; ui.begin_frame = &no_op; ui.end_frame = &no_op;
    ui.screen_inset_width = &inset; ui.screen_inset_height = &inset;
    ui.draw_border_rect = &container; ui.draw_custom_button = &custom_button;
    ui.fill_rect = &container_fill;
    ui.control_pressed = &pressed; ui.draw_back_button = &back;
    ui.control_absolute_origin = &origin; ui.set_font_color_rgb = &font_color_rgb;
    ui.draw_string_with_font = &string_with_font;
    ui.load_option_images = &load_option_images;
    ui.unload_option_images = &unload_option_images;
    ui.draw_native_toggle = &draw_native_toggle;
    ui.draw_native_text_button_background = &draw_native_text_button_background;
    ui.draw_native_text_button_label = &draw_native_text_button_label;
    ui.set_button_enabled = &set_button_enabled;
    ui.get_clip_rect = &clip_get;
    ui.set_clip = &clip_set;
    ui.reset_clip = &clip_reset;
    return ui;
}
QolResult register_provider(const PanelProviderSpec* provider) { g_provider = provider; return ok(); }
QolResult add_task(int32_t point, QolFrameHandler fn, void*, int32_t interval,
                   int32_t count, uint64_t* out) {
    assert(point == 1 && interval == 1 && count == 0);
    g_frame = fn; *out = 42; return ok();
}
QolResult remove_task(uint64_t id) { assert(id == 42); ++g_removed_tasks; return ok(); }
HttpRes send(const char* body) {
    static uint8_t buffer[256]{};
    HttpReq req{}; req.body = reinterpret_cast<const uint8_t*>(body);
    req.body_len = static_cast<uint32_t>(std::strlen(body));
    HttpRes res{}; res.body = buffer; res.body_capacity = sizeof(buffer);
    assert(succeeded(qol::features::core_ui::debug_scroll_http_command(&req, &res, nullptr)));
    return res;
}
}  // namespace

namespace qol::features::core_ui {
bool ui_panels_open(const char* id) {
    assert(g_main_thread && std::strcmp(id, "debug_scroll") == 0);
    ++g_open_calls;
    if (!g_provider->open()) return false;
    g_provider->callbacks->enter();
    return true;
}
void ui_panels_close(const char* id) {
    assert(g_main_thread && std::strcmp(id, "debug_scroll") == 0);
    ++g_close_calls;
    if (g_provider != nullptr && g_provider->active()) g_provider->close();
}
// 宿主 back 请求：真实宿主经帧任务在弹窗回调之外执行。测试同样只置标志，
// 由 main 在回调返回后调用 flush_back_request() 应用（模拟帧边界）。
void ui_panels_request_back() { g_back_requested = true; }
int ui_panels_depth() { return 1; }
}  // namespace qol::features::core_ui

namespace {
void flush_back_request() {
    if (!g_back_requested) return;
    g_back_requested = false;
    if (g_main_thread && g_provider != nullptr && g_provider->active()) g_provider->close();
}
}  // namespace

int main() {
    using namespace qol::features::core_ui;
    int checks = 0;
#define CHECK(expr) do { assert(expr); ++checks; } while (false)
    static_assert(offsetof(QolUiComponents, scroll_list_create) ==
        offsetof(QolUiComponents, draw_container_ornamented) + sizeof(void*));
    auto ui = make_adapter();
    CHECK(ui_components_bind(&ui));
    g_api = ui_components_api();
    CHECK(g_api->major == 1 && g_api->minor == 14);
    QolUiHandle root = nullptr, list = nullptr;
    CHECK(succeeded(create(0, nullptr, &root)));
    UiScrollListDesc desc{{12, 20, 200, 140}, 56, 24, 2, &draw_item, &select, &g_selected};
    CHECK(succeeded(g_api->scroll_list_create(&desc, root, &list)));
    auto* scroll = static_cast<Control*>(list);
    CHECK(scroll->parent == root && scroll->kind == 4);
    CHECK(scroll->rect.x == 12 && scroll->rect.y == 20 && scroll->rect.h == 140);
    CHECK(scroll->desc.content_extent == 1344 && scroll->desc.drag_threshold == 12);
    CHECK(scroll->children.size() == 1 && scroll->target == scroll->children[0].get());
    CHECK(scroll->target->kind == 3 && scroll->target->parent == scroll);
    CHECK(scroll->target->rect.x == 0 && scroll->target->rect.h == 1344);
    CHECK(scroll->target->children.size() == 24);
    for (uint32_t i = 0; i < 24; ++i) {
        const auto* row = scroll->target->children[i].get();
        // 行控件只承担触摸命中与 ExecuteProc；不得绑定原生绘制回调或写入内联文本。
        CHECK(row->kind == 1 && row->scroll == scroll && row->click != nullptr && row->draw == nullptr);
        CHECK(!row->text_overflow && row->text[0] == 0);
        CHECK(row->rect.x == 0 && row->rect.y == i * 56 && row->rect.w == 200 && row->rect.h == 56);
    }
    // Native capsule is a child of a borrowed row, scroll-bound to the list and emitted by
    // the native event tree only. Destroying the list removes its whole native subtree once.
    QolUiHandle toggle_handle = nullptr;
    uint32_t toggle_events = 0;
    uint32_t toggle_consumed = 0;
    UiNativeToggleDesc toggle_desc{{68, 4, 96, 44}, 0, 1,
        [](void* user, int32_t desired) {
            auto* count = static_cast<uint32_t*>(user);
            *count += static_cast<uint32_t>(desired + 1);
        }, &toggle_events};
    QolUiHandle row = nullptr;
    CHECK(succeeded(g_api->scroll_list_get_row(list, 3, &row)) && row != nullptr);
    CHECK(g_api->destroy(row).code == kQolErrNotFound);  // row is borrowed from its list.
    CHECK(succeeded(g_api->native_toggle_create(&toggle_desc, row, &toggle_handle)));
    CHECK(static_cast<Control*>(toggle_handle)->scroll == list);
    CHECK(g_api->native_toggle_get_value(toggle_handle, &g_native_toggle_value).ok == kQolOk &&
          g_native_toggle_value == 0);
    g_execute = static_cast<Control*>(toggle_handle);
    CHECK(succeeded(g_api->dispatch_native_event(root, 0x17, 5, 6, &toggle_consumed)));
    CHECK(toggle_events == 0);  // press alone does not request a new value.
    CHECK(succeeded(g_api->dispatch_native_event(root, 0x19, 5, 6, &toggle_consumed)));
    CHECK(toggle_events == 0);  // an intermediate/move event does not activate ExecuteProc.
    CHECK(succeeded(g_api->dispatch_native_event(root, 0x18, 5, 6, &toggle_consumed)));
    CHECK(toggle_events == 2);  // one desired=1 notification; value remains controlled at 0
    CHECK(succeeded(g_api->native_toggle_get_value(toggle_handle, &g_native_toggle_value)) &&
          g_native_toggle_value == 0);
    CHECK(succeeded(g_api->native_toggle_set_value(toggle_handle, 1)));
    CHECK(succeeded(g_api->native_toggle_set_enabled(toggle_handle, 0)));
    g_execute->click(g_execute->user);
    CHECK(toggle_events == 2);
    CHECK(succeeded(g_api->native_toggle_set_enabled(toggle_handle, 1)));
    scroll->offset = 37;
    CHECK(succeeded(g_api->native_toggle_draw(toggle_handle)));
    CHECK(g_native_toggle_calls == 1 && g_native_toggle_rect.x == 80 &&
          g_native_toggle_rect.y == 20 + 3 * 56 + 4 - 37 && g_native_toggle_rect.w == 96);
    g_execute = nullptr;
    g_events = 0;
    g_selections = 0;
    CHECK(succeeded(g_api->scroll_list_process(list)) && g_processes == 1);
    // 内容绘制直接回调 draw_item；不再经原生 draw_control（g_row_draws 保持 0）。
    // Row content must intersect the saved parent clip, not rely on the driver's
    // post-content reset. A real parent rect proves the intersection.
    g_clip = {0, 0, 120, 100};
    g_row_clips.clear();
    CHECK(succeeded(g_api->scroll_list_draw(list)) && g_scroll_draws == 1 && g_row_draws == 0);
    CHECK(g_row_clips.size() == 24 && g_row_clips.front().x == 12 && g_row_clips.front().y == 20 &&
          g_row_clips.front().w == 108 && g_row_clips.front().h == 80);
    CHECK(g_clip.x == 0 && g_clip.y == 0 && g_clip.w == 120 && g_clip.h == 100);
    // A parent sentinel still bounds content to the screen, then restores no-clip.
    g_clip = {-1, -1, -1, -1};
    g_row_clips.clear();
    CHECK(succeeded(g_api->scroll_list_draw(list)));
    CHECK(g_row_clips.front().x == 12 && g_row_clips.front().y == 20 &&
          g_row_clips.front().w == 200 && g_row_clips.front().h == 140);
    CHECK(g_clip.x == -1 && g_clip.y == -1 && g_clip.w == -1 && g_clip.h == -1);
    g_row_clips.clear();
    g_draw_order.clear();
    g_draw_selected.clear();
    CHECK(succeeded(g_api->scroll_list_draw(list)));
    g_scroll_draws = 1;
    CHECK(g_draw_order.size() == 24 && g_draw_order[23] == 23);
    CHECK(g_draw_selected[2] == 1 && g_draw_selected[0] == 0);
    for (uint32_t i = 0; i < 24; ++i) {
        const auto* row = scroll->target->children[i].get();
        CHECK(!row->text_overflow && row->text[0] == 0 && row->click != nullptr);
    }
    uint32_t consumed = 0;
    g_execute = scroll->target->children[7].get();
    CHECK(succeeded(g_api->dispatch_native_event(root, 0x18, 0x12345678, 0xabcdef, &consumed)));
    CHECK(g_events == 1 && g_event_root == root && g_event == 0x18 && g_param == 0x12345678 &&
          g_param2 == 0xabcdef && consumed == 7);
    CHECK(g_selections == 1 && g_selected == 7);
    g_draw_selected.clear(); g_draw_order.clear();
    CHECK(succeeded(g_api->scroll_list_draw(list)) && g_draw_selected[7] == 1 && g_draw_selected[2] == 0);
    CHECK(succeeded(g_api->scroll_list_set_selection_enabled(list, 0)));
    const int selections_before_disabled_click = g_selections;
    g_execute = scroll->target->children[8].get();
    CHECK(succeeded(g_api->dispatch_native_event(root, 0x18, 8, 9, &consumed)));
    CHECK(g_selections == selections_before_disabled_click);
    QolUiHandle action_button = nullptr;
    CHECK(succeeded(g_api->scroll_row_button_create(list, 9, {100, 0, 80, 56},
                                                     &child_button_click, nullptr,
                                                     &action_button)));
    CHECK(static_cast<Control*>(action_button)->parent == scroll->target->children[9].get());
    CHECK(static_cast<Control*>(action_button)->scroll == list);
    g_execute = static_cast<Control*>(action_button);
    CHECK(succeeded(g_api->dispatch_native_event(root, 0x17, 0, 0, &consumed)));
    CHECK(succeeded(g_api->dispatch_native_event(root, 0x19, 0, 0, &consumed)));
    CHECK(g_child_button_clicks == 0);  // Press/move do not activate ExecuteProc; scroll owns gesture tracking.
    CHECK(succeeded(g_api->dispatch_native_event(root, 0x18, 0, 0, &consumed)));
    CHECK(g_child_button_clicks == 1 && g_selections == selections_before_disabled_click);
    g_draw_selected.clear(); g_draw_order.clear();
    CHECK(succeeded(g_api->scroll_list_draw(list)));
    CHECK(std::find(g_draw_selected.begin(), g_draw_selected.end(), 1) == g_draw_selected.end());
    g_execute = nullptr;
    CHECK(succeeded(g_api->scroll_list_set_selection_enabled(list, 1)));
    CHECK(succeeded(g_api->scroll_set_offset(list, 121)));
    int64_t offset = -1;
    CHECK(succeeded(g_api->scroll_list_get_offset(list, &offset)) && offset == 121);
    scroll->offset = 333;  // 模拟原生 Process 改变 offset，组件必须读取原生值。
    CHECK(succeeded(g_api->scroll_list_get_offset(list, &offset)) && offset == 333);
    CHECK(g_api->scroll_set_offset(list, 1205).code == kQolErrInvalidArgument);
    CHECK(g_api->scroll_list_get_offset(list, nullptr).code == kQolErrInvalidArgument);
    QolUiHandle nested = nullptr;
    CHECK(g_api->scroll_list_create(&desc, list, &nested).code == kQolErrUnsupported);
    g_main_thread = false;
    CHECK(g_api->scroll_list_process(list).code == kQolErrNotReady);
    CHECK(g_api->scroll_list_draw(list).code == kQolErrNotReady);
    CHECK(g_api->scroll_list_get_offset(list, &offset).code == kQolErrNotReady);
    CHECK(g_api->scroll_set_offset(list, 0).code == kQolErrNotReady);
    CHECK(g_api->destroy(list).code == kQolErrNotReady);
    CHECK(g_api->dispatch_native_event(root, 1, 2, 3, &consumed).code == kQolErrNotReady);
    g_main_thread = true;
    g_destroy_on_select = list;
    g_execute = scroll->target->children[7].get();
    CHECK(succeeded(g_api->dispatch_native_event(root, 0x18, 8, 9, &consumed)));
    CHECK(g_live == 29);  // 回调结束仍未同步释放；列表只拥有一棵 native 子树。
    g_execute = nullptr; g_destroy_on_select = nullptr;
    CHECK(g_api->scroll_list_process(list).code == kQolErrNotFound);
    CHECK(g_api->native_toggle_get_value(toggle_handle, &g_native_toggle_value).code ==
          kQolErrNotFound);
    CHECK(g_live == 1 && static_cast<Control*>(root)->children.empty() && g_deletes == 1);
    CHECK(g_api->destroy(list).code == kQolErrNotFound);

    // Settings action shares the native toggle's right-side rectangle and remains a scroll-owned
    // ControlButton child. Its draw origin follows scroll displacement without resetting the list clip.
    UiScrollListDesc settings_desc{{0, 0, 572, 96}, 96, 2, -1, &draw_item, nullptr, &g_selected};
    QolUiHandle settings_list = nullptr;
    CHECK(succeeded(g_api->scroll_list_create(&settings_desc, root, &settings_list)));
    CHECK(succeeded(g_api->scroll_list_set_selection_enabled(settings_list, 0)));
    QolUiHandle settings_row = nullptr;
    CHECK(succeeded(g_api->scroll_list_get_row(settings_list, 0, &settings_row)));
    const UiButtonDesc settings_action{{332, 8, 220, 80}, "打开",
                                        kUiTextButtonFontLarger,
                                        &text_button_click, nullptr};
    QolUiHandle settings_button = nullptr;
    CHECK(succeeded(g_api->text_button_create(&settings_action, settings_row, &settings_button)));
    auto* settings_control = static_cast<Control*>(settings_button);
    CHECK(settings_control->parent == settings_row && settings_control->scroll == settings_list);
    CHECK(!settings_control->text_overflow && settings_control->text[0] == '\0');
    CHECK(settings_control->rect.x == 332 && settings_control->rect.y == 8 &&
          settings_control->rect.w == 220 && settings_control->rect.h == 80);
    CHECK(succeeded(g_api->text_button_draw(settings_button)));
    CHECK(g_text_label_font == 1);
    CHECK(g_text_button_draws == 1 && g_text_label_draws == 1 &&
          g_text_background_rect.x == 332 && g_text_background_rect.y == 8 &&
          g_text_label_rect.x == 332 && g_text_label_rect.y == 8 && g_text_label == "打开");
    static_cast<Control*>(settings_list)->offset = 17;
    CHECK(succeeded(g_api->text_button_draw(settings_button)));
    CHECK(g_text_background_rect.y == -9 && g_text_label_rect.y == -9);

    uint32_t settings_consumed = 0;
    g_execute = settings_control;
    const int events_before_action = g_events;
    g_cancel_next_release = true;
    CHECK(succeeded(g_api->dispatch_native_event(settings_list, 0x17, 11, 12,
                                                  &settings_consumed)));
    CHECK(succeeded(g_api->dispatch_native_event(settings_list, 0x19, 13, 14,
                                                  &settings_consumed)));
    CHECK(succeeded(g_api->dispatch_native_event(settings_list, 0x18, 900, 900,
                                                  &settings_consumed)));
    CHECK(g_text_button_clicks == 0 && g_events == events_before_action + 3);
    CHECK(succeeded(g_api->dispatch_native_event(settings_list, 0x17, 11, 12,
                                                  &settings_consumed)));
    CHECK(succeeded(g_api->dispatch_native_event(settings_list, 0x19, 13, 14,
                                                  &settings_consumed)));
    CHECK(succeeded(g_api->dispatch_native_event(settings_list, 0x18, 11, 12,
                                                  &settings_consumed)));
    CHECK(g_text_button_clicks == 1 && g_events == events_before_action + 6);
    g_draw_selected.clear();
    CHECK(succeeded(g_api->scroll_list_draw(settings_list)) &&
          std::find(g_draw_selected.begin(), g_draw_selected.end(), 1) == g_draw_selected.end());
    CHECK(succeeded(g_api->text_button_set_enabled(settings_button, 0)));
    CHECK(!settings_control->enabled);
    CHECK(succeeded(g_api->dispatch_native_event(settings_list, 0x17, 11, 12,
                                                  &settings_consumed)));
    CHECK(succeeded(g_api->dispatch_native_event(settings_list, 0x18, 11, 12,
                                                  &settings_consumed)));
    CHECK(g_text_button_clicks == 1);
    g_execute = nullptr;
    CHECK(succeeded(g_api->destroy(settings_list)) && g_live == 1);
    CHECK(g_api->text_button_draw(settings_button).code == kQolErrNotFound);

    auto invalid = desc;
    invalid.item_height = 0;
    CHECK(g_api->scroll_list_create(&invalid, root, &list).code == kQolErrInvalidArgument && list == nullptr);
    invalid = desc; invalid.viewport.w = 0;
    CHECK(g_api->scroll_list_create(&invalid, root, &list).code == kQolErrInvalidArgument);
    invalid = desc; invalid.item_count = UINT32_MAX;
    CHECK(g_api->scroll_list_create(&invalid, root, &list).code == kQolErrInvalidArgument);
    invalid = desc; invalid.item_height = INT32_MAX;
    CHECK(g_api->scroll_list_create(&invalid, root, &list).code == kQolErrInvalidArgument);
    invalid = desc; invalid.initial_selected = 24;
    CHECK(g_api->scroll_list_create(&invalid, root, &list).code == kQolErrInvalidArgument);
    invalid = desc; invalid.viewport.x = INT64_MAX;
    CHECK(g_api->scroll_list_create(&invalid, root, &list).code == kQolErrInvalidArgument);
    invalid = desc; invalid.item_count = 0; invalid.initial_selected = -1; invalid.draw_item = nullptr;
    CHECK(succeeded(g_api->scroll_list_create(&invalid, root, &list)));
    CHECK(static_cast<Control*>(list)->desc.content_extent == 0 &&
          static_cast<Control*>(list)->target->children.empty());
    CHECK(succeeded(g_api->scroll_list_draw(list)));
    CHECK(succeeded(g_api->scroll_set_offset(list, 0)));
    CHECK(succeeded(g_api->destroy(list)));

    // 无消费者选择动作仍有有效 ExecuteProc；短内容 offset 只能为 0。
    invalid = desc; invalid.item_count = 1; invalid.initial_selected = -1; invalid.on_select = nullptr;
    CHECK(succeeded(g_api->scroll_list_create(&invalid, root, &list)));
    g_execute = static_cast<Control*>(list)->target->children[0].get();
    const int selections_before = g_selections;
    CHECK(succeeded(g_api->dispatch_native_event(root, 0x18, 0, 0, &consumed)));
    CHECK(g_selections == selections_before);
    CHECK(g_api->scroll_set_offset(list, 1).code == kQolErrInvalidArgument);
    g_execute = nullptr;
    g_destroy_on_draw = list;
    CHECK(succeeded(g_api->scroll_list_draw(list)) && g_live == 4);
    g_destroy_on_draw = nullptr;
    CHECK(g_api->scroll_list_process(list).code == kQolErrNotFound && g_live == 1);

    // 两个独立列表顺序绘制，offset 互不影响。
    QolUiHandle second = nullptr;
    CHECK(succeeded(g_api->scroll_list_create(&desc, root, &list)));
    CHECK(succeeded(g_api->scroll_list_create(&desc, root, &second)));
    CHECK(succeeded(g_api->scroll_set_offset(list, 90)));
    CHECK(succeeded(g_api->scroll_list_get_offset(second, &offset)) && offset == 0);
    CHECK(succeeded(g_api->scroll_list_draw(list)) && succeeded(g_api->scroll_list_draw(second)));
    CHECK(succeeded(g_api->destroy(list)) && succeeded(g_api->destroy(second)) && g_live == 1);

    // 每个 native 接线步骤注入失败；只能通过一次 scroll subtree 删除回滚。
    // 接线步数：scroll_create/建 group/set_rect/set_target(4) + 3 行 ×(建行/set_rect/set_click/set_scroll) = 16。
    desc.item_count = 3;
    for (int failure = 1; failure <= 16; ++failure) {
        g_calls = 0; g_fail_at = failure;
        CHECK(!succeeded(g_api->scroll_list_create(&desc, root, &list)) && list == nullptr);
        CHECK(g_live == 1 && static_cast<Control*>(root)->children.empty());
    }
    g_fail_at = -1;
    // 回滚删除也失败时保留上下文，下一安全点重试而非丢失所有权。
    g_calls = 0; g_fail_at = 3; g_fail_delete = true;
    CHECK(!succeeded(g_api->scroll_list_create(&desc, root, &list)) && list == nullptr && g_live == 3);
    g_fail_delete = false; g_fail_at = -1;
    CHECK(succeeded(g_api->scroll_list_create(&desc, root, &list)));
    CHECK(succeeded(g_api->scroll_list_process(list)) && g_live == 6);
    CHECK(succeeded(g_api->destroy(list)) && g_live == 1);
    CHECK(succeeded(g_api->scroll_list_create(&desc, root, &list)));
    g_fail_delete = true;
    CHECK(g_api->destroy(list).code == kQolErrNotReady && g_live == 6);
    g_fail_delete = false;
    CHECK(succeeded(g_api->destroy(list)) && g_live == 1);
    CHECK(succeeded(remove_control(root)) && g_live == 0);

    UiPanelDesc panel{{0, 0, 960, 640}, {}, 0, 0};
    CHECK(succeeded(g_api->panel_create(&panel, nullptr, &root)));
    CHECK(succeeded(g_api->scroll_list_create(&desc, root, &list)));
    UiToggleDesc toggle{{0, 0, 100, 40}, "unsafe", 0, 0, nullptr, nullptr};
    QolUiHandle unsafe = nullptr;
    CHECK(succeeded(g_api->toggle_create(&toggle, root, &unsafe)));
    const int events_before = g_events;
    CHECK(g_api->dispatch_native_event(root, 1, 2, 3, &consumed).code == kQolErrUnsupported);
    CHECK(g_events == events_before);
    CHECK(succeeded(g_api->destroy(root)) && g_live == 2);
    CHECK(g_api->destroy(list).code == kQolErrNotFound);
    CHECK(succeeded(remove_control(root)) && g_live == 0);

    QolHostSdk sdk{}; sdk.ui = &ui;
    sdk.panels.register_provider = &register_provider;
    sdk.scheduler.add = &add_task; sdk.scheduler.remove = &remove_task;
    CHECK(debug_scroll_register(&sdk));
    CHECK(g_provider != nullptr && g_provider->callbacks->enter != nullptr &&
          g_provider->callbacks->process != nullptr && g_provider->callbacks->f3 != nullptr &&
          g_provider->callbacks->f4 != nullptr && g_provider->callbacks->event != nullptr);
    CHECK(send("{\"open\":true}").status == 503);
    debug_scroll_set_enabled(true);
    CHECK(send("{\"open\":1}").status == 400);
    CHECK(send("{\"open\":truejunk}").status == 400);
    CHECK(send("{\"open\":true,\"extra\":1}").status == 400);
    g_main_thread = false;
    CHECK(send("{ \"open\" : true }").status == 200);
    CHECK(g_open_calls == 0 && g_live == 0);
    CHECK(g_frame(1, nullptr) == 1 && g_open_calls == 0);
    g_main_thread = true;
    CHECK(g_frame(2, nullptr) == 1 && g_open_calls == 1 && g_provider->active() &&
          g_atlases.unit(kOptionImageUnit).loaded);
    CHECK(g_live == 31);  // root + scroll + group + 24 rows + 3 inactive labels + back。
    CHECK(std::strcmp(g_provider->panel_id, "debug_scroll") == 0);
    g_provider->callbacks->process();
    CHECK(g_processes == 3);
    // 回归：debug_scroll 行经 draw_item 自绘，不经原生 draw_control、不写按钮内联文本；
    // 假 set_text 会记录越界（text_overflow）并 fail-closed，ExecuteProc 未被破坏。
    {
        auto* dbg_root = g_native_roots.front().get();
        auto* dbg_scroll = dbg_root->children[0].get();
        CHECK(dbg_scroll->kind == 4 && dbg_scroll->target != nullptr);
        auto* dbg_group = dbg_scroll->target;
        CHECK(dbg_group->children.size() == 24);
        uint32_t row_overflows = 0;
        for (const auto& row : dbg_group->children) {
            if (row->text_overflow) ++row_overflows;
            CHECK(row->draw == nullptr && row->click != nullptr);
        }
        CHECK(row_overflows == 0 && g_row_draws == 0);
    }
    const int events = g_events;
    CHECK(g_provider->callbacks->event(0x17, 0x123, 0x456) == 7);
    CHECK(g_events == events + 1 && g_event == 0x17 && g_param == 0x123 && g_param2 == 0x456);
    // 返回按钮 ExecuteProc → 宿主 back（测试桩同步关闭并销毁控件树）。
    auto* popup_root = g_native_roots.front().get();
    g_execute = popup_root->children.back().get();
    CHECK(g_provider->callbacks->event(0x18, 10, 11) == 7);
    g_execute = nullptr;
    flush_back_request();  // 帧边界应用延迟关闭
    // 关闭后 0x59 仍加载、未被物理卸载：仍存活的原生消费者可继续绘制。
    CHECK(g_live == 0 && !g_provider->active() && g_atlases.native_draw(g_native_option));
    CHECK(send("{\"open\":true}").status == 200);
    g_frame(3, nullptr);
    CHECK(g_live == 31);
    // f3 → 宿主 back（帧边界应用）。
    g_provider->callbacks->f3();
    flush_back_request();
    // UnitLoad 幂等：f3 重开不重复加载、不增引用；f3 关闭不卸载。
    CHECK(g_live == 0 && !g_provider->active());
    CHECK(g_atlases.unit(kOptionImageUnit).generation == 1 &&
          g_atlases.native_draw(g_native_option));
    CHECK(send("{\"open\":true}").status == 200);
    CHECK(send("{\"open\":false}").status == 200);
    const int opened_before = g_open_calls;
    g_frame(4, nullptr);
    CHECK(g_live == 0 && g_open_calls == opened_before);
    CHECK(send("{\"open\":true}").status == 200);
    g_frame(4, nullptr);
    debug_scroll_set_enabled(false);
    CHECK(g_live == 31);  // 停用请求不在调用线程删树。
    g_frame(5, nullptr);
    // 停用路径同样不得卸载共享图集。
    CHECK(g_live == 0 && g_atlases.native_draw(g_native_option));
    debug_scroll_unregister(&sdk);
    CHECK(g_removed_tasks == 1);
    // 模块整个生命周期对共享 option 图集的物理卸载次数为 0；重复加载保持幂等。
    CHECK(g_atlases.protected_unloads() == 0 &&
          g_atlases.unit(kOptionImageUnit).generation == 1);
    CHECK(succeeded(create(0, nullptr, &root)));
    CHECK(succeeded(g_api->scroll_list_create(&desc, root, &list)));
    ui_components_unbind();
    CHECK(g_live == 1);  // unbind 归还 owned 列表，不删除 borrowed parent。
    CHECK(succeeded(remove_control(root)) && g_live == 0);
    std::printf("native_scroll_list checks: %d\n", checks);
    return 0;
#undef CHECK
}
