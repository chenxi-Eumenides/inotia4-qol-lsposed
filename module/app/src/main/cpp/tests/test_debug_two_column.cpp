#include <algorithm>
#include <array>
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
#include "plugins/core.custom_ui/ui_option_group.h"
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

enum class FrameEvent { kBegin, kImage, kStyledContainer, kNavigation, kEnd };

struct ImageDrawCall {
    int32_t unit = 0;
    int32_t location = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t type = 0;
    int32_t flip = 0;
};

struct ImageLocationCall {
    int32_t unit = 0;
    int32_t location = 0;
};

struct DrawPartCall {
    int32_t x = 0;
    int32_t y = 0;
    int32_t type = 0;
    int32_t flip = 0;
};

struct ContainerFrameCall {
    UiRect rect{};
    uint32_t color = 0;
    uint32_t alpha = 0;
};

struct PaintedButtonCall {
    uint32_t appearance = 0;
    std::string text;
};

struct ScaledDrawCall {
    QolUiHandle group = nullptr;
    QolUiHandle location = nullptr;
    int32_t x = 0;
    int32_t y = 0;
    int32_t type = 0;
    int32_t flip = 0;
    int32_t scale = 0;
};

struct DrawnStringCall {
    std::string text;
    int32_t x = 0;
    int32_t y = 0;
};

struct PressedQuery {
    QolUiHandle handle = nullptr;
    int32_t value = 0;
};

struct BackDrawCall {
    QolUiHandle handle = nullptr;
    uint8_t pressed = 0;
};

std::vector<std::unique_ptr<Control>> g_native_roots;
bool g_main_thread = true;
int g_live = 0;
int g_deletes = 0;
int g_creates = 0;
int g_string_draws = 0;
int g_native_toggle_draws = 0;
int g_text_button_draws = 0;
int g_native_text_background_draws = 0;
std::vector<std::string> g_native_text_labels;
std::vector<PaintedButtonCall> g_painted_buttons;
int g_draw_control_calls = 0;
std::vector<QolUiHandle> g_draw_control_handles;
int g_close_draws = 0;
int g_back_draws = 0;
std::vector<ScaledDrawCall> g_scaled_draw_calls;
int g_alpha_fill_draws = 0;
int g_fill_draws = 0;
std::vector<UiRect> g_fill_rects;
std::vector<uint32_t> g_fill_colors;
std::vector<std::string> g_drawn_strings;
std::vector<DrawnStringCall> g_drawn_string_calls;
std::vector<int32_t> g_loaded_image_units;
std::vector<ImageDrawCall> g_image_draw_calls;
std::vector<int32_t> g_image_group_units;
std::vector<ImageLocationCall> g_image_location_calls;
std::vector<DrawPartCall> g_draw_part_calls;
std::vector<ContainerFrameCall> g_container_frame_calls;
std::vector<FrameEvent> g_frame_events;
std::vector<PressedQuery> g_pressed_queries;
std::vector<BackDrawCall> g_back_draw_calls;
int g_control_tree_event_calls = 0;
bool g_reject_decorated_text_button = false;
std::vector<uint32_t> g_text_button_layout_appearances;
int g_fake_group = 0;
int g_fake_arrow_group = 0;
std::array<int, 4> g_fake_parts{};
std::array<int, 4> g_fake_arrow_locations{};
int g_fake_icon_location = 0;
// 记录开关胶囊绘制 rect（屏幕绝对），验证右栏内容不与文本行重叠、不越 viewport。
std::vector<UiRect> g_toggle_rects;
const PanelProviderSpec* g_provider = nullptr;
// fake 原生 TouchHandle：按下置态、移出取消、松开仍落于导航按钮内才 ExecuteProc。
Control* g_nav_control = nullptr;
bool g_nav_pressed = false;
Control* g_pressed_control = nullptr;
bool g_fail_container_frame = false;
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
QolResult begin_frame() {
    g_frame_events.push_back(FrameEvent::kBegin);
    return ok();
}
QolResult end_frame() {
    g_frame_events.push_back(FrameEvent::kEnd);
    return ok();
}

