#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>

#include "contracts/ui.h"
#include "plugins/core.custom_ui/ui_components.h"

// 返回按钮助手内部调用宿主 back 入口；本测试不链接面板栈实现。
namespace qol::features::core_ui {
void ui_panels_request_back() {}
int ui_panels_depth() { return 1; }
}  // namespace qol::features::core_ui

namespace {

struct FakeControl {
    uint32_t kind = 0;
    FakeControl* parent = nullptr;
    qol::contracts::UiRect rect{};
    char text[64] = {};
    qol::contracts::QolUiClickFn click = nullptr;
    void* click_user = nullptr;
    qol::contracts::QolUiDrawFn draw = nullptr;
    bool enabled = true;
};

int g_clicks = 0;
int g_changes = 0;
int g_scrolls = 0;
int32_t g_last_value = -1;
int32_t g_last_offset = -1;
qol::contracts::UiRect g_last_container{};
int g_container_draws = 0;
int g_part_draws = 0;
int32_t g_part_x[4]{};
int32_t g_part_y[4]{};
int32_t g_part_loc[4]{};
int g_fake_group = 0;
int g_fake_parts[4]{};
int g_fake_main_thread = 1;
int g_native_button_draws = 0;
int g_native_button_deletes = 0;
qol::contracts::UiRect g_native_button_rect{};
int g_native_label_draws = 0;
qol::contracts::UiRect g_native_label_rect{};
std::string g_native_label_text;
int32_t g_native_label_font = -1;
qol::contracts::QolUiHandle g_main_menu_button = nullptr;
qol::contracts::QolUiHandle g_main_menu_replacement = nullptr;
void* g_main_menu_click_owner = nullptr;
bool g_main_menu_parent_is_current = true;
bool g_main_menu_identity_known = true;
bool g_main_menu_invalidation_allowed = true;
bool g_fail_next_rect = false;
bool g_fail_next_text = false;
bool g_fail_next_draw_proc = false;
bool g_fail_next_click_proc = false;
constexpr int32_t kPartWidths[] = {39, 43, 41, 45};
constexpr int32_t kPartHeights[] = {40, 42, 44, 46};

int32_t is_main_thread() { return g_fake_main_thread; }

void on_click(void* user) {
    assert(user != nullptr);
    ++g_clicks;
}

void on_changed(void*, int32_t value) {
    ++g_changes;
    g_last_value = value;
}

void on_scroll(void*, int32_t offset) {
    ++g_scrolls;
    g_last_offset = offset;
}

QolResult ok() { return qol::contracts::ok(); }

QolResult create_control(uint32_t kind, qol::contracts::QolUiHandle parent,
                         qol::contracts::QolUiHandle* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    auto* control = new FakeControl();
    control->kind = kind;
    control->parent = static_cast<FakeControl*>(parent);
    *out = control;
    return ok();
}

QolResult set_rect(qol::contracts::QolUiHandle handle, qol::contracts::UiRect rect) {
    if (handle == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    if (g_fail_next_rect) {
        g_fail_next_rect = false;
        return qol::contracts::error(kQolErrNotReady, "rect failure", 13);
    }
    static_cast<FakeControl*>(handle)->rect = rect;
    return ok();
}

QolResult set_text(qol::contracts::QolUiHandle handle, const char* text) {
    if (handle == nullptr || text == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    if (g_fail_next_text) {
        g_fail_next_text = false;
        return qol::contracts::error(kQolErrNotReady, "text failure", 13);
    }
    std::snprintf(static_cast<FakeControl*>(handle)->text,
                  sizeof(static_cast<FakeControl*>(handle)->text), "%s", text);
    return ok();
}

QolResult set_draw_proc(qol::contracts::QolUiHandle handle, qol::contracts::QolUiDrawFn draw) {
    if (handle == nullptr || draw == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    if (g_fail_next_draw_proc) {
        g_fail_next_draw_proc = false;
        return qol::contracts::error(kQolErrNotReady, "draw failure", 13);
    }
    static_cast<FakeControl*>(handle)->draw = draw;
    return ok();
}

QolResult set_click_proc(qol::contracts::QolUiHandle handle, qol::contracts::QolUiClickFn click,
                         void* user) {
    if (handle == nullptr || click == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    if (g_fail_next_click_proc) {
        g_fail_next_click_proc = false;
        return qol::contracts::error(kQolErrNotReady, "click failure", 13);
    }
    auto* control = static_cast<FakeControl*>(handle);
    control->click = click;
    control->click_user = user;
    if (g_main_menu_button != nullptr && control->parent ==
        static_cast<FakeControl*>(g_main_menu_button)->parent) g_main_menu_click_owner = user;
    return ok();
}

QolResult set_button_enabled(qol::contracts::QolUiHandle handle, uint8_t enabled) {
    if (handle == nullptr || enabled > 1) return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    static_cast<FakeControl*>(handle)->enabled = enabled != 0;
    return ok();
}

QolResult draw_native_text_button_background(qol::contracts::UiRect rect, uint8_t pressed) {
    if (pressed > 1) return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    ++g_native_button_draws;
    g_native_button_rect = rect;
    return ok();
}

QolResult draw_native_text_button_label(qol::contracts::UiRect rect, const char* text,
                                        uint32_t, int32_t font) {
    if (text == nullptr || (font != 0 && font != 1)) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    ++g_native_label_draws;
    g_native_label_rect = rect;
    g_native_label_text = text;
    g_native_label_font = font;
    return ok();
}

QolResult delete_control(qol::contracts::QolUiHandle handle) {
    if (handle == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    ++g_native_button_deletes;
    if (handle == g_main_menu_replacement) {
        g_main_menu_replacement = nullptr;
        g_main_menu_click_owner = nullptr;
    }
    return ok();
}

QolResult control_tree_event(qol::contracts::QolUiHandle handle, uint64_t event, uint64_t param,
                             uint64_t, uint32_t* consumed) {
    if (handle == nullptr || param == 0 || consumed == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    const auto* point = reinterpret_cast<const int64_t*>(static_cast<uintptr_t>(param));
    auto* control = static_cast<FakeControl*>(handle);
    int64_t x = 0;
    int64_t y = 0;
    for (const FakeControl* current = control; current != nullptr; current = current->parent) {
        x += current->rect.x;
        y += current->rect.y;
    }
    const bool inside = point[0] >= x && point[0] < x + control->rect.w &&
        point[1] >= y && point[1] < y + control->rect.h;
    if (event == 0x17) {
        control->enabled = inside;
        *consumed = inside ? 1u : 0u;
    } else if (event == 0x18) {
        if (control->enabled && inside && control->click != nullptr) {
            control->click(control->click_user);
        }
        control->enabled = false;
        *consumed = inside ? 1u : 0u;
    }
    return ok();
}

int32_t hit_test(const qol::contracts::QolUiHandle handle, int64_t x, int64_t y,
                 qol::contracts::UiRect rect) {
    if (handle == nullptr) return -kQolErrInvalidArgument;
    const auto* control = static_cast<const FakeControl*>(handle);
    int64_t absolute_x = 0;
    int64_t absolute_y = 0;
    for (const FakeControl* current = control; current != nullptr; current = current->parent) {
        absolute_x += current->rect.x;
        absolute_y += current->rect.y;
    }
    return x >= absolute_x + rect.x && x < absolute_x + rect.x + rect.w &&
        y >= absolute_y + rect.y && y < absolute_y + rect.y + rect.h;
}

QolResult draw_control(qol::contracts::QolUiHandle) { return ok(); }

QolResult get_rect(qol::contracts::QolUiHandle handle, qol::contracts::UiRect* out) {
    if (handle == nullptr || out == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    *out = static_cast<FakeControl*>(handle)->rect;
    return ok();
}

QolResult main_menu_more_games_snapshot(qol::contracts::QolUiHandle* out_button,
                                        qol::contracts::QolUiHandle* out_parent,
                                        qol::contracts::UiRect* out_rect) {
    if (out_button == nullptr || out_parent == nullptr || out_rect == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    *out_button = nullptr;
    *out_parent = nullptr;
    if (!g_main_menu_identity_known || g_main_menu_button == nullptr) {
        return qol::contracts::error(kQolErrNotReady, "unknown", 7);
    }
    auto* button = static_cast<FakeControl*>(g_main_menu_button);
    *out_button = button;
    *out_parent = button->parent;
    *out_rect = button->rect;
    return ok();
}

QolResult main_menu_text_button_is_live(qol::contracts::QolUiHandle original,
                                        qol::contracts::QolUiHandle child,
                                        qol::contracts::QolUiClickFn expected_execute,
                                        void* owner, int32_t* out_live) {
    if (original == nullptr || child == nullptr || expected_execute == nullptr ||
        owner == nullptr || out_live == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    if (!g_main_menu_identity_known) return qol::contracts::error(kQolErrNotReady, "unknown", 7);
    *out_live = g_main_menu_parent_is_current && original == g_main_menu_button &&
        child == g_main_menu_replacement && owner == g_main_menu_click_owner ? 1 : 0;
    return ok();
}

QolResult invalidate_main_menu_binding(qol::contracts::QolUiHandle child, void* owner) {
    if (child == nullptr || owner == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    if (!g_main_menu_identity_known || !g_main_menu_invalidation_allowed) {
        return qol::contracts::error(kQolErrNotReady, "unknown", 7);
    }
    if (owner == g_main_menu_click_owner) g_main_menu_click_owner = nullptr;
    return ok();
}

int g_back_draws = 0;
QolResult draw_back_button(qol::contracts::QolUiHandle handle, uint8_t) {
    if (handle == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    ++g_back_draws;
    return ok();
}

// 文本原语夹具：记录 draw_control_text 解析出的绝对坐标与颜色。
uint32_t g_last_text_rgb = 0;
std::string g_last_text;
int32_t g_last_text_x = -1;
int32_t g_last_text_y = -1;

QolResult set_font_color_rgb(int32_t red, int32_t green, int32_t blue) {
    g_last_text_rgb = static_cast<uint32_t>(red & 0xff) |
        (static_cast<uint32_t>(green & 0xff) << 8) | (static_cast<uint32_t>(blue & 0xff) << 16);
    return ok();
}

QolResult draw_string_with_font(const char* text, int32_t x, int32_t y, int32_t, int32_t) {
    if (text == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    g_last_text = text;
    g_last_text_x = x;
    g_last_text_y = y;
    return ok();
}

QolResult control_absolute_origin(qol::contracts::QolUiHandle handle, int64_t* out_x,
                                  int64_t* out_y) {
    if (handle == nullptr || out_x == nullptr || out_y == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    int64_t x = 0;
    int64_t y = 0;
    for (const FakeControl* current = static_cast<const FakeControl*>(handle); current != nullptr;
         current = current->parent) {
        x += current->rect.x;
        y += current->rect.y;
    }
    *out_x = x;
    *out_y = y;
    return ok();
}

QolResult control_pressed(qol::contracts::QolUiHandle handle, int32_t* out_pressed) {
    if (handle == nullptr || out_pressed == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    *out_pressed = 0;
    return ok();
}

int g_option_loads = 0;
QolResult load_option_images() {
    ++g_option_loads;
    return ok();
}

qol::contracts::UiRect g_menu_guard_rect{};
bool g_menu_available = true, g_pressed = false;
qol::contracts::UiTextButtonPaintDesc g_paint{};
uint8_t g_paint_pressed = 0;
QolResult paint_layout(const qol::contracts::UiTextButtonPaintDesc* desc,
                        qol::contracts::UiTextButtonPaintLayout* out) {
    if (desc->font_size == 30) return qol::contracts::error(kQolErrUnsupported, "font", 4);
    auto r = desc->visual_rect;
    if (r.w == 300 && r.h == 65) r = {r.x + 14, r.y, 272, 64};
    *out = {r, {r.x - 2, r.y - 2, r.w + 4, r.h + 4}, r}; return ok();
}
QolResult paint_draw(const qol::contracts::UiTextButtonPaintDesc* desc, const char*, uint32_t,
                     uint8_t pressed) { g_paint = *desc; g_paint_pressed = pressed; return ok(); }
QolResult menu_rect_available(qol::contracts::QolUiHandle, qol::contracts::UiRect rect,
                               uint8_t* out) {
    g_menu_guard_rect = rect;
    *out = g_menu_available;
    return ok();
}
int g_fill_count = 0;
QolResult fill_rect(qol::contracts::UiRect, uint32_t) {
    ++g_fill_count;
    return ok();
}

QolResult draw_border_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    g_last_container = {x, y, w, h};
    ++g_container_draws;
    return ok();
}

QolResult image_group(int32_t unit, qol::contracts::QolUiHandle* out) {
    if (unit != 0x0f || out == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    *out = &g_fake_group;
    return ok();
}

QolResult image_loc(int32_t unit, int32_t loc, qol::contracts::QolUiHandle* out) {
    if (unit != 0x0f || out == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    constexpr int32_t locations[] = {0x30, 0x31, 0x35, 0x36};
    for (int i = 0; i < 4; ++i) {
        if (loc == locations[i]) {
            *out = &g_fake_parts[i];
            return ok();
        }
    }
    return qol::contracts::error(kQolErrNotFound, "missing", 7);
}

QolResult draw_part(qol::contracts::QolUiHandle group, int32_t x, int32_t y,
                    qol::contracts::QolUiHandle part, int32_t type, int32_t flip) {
    assert(group == &g_fake_group && type == 0 && flip == 1 && g_part_draws < 4);
    g_part_x[g_part_draws] = x;
    g_part_y[g_part_draws] = y;
    for (int i = 0; i < 4; ++i) {
        if (part == &g_fake_parts[i]) g_part_loc[g_part_draws] = i;
    }
    ++g_part_draws;
    return ok();
}

QolResult image_loc_size(int32_t unit, int32_t loc, int32_t* out_width, int32_t* out_height) {
    if (unit != 0x0f || out_width == nullptr || out_height == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    constexpr int32_t locations[] = {0x30, 0x31, 0x35, 0x36};
    for (int i = 0; i < 4; ++i) {
        if (loc == locations[i]) {
            *out_width = kPartWidths[i];
            *out_height = kPartHeights[i];
            return ok();
        }
    }
    return qol::contracts::error(kQolErrNotFound, "missing", 7);
}

qol::contracts::QolUiAdapter make_fake_adapter() {
    qol::contracts::QolUiAdapter adapter{};
    adapter.size = sizeof(adapter);
    adapter.major = 1;
    adapter.minor = qol::contracts::kUiContractMinor;
    adapter.create_control = &create_control;
    adapter.set_rect = &set_rect;
    adapter.set_text = &set_text;
    adapter.set_draw_proc = &set_draw_proc;
    adapter.set_click_proc = &set_click_proc;
    adapter.hit_test = &hit_test;
    adapter.draw_control = &draw_control;
    adapter.get_rect = &get_rect;
    adapter.draw_back_button = &draw_back_button;
    adapter.load_option_images = &load_option_images;
    adapter.draw_border_rect = &draw_border_rect;
    adapter.fill_rect = &fill_rect;
    adapter.image_group = &image_group;
    adapter.image_loc = &image_loc;
    adapter.draw_part = &draw_part;
    adapter.image_loc_size = &image_loc_size;
    adapter.set_font_color_rgb = &set_font_color_rgb;
    adapter.draw_string_with_font = &draw_string_with_font;
    adapter.control_absolute_origin = &control_absolute_origin;
    adapter.control_pressed = &control_pressed;
    adapter.is_main_thread = &is_main_thread;
    adapter.minor = qol::contracts::kUiAdapterMinor;
    adapter.set_button_enabled = &set_button_enabled;
    adapter.main_menu_more_games_snapshot = &main_menu_more_games_snapshot;
    adapter.main_menu_text_button_is_live = &main_menu_text_button_is_live;
    adapter.main_menu_text_button_invalidate_binding = &invalidate_main_menu_binding;
    adapter.draw_native_text_button_background = &draw_native_text_button_background;
    adapter.draw_native_text_button_label = &draw_native_text_button_label;
    adapter.delete_control = &delete_control;
    adapter.control_tree_event = &control_tree_event;
    adapter.layout_native_text_button = &paint_layout;
    adapter.draw_native_text_button = &paint_draw;
    adapter.main_menu_replacement_rect_available = &menu_rect_available;
    return adapter;
}

const qol::contracts::QolUiAdapter kFakeAdapter = make_fake_adapter();

}  // namespace

int main() {
    int assertions = 0;
#define CHECK(condition) \
    do {                    \
        assert(condition);  \
        ++assertions;       \
    } while (false)

    CHECK(qol::features::core_ui::ui_components_bind(&kFakeAdapter));
    const auto* components = qol::features::core_ui::ui_components_api();
    CHECK(components != nullptr);
    CHECK(components->size == sizeof(qol::contracts::QolUiComponents));
    CHECK(components->panel_create != nullptr && components->scroll_view_create != nullptr);

    qol::contracts::UiPanelDesc panel_desc{{100, 50, 100, 100}, {0, 0, 100, 100}, 0, 0};
    qol::contracts::QolUiHandle root = nullptr;
    CHECK(components->panel_create(&panel_desc, nullptr, &root).ok == kQolOk);

    char button_text[] = "button";
    qol::contracts::UiButtonDesc button_desc{{2, 3, 20, 10}, button_text, 7, &on_click, root};
    qol::contracts::QolUiHandle button = nullptr;
    const int deletes_before_text_failure = g_native_button_deletes;
    qol::contracts::UiButtonDesc failed_text_desc{{2, 3, 20, 10}, "failed", 7, &on_click, root};
    qol::contracts::QolUiHandle failed_text_button = nullptr;
    g_fail_next_text = true;
    CHECK(components->button_create(&failed_text_desc, root, &failed_text_button).code ==
          kQolErrNotReady);
    CHECK(failed_text_button == nullptr &&
          g_native_button_deletes == deletes_before_text_failure + 1);
    CHECK(components->button_create(&button_desc, root, &button).ok == kQolOk);
    button_text[0] = 'X';
    CHECK(std::strcmp(static_cast<FakeControl*>(button)->text, "button") == 0);

    qol::contracts::UiLabelDesc label_desc{{40, 3, 20, 10}, "label", 0, qol::contracts::UiAlign::kLeft};
    qol::contracts::QolUiHandle label = nullptr;
    CHECK(components->label_create(&label_desc, root, &label).ok == kQolOk);

    qol::contracts::UiToggleDesc toggle_desc{{2, 30, 20, 10}, "toggle", 0, 0, &on_changed, nullptr};
    qol::contracts::QolUiHandle toggle = nullptr;
    CHECK(components->toggle_create(&toggle_desc, root, &toggle).ok == kQolOk);

    const char* options[] = {"one", "two"};
    qol::contracts::UiSelectDesc select_desc{{40, 30, 20, 10}, options, 2, 0, 0, &on_changed, nullptr};
    qol::contracts::QolUiHandle select = nullptr;
    CHECK(components->select_create(&select_desc, root, &select).ok == kQolOk);

    qol::contracts::UiScrollViewDesc scroll_desc{{70, 30, 20, 10}, 100, 10, 20, 0, &on_scroll, nullptr};
    qol::contracts::QolUiHandle scroll = nullptr;
    CHECK(components->scroll_view_create(&scroll_desc, root, &scroll).ok == kQolOk);

    CHECK(components->button_set_text(button, "updated").ok == kQolOk);
    CHECK(std::strcmp(static_cast<FakeControl*>(button)->text, "updated") == 0);
    CHECK(components->label_set_text(label, "updated label").ok == kQolOk);
    CHECK(components->set_rect(button, {4, 5, 20, 10}).ok == kQolOk);
    CHECK(components->toggle_set_value(toggle, 1).ok == kQolOk);
    CHECK(components->select_set_selected(select, 1).ok == kQolOk);
    CHECK(components->scroll_set_offset(scroll, 50).ok == kQolOk);
    CHECK(components->select_set_selected(select, 2).code == kQolErrInvalidArgument);
    CHECK(components->scroll_set_offset(scroll, 81).code == kQolErrInvalidArgument);

    // draw_control_text：按控件绝对原点绘制传入字符串，不触发控件文本写入。
    CHECK(components->draw_control_text(button, "hello", 4, 6, 0x00112233u, 0, 1).ok == kQolOk);
    CHECK(g_last_text == "hello" && g_last_text_x == 108 && g_last_text_y == 61);
    CHECK(g_last_text_rgb == 0x00112233u);
    CHECK(std::strcmp(static_cast<FakeControl*>(button)->text, "updated") == 0);
    CHECK(components->draw_control_text(nullptr, "x", 0, 0, 0, 0, 1).code == kQolErrInvalidArgument);
    CHECK(components->draw_control_text(button, nullptr, 0, 0, 0, 0, 1).code == kQolErrInvalidArgument);

    CHECK(components->dispatch_touch(root, 0, 105, 58).ok == kQolOk);
    CHECK(g_clicks == 0);
    CHECK(components->dispatch_touch(root, 2, 105, 58).ok == kQolOk);
    CHECK(g_clicks == 0);
    CHECK(components->dispatch_touch(root, 1, 105, 58).ok == kQolOk);
    CHECK(g_clicks == 1);
    CHECK(components->dispatch_touch(root, 1, 103, 83).ok == kQolOk);
    CHECK(g_changes == 1 && g_last_value == 0);
    CHECK(components->dispatch_touch(root, 1, 141, 83).ok == kQolOk);
    CHECK(g_changes == 2 && g_last_value == 0);
    CHECK(components->dispatch_touch(root, 1, 171, 83).ok == kQolOk);
    CHECK(g_scrolls == 1 && g_last_offset == 50);
    CHECK(components->dispatch_touch(root, 1, 300, 300).ok == kQolOk);

    CHECK(components->destroy(button).ok == kQolOk);
    CHECK(components->destroy(button).code == kQolErrNotFound);
    CHECK(components->destroy(nullptr).code == kQolErrInvalidArgument);
    CHECK(components->draw_container({10, 20, 320, 140}).ok == kQolOk);
    CHECK(g_container_draws == 1 && g_last_container.x == 10 && g_last_container.y == 20 &&
          g_last_container.w == 320 && g_last_container.h == 140);
    CHECK(components->draw_container({0, 0, 47, 100}).code == kQolErrInvalidArgument);
    CHECK(components->draw_container({INT64_MAX, 0, 100, 100}).code == kQolErrInvalidArgument);
    CHECK(components->draw_container({INT32_MAX, 0, 48, 100}).code == kQolErrInvalidArgument);
    CHECK(components->draw_container_ornamented({10, 20, 320, 140}).ok == kQolOk);
    CHECK(g_container_draws == 2 && g_last_container.x == 10 && g_last_container.y == 20 &&
          g_last_container.w == 320 && g_last_container.h == 140);
    CHECK(g_part_draws == 4);
    CHECK(g_part_loc[0] == 0 && g_part_x[0] == 8 && g_part_y[0] == 18);
    CHECK(g_part_loc[1] == 1 && g_part_x[1] == 287 && g_part_y[1] == 18);
    CHECK(g_part_loc[2] == 2 && g_part_x[2] == 8 && g_part_y[2] == 116);
    CHECK(g_part_loc[3] == 3 && g_part_x[3] == 285 && g_part_y[3] == 114);
    CHECK(components->draw_container_ornamented({INT32_MIN, 0, 320, 140}).code ==
          kQolErrInvalidArgument);

    // 返回按钮助手：组件树按钮（手工命中，按下→抬起在按钮内请求宿主 back）与原生事件树按钮。
    qol::contracts::QolUiHandle back = nullptr;
    CHECK(components->back_button_create(root, {8, 8, 74, 81}, &back).ok == kQolOk);
    CHECK(back != nullptr && g_option_loads >= 1);  // 助手借用 0x59 图集（不卸载）。
    CHECK(components->back_button_touch(back, 1, 110, 60).ok == kQolOk);
    CHECK(components->back_button_touch(back, 2, 110, 60).ok == kQolOk);
    CHECK(components->back_button_draw(back).ok == kQolOk && g_back_draws == 1);
    qol::contracts::QolUiHandle raw_parent = nullptr;
    CHECK(create_control(0, nullptr, &raw_parent).ok == kQolOk);
    qol::contracts::QolUiHandle raw_back = nullptr;
    CHECK(components->back_button_create(raw_parent, {0, 0, 74, 81}, &raw_back).ok == kQolOk);
    CHECK(raw_back != nullptr);
    CHECK(components->back_button_touch(raw_back, 1, 0, 0).code == kQolErrUnsupported);
    CHECK(components->back_button_draw(raw_back).ok == kQolOk && g_back_draws == 2);
    CHECK(components->back_request().ok == kQolOk);
    CHECK(components->back_button_touch(nullptr, 1, 0, 0).code == kQolErrInvalidArgument);
    CHECK(components->back_button_draw(nullptr).code == kQolErrInvalidArgument);

    // 可缩放开关：任意 rect 自绘（关/开）+ 仅按自身矩形命中的触摸翻转。
    CHECK(components->switch_draw({400, 300, 96, 44}, 0).ok == kQolOk);
    CHECK(components->switch_draw({400, 300, 96, 44}, 1).ok == kQolOk);
    CHECK(components->switch_draw({400, 300, 150, 68}, 1).ok == kQolOk);
    CHECK(g_fill_count > 0);
    CHECK(components->switch_draw({0, 0, 0, 44}, 0).code == kQolErrInvalidArgument);
    CHECK(components->switch_draw({0, 0, 96, -1}, 0).code == kQolErrInvalidArgument);
    int32_t switch_value = 0;
    CHECK(components->switch_touch({400, 300, 96, 44}, 2, 448, 322, &switch_value).ok == kQolOk);
    CHECK(switch_value == 0);  // 抬起不切换
    CHECK(components->switch_touch({400, 300, 96, 44}, 1, 448, 322, &switch_value).ok == kQolOk);
    CHECK(switch_value == 1);
    CHECK(components->switch_touch({400, 300, 96, 44}, 1, 448, 322, &switch_value).ok == kQolOk);
    CHECK(switch_value == 0);
    CHECK(components->switch_touch({400, 300, 96, 44}, 1, 399, 322, &switch_value).ok == kQolOk);
    CHECK(switch_value == 0);  // 命中范围外不切换
    CHECK(components->switch_touch({400, 300, 96, 44}, 1, 448, 322, nullptr).code ==
          kQolErrInvalidArgument);
    CHECK(components->switch_touch({0, 0, 0, 44}, 1, 0, 0, &switch_value).code ==
          kQolErrInvalidArgument);
    g_fake_main_thread = 0;
    CHECK(components->switch_draw({400, 300, 96, 44}, 0).code == kQolErrNotReady);
    CHECK(components->switch_touch({400, 300, 96, 44}, 1, 448, 322, &switch_value).code ==
          kQolErrNotReady);
    CHECK(switch_value == 0);
    g_fake_main_thread = 1;

    CHECK(components->destroy(root).ok == kQolOk);
    CHECK(components->destroy(root).code == kQolErrNotFound);
    qol::features::core_ui::ui_components_unbind();

    CHECK(qol::features::core_ui::ui_components_bind(&kFakeAdapter));
    qol::contracts::QolUiHandle text_root = nullptr;
    CHECK(components->panel_create(&panel_desc, nullptr, &text_root).ok == kQolOk);
    std::string long_text = "长文本 UTF-8 label longer than thirty-one bytes";
    qol::contracts::UiButtonDesc text_desc{{10, 12, 220, 48}, long_text.c_str(), 0,
                                            &on_click, &g_clicks};
    qol::contracts::QolUiHandle text_button = nullptr;
    const int deletes_before_rect_failure = g_native_button_deletes;
    g_fail_next_rect = true;
    CHECK(components->text_button_create(&text_desc, text_root, &text_button).code == kQolErrNotReady);
    CHECK(text_button == nullptr && g_native_button_deletes == deletes_before_rect_failure + 1);
    CHECK(components->text_button_create(nullptr, text_root, &text_button).code ==
          kQolErrInvalidArgument);
    CHECK(text_button == nullptr);
    qol::contracts::UiButtonDesc undersized_desc{{10, 12, 116, 48}, "small", 0,
                                                  &on_click, &g_clicks};
    CHECK(components->text_button_create(&undersized_desc, text_root, &text_button).code ==
          kQolErrInvalidArgument);
    CHECK(text_button == nullptr);
    CHECK(components->text_button_create(&text_desc, text_root, &text_button).ok == kQolOk);
    long_text.assign("temporary replacement");
    CHECK(components->text_button_draw(text_button).ok == kQolOk);
    CHECK(g_native_button_draws == 1 && g_native_button_rect.x == 110 &&
          g_native_button_rect.y == 62 && g_native_button_rect.w == 220);
    CHECK(g_native_label_draws == 1 && g_native_label_rect.x == 110 &&
          g_native_label_rect.y == 62 && g_native_label_text == "长文本 UTF-8 label longer than thirty-one bytes");
    CHECK(components->button_set_text(text_button, "更新后的按钮文字").ok == kQolOk);
    CHECK(components->text_button_draw(text_button).ok == kQolOk);
    CHECK(g_native_label_text == "更新后的按钮文字");
    CHECK(components->button_set_text(text_button, nullptr).code == kQolErrInvalidArgument);
    const char malformed_utf8[] = "\xf0\x28\x8c\x28";
    CHECK(components->button_set_text(text_button, malformed_utf8).code == kQolErrInvalidArgument);
    CHECK(components->text_button_draw(text_button).ok == kQolOk);
    CHECK(g_native_label_text == "更新后的按钮文字");
    CHECK(g_native_label_font == 0);
    qol::contracts::UiButtonDesc larger_desc{{10, 12, 220, 48}, "放大",
                                             qol::contracts::kUiTextButtonFontLarger, &on_click,
                                             &g_clicks};
    qol::contracts::QolUiHandle larger_button = nullptr;
    CHECK(components->text_button_create(&larger_desc, text_root, &larger_button).ok == kQolOk);
    CHECK(components->text_button_draw(larger_button).ok == kQolOk);
    CHECK(g_native_label_font == 1);
    CHECK(components->destroy(larger_button).ok == kQolOk);
    qol::contracts::QolUiHandle bad_button = nullptr;
    qol::contracts::UiButtonDesc bad_style_desc{{10, 12, 220, 48}, "bad", 2, &on_click, &g_clicks};
    CHECK(components->text_button_create(&bad_style_desc, text_root, &bad_button).code ==
          kQolErrInvalidArgument);
    CHECK(bad_button == nullptr);
    CHECK(components->button_set_text(text_button, "更新后的按钮文字").ok == kQolOk);
    CHECK(components->text_button_set_enabled(text_button, 0).ok == kQolOk);
    auto* fake_text_button = static_cast<FakeControl*>(text_button);
    fake_text_button->click(fake_text_button->click_user);
    CHECK(g_clicks == 1);
    CHECK(components->text_button_set_enabled(text_button, 1).ok == kQolOk);
    fake_text_button->click(fake_text_button->click_user);
    CHECK(g_clicks == 2);
    int64_t touch_inside[] = {120, 80};
    int64_t touch_outside[] = {500, 500};
    uint32_t consumed = 0;
    CHECK(components->dispatch_native_event(text_button, 0x17,
        reinterpret_cast<uintptr_t>(touch_inside), 0, &consumed).ok == kQolOk && consumed == 1);
    CHECK(components->dispatch_native_event(text_button, 0x18,
        reinterpret_cast<uintptr_t>(touch_inside), 0, &consumed).ok == kQolOk && consumed == 1);
    CHECK(g_clicks == 3);
    CHECK(components->dispatch_native_event(text_button, 0x17,
        reinterpret_cast<uintptr_t>(touch_inside), 0, &consumed).ok == kQolOk);
    CHECK(components->dispatch_native_event(text_button, 0x18,
        reinterpret_cast<uintptr_t>(touch_outside), 0, &consumed).ok == kQolOk);
    CHECK(g_clicks == 3);
    CHECK(components->dispatch_native_event(text_button, 0x17,
        reinterpret_cast<uintptr_t>(touch_outside), 0, &consumed).ok == kQolOk);
    CHECK(components->dispatch_native_event(text_button, 0x18,
        reinterpret_cast<uintptr_t>(touch_inside), 0, &consumed).ok == kQolOk);
    CHECK(g_clicks == 3);
    CHECK(components->destroy(text_button).ok == kQolOk);
    CHECK(g_native_button_deletes == 4);
    CHECK(components->destroy(text_button).code == kQolErrNotFound);
    CHECK(components->destroy(text_root).ok == kQolOk);
    qol::features::core_ui::ui_components_unbind();

    CHECK(qol::features::core_ui::ui_components_bind(&kFakeAdapter));
    FakeControl menu_parent{};
    menu_parent.rect = {0, 0, 960, 640};
    FakeControl menu_button{};
    menu_button.parent = &menu_parent;
    menu_button.rect = {612, 488, 220, 52};
    g_main_menu_button = &menu_button;
    g_main_menu_parent_is_current = true;
    g_main_menu_identity_known = true;
    g_main_menu_invalidation_allowed = true;
    const qol::contracts::UiMainMenuTextButtonDesc menu_desc{
        "模块设置", qol::contracts::kUiTextButtonFontDefault, &on_click, &g_clicks};
    qol::contracts::QolUiHandle original_menu_button = nullptr;
    qol::contracts::QolUiHandle replacement_menu_button = nullptr;
    const int deletes_before_draw_failure = g_native_button_deletes;
    g_fail_next_draw_proc = true;
    CHECK(components->main_menu_text_button_create(&menu_desc, &original_menu_button,
        &replacement_menu_button).code == kQolErrNotReady);
    CHECK(original_menu_button == nullptr && replacement_menu_button == nullptr &&
          g_native_button_deletes == deletes_before_draw_failure + 1);
    const int deletes_before_click_failure = g_native_button_deletes;
    g_fail_next_click_proc = true;
    CHECK(components->main_menu_text_button_create(&menu_desc, &original_menu_button,
        &replacement_menu_button).code == kQolErrNotReady);
    CHECK(original_menu_button == nullptr && replacement_menu_button == nullptr &&
          g_native_button_deletes == deletes_before_click_failure + 1);
    CHECK(components->main_menu_text_button_create(&menu_desc, &original_menu_button,
        &replacement_menu_button).ok == kQolOk);
    CHECK(original_menu_button == &menu_button && replacement_menu_button != nullptr);
    g_main_menu_replacement = replacement_menu_button;
    auto* replacement_control = static_cast<FakeControl*>(replacement_menu_button);
    CHECK(replacement_control->parent == &menu_parent);
    CHECK(replacement_control->rect.x == menu_button.rect.x &&
          replacement_control->rect.y == menu_button.rect.y &&
          replacement_control->rect.w == menu_button.rect.w &&
          replacement_control->rect.h == menu_button.rect.h);
    CHECK(replacement_control->draw != nullptr && replacement_control->click != nullptr);
    uint8_t replacement_live = 0;
    CHECK(components->main_menu_text_button_is_live(replacement_menu_button,
        &replacement_live).ok == kQolOk && replacement_live == 1);
    replacement_control->draw(replacement_control);
    CHECK(g_native_button_draws == 5);
    CHECK(g_native_label_text == "模块设置");
    CHECK(g_native_button_rect.w == replacement_control->rect.w);
    CHECK(g_native_button_rect.h == replacement_control->rect.w * 48 / 220);
    CHECK(g_native_label_rect.x == g_native_button_rect.x &&
          g_native_label_rect.y == g_native_button_rect.y &&
          g_native_label_rect.h == g_native_button_rect.h);
    CHECK(replacement_control->rect.h == menu_button.rect.h);
    replacement_control->click(replacement_control->click_user);
    CHECK(g_clicks == 4);

    const int deletions_before_unknown_state = g_native_button_deletes;
    g_main_menu_identity_known = false;
    CHECK(components->main_menu_text_button_is_live(replacement_menu_button,
        &replacement_live).code == kQolErrNotReady);
    CHECK(components->destroy(replacement_menu_button).code == kQolErrNotReady);
    CHECK(g_native_button_deletes == deletions_before_unknown_state);
    CHECK(g_main_menu_click_owner == replacement_control->click_user);
    replacement_control->click(replacement_control->click_user);
    CHECK(g_clicks == 5);

    int unrelated_owner = 0;
    g_main_menu_identity_known = true;
    g_main_menu_click_owner = &unrelated_owner;
    CHECK(components->destroy(replacement_menu_button).ok == kQolOk);
    CHECK(g_native_button_deletes == deletions_before_unknown_state);
    CHECK(g_main_menu_click_owner == &unrelated_owner);

    g_main_menu_click_owner = nullptr;
    qol::contracts::QolUiHandle second_original = nullptr;
    qol::contracts::QolUiHandle second_replacement = nullptr;
    CHECK(components->main_menu_text_button_create(&menu_desc, &second_original,
        &second_replacement).ok == kQolOk);
    CHECK(second_original == &menu_button && second_replacement != nullptr);
    g_main_menu_replacement = second_replacement;
    auto* second_control = static_cast<FakeControl*>(second_replacement);
    CHECK(g_main_menu_click_owner == second_control->click_user);
    g_main_menu_replacement = nullptr;
    const int deletions_before_parent_rebuild = g_native_button_deletes;
    CHECK(components->destroy(second_replacement).ok == kQolOk);
    CHECK(g_native_button_deletes == deletions_before_parent_rebuild);
    CHECK(g_main_menu_click_owner == nullptr);

    qol::contracts::QolUiHandle third_original = nullptr;
    qol::contracts::QolUiHandle third_replacement = nullptr;
    CHECK(components->main_menu_text_button_create(&menu_desc, &third_original,
        &third_replacement).ok == kQolOk);
    CHECK(third_original == &menu_button && third_replacement != nullptr);
    g_main_menu_replacement = third_replacement;
    auto* third_control = static_cast<FakeControl*>(third_replacement);
    g_main_menu_identity_known = false;
    g_main_menu_invalidation_allowed = false;
    const int deletions_before_unbind_retry = g_native_button_deletes;
    qol::features::core_ui::ui_components_unbind();
    CHECK(!qol::features::core_ui::ui_components_bind(&kFakeAdapter));
    CHECK(g_native_button_deletes == deletions_before_unbind_retry);
    CHECK(g_main_menu_click_owner == third_control->click_user);
    third_control->click(third_control->click_user);
    CHECK(g_clicks == 6);
    g_main_menu_identity_known = true;
    g_main_menu_invalidation_allowed = true;
    CHECK(qol::features::core_ui::ui_components_bind(&kFakeAdapter));
    CHECK(g_native_button_deletes == deletions_before_unbind_retry + 1);
    CHECK(g_main_menu_click_owner == nullptr);
    g_main_menu_button = nullptr;
    qol::features::core_ui::ui_components_unbind();

    auto older_adapter = make_fake_adapter();
    older_adapter.minor = qol::contracts::kUiAdapterMinor - 2;
    older_adapter.size = static_cast<uint32_t>(offsetof(
        qol::contracts::QolUiAdapter, draw_native_text_button_background));
    CHECK(qol::features::core_ui::ui_components_bind(&older_adapter));
    qol::contracts::QolUiHandle old_root = nullptr;
    CHECK(components->panel_create(&panel_desc, nullptr, &old_root).ok == kQolOk);
    qol::contracts::QolUiHandle unavailable_button = nullptr;
    CHECK(components->text_button_create(&text_desc, old_root, &unavailable_button).code ==
          kQolErrNotReady);
    CHECK(unavailable_button == nullptr);
    CHECK(components->destroy(old_root).ok == kQolOk);
    qol::features::core_ui::ui_components_unbind();

    CHECK(qol::features::core_ui::ui_components_bind(&kFakeAdapter));
    qol::contracts::QolUiHandle explicit_root = nullptr, explicit_button = nullptr;
    CHECK(components->panel_create(&panel_desc, nullptr, &explicit_root).ok == kQolOk);
    CHECK(components->capsule_text_button_create(&text_desc, explicit_root, &explicit_button).ok == kQolOk);
    CHECK(components->text_button_draw(explicit_button).ok == kQolOk);
    CHECK(g_paint.appearance == qol::contracts::kUiTextButtonCapsule);
    CHECK(components->text_button_set_enabled(explicit_button, 0).ok == kQolOk);
    CHECK(components->text_button_draw(explicit_button).ok == kQolOk && g_paint_pressed == 0);
    CHECK(components->destroy(explicit_button).ok == kQolOk);
    CHECK(components->decorated_text_button_create(&text_desc, explicit_root, &explicit_button).ok == kQolOk);
    CHECK(components->text_button_draw(explicit_button).ok == kQolOk);
    CHECK(g_paint.appearance == qol::contracts::kUiTextButtonDecorated);
    CHECK(components->destroy(explicit_root).ok == kQolOk);
    g_main_menu_button = &menu_button;
    // 实测形状：native snapshot 300×92；可见绘制按请求尺寸 290×72 以原生中心为锚点，
    // 命中区必须等于原生 snapshot rect，而不是可见/按压画布。
    menu_button.rect = {869, 470, 300, 92};
    const qol::contracts::UiMainMenuDecoratedButtonDesc new_menu{"模块设置",0,28,290,72,
        menu_desc.on_click,menu_desc.user};
    qol::contracts::QolUiHandle new_original = nullptr, new_replacement = nullptr;
    g_menu_available = false;
    CHECK(components->main_menu_decorated_button_create(&new_menu, &new_original,
        &new_replacement).code == kQolErrNotReady && !new_replacement && !new_original);
    g_menu_available = true;
    CHECK(components->main_menu_decorated_button_create(&new_menu, &new_original,
        &new_replacement).ok == kQolOk);
    g_main_menu_replacement = new_replacement;
    // 交给 guard 与最终控件 rect 都是原生 snapshot rect 原样，不含 pressed halo 外扩。
    CHECK(g_menu_guard_rect.x == 869 && g_menu_guard_rect.y == 470 &&
          g_menu_guard_rect.w == 300 && g_menu_guard_rect.h == 92);
    CHECK(new_original == &menu_button);
    auto* new_control = static_cast<FakeControl*>(new_replacement);
    CHECK(new_control->rect.x == 869 && new_control->rect.y == 470 &&
          new_control->rect.w == 300 && new_control->rect.h == 92);
    // 绘制范围独立：paint_local = 可见框 − 控件原点；控件原点 = 父原点(0,0)+控件 rect，
    // 故 draw 收到 {874,480,290,72}，等价于 paint_local {5,10,290,72}。
    CHECK(components->text_button_draw(new_replacement).ok == kQolOk);
    CHECK(g_paint.font_size == 28 && g_paint.appearance == qol::contracts::kUiTextButtonDecorated);
    CHECK(g_paint.visual_rect.x == 874 && g_paint.visual_rect.y == 480 &&
          g_paint.visual_rect.w == 290 && g_paint.visual_rect.h == 72);
    CHECK(components->destroy(new_replacement).ok == kQolOk);
    qol::features::core_ui::ui_components_unbind();
    std::printf("core_ui_components assertions: %d\n", assertions);
    return 0;
#undef CHECK
}