QolResult create(uint32_t kind, QolUiHandle parent, QolUiHandle* out) {
    auto node = std::make_unique<Control>();
    node->kind = kind;
    node->parent = static_cast<Control*>(parent);
    *out = node.get();
    if (parent != nullptr) static_cast<Control*>(parent)->children.push_back(std::move(node));
    else g_native_roots.push_back(std::move(node));
    ++g_live;
    ++g_creates;
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
QolResult pressed(QolUiHandle handle, int32_t* out) {
    if (handle == nullptr || out == nullptr) return error(kQolErrInvalidArgument, "invalid", 7);
    *out = g_pressed_control == static_cast<Control*>(handle) ? 1 : 0;
    g_pressed_queries.push_back({handle, *out});
    return ok();
}
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
    ++g_control_tree_event_calls;
    *consumed = 1;
    // 模拟原生事件树：按钮须在同一控件上按下并抬起才会触发 ExecuteProc。
    if (root == nullptr || param == 0) return ok();
    const auto* point = reinterpret_cast<const int64_t*>(static_cast<uintptr_t>(param));
    auto* root_control = static_cast<Control*>(root);
    Control* target = nullptr;
    for (const auto& child : root_control->children) {
        if (child->click != nullptr &&
            hit_test(child.get(), point[0], point[1], {0, 0, child->rect.w, child->rect.h}) > 0) {
            target = child.get();
            break;
        }
    }
    if (ev == 0x17) {
        g_pressed_control = target;
        g_nav_pressed = target == g_nav_control;
    } else if (ev == 0x19) {
        if (target != g_pressed_control) g_pressed_control = nullptr;
        g_nav_pressed = g_pressed_control == g_nav_control;
    } else if (ev == 0x18) {
        Control* pressed = g_pressed_control;
        const bool activate = pressed != nullptr && pressed == target;
        g_pressed_control = nullptr;
        g_nav_pressed = false;
        if (activate && pressed->click != nullptr) {
            pressed->click(pressed->user);
        }
    }
    return ok();
}
QolResult draw_control(QolUiHandle handle) {
    ++g_draw_control_calls;
    g_draw_control_handles.push_back(handle);
    return ok();
}
QolResult fill_rect(UiRect rect, uint32_t color) {
    ++g_fill_draws;
    g_fill_rects.push_back(rect);
    g_fill_colors.push_back(color);
    return ok();
}
QolResult fill_rect_alpha(UiRect, uint32_t, uint32_t) { ++g_alpha_fill_draws; return ok(); }
QolResult custom_button(QolUiHandle, UiRect, uint32_t, uint32_t, int32_t, const char*, uint32_t,
                        int32_t, int32_t) {
    return ok();
}
QolResult draw_text(QolUiHandle, int32_t, int32_t, uint32_t) { return ok(); }
QolResult string_with_font(const char* text, int32_t x, int32_t y, int32_t, int32_t) {
    ++g_string_draws;
    g_drawn_strings.emplace_back(text == nullptr ? "" : text);
    g_drawn_string_calls.push_back({text == nullptr ? "" : text, x, y});
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
QolResult draw_native_text_button_background(UiRect, uint8_t) {
    ++g_text_button_draws;
    ++g_native_text_background_draws;
    return ok();
}
QolResult draw_native_text_button_label(UiRect, const char* text, uint32_t, int32_t) {
    if (text != nullptr) g_native_text_labels.emplace_back(text);
    return ok();
}
// 显式外观文本按钮的绘画接口：布局原样返回 visual_rect，绘画只计数。
QolResult layout_native_text_button(const UiTextButtonPaintDesc* desc,
                                    UiTextButtonPaintLayout* out) {
    if (desc != nullptr) {
        g_text_button_layout_appearances.push_back(desc->appearance);
        if (g_reject_decorated_text_button &&
            desc->appearance == kUiTextButtonDecorated) {
            return error(kQolErrNotReady, "decorated paint unavailable", 23);
        }
    }
    if (desc != nullptr && out != nullptr) {
        out->normal_bounds = desc->visual_rect;
        out->pressed_bounds = desc->visual_rect;
        out->label_bounds = desc->visual_rect;
    }
    return ok();
}
QolResult paint_native_text_button(const UiTextButtonPaintDesc* desc, const char* text,
                                   uint32_t, uint8_t) {
    ++g_text_button_draws;
    if (desc != nullptr) {
        g_painted_buttons.push_back({desc->appearance, text == nullptr ? "" : text});
    }
    return ok();
}
QolResult draw_native_close_button(UiRect, uint8_t) {
    ++g_close_draws;
    g_frame_events.push_back(FrameEvent::kNavigation);
    return ok();
}
QolResult draw_back_button(QolUiHandle handle, uint8_t pressed_state) {
    ++g_back_draws;
    g_back_draw_calls.push_back({handle, pressed_state});
    return ok();
}
QolResult load_image_unit(int32_t unit) {
    g_loaded_image_units.push_back(unit);
    return ok();
}
QolResult draw_image_part(int32_t unit, int32_t location, int32_t x, int32_t y,
                          int32_t type, int32_t flip) {
    g_image_draw_calls.push_back({unit, location, x, y, type, flip});
    g_frame_events.push_back(FrameEvent::kImage);
    return ok();
}
QolResult image_group(int32_t unit, QolUiHandle* out) {
    if (out == nullptr) return error(kQolErrInvalidArgument, "invalid", 7);
    g_image_group_units.push_back(unit);
    if (unit == 0x0f) *out = &g_fake_group;
    else if (unit == 0x59) *out = &g_fake_arrow_group;
    else return error(kQolErrInvalidArgument, "invalid", 7);
    return ok();
}
QolResult image_loc(int32_t unit, int32_t location, QolUiHandle* out) {
    if (out == nullptr) return error(kQolErrInvalidArgument, "invalid", 7);
    g_image_location_calls.push_back({unit, location});
    if (unit == 0x59 && location == 0x03) {
        *out = &g_fake_icon_location;
        return ok();
    }
    if (unit == 0x59) {
        constexpr int32_t arrow_locations[] = {0x09, 0x0a, 0x0b, 0x0c};
        for (size_t i = 0; i < 4; ++i) {
            if (location == arrow_locations[i]) {
                *out = &g_fake_arrow_locations[i];
                return ok();
            }
        }
        return error(kQolErrNotFound, "missing", 7);
    }
    if (unit != 0x0f) return error(kQolErrInvalidArgument, "invalid", 7);
    constexpr int32_t frame_locations[] = {0x30, 0x31, 0x35, 0x36};
    for (size_t i = 0; i < 4; ++i) {
        if (location == frame_locations[i]) {
            *out = &g_fake_parts[i];
            return ok();
        }
    }
    return error(kQolErrNotFound, "missing", 7);
}
QolResult image_loc_size(int32_t unit, int32_t location, int32_t* width, int32_t* height) {
    if (width == nullptr || height == nullptr) return error(kQolErrInvalidArgument, "invalid", 7);
    if (unit == 0x59 && location == 0x03) {
        *width = 40;
        *height = 40;
        return ok();
    }
    if (unit == 0x59 && location >= 0x09 && location <= 0x0c) {
        *width = 16;
        *height = 16;
        return ok();
    }
    *width = 16;
    *height = 16;
    return ok();
}
QolResult draw_part_scaled(QolUiHandle group, int32_t x, int32_t y, QolUiHandle location,
                          int32_t type, int32_t flip, int32_t scale) {
    g_scaled_draw_calls.push_back({group, location, x, y, type, flip, scale});
    return ok();
}
QolResult draw_part(QolUiHandle, int32_t x, int32_t y, QolUiHandle, int32_t type, int32_t flip) {
    g_draw_part_calls.push_back({x, y, type, flip});
    return ok();
}
QolResult draw_container_frame(UiRect rect, uint32_t color, uint32_t alpha) {
    g_container_frame_calls.push_back({rect, color, alpha});
    g_frame_events.push_back(FrameEvent::kStyledContainer);
    return g_fail_container_frame && g_container_frame_calls.size() == 3
        ? error(kQolErrInternal, "styled preview failure", 23) : ok();
}
int g_option_group_draws = 0;
QolResult draw_help_option_row(UiRect, uint8_t, uint8_t) {
    ++g_option_group_draws;
    return ok();
}
void option_group_changed(void* user, int32_t selected) {
    *static_cast<int32_t*>(user) = selected;
}
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
    ui.begin_frame = &begin_frame;
    ui.end_frame = &end_frame;
    ui.fill_rect = &fill_rect;
    ui.fill_rect_alpha = &fill_rect_alpha;
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
    ui.draw_image_part = &draw_image_part;
    ui.draw_part_scaled = &draw_part_scaled;
    ui.draw_container_frame = &draw_container_frame;
    ui.draw_help_option_row = &draw_help_option_row;
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
    const UiRect panel_rect_1008{0, 0, 1008, 672};
    const UiRect panel_rect_1422{0, 0, 1422, 640};
    const UiRect panel_nav{qol::contracts::ui_theme::kNavBoxAbsX,
                           qol::contracts::ui_theme::kNavBoxAbsY,
                           qol::contracts::ui_theme::kNavBoxW,
                           qol::contracts::ui_theme::kNavBoxH};
    const UiRect left_panel_1008{102, 26, 300, 620};
    const UiRect right_panel_1008{414, 26, 576, 620};
    const UiRect preview_1008{446, 102, 512, 240};
    const UiRect left_panel_1422{102, 26, 300, 588};
    const UiRect right_panel_1422{414, 26, 990, 588};
    const UiRect preview_1422{446, 102, 926, 240};
    UiRect panel_rect = panel_rect_1008;
    UiRect left_panel = left_panel_1008;
    UiRect right_panel = right_panel_1008;
    UiRect preview = preview_1008;

    auto ui = make_adapter();
    CHECK(ui_components_bind(&ui));
    const QolUiComponents* api = ui_components_api();
    CHECK(api != nullptr && api->major == kUiContractMajor && api->minor == kUiContractMinor &&
          api->size >= offsetof(QolUiComponents, draw_container_styled) +
                       sizeof(api->draw_container_styled) &&
          api->nav_button_create != nullptr &&
          api->draw_container_styled != nullptr);  // 旧容器槽已随 Major 2 移除。

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
    CHECK(g_loaded_image_units.size() == 4);
    CHECK(g_loaded_image_units[0] == 0x4f && g_loaded_image_units[1] == 0x50 &&
          g_loaded_image_units[2] == 0x0f && g_loaded_image_units[3] == 0x59);

    Control* root = root_control();
    CHECK(root != nullptr && root->kind == 0 && root->children.size() == 14);
    CHECK(rect_matches(root->rect, panel_rect));
    CHECK(g_live == 15);
    Control* panel_nav_control = root->children[0].get();
    g_nav_control = panel_nav_control;
    CHECK(panel_nav_control->kind == 1 && panel_nav_control->click != nullptr);
    CHECK(rect_matches(panel_nav_control->rect, panel_nav));
    Control* category_controls[7]{};
    for (size_t i = 0; i < 7; ++i) {
        category_controls[i] = root->children[i + 1].get();
        CHECK(category_controls[i]->kind == 1 && category_controls[i]->click != nullptr);
        CHECK(category_controls[i]->rect.x == 134 &&
              category_controls[i]->rect.y == 94 + static_cast<int64_t>(i) * 56 &&
              category_controls[i]->rect.w == 236 && category_controls[i]->rect.h == 48);
    }
    Control* style_controls[6]{};
    for (size_t i = 0; i < 6; ++i) style_controls[i] = root->children[i + 8].get();
    const auto assert_style_control_rects = [&]() {
        const int64_t color_x[] = {446, 570, 446, 570};
        const int64_t color_y[] = {442, 442, 494, 494};
        for (size_t i = 0; i < 4; ++i) {
            CHECK(style_controls[i]->kind == 1 && style_controls[i]->click != nullptr);
            CHECK(style_controls[i]->rect.x == color_x[i] && style_controls[i]->rect.y == color_y[i]);
            CHECK(style_controls[i]->rect.w == 112 && style_controls[i]->rect.h == 44);
        }
        for (size_t i = 0; i < 2; ++i) {
            CHECK(style_controls[i + 4]->kind == 1 && style_controls[i + 4]->click != nullptr);
            CHECK(style_controls[i + 4]->rect.x == 698 + static_cast<int64_t>(i) * 92);
            CHECK(style_controls[i + 4]->rect.y == 442 && style_controls[i + 4]->rect.w == 80 &&
                  style_controls[i + 4]->rect.h == 44);
        }
    };
    const auto find_child_at = [&](UiRect rect) -> Control* {
        for (const auto& child : root->children) {
            if (rect_matches(child->rect, rect)) return child.get();
        }
        return nullptr;
    };
    const auto bind_style_controls = [&]() {
        const UiRect rects[] = {
            {446, 442, 112, 44}, {570, 442, 112, 44},
            {446, 494, 112, 44}, {570, 494, 112, 44},
            {698, 442, 80, 44}, {790, 442, 80, 44},
        };
        for (size_t i = 0; i < 6; ++i) {
            style_controls[i] = find_child_at(rects[i]);
            CHECK(style_controls[i] != nullptr);
        }
        assert_style_control_rects();
    };
    bind_style_controls();

    int64_t root_x = 0;
    int64_t root_y = 0;
    CHECK(origin(root, &root_x, &root_y).code == kQolOk);
    const auto render_panel_frame = [&]() {
        g_frame_events.clear();
        g_image_draw_calls.clear();
        g_image_group_units.clear();
        g_image_location_calls.clear();
        g_draw_part_calls.clear();
        g_scaled_draw_calls.clear();
        g_painted_buttons.clear();
        g_native_text_background_draws = 0;
        g_native_text_labels.clear();
        g_draw_control_calls = 0;
        g_draw_control_handles.clear();
        g_text_button_draws = 0;
        g_close_draws = 0;
        g_back_draws = 0;
        g_container_frame_calls.clear();
        g_fill_rects.clear();
        g_fill_colors.clear();
        g_text_button_layout_appearances.clear();
        g_drawn_strings.clear();
        g_drawn_string_calls.clear();
        g_pressed_queries.clear();
        g_back_draw_calls.clear();
        g_control_tree_event_calls = 0;
        g_fill_draws = 0;
        g_alpha_fill_draws = 0;
        g_provider->callbacks->process();
    };
    const auto assert_flat_button_fills = [&](size_t extra_native_outlines = 0,
                                              bool include_style_controls = true) {
        const size_t base_fill_count = include_style_controls ? 66 : 36;
        CHECK(g_fill_draws == static_cast<int>(base_fill_count + extra_native_outlines) &&
              g_fill_rects.size() == base_fill_count + extra_native_outlines &&
              g_fill_colors.size() == base_fill_count + extra_native_outlines);
        CHECK(g_alpha_fill_draws == 0);
        const int64_t selected_marker_y = 102 + (include_style_controls ? 0 : 4 * 56);
        CHECK(rect_matches(g_fill_rects[0], {122, selected_marker_y, 4, 32}));
        const auto assert_button_fills = [&](Control* const* controls, size_t count,
                                             size_t first_fill) {
            for (size_t i = 0; i < count; ++i) {
                int64_t button_x = 0;
                int64_t button_y = 0;
                CHECK(origin(controls[i], &button_x, &button_y).code == kQolOk);
                const UiRect rect{button_x, button_y, controls[i]->rect.w, controls[i]->rect.h};
                const UiRect expected[] = {
                    rect,
                    {rect.x, rect.y, rect.w, 2},
                    {rect.x, rect.y + rect.h - 2, rect.w, 2},
                    {rect.x, rect.y, 2, rect.h},
                    {rect.x + rect.w - 2, rect.y, 2, rect.h},
                };
                for (size_t part = 0; part < 5; ++part) {
                    CHECK(rect_matches(g_fill_rects[first_fill + i * 5 + part], expected[part]));
                }
            }
        };
        assert_button_fills(category_controls, 7, 1);
        if (include_style_controls) assert_button_fills(style_controls, 6, 36);
    };
    const auto assert_native_button_outlines = [&](Control* const* controls,
                                                    int32_t pressed_index) {
        assert_flat_button_fills(12, false);
        for (size_t i = 0; i < 3; ++i) {
            int64_t x = 0;
            int64_t y = 0;
            CHECK(origin(controls[i], &x, &y).code == kQolOk);
            const UiRect rect{x, y, controls[i]->rect.w, controls[i]->rect.h};
            const UiRect expected[] = {
                {rect.x, rect.y, rect.w, 1},
                {rect.x, rect.y + rect.h - 1, rect.w, 1},
                {rect.x, rect.y, 1, rect.h},
                {rect.x + rect.w - 1, rect.y, 1, rect.h},
            };
            const uint32_t color = static_cast<int32_t>(i) == pressed_index
                ? qol::contracts::ui_theme::kOk : qol::contracts::ui_theme::kFrameGold;
            for (size_t edge = 0; edge < 4; ++edge) {
                const size_t index = 36 + i * 4 + edge;
                CHECK(rect_matches(g_fill_rects[index], expected[edge]));
                CHECK(g_fill_colors[index] == color);
            }
        }
    };
    const auto assert_native_button_queries = [&](Control* const* controls,
                                                    int32_t pressed_index) {
        for (size_t i = 0; i < 3; ++i) {
            const auto query = std::find_if(g_pressed_queries.begin(), g_pressed_queries.end(),
                [controls, i](const PressedQuery& entry) {
                    return entry.handle == controls[i];
                });
            CHECK(query != g_pressed_queries.end());
            CHECK(query->value == (static_cast<int32_t>(i) == pressed_index ? 1 : 0));
            CHECK(std::count_if(g_pressed_queries.begin(), g_pressed_queries.end(),
                [controls, i](const PressedQuery& entry) {
                    return entry.handle == controls[i];
                }) == 1);
        }
    };
    const auto has_drawn_string = [](const std::vector<std::string>& strings,
                                     const std::string& expected) {
        return std::find(strings.begin(), strings.end(), expected) != strings.end();
    };
    const auto has_drawn_text = [](const std::vector<std::string>& strings,
                                   const std::string& fragment) {
        return std::any_of(strings.begin(), strings.end(), [&fragment](const std::string& text) {
            return text.find(fragment) != std::string::npos;
        });
    };

    const uint32_t fill_colors[] = {0x0000, 0x8410, 0xffff, 0xf800};
    const char* fill_color_names[] = {"黑色", "灰色", "白色", "红色"};
    const auto expect_style = [&](uint32_t color, uint32_t opacity) {
        render_panel_frame();
        const UiRect styled_rects[] = {left_panel, right_panel, preview};
        // Major 2 只保留 styled 容器；方框页由两个外壳 + 示意共 3 次 frame 调用覆盖。
        CHECK(g_container_frame_calls.size() == 3);
        for (size_t i = 0; i < 3; ++i) {
            const uint32_t expected_color = i == 2 ? color : 0x0000;
            const uint32_t expected_opacity = i == 2 ? opacity : 60;
            CHECK(rect_matches(g_container_frame_calls[i].rect, styled_rects[i]));
            CHECK(g_container_frame_calls[i].color == expected_color &&
                  g_container_frame_calls[i].alpha == expected_opacity &&
                  g_container_frame_calls[i].alpha <= 100);
        }

        CHECK(g_image_draw_calls.size() == 2);
        CHECK(g_image_draw_calls[0].unit == 0x4f && g_image_draw_calls[0].location == 0);
        CHECK(g_image_draw_calls[0].x == root_x + panel_rect.w / 2 - 300 &&
              g_image_draw_calls[0].y == root_y + panel_rect.h / 2 &&
              g_image_draw_calls[0].type == 2 && g_image_draw_calls[0].flip == 1);
        CHECK(g_image_draw_calls[1].unit == 0x50 && g_image_draw_calls[1].location == 0);
        CHECK(g_image_draw_calls[1].x == root_x + panel_rect.w / 2 + 300 &&
              g_image_draw_calls[1].y == root_y + panel_rect.h / 2 &&
              g_image_draw_calls[1].type == 2 && g_image_draw_calls[1].flip == 1);
        CHECK(g_image_draw_calls[0].x == (panel_rect.w == 1008 ? 204 : 411) &&
              g_image_draw_calls[0].y == panel_rect.h / 2);
        CHECK(g_image_draw_calls[1].x == (panel_rect.w == 1008 ? 804 : 1011) &&
              g_image_draw_calls[1].y == panel_rect.h / 2);

        CHECK(g_image_group_units.size() == 3);
        CHECK(g_image_location_calls.size() == 12 && g_draw_part_calls.size() == 12);
        const int32_t locations[] = {0x30, 0x31, 0x35, 0x36};
        for (size_t frame = 0; frame < 3; ++frame) {
            CHECK(g_image_group_units[frame] == 0x0f);
            const UiRect rect = styled_rects[frame];
            const int32_t ornament_x[] = {
                static_cast<int32_t>(rect.x - 2), static_cast<int32_t>(rect.x + rect.w - 16),
                static_cast<int32_t>(rect.x - 2), static_cast<int32_t>(rect.x + rect.w - 16)};
            const int32_t ornament_y[] = {
                static_cast<int32_t>(rect.y - 2), static_cast<int32_t>(rect.y - 2),
                static_cast<int32_t>(rect.y + rect.h - 16),
                static_cast<int32_t>(rect.y + rect.h - 16)};
            for (size_t part = 0; part < 4; ++part) {
                const size_t index = frame * 4 + part;
                CHECK(g_image_location_calls[index].unit == 0x0f &&
                      g_image_location_calls[index].location == locations[part]);
                CHECK(g_draw_part_calls[index].x == ornament_x[part] &&
                      g_draw_part_calls[index].y == ornament_y[part]);
                CHECK(g_draw_part_calls[index].type == 0 && g_draw_part_calls[index].flip == 1);
            }
        }

        CHECK(g_frame_events.size() == 8);
        CHECK(g_frame_events[0] == FrameEvent::kBegin &&
              g_frame_events[1] == FrameEvent::kImage &&
              g_frame_events[2] == FrameEvent::kImage &&
              g_frame_events[3] == FrameEvent::kStyledContainer &&
              g_frame_events[4] == FrameEvent::kStyledContainer &&
              g_frame_events[5] == FrameEvent::kStyledContainer &&
              g_frame_events[6] == FrameEvent::kNavigation &&
              g_frame_events[7] == FrameEvent::kEnd);

        assert_flat_button_fills();  // 填充仅来自 6 个平面文本按钮，无额外背景色块。
        CHECK(has_drawn_string(g_drawn_strings, "选项"));
        CHECK(has_drawn_string(g_drawn_strings, "方框"));
        CHECK(has_drawn_string(g_drawn_strings, "底色 RGB565"));
        size_t color_index = 0;
        while (color_index < 4 && fill_colors[color_index] != color) ++color_index;
        CHECK(color_index < 4);
        const std::string named_color = std::string(fill_color_names[color_index]) + " " +
            (color == 0x0000 ? "0x0000" : color == 0x8410 ? "0x8410" :
             color == 0xffff ? "0xFFFF" : "0xF800");
        CHECK(has_drawn_string(g_drawn_strings, "当前 " + named_color));
        CHECK(has_drawn_string(g_drawn_strings, "不透明度 " + std::to_string(opacity) + "%"));
        CHECK(has_drawn_string(g_drawn_strings, "0 全透明 · 100 全不透明"));
        CHECK(has_drawn_string(g_drawn_strings, "已应用：" + named_color +
              " · 不透明度 " + std::to_string(opacity) + "%"));
        CHECK(has_drawn_string(g_drawn_strings, "示意 " + std::to_string(preview.w) + " × " +
              std::to_string(preview.h)));
        for (const char* label : {"方框", "文本按钮", "箭头按钮", "图标按钮",
                                  "普通控件按钮", "返回/导航", "列表/主菜单专用",
                                  "黑色 0000", "灰色 8410", "白色 FFFF", "红色 F800",
                                  "-10", "+10"}) {
            CHECK(has_drawn_string(g_drawn_strings, label));
        }
        CHECK(g_native_toggle_draws == 0 && g_alpha_fill_draws == 0);
        CHECK(g_close_draws > 0);
    };

    expect_style(0x0000, 60);
    CHECK(g_choice_open_count == 0 && !g_choice_active);
    CHECK(g_confirmation_open_count == 0 && !g_confirmation_active);

    // styled 容器在 48×48 边界接受绘制，任一维度小于 48 或参数越界时拒绝且不产生调用。
    const size_t frames_before_minimum_checks = g_container_frame_calls.size();
    const size_t parts_before_minimum_checks = g_draw_part_calls.size();
    const auto check_styled_rejected = [&](UiRect rect, uint32_t fill, uint32_t alpha,
                                           uint8_t ornamented) {
        const UiContainerStyleDesc rejected{rect, fill, alpha, ornamented};
        CHECK(api->draw_container_styled(&rejected).code == kQolErrInvalidArgument);
    };
    check_styled_rejected({200, 200, 47, 48}, 0, 60, 1);
    check_styled_rejected({200, 200, 48, 47}, 0, 60, 1);
    check_styled_rejected({200, 200, 48, 48}, 0x10000u, 60, 0);
    check_styled_rejected({200, 200, 48, 48}, 0, 101u, 0);
    check_styled_rejected({200, 200, 48, 48}, 0, 60, 2);
    check_styled_rejected({INT32_MIN, 0, 320, 140}, 0, 60, 1);  // ornament 扩框下界。
    CHECK(g_container_frame_calls.size() == frames_before_minimum_checks);
    CHECK(g_draw_part_calls.size() == parts_before_minimum_checks);
    const UiContainerStyleDesc styled_minimal{{200, 200, 48, 48}, 0, 60u, 1};
    CHECK(api->draw_container_styled(&styled_minimal).ok == kQolOk);
    CHECK(g_container_frame_calls.size() == frames_before_minimum_checks + 1);
    CHECK(rect_matches(g_container_frame_calls.back().rect, {200, 200, 48, 48}));
    CHECK(g_container_frame_calls.back().color == 0u && g_container_frame_calls.back().alpha == 60u);
    CHECK(g_draw_part_calls.size() == parts_before_minimum_checks + 4);
    // ornament 覆盖层原样保留四角扩框几何：{x-2,y-2}/{x+w-16,y-2}/{x-2,y+h-16}/{x+w-16,y+h-16}。
    const int32_t ornament_x[] = {198, 232, 198, 232};
    const int32_t ornament_y[] = {198, 198, 232, 232};
    for (size_t part = 0; part < 4; ++part) {
        CHECK(g_draw_part_calls[parts_before_minimum_checks + part].x == ornament_x[part] &&
              g_draw_part_calls[parts_before_minimum_checks + part].y == ornament_y[part] &&
              g_draw_part_calls[parts_before_minimum_checks + part].type == 0 &&
              g_draw_part_calls[parts_before_minimum_checks + part].flip == 1);
    }

    const auto dispatch_control_click = [&](Control* control) {
        int64_t point[] = {control->rect.x + control->rect.w / 2,
                           control->rect.y + control->rect.h / 2};
        const uint64_t param = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(point));
        CHECK(g_provider->callbacks->event(0x17, param, 0x10) == 1);
        CHECK(g_provider->callbacks->event(0x18, param, 0x10) == 1);
    };
    const char* category_names[] = {
        "方框", "文本按钮", "箭头按钮", "图标按钮", "普通控件按钮", "返回/导航", "列表/主菜单专用"};
    const auto has_category_heading = [&](size_t category) {
        return std::any_of(g_drawn_string_calls.begin(), g_drawn_string_calls.end(),
            [&](const DrawnStringCall& call) {
                return call.text == category_names[category] &&
                    call.x == right_panel.x + 32 && call.y == right_panel.y + 28;
            });
    };
    const auto request_category = [&](size_t category) {
        const int creates_before = g_creates;
        const int deletes_before = g_deletes;
        dispatch_control_click(category_controls[category]);
        CHECK(g_creates == creates_before && g_deletes == deletes_before);
    };
    const auto assert_shell_only_frame = [&]() {
        const UiRect shell_rects[] = {left_panel, right_panel};
        CHECK(g_image_draw_calls.size() == 2);
        CHECK(g_image_draw_calls[0].unit == 0x4f &&
              g_image_draw_calls[0].x == root_x + panel_rect.w / 2 - 300 &&
              g_image_draw_calls[0].y == root_y + panel_rect.h / 2);
        CHECK(g_image_draw_calls[1].unit == 0x50 &&
              g_image_draw_calls[1].x == root_x + panel_rect.w / 2 + 300 &&
              g_image_draw_calls[1].y == root_y + panel_rect.h / 2);
        CHECK(g_container_frame_calls.size() == 2);
        CHECK(g_draw_part_calls.size() == 8);
        CHECK(g_image_location_calls.size() >= 8 && g_image_group_units.size() >= 2);
        const int32_t locations[] = {0x30, 0x31, 0x35, 0x36};
        for (size_t shell = 0; shell < 2; ++shell) {
            CHECK(rect_matches(g_container_frame_calls[shell].rect, shell_rects[shell]));
            CHECK(g_container_frame_calls[shell].color == 0x0000 &&
                  g_container_frame_calls[shell].alpha == 60);
            CHECK(g_image_group_units[shell] == 0x0f);
            const UiRect rect = shell_rects[shell];
            const int32_t xs[] = {static_cast<int32_t>(rect.x - 2),
                static_cast<int32_t>(rect.x + rect.w - 16), static_cast<int32_t>(rect.x - 2),
                static_cast<int32_t>(rect.x + rect.w - 16)};
            const int32_t ys[] = {static_cast<int32_t>(rect.y - 2),
                static_cast<int32_t>(rect.y - 2), static_cast<int32_t>(rect.y + rect.h - 16),
                static_cast<int32_t>(rect.y + rect.h - 16)};
            for (size_t part = 0; part < 4; ++part) {
                const size_t index = shell * 4 + part;
                CHECK(g_image_location_calls[index].unit == 0x0f &&
                      g_image_location_calls[index].location == locations[part]);
                CHECK(g_draw_part_calls[index].x == xs[part] && g_draw_part_calls[index].y == ys[part]);
            }
        }
        CHECK(g_frame_events.size() >= 7 &&
              g_frame_events[0] == FrameEvent::kBegin &&
              g_frame_events[1] == FrameEvent::kImage &&
              g_frame_events[2] == FrameEvent::kImage &&
              g_frame_events[3] == FrameEvent::kStyledContainer &&
              g_frame_events[4] == FrameEvent::kStyledContainer &&
              g_frame_events.back() == FrameEvent::kEnd);
        CHECK(std::count(g_frame_events.begin(), g_frame_events.end(),
                         FrameEvent::kStyledContainer) == 2);
    };
    // 颜色平面文本按钮经真实原生事件按下/抬起流程更新 styled-container 描述符。
    for (size_t i = 0; i < 4; ++i) {
        dispatch_control_click(style_controls[i]);
        expect_style(fill_colors[i], 60);
    }

    // 不透明度按钮每次变更 10%，并在 0/100 两端饱和而不越界。
    dispatch_control_click(style_controls[4]);
    expect_style(0xf800, 50);
    dispatch_control_click(style_controls[5]);
    expect_style(0xf800, 60);
    for (uint32_t opacity = 50;; opacity -= 10) {
        dispatch_control_click(style_controls[4]);
        expect_style(0xf800, opacity);
        if (opacity == 0) break;
    }
    dispatch_control_click(style_controls[4]);
    expect_style(0xf800, 0);
    for (uint32_t opacity = 10; opacity <= 100; opacity += 10) {
        dispatch_control_click(style_controls[5]);
        expect_style(0xf800, opacity);
    }
    dispatch_control_click(style_controls[5]);
    expect_style(0xf800, 100);

    // 样式绘制失败时明确显示“未应用”，不能显示成功状态。
    g_fail_container_frame = true;
    render_panel_frame();
    g_fail_container_frame = false;
    CHECK(g_container_frame_calls.size() == 3);
    CHECK(g_container_frame_calls[0].color == 0x0000 && g_container_frame_calls[0].alpha == 60);
    CHECK(g_container_frame_calls[1].color == 0x0000 && g_container_frame_calls[1].alpha == 60);
    CHECK(g_container_frame_calls[2].color == 0xf800 && g_container_frame_calls[2].alpha == 100);
    CHECK(has_drawn_text(g_drawn_strings, "示意未应用"));
    CHECK(has_drawn_text(g_drawn_strings, "未应用：红色 0xF800 · 不透明度 100%"));
    CHECK(!has_drawn_text(g_drawn_strings, "已应用"));
    assert_flat_button_fills();
    CHECK(g_draw_part_calls.size() == 8);
    expect_style(0xf800, 100);

    // styled-container adapter 接口缺失时明确失败状态，不回退旧容器绘制，也不误报样式已应用。
    const auto draw_container_frame_callback = ui.draw_container_frame;
    ui.draw_container_frame = nullptr;
    render_panel_frame();
    CHECK(g_container_frame_calls.empty());
    CHECK(has_drawn_text(g_drawn_strings, "示意未应用"));
    CHECK(has_drawn_text(g_drawn_strings, "面板外框绘制错误"));
    assert_flat_button_fills();
    CHECK(!has_drawn_text(g_drawn_strings, "已应用"));
    ui.draw_container_frame = draw_container_frame_callback;
    expect_style(0xf800, 100);

    // 背景图像绘制接口缺失时安全跳过背景，但仍正常绘制样式容器和导航。
    const auto draw_image_part_callback = ui.draw_image_part;
    ui.draw_image_part = nullptr;
    render_panel_frame();
    CHECK(g_image_draw_calls.empty());
    CHECK(g_container_frame_calls.size() == 3);
    CHECK(has_drawn_text(g_drawn_strings, "已应用：红色 0xF800 · 不透明度 100%"));
    assert_flat_button_fills();
    ui.draw_image_part = draw_image_part_callback;
    expect_style(0xf800, 100);

    const UiRect box_style_rects[] = {
        {446, 442, 112, 44}, {570, 442, 112, 44}, {446, 494, 112, 44},
        {570, 494, 112, 44}, {698, 442, 80, 44}, {790, 442, 80, 44}};

    // 从 category 0 经面板事件回调进入 category 5；两个有效坐标事件必须进入 native adapter。
    const int events_before_back_nav = g_control_tree_event_calls;
    const int creates_before_back_nav = g_creates;
    const int deletes_before_back_nav = g_deletes;
    request_category(5);
    CHECK(g_control_tree_event_calls == events_before_back_nav + 2);
    CHECK(g_creates == creates_before_back_nav && g_deletes == deletes_before_back_nav);
    render_panel_frame();
    CHECK(g_creates > creates_before_back_nav && g_deletes > deletes_before_back_nav);
    assert_shell_only_frame();
    CHECK(has_drawn_string(g_drawn_strings, category_names[5]));
    CHECK(has_category_heading(5));
    CHECK(g_back_draws == 2 && g_close_draws == 3);
    CHECK(g_scaled_draw_calls.empty() && g_draw_control_calls == 0);

    // Back appearance is a no-op button; verify the queried pressed state reaches the adapter draw.
    const UiRect back_preview_rect{right_panel.x + 32, right_panel.y + 84, 78, 86};
    Control* back_preview = find_child_at(back_preview_rect);
    CHECK(back_preview != nullptr && back_preview->click != nullptr);
    g_back_requested = false;
    int64_t back_preview_hit[] = {back_preview_rect.x + back_preview_rect.w / 2,
                                  back_preview_rect.y + back_preview_rect.h / 2};
    const uint64_t back_preview_param = static_cast<uint64_t>(
        reinterpret_cast<uintptr_t>(back_preview_hit));
    const int event_calls_before_back_press = g_control_tree_event_calls;
    CHECK(g_provider->callbacks->event(0x17, back_preview_param, 0x10) == 1);
    CHECK(g_control_tree_event_calls == event_calls_before_back_press + 1);
    CHECK(g_pressed_control == back_preview);
    render_panel_frame();
    CHECK(std::any_of(g_pressed_queries.begin(), g_pressed_queries.end(),
        [back_preview](const PressedQuery& query) {
            return query.handle == back_preview && query.value == 1;
        }));
    CHECK(std::any_of(g_back_draw_calls.begin(), g_back_draw_calls.end(),
        [back_preview](const BackDrawCall& call) {
            return call.handle == back_preview && call.pressed == 1;
        }));
    CHECK(!g_back_requested);
    CHECK(g_provider->callbacks->event(0x18, back_preview_param, 0x10) == 1);
    CHECK(g_control_tree_event_calls == 1 && g_pressed_control == nullptr);
    CHECK(!g_back_requested);  // The preview back appearance has no back action.
    render_panel_frame();
    CHECK(std::any_of(g_pressed_queries.begin(), g_pressed_queries.end(),
        [back_preview](const PressedQuery& query) {
            return query.handle == back_preview && query.value == 0;
        }));
    CHECK(std::any_of(g_back_draw_calls.begin(), g_back_draw_calls.end(),
        [back_preview](const BackDrawCall& call) {
            return call.handle == back_preview && call.pressed == 0;
        }));

    // The actual nav control invokes its callback, and enabling it rebuilds at the process safe point.
    const UiRect nav_enable_rect{right_panel.x + 32, right_panel.y + 274, 150, 44};
    Control* nav_enable = find_child_at(nav_enable_rect);
    CHECK(nav_enable != nullptr && nav_enable->click != nullptr);
    const int creates_before_nav_enable = g_creates;
    const int deletes_before_nav_enable = g_deletes;
    dispatch_control_click(nav_enable);
    CHECK(g_creates == creates_before_nav_enable && g_deletes == deletes_before_nav_enable);
    CHECK(!g_back_requested);
    render_panel_frame();
    CHECK(g_creates > creates_before_nav_enable && g_deletes > deletes_before_nav_enable);
    assert_shell_only_frame();
    CHECK(has_category_heading(5) && g_back_draws == 2 && g_close_draws == 3);
    Control* force_close_nav = find_child_at({right_panel.x + 32 + 2 * 94,
                                               right_panel.y + 84, 78, 86});
    CHECK(force_close_nav != nullptr && force_close_nav->click != nullptr &&
          force_close_nav->enabled);
    g_back_requested = false;
    const int event_calls_before_nav = g_control_tree_event_calls;
    dispatch_control_click(force_close_nav);
    CHECK(g_control_tree_event_calls == event_calls_before_nav + 2);
    CHECK(g_back_requested);

    // Category 5 handles are destroyed on the next process before category 0 is rebuilt.
    g_back_requested = false;
    const int creates_before_return_to_box = g_creates;
    const int deletes_before_return_to_box = g_deletes;
    request_category(0);
    CHECK(g_creates == creates_before_return_to_box && g_deletes == deletes_before_return_to_box);
    CHECK(g_control_tree_event_calls == event_calls_before_nav + 4);
    render_panel_frame();
    CHECK(g_creates > creates_before_return_to_box && g_deletes > deletes_before_return_to_box);
    CHECK(api->button_set_text(back_preview, "stale").code == kQolErrNotFound &&
          !g_back_requested);
    CHECK(has_category_heading(0));
    for (size_t i = 0; i < 6; ++i) {
        style_controls[i] = find_child_at(box_style_rects[i]);
        CHECK(style_controls[i] != nullptr);
    }
    assert_style_control_rects();
    expect_style(0xf800, 100);

    // Category selection only queues work in the event callback; page handles rebuild on process.
    const int creates_before_text_page = g_creates;
    const int deletes_before_text_page = g_deletes;
    request_category(1);
    CHECK(g_creates == creates_before_text_page && g_deletes == deletes_before_text_page);
    g_reject_decorated_text_button = true;
    render_panel_frame();
    g_reject_decorated_text_button = false;
    CHECK(g_creates > creates_before_text_page && g_deletes > deletes_before_text_page);
    assert_shell_only_frame();
    CHECK(has_drawn_string(g_drawn_strings, category_names[1]));
    CHECK(has_category_heading(1));
    CHECK(g_text_button_layout_appearances.size() == 3 &&
          g_text_button_layout_appearances[0] == kUiTextButtonCapsule &&
          g_text_button_layout_appearances[1] == kUiTextButtonDecorated &&
          g_text_button_layout_appearances[2] == kUiTextButtonCapsule);
    CHECK(g_painted_buttons.size() == 1 &&
          g_painted_buttons[0].appearance == kUiTextButtonCapsule);
    CHECK(g_native_text_background_draws == 1 && !g_native_text_labels.empty());
    CHECK(std::all_of(g_native_text_labels.begin(), g_native_text_labels.end(),
                      [](const std::string& text) { return text == "示例"; }));
    CHECK(has_drawn_text(g_drawn_strings,
                         "工厂不可用 code " + std::to_string(kQolErrNotReady)));
    CHECK(!has_drawn_string(g_drawn_strings, "页面控件创建失败，已清理。"));
    const UiRect capsule_sample_rect{right_panel.x + 136, right_panel.y + 84, 300, 60};
    const UiRect legacy_sample_rect{right_panel.x + 136, right_panel.y + 152, 300, 60};
    const UiRect decorated_sample_rect{right_panel.x + 136, right_panel.y + 220, 300, 60};
    Control* capsule_sample = find_child_at(capsule_sample_rect);
    Control* legacy_sample = find_child_at(legacy_sample_rect);
    CHECK(capsule_sample != nullptr && capsule_sample->click != nullptr);
    CHECK(legacy_sample != nullptr && legacy_sample->click != nullptr);
    CHECK(find_child_at(decorated_sample_rect) == nullptr);  // NotReady is not replaced by a fallback.
    const int text_sample_events = g_control_tree_event_calls;
    dispatch_control_click(capsule_sample);
    dispatch_control_click(legacy_sample);
    CHECK(g_control_tree_event_calls == text_sample_events + 4);
    CHECK(g_scaled_draw_calls.empty() && g_draw_control_calls == 0);

    // The second text subpage replaces compact/capsule and flat samples at the next process safe point.
    const UiRect next_text_page_rect{right_panel.x + right_panel.w - 152,
                                     right_panel.y + 18, 120, 42};
    Control* next_text_page = find_child_at(next_text_page_rect);
    CHECK(next_text_page != nullptr && next_text_page->click != nullptr);
    const int creates_before_subpage = g_creates;
    const int deletes_before_subpage = g_deletes;
    dispatch_control_click(next_text_page);
    CHECK(g_creates == creates_before_subpage && g_deletes == deletes_before_subpage);
    render_panel_frame();
    CHECK(g_creates > creates_before_subpage && g_deletes > deletes_before_subpage);
    assert_shell_only_frame();
    CHECK(g_text_button_layout_appearances.size() == 2 &&
          g_text_button_layout_appearances[0] == kUiTextButtonCapsule &&
          g_text_button_layout_appearances[1] == kUiTextButtonCapsule);
    CHECK(g_painted_buttons.size() == 1 &&
          g_painted_buttons[0].appearance == kUiTextButtonCapsule);
    CHECK(g_native_text_background_draws == 0 && g_scaled_draw_calls.empty());
    CHECK(has_drawn_text(g_drawn_strings, "2/2 紧凑/平面"));
    const UiRect compact_sample_rect{right_panel.x + 136, right_panel.y + 84, 300, 60};
    const UiRect flat_sample_rect{right_panel.x + 136, right_panel.y + 152, 300, 60};
    Control* compact_sample = find_child_at(compact_sample_rect);
    Control* flat_sample = find_child_at(flat_sample_rect);
    CHECK(compact_sample != nullptr && compact_sample->click != nullptr);
    CHECK(flat_sample != nullptr && flat_sample->click != nullptr);
    CHECK(std::any_of(g_fill_rects.begin(), g_fill_rects.end(),
        [&](UiRect rect) { return rect_matches(rect, flat_sample_rect); }));
    const int compact_flat_events = g_control_tree_event_calls;
    dispatch_control_click(compact_sample);
    dispatch_control_click(flat_sample);
    CHECK(g_control_tree_event_calls == compact_flat_events + 4);

    for (size_t category = 2; category < 5; ++category) {
        const int creates_before_category = g_creates;
        const int deletes_before_category = g_deletes;
        request_category(category);
        render_panel_frame();
        assert_shell_only_frame();
        CHECK(has_drawn_string(g_drawn_strings, category_names[category]));
        CHECK(has_category_heading(category));
        CHECK(std::any_of(g_drawn_string_calls.begin(), g_drawn_string_calls.end(),
            [&](const DrawnStringCall& call) {
                return call.text == category_names[category] &&
                    call.x == right_panel.x + 32 && call.y == right_panel.y + 28;
            }));
        if (category == 2) {
            CHECK(g_scaled_draw_calls.size() == 2);
            CHECK(g_scaled_draw_calls[0].group == &g_fake_arrow_group &&
                  g_scaled_draw_calls[0].location == &g_fake_arrow_locations[0] &&
                  g_scaled_draw_calls[0].type == 1 && g_scaled_draw_calls[0].flip == 0 &&
                  g_scaled_draw_calls[0].scale == 300);
            CHECK(g_scaled_draw_calls[1].group == &g_fake_arrow_group &&
                  g_scaled_draw_calls[1].location == &g_fake_arrow_locations[2] &&
                  g_scaled_draw_calls[1].type == 1 && g_scaled_draw_calls[1].scale == 300);
            CHECK(g_painted_buttons.empty() && g_native_text_background_draws == 0);
        } else if (category == 3) {
            CHECK(g_scaled_draw_calls.size() == 2);
            for (const auto& call : g_scaled_draw_calls) {
                CHECK(call.group == &g_fake_arrow_group && call.location == &g_fake_icon_location &&
                      call.type == 1 && call.flip == 0 && call.scale == 120);
            }
        } else if (category == 4) {
            CHECK(g_creates >= creates_before_category + 3 &&
                  g_deletes > deletes_before_category);
            const UiRect native_rects[] = {
                {right_panel.x + 32, right_panel.y + 92, 204, 48},
                {right_panel.x + 32, right_panel.y + 168, right_panel.w - 64, 60},
                {right_panel.x + 32, right_panel.y + 256, 300, 80},
            };
            Control* native_controls[3]{};
            for (size_t i = 0; i < 3; ++i) {
                native_controls[i] = find_child_at(native_rects[i]);
                CHECK(native_controls[i] != nullptr && native_controls[i]->kind == 1 &&
                      native_controls[i]->click != nullptr && native_controls[i]->draw == nullptr &&
                      native_controls[i]->enabled);
            }
            CHECK(g_draw_control_calls == 3 && g_draw_control_handles.size() == 3);
            for (size_t i = 0; i < 3; ++i) {
                CHECK(g_draw_control_handles[i] == native_controls[i]);
            }
            CHECK(g_scaled_draw_calls.empty() && g_painted_buttons.empty());
            CHECK(has_drawn_string(g_drawn_strings,
                "button_create 本身不设置 DrawProc/默认外观。"));
            CHECK(has_drawn_string(g_drawn_strings,
                "无 enabled/active/颜色接口，不能禁用或改色。"));
            CHECK(has_drawn_string(g_drawn_strings, "真实按下：未按下"));
            assert_native_button_queries(native_controls, -1);
            assert_native_button_outlines(native_controls, -1);

            // The third hitbox is away from older page controls and exercises real event dispatch.
            g_back_requested = false;
            const UiRect pressed_rect = native_rects[2];
            int64_t native_hit[] = {pressed_rect.x + pressed_rect.w / 2,
                                    pressed_rect.y + pressed_rect.h / 2};
            const uint64_t native_hit_param = static_cast<uint64_t>(
                reinterpret_cast<uintptr_t>(native_hit));
            CHECK(g_provider->callbacks->event(0x17, native_hit_param, 0x10) == 1);
            CHECK(g_pressed_control == native_controls[2]);
            render_panel_frame();
            assert_shell_only_frame();
            CHECK(g_draw_control_calls == 3 && g_draw_control_handles.size() == 3);
            assert_native_button_queries(native_controls, 2);
            assert_native_button_outlines(native_controls, 2);
            CHECK(has_drawn_string(g_drawn_strings, "真实按下：按下"));
            CHECK(has_drawn_string(g_drawn_strings,
                "button_create 本身不设置 DrawProc/默认外观。"));
            CHECK(has_drawn_string(g_drawn_strings,
                "无 enabled/active/颜色接口，不能禁用或改色。"));
            CHECK(!g_back_requested);
            CHECK(g_provider->callbacks->event(0x18, native_hit_param, 0x10) == 1);
            CHECK(g_pressed_control == nullptr && !g_back_requested);
            render_panel_frame();
            assert_shell_only_frame();
            assert_native_button_queries(native_controls, -1);
            assert_native_button_outlines(native_controls, -1);
            CHECK(has_drawn_string(g_drawn_strings, "真实按下：未按下"));
            CHECK(g_scaled_draw_calls.empty() && g_painted_buttons.empty());
        }
    }

    request_category(6);
    render_panel_frame();
    assert_shell_only_frame();
    CHECK(has_drawn_string(g_drawn_strings, category_names[6]));
    CHECK(has_category_heading(6));
    CHECK(g_scaled_draw_calls.empty() && g_draw_control_calls == 0 &&
          g_back_draws == 0 && g_native_text_background_draws == 0 &&
          g_painted_buttons.empty());  // Restricted examples are described, not fabricated.

    // Returning to category 0 rebuilds its page on process while preserving the preview style.
    const int creates_before_box_page = g_creates;
    request_category(0);
    CHECK(g_creates == creates_before_box_page);
    render_panel_frame();
    CHECK(g_creates > creates_before_box_page && g_container_frame_calls.size() == 3);
    CHECK(has_category_heading(0));
    for (size_t i = 0; i < 6; ++i) {
        style_controls[i] = find_child_at(box_style_rects[i]);
        CHECK(style_controls[i] != nullptr);
    }
    assert_style_control_rects();
    expect_style(0xf800, 100);

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

    const size_t frames_before_close = g_container_frame_calls.size();
    // 保留的两行选项组组件：创建、绘制、点击切换、销毁。
    {
        int32_t changed = -1;
        UiOptionGroupDesc desc{{0, 0, 300, 120}, {"选项 A", "选项 B"}, 0,
                               &option_group_changed, &changed};
        UiOptionGroup group{};
        CHECK(option_group_create(api, root, &desc, &group).code == kQolOk);
        CHECK(group.handles[0] != nullptr && group.handles[1] != nullptr &&
              group.selected == 0);
        g_option_group_draws = 0;
        option_group_draw(&ui, &group);
        CHECK(g_option_group_draws == 2);
        auto* second = static_cast<Control*>(group.handles[1]);
        CHECK(second != nullptr && second->click != nullptr);
        second->click(second->user);
        CHECK(group.selected == 1 && changed == 1);
        option_group_destroy(api, &group);
        CHECK(group.handles[0] == nullptr && group.handles[1] == nullptr);
    }

    g_provider->close();
    CHECK(!g_provider->active());
    g_provider->callbacks->process();
    CHECK(g_container_frame_calls.size() == frames_before_close);  // 关闭后不再绘制。
    CHECK(g_choice_open_count == 0 && g_confirmation_open_count == 0);
    remove_control(root);  // 组件归还其句柄；原生树由宿主释放。
    CHECK(g_live == 0);

    // 宽屏逻辑分辨率使用同样的双面板偏移，右侧预览随剩余宽度扩展。
    g_fake_inset_x = 231;
    g_fake_inset_y = 0;
    CHECK(g_provider->open());
    g_provider->callbacks->enter();
    CHECK(g_provider->active());
    CHECK(g_loaded_image_units.size() == 12);
    CHECK(g_loaded_image_units[8] == 0x4f && g_loaded_image_units[9] == 0x50 &&
          g_loaded_image_units[10] == 0x0f && g_loaded_image_units[11] == 0x59);
    panel_rect = panel_rect_1422;
    left_panel = left_panel_1422;
    right_panel = right_panel_1422;
    preview = preview_1422;
    root = root_control();
    CHECK(root != nullptr && root->kind == 0 && root->children.size() == 14);
    CHECK(rect_matches(root->rect, panel_rect_1422) && g_live == 15);
    panel_nav_control = root->children[0].get();
    g_nav_control = panel_nav_control;
    CHECK(rect_matches(panel_nav_control->rect, panel_nav));
    for (size_t i = 0; i < 7; ++i) category_controls[i] = root->children[i + 1].get();
    for (size_t i = 0; i < 6; ++i) style_controls[i] = find_child_at(box_style_rects[i]);
    assert_style_control_rects();
    CHECK(origin(root, &root_x, &root_y).code == kQolOk);
    expect_style(0x0000, 60);
    const size_t frames_before_wide_close = g_container_frame_calls.size();
    g_provider->close();
    CHECK(!g_provider->active());
    g_provider->callbacks->process();
    CHECK(g_container_frame_calls.size() == frames_before_wide_close);
    remove_control(root);
    CHECK(g_live == 0);

    // HelpPage 的选择、绘制回调及销毁仍作为独立组件契约回归，不属于 Debug Components 示例页。
    int help_option_users[2]{};
    const UiHelpOption help_options[] = {
        {&help_option_users[0], &help_detail_draw},
        {&help_option_users[1], &help_detail_draw},
    };
    const UiHelpPageDesc help_desc{
        panel_rect_1008, {94, 18, 183, 636}, {309, 18, 613, 636}, 56,
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
    // HelpPage 内部经 draw_container_styled：page rect 全屏时只画两个 ornamented 栏框，
    // 每次 draw 共 2 次 frame 调用（fill 固定黑 60%）+ 8 次角装饰。
    // styled 通道缺失时 HelpPage 明确失败，不回退旧边框绘制也不产生半帧输出。
    const auto help_draw_container_frame_callback = ui.draw_container_frame;
    ui.draw_container_frame = nullptr;
    const size_t help_frames_before_failure = g_container_frame_calls.size();
    CHECK(api->help_page_draw(help_page).code == kQolErrNotReady);
    CHECK(g_container_frame_calls.size() == help_frames_before_failure);
    ui.draw_container_frame = help_draw_container_frame_callback;
    const size_t help_frames_before_draw = g_container_frame_calls.size();
    const size_t help_parts_before_draw = g_draw_part_calls.size();
    CHECK(api->help_page_draw(help_page).code == kQolOk);
    CHECK(g_container_frame_calls.size() == help_frames_before_draw + 2);
    CHECK(g_draw_part_calls.size() == help_parts_before_draw + 8);
    for (size_t frame = help_frames_before_draw;
         frame < g_container_frame_calls.size(); ++frame) {
        CHECK(g_container_frame_calls[frame].color == 0x0000u &&
              g_container_frame_calls[frame].alpha == 60u);
    }
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
