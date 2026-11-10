#include <array>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "contracts/ui.h"
#include "contracts/ui_theme.h"
#include "plugins/core.custom_ui/ui_components.h"

// 返回按钮助手内部调用宿主 back 入口；本测试不链接面板栈实现。
namespace qol::features::core_ui {
void ui_panels_request_back() {}
int ui_panels_depth() { return 1; }
bool ui_panels_has_original_popup_baseline() { return false; }
}  // namespace qol::features::core_ui

namespace {

struct FakeControl {
    uint32_t kind = 0;
    FakeControl* parent = nullptr;
    std::vector<FakeControl*> children;
    bool visible = false;
    qol::contracts::UiRect rect{};
    char text[64] = {};
    qol::contracts::QolUiClickFn click = nullptr;
    void* click_user = nullptr;
    qol::contracts::QolUiDrawFn draw = nullptr;
    void* control_data = nullptr;
    void* control_proc = nullptr;
    int32_t control_user_type = 0;
    bool enabled = true;
    bool pressed = false;
};

struct FillRectCall {
    qol::contracts::UiRect rect;
    uint32_t color;
};

struct OverlayCall {
    qol::contracts::UiRect rect;
    uint32_t color;
    uint32_t alpha;
};

struct ScaledPartCall {
    qol::contracts::QolUiHandle group;
    qol::contracts::QolUiHandle part;
    int32_t x;
    int32_t y;
    int32_t type;
    int32_t flip;
    int32_t scale;
};

int g_clicks = 0;
int g_changes = 0;
int g_scrolls = 0;
int32_t g_last_value = -1;
int32_t g_last_offset = -1;
qol::contracts::UiRect g_last_styled_container{};
uint32_t g_last_styled_fill = 0;
uint32_t g_last_styled_alpha = 0;
int g_styled_container_draws = 0;
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
uint8_t g_native_button_pressed = 0;
int g_native_label_draws = 0;
qol::contracts::UiRect g_native_label_rect{};
std::string g_native_label_text;
uint32_t g_native_label_color = 0;
int32_t g_native_label_font = -1;
uint32_t g_paint_text_color = 0;
std::vector<FillRectCall> g_fill_rect_calls;
std::vector<OverlayCall> g_overlay_calls;
std::vector<ScaledPartCall> g_scaled_part_calls;
int32_t g_scaled_image_width = 40;
int32_t g_scaled_image_height = 40;
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
    if (control->parent != nullptr) control->parent->children.push_back(control);
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
    auto* control = static_cast<FakeControl*>(handle);
    control->enabled = enabled != 0;
    if (!control->enabled) control->pressed = false;
    return ok();
}

QolResult draw_native_text_button_background(qol::contracts::UiRect rect, uint8_t pressed) {
    if (pressed > 1) return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    ++g_native_button_draws;
    g_native_button_rect = rect;
    g_native_button_pressed = pressed;
    return ok();
}

QolResult draw_native_text_button_label(qol::contracts::UiRect rect, const char* text,
                                        uint32_t color, int32_t font) {
    if (text == nullptr || (font != 0 && font != 1)) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    ++g_native_label_draws;
    g_native_label_rect = rect;
    g_native_label_text = text;
    g_native_label_color = color;
    g_native_label_font = font;
    return ok();
}

QolResult draw_control_alpha_overlay(qol::contracts::QolUiHandle handle,
                                     qol::contracts::UiRect rect, uint32_t color,
                                     uint32_t alpha) {
    if (handle == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    g_overlay_calls.push_back({rect, color, alpha});
    return ok();
}

QolResult delete_control(qol::contracts::QolUiHandle handle) {
    if (handle == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    auto* control = static_cast<FakeControl*>(handle);
    if (control->parent != nullptr) {
        auto& siblings = control->parent->children;
        for (auto it = siblings.begin(); it != siblings.end(); ++it) {
            if (*it == control) {
                siblings.erase(it);
                break;
            }
        }
    }
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
        control->pressed = inside && control->enabled;
        *consumed = inside ? 1u : 0u;
    } else if (event == 0x19) {
        if (!inside) control->pressed = false;
        *consumed = inside ? 1u : 0u;
    } else if (event == 0x18) {
        if (control->enabled && control->pressed && inside && control->click != nullptr) {
            control->click(control->click_user);
        }
        control->pressed = false;
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
    *out_pressed = static_cast<FakeControl*>(handle)->pressed ? 1 : 0;
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
QolResult paint_draw(const qol::contracts::UiTextButtonPaintDesc* desc, const char*, uint32_t color,
                     uint8_t pressed) {
    g_paint = *desc;
    g_paint_pressed = pressed;
    g_paint_text_color = color;
    return ok();
}
QolResult menu_rect_available(qol::contracts::QolUiHandle, qol::contracts::UiRect rect,
                               uint8_t* out) {
    g_menu_guard_rect = rect;
    *out = g_menu_available;
    return ok();
}
int g_fill_count = 0;
QolResult fill_rect(qol::contracts::UiRect rect, uint32_t color) {
    ++g_fill_count;
    g_fill_rect_calls.push_back({rect, color});
    return ok();
}

struct ShapeDraw {
    qol::contracts::UiRect rect;
    uint32_t color;
    uint32_t alpha;
};
std::vector<ShapeDraw> g_shape_draws;
QolResult fill_rect_alpha(qol::contracts::UiRect rect, uint32_t color, uint32_t alpha) {
    g_shape_draws.push_back({rect, color, alpha});
    return ok();
}

bool shape_spans_are_disjoint(uint32_t alpha, uint32_t border, uint32_t fill,
                              size_t* out_border_pixels, size_t* out_fill_pixels) {
    std::set<std::pair<int64_t, int64_t>> painted;
    size_t border_pixels = 0;
    size_t fill_pixels = 0;
    for (const ShapeDraw& span : g_shape_draws) {
        if (span.alpha != alpha || span.rect.w <= 0 || span.rect.h <= 0) return false;
        for (int64_t y = span.rect.y; y < span.rect.y + span.rect.h; ++y) {
            for (int64_t x = span.rect.x; x < span.rect.x + span.rect.w; ++x) {
                if (!painted.emplace(x, y).second) return false;
                if (span.color == border) ++border_pixels;
                else if (span.color == fill) ++fill_pixels;
                else return false;
            }
        }
    }
    if (out_border_pixels != nullptr) *out_border_pixels = border_pixels;
    if (out_fill_pixels != nullptr) *out_fill_pixels = fill_pixels;
    return border_pixels > 0 && fill_pixels > 0;
}


FakeControl* g_fake_equip_bag_root = nullptr;

QolResult find_root(qol::contracts::QolUiRootKind kind, qol::contracts::QolUiHandle* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    *out = kind == qol::contracts::QolUiRootKind::kEquipBagContainer ? g_fake_equip_bag_root
                                                                    : nullptr;
    return *out == nullptr ? qol::contracts::error(kQolErrNotFound, "missing", 7) : ok();
}

QolResult child_count(qol::contracts::QolUiHandle handle, uint32_t* out) {
    if (handle == nullptr || out == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    *out = static_cast<uint32_t>(static_cast<FakeControl*>(handle)->children.size());
    return ok();
}

QolResult child_at(qol::contracts::QolUiHandle handle, uint32_t index,
                   qol::contracts::QolUiHandle* out) {
    if (handle == nullptr || out == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    const auto& children = static_cast<FakeControl*>(handle)->children;
    if (index >= children.size()) return qol::contracts::error(kQolErrNotFound, "missing", 7);
    *out = children[index];
    return ok();
}

QolResult set_control_show(qol::contracts::QolUiHandle handle, uint8_t show) {
    if (handle == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    static_cast<FakeControl*>(handle)->visible = show != 0;
    return ok();
}

QolResult click_binding_is_current(qol::contracts::QolUiHandle handle,
                                   qol::contracts::QolUiClickFn expected, void* owner,
                                   int32_t* out_live) {
    if (handle == nullptr || expected == nullptr || owner == nullptr || out_live == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    auto* control = static_cast<FakeControl*>(handle);
    *out_live = control->click == expected && control->click_user == owner ? 1 : 0;
    return ok();
}

QolResult image_unit_load(int32_t) { return ok(); }

QolResult draw_part_scaled(qol::contracts::QolUiHandle group, int32_t x, int32_t y,
                           qol::contracts::QolUiHandle part, int32_t type, int32_t flip,
                           int32_t scale) {
    g_scaled_part_calls.push_back({group, part, x, y, type, flip, scale});
    return ok();
}

QolResult control_type(qol::contracts::QolUiHandle handle, int32_t* out_type) {
    if (handle == nullptr || out_type == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    *out_type = 3;
    return ok();
}

QolResult control_user_type(qol::contracts::QolUiHandle handle, int32_t* out_type) {
    if (handle == nullptr || out_type == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    *out_type = static_cast<FakeControl*>(handle)->control_user_type;
    return ok();
}

QolResult control_data(qol::contracts::QolUiHandle handle, qol::contracts::QolUiHandle* out) {
    if (handle == nullptr || out == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    *out = static_cast<FakeControl*>(handle)->control_data;
    return *out == nullptr ? qol::contracts::error(kQolErrNotFound, "missing", 7) : ok();
}

QolResult control_proc_matches(qol::contracts::QolUiHandle handle,
                               qol::contracts::QolUiControlProcFn expected, int32_t* out) {
    if (handle == nullptr || expected == nullptr || out == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    *out = static_cast<FakeControl*>(handle)->control_proc ==
            reinterpret_cast<void*>(expected) ? 1 : 0;
    return ok();
}

QolResult set_control_proc(qol::contracts::QolUiHandle handle,
                           qol::contracts::QolUiControlProcFn proc) {
    if (handle == nullptr || proc == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    static_cast<FakeControl*>(handle)->control_proc = reinterpret_cast<void*>(proc);
    return ok();
}

QolResult allocate_control_data(uint32_t size, qol::contracts::QolUiHandle* out) {
    if (size == 0 || out == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    *out = std::calloc(1, size);
    return *out == nullptr ? qol::contracts::error(kQolErrNotReady, "oom", 3) : ok();
}

QolResult set_control_data(qol::contracts::QolUiHandle handle, qol::contracts::QolUiHandle data) {
    if (handle == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    static_cast<FakeControl*>(handle)->control_data = data;
    return ok();
}

QolResult draw_container_frame(qol::contracts::UiRect rect, uint32_t fill_rgb565,
                               uint32_t alpha) {
    g_last_styled_container = rect;
    g_last_styled_fill = fill_rgb565;
    g_last_styled_alpha = alpha;
    ++g_styled_container_draws;
    return ok();
}

int g_fake_icon_group = 0;
int g_fake_icon_loc = 0;
int g_fake_arrow_group = 0;
std::array<int, 4> g_fake_arrow_locs{};

QolResult image_group(int32_t unit, qol::contracts::QolUiHandle* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    if (unit == 0x16) {
        *out = &g_fake_icon_group;
        return ok();
    }
    if (unit == 0x59) {
        *out = &g_fake_arrow_group;
        return ok();
    }
    if (unit != 0x0f) return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    *out = &g_fake_group;
    return ok();
}

QolResult image_loc(int32_t unit, int32_t loc, qol::contracts::QolUiHandle* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    if (unit == 0x16 && loc == 0) {
        *out = &g_fake_icon_loc;
        return ok();
    }
    if (unit == 0x59) {
        constexpr int32_t locations[] = {0x09, 0x0a, 0x0b, 0x0c};
        for (size_t i = 0; i < 4; ++i) {
            if (loc == locations[i]) {
                *out = &g_fake_arrow_locs[i];
                return ok();
            }
        }
        return qol::contracts::error(kQolErrNotFound, "missing", 7);
    }
    if (unit != 0x0f) return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
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
    if (out_width == nullptr || out_height == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    if ((unit == 0x16 && loc == 0) ||
        (unit == 0x59 && (loc == 0x09 || loc == 0x0a || loc == 0x0b || loc == 0x0c))) {
        *out_width = g_scaled_image_width;
        *out_height = g_scaled_image_height;
        return ok();
    }
    if (unit != 0x0f) return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
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
    adapter.major = qol::contracts::kUiAdapterMajor;
    adapter.minor = qol::contracts::kUiAdapterMinor;
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
    adapter.draw_container_frame = &draw_container_frame;
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
    adapter.draw_control_alpha_overlay = &draw_control_alpha_overlay;
    adapter.delete_control = &delete_control;
    adapter.control_tree_event = &control_tree_event;
    adapter.find_root = &find_root;
    adapter.child_count = &child_count;
    adapter.child_at = &child_at;
    adapter.set_control_show = &set_control_show;
    adapter.click_binding_is_current = &click_binding_is_current;
    adapter.control_proc_matches = &control_proc_matches;
    adapter.control_type = &control_type;
    adapter.control_user_type = &control_user_type;
    adapter.control_data = &control_data;
    adapter.set_control_proc = &set_control_proc;
    adapter.allocate_control_data = &allocate_control_data;
    adapter.set_control_data = &set_control_data;
    adapter.image_unit_load = &image_unit_load;
    adapter.draw_part_scaled = &draw_part_scaled;
    adapter.fill_rect_alpha = &fill_rect_alpha;
    adapter.layout_native_text_button = &paint_layout;
    adapter.draw_native_text_button = &paint_draw;
    adapter.main_menu_replacement_rect_available = &menu_rect_available;
    return adapter;
}

const qol::contracts::QolUiAdapter kFakeAdapter = make_fake_adapter();

}  // namespace

QolResult anchored_layout(qol::contracts::QolUiHandle, qol::contracts::UiRect* out, void*) {
    *out = {5, 6, 20, 20};
    return ok();
}

uint64_t raw_tab_proc(void*, uint64_t, void*, void*) { return 0; }

int g_raw_creates = 0;
QolResult raw_tab_create(qol::contracts::QolUiHandle parent, qol::contracts::UiRect rect, void*,
                         qol::contracts::QolUiHandle* out) {
    if (parent == nullptr || out == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    ++g_raw_creates;
    auto* control = new FakeControl();
    control->parent = static_cast<FakeControl*>(parent);
    control->rect = rect;
    control->control_proc = reinterpret_cast<void*>(&raw_tab_proc);
    control->control_user_type = 2;
    void* data = std::calloc(1, 16);
    control->control_data = data;
    control->parent->children.push_back(control);
    *out = control;
    return ok();
}

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

    // Minor 22 shape primitives use RGB565 + 0..100 alpha and bounded logical geometry.
    {
        using ShapeDesc = qol::contracts::UiShapeDesc;
        static_assert(std::is_standard_layout<ShapeDesc>::value, "shape desc must be POD-like");
        static_assert(std::is_trivial<ShapeDesc>::value, "shape desc must be trivial");
        ShapeDesc shape{{10, 20, 12, 10}, 0x1234, 0x5678, 60, 2, 0};
        g_shape_draws.clear();
        CHECK(components->draw_rect(&shape).ok == kQolOk);
        CHECK(g_shape_draws.size() == 5);
        CHECK(g_shape_draws[0].rect.x == 10 && g_shape_draws[0].rect.y == 20 &&
              g_shape_draws[0].rect.w == 12 && g_shape_draws[0].rect.h == 2 &&
              g_shape_draws[0].color == 0x5678 && g_shape_draws[0].alpha == 60);
        CHECK(g_shape_draws[4].rect.x == 12 && g_shape_draws[4].rect.y == 22 &&
              g_shape_draws[4].rect.w == 8 && g_shape_draws[4].rect.h == 6 &&
              g_shape_draws[4].color == 0x1234 && g_shape_draws[4].alpha == 60);

        // Golden radius-4 mask: per-row insets [2,1,0,0,0,0,0,0,0,0,1,2].
        shape = {{30, 40, 12, 12}, 0x001f, 0xf800, 100, 0, 4};
        g_shape_draws.clear();
        CHECK(components->draw_rounded_rect(&shape).ok == kQolOk);
        const int64_t expected_insets[] = {2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2};
        bool rounded_mask[12][12]{};
        for (const ShapeDraw& span : g_shape_draws) {
            CHECK(span.alpha == 100 && span.color == 0x001f);
            for (int64_t y = span.rect.y; y < span.rect.y + span.rect.h; ++y) {
                const int64_t row = y - 40;
                CHECK(row >= 0 && row < 12);
                for (int64_t x = span.rect.x; x < span.rect.x + span.rect.w; ++x) {
                    CHECK(x >= 30 && x < 42 && !rounded_mask[row][x - 30]);
                    rounded_mask[row][x - 30] = true;
                }
            }
        }
        for (int y = 0; y < 12; ++y) {
            for (int x = 0; x < 12; ++x) {
                const bool expected_pixel = x >= expected_insets[y] &&
                    x < 12 - expected_insets[y];
                CHECK(rounded_mask[y][x] == expected_pixel);
                CHECK(rounded_mask[y][x] == rounded_mask[11 - y][x]);
                CHECK(rounded_mask[y][x] == rounded_mask[y][11 - x]);
            }
        }
        shape = {{80, 90, 40, 40}, 0x001f, 0xf800, 100, 0, 18};
        g_shape_draws.clear();
        CHECK(components->draw_rounded_rect(&shape).ok == kQolOk);
        bool radius18_top_row = false;
        for (const ShapeDraw& span : g_shape_draws) {
            if (span.rect.y == 90) {
                radius18_top_row = span.rect.x == 94 && span.rect.w == 12 && span.rect.h == 1;
            }
        }
        CHECK(radius18_top_row);

        shape = {{50, 60, 9, 9}, 0x07e0, 0xf800, 75, 1, 0};
        g_shape_draws.clear();
        CHECK(components->draw_circle(&shape).ok == kQolOk);
        CHECK(!g_shape_draws.empty() && g_shape_draws.front().rect.y == 60 &&
              g_shape_draws.front().rect.h == 1 && g_shape_draws.front().rect.w < 9 &&
              g_shape_draws.front().color == 0xf800);
        size_t border_pixels = 0, fill_pixels = 0;
        CHECK(shape_spans_are_disjoint(75, 0xf800, 0x07e0,
                                       &border_pixels, &fill_pixels));
        CHECK(border_pixels > 0 && fill_pixels > 0);
        for (const ShapeDraw& draw : g_shape_draws) {
            CHECK(draw.alpha == 75);
        }

        // At translucent alpha, all three shapes partition their mask into disjoint spans;
        // no border or fill pixel is submitted twice.
        shape = {{100, 110, 12, 10}, 0x001f, 0xf800, 48, 2, 0};
        g_shape_draws.clear();
        CHECK(components->draw_rect(&shape).ok == kQolOk);
        CHECK(shape_spans_are_disjoint(48, 0xf800, 0x001f,
                                       &border_pixels, &fill_pixels));
        CHECK(border_pixels == 72 && fill_pixels == 48);

        shape = {{100, 110, 12, 12}, 0x001f, 0xf800, 48, 1, 4};
        g_shape_draws.clear();
        CHECK(components->draw_rounded_rect(&shape).ok == kQolOk);
        CHECK(shape_spans_are_disjoint(48, 0xf800, 0x001f,
                                       &border_pixels, &fill_pixels));

        shape = {{100, 110, 9, 9}, 0x001f, 0xf800, 48, 1, 0};
        g_shape_draws.clear();
        CHECK(components->draw_circle(&shape).ok == kQolOk);
        CHECK(shape_spans_are_disjoint(48, 0xf800, 0x001f,
                                       &border_pixels, &fill_pixels));

        const size_t draws_before_invalid = g_shape_draws.size();
        shape.rect = {INT32_MAX, 0, 2, 2};
        CHECK(components->draw_rect(&shape).code == kQolErrInvalidArgument);
        shape.rect = {0, 0, 12, 8};
        shape.radius = 5;
        CHECK(components->draw_rounded_rect(&shape).code == kQolErrInvalidArgument);
        shape.radius = 0;
        shape.alpha = 101;
        CHECK(components->draw_circle(&shape).code == kQolErrInvalidArgument);
        shape.alpha = 100;
        shape.rect.w = 10;
        CHECK(components->draw_circle(&shape).code == kQolErrInvalidArgument);
        shape.rect = {0, 0, 600, 600};
        shape.radius = 257;
        CHECK(components->draw_rounded_rect(&shape).code == kQolErrInvalidArgument);
        shape.rect = {0, 0, 514, 514};
        shape.radius = 0;
        CHECK(components->draw_circle(&shape).code == kQolErrInvalidArgument);
        CHECK(g_shape_draws.size() == draws_before_invalid);
    }


    // anchored_button：root 存活时只挂载一次；根子树被游戏重建后只注销记录并重建，
    // 绝不对失效 native 调 delete_control；root 消失时清空状态、不触碰 native。
    {
        FakeControl bag_root{};
        bag_root.visible = true;
        bag_root.rect = {100, 200, 300, 300};
        g_fake_equip_bag_root = &bag_root;
        qol::contracts::QolUiAnchoredButton anchored{};
        qol::contracts::UiIconButtonDesc icon_desc{{0, 0, 10, 10}, 0x16, 0, 1, 1, "x", &on_click,
                                                   nullptr, 0};
        qol::contracts::UiAnchoredButtonDesc anchored_desc{
            static_cast<uint32_t>(qol::contracts::QolUiRootKind::kEquipBagContainer), icon_desc};
        uint8_t created = 0;
        const int deletes_before_anchor = g_native_button_deletes;
        CHECK(components->anchored_button_ensure(&anchored, &anchored_desc, &anchored_layout,
                                                 nullptr, &created).ok == kQolOk);
        CHECK(created == 1 && anchored.mounted == 1 && anchored.handle != nullptr);
        CHECK(bag_root.children.size() == 1 && bag_root.children[0] == anchored.handle);
        CHECK(static_cast<FakeControl*>(anchored.handle)->rect.w == 20);
        CHECK(static_cast<FakeControl*>(anchored.handle)->visible);
        const qol::contracts::QolUiHandle first = anchored.handle;
        CHECK(components->anchored_button_ensure(&anchored, &anchored_desc, &anchored_layout,
                                                 nullptr, &created).ok == kQolOk);
        CHECK(created == 0 && anchored.handle == first);
        // 模拟游戏销毁容器子树：记录仍在，但句柄已不在子链表。
        bag_root.children.clear();
        CHECK(components->anchored_button_ensure(&anchored, &anchored_desc, &anchored_layout,
                                                 nullptr, &created).ok == kQolOk);
        CHECK(created == 1 && anchored.handle != nullptr);
        CHECK(components->set_rect(first, {1, 2, 3, 4}).ok == kQolErrNotFound);
        CHECK(g_native_button_deletes == deletes_before_anchor);
        CHECK(components->anchored_button_draw(&anchored).ok == kQolOk);
        CHECK(components->anchored_button_set_visible(&anchored, 0).ok == kQolOk);
        CHECK(!static_cast<FakeControl*>(anchored.handle)->visible);
        CHECK(components->anchored_button_release(&anchored).ok == kQolOk);
        CHECK(anchored.handle == nullptr && anchored.mounted == 0);
        CHECK(g_native_button_deletes == deletes_before_anchor);
        g_fake_equip_bag_root = nullptr;
        CHECK(components->anchored_button_ensure(&anchored, &anchored_desc, &anchored_layout,
                                                 nullptr, &created).ok == kQolOk);
        CHECK(anchored.handle == nullptr);
    }


    // anchored_control：任意原生控件（自定义 CO_PROC + 自带 data）的挂载与失效重建。
    // 关键场景：游戏销毁子树后，地址被复用成「别的 proc」的控件——身份不符必须重建，
    // 且绝不 delete_control。
    {
        FakeControl raw_root{};
        raw_root.rect = {0, 0, 200, 200};
        g_fake_equip_bag_root = &raw_root;
        qol::contracts::QolUiAnchoredControl anchored{};
        qol::contracts::UiAnchoredControlDesc control_desc{
            static_cast<uint32_t>(qol::contracts::QolUiRootKind::kEquipBagContainer), &raw_tab_proc,
            &anchored_layout, &raw_tab_create, nullptr};
        uint8_t created = 0;
        const int raw_deletes_before = g_native_button_deletes;
        CHECK(components->anchored_control_ensure(&anchored, &control_desc, &created).ok == kQolOk);
        CHECK(created == 1 && anchored.mounted == 1 && anchored.handle != nullptr);
        CHECK(anchored.expected_proc == reinterpret_cast<void*>(&raw_tab_proc));
        const qol::contracts::QolUiHandle raw_first = anchored.handle;
        CHECK(components->anchored_control_ensure(&anchored, &control_desc, &created).ok == kQolOk);
        CHECK(created == 0 && anchored.handle == raw_first);
        // 地址复用成别的 proc：身份不符 → 注销记录并重建。
        raw_root.children.clear();
        auto* reused = new FakeControl();
        reused->parent = &raw_root;
        reused->control_proc = reinterpret_cast<void*>(&on_click);
        reused->control_user_type = 2;
        reused->control_data = std::calloc(1, 16);
        raw_root.children.push_back(reused);
        CHECK(components->anchored_control_ensure(&anchored, &control_desc, &created).ok == kQolOk);
        CHECK(created == 1 && anchored.handle != reused);
        CHECK(g_native_button_deletes == raw_deletes_before);
        // detach：只有证明存活时才交出句柄，并清空状态。
        qol::contracts::QolUiHandle detached = nullptr;
        uint8_t live = 0;
        CHECK(components->anchored_control_detach(&anchored, &detached, &live).ok == kQolOk);
        CHECK(live == 1 && detached != nullptr && anchored.mounted == 0);
        CHECK(components->anchored_control_detach(&anchored, &detached, &live).ok == kQolOk);
        CHECK(live == 0 && detached == nullptr);
        g_fake_equip_bag_root = nullptr;
    }

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

    // forget_native：native 由外部（游戏控件树）销毁时只注销记录，不调用 delete_control；
    // 注销后同一句柄的所有组件入口都必须 NotFound，不能再次删除该原生地址。
    {
        qol::contracts::QolUiHandle foreign_root = nullptr;
        CHECK(components->panel_create(&panel_desc, nullptr, &foreign_root).ok == kQolOk);
        qol::contracts::UiButtonDesc foreign_desc{{0, 0, 10, 10}, "foreign", 7, &on_click,
                                                   foreign_root};
        qol::contracts::QolUiHandle foreign_button = nullptr;
        CHECK(components->button_create(&foreign_desc, foreign_root, &foreign_button).ok ==
              kQolOk);
        const int deletes_before_forget = g_native_button_deletes;
        CHECK(components->forget_native(foreign_button).ok == kQolOk);
        CHECK(g_native_button_deletes == deletes_before_forget);
        CHECK(components->forget_native(foreign_button).ok == kQolErrNotFound);
        CHECK(components->set_rect(foreign_button, {1, 2, 3, 4}).ok == kQolErrNotFound);
        CHECK(components->destroy(foreign_button).ok == kQolErrNotFound);
        // 父组件记录保持可用：注销只作用于给定句柄。
        CHECK(components->set_rect(foreign_root, {0, 0, 20, 20}).ok == kQolOk);
        CHECK(components->destroy(foreign_root).ok == kQolOk);
    }

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
    // 旧 draw_container / draw_container_ornamented 已随 Major 2 移除；容器统一走 styled，
    // 参数边界在此验证（48 最小尺寸、整数溢出、ornament 扩框下界）。
    const int styled_draws_before_boundary = g_styled_container_draws;
    const int parts_before_boundary = g_part_draws;
    qol::contracts::UiContainerStyleDesc boundary{{10, 20, 320, 140}, 0, 60u, 0};
    boundary.rect = {0, 0, 47, 100};
    CHECK(components->draw_container_styled(&boundary).code == kQolErrInvalidArgument);
    boundary.rect = {INT64_MAX, 0, 100, 100};
    CHECK(components->draw_container_styled(&boundary).code == kQolErrInvalidArgument);
    boundary.rect = {INT32_MAX, 0, 48, 100};
    CHECK(components->draw_container_styled(&boundary).code == kQolErrInvalidArgument);
    boundary.rect = {INT32_MIN, 0, 320, 140};
    boundary.ornamented = 1;  // ornament 扩框要求 x/y >= INT32_MIN+2。
    CHECK(components->draw_container_styled(&boundary).code == kQolErrInvalidArgument);
    boundary.rect = {10, 20, 320, 140};
    boundary.ornamented = 0;
    CHECK(components->draw_container_styled(&boundary).ok == kQolOk);
    CHECK(g_styled_container_draws == styled_draws_before_boundary + 1 &&
          g_last_styled_container.x == 10 &&
          g_last_styled_container.y == 20 && g_last_styled_container.w == 320 &&
          g_last_styled_container.h == 140 && g_last_styled_fill == 0u &&
          g_last_styled_alpha == 60u);
    CHECK(g_part_draws == parts_before_boundary);  // ornamented=0 不查询也不绘制角装饰。

    qol::contracts::UiContainerStyleDesc styled{{20, 30, 1318, 604}, 0x1904u, 60u, 0};
    CHECK(components->draw_container_styled(&styled).ok == kQolOk);
    CHECK(g_styled_container_draws == styled_draws_before_boundary + 2 &&
          g_last_styled_container.x == 20 &&
          g_last_styled_container.y == 30 && g_last_styled_container.w == 1318 &&
          g_last_styled_container.h == 604 && g_last_styled_fill == 0x1904u &&
          g_last_styled_alpha == 60u && g_part_draws == parts_before_boundary);

    styled.ornamented = 1;
    g_part_draws = 0;
    CHECK(components->draw_container_styled(&styled).ok == kQolOk);
    CHECK(g_styled_container_draws == styled_draws_before_boundary + 3 && g_part_draws == 4);
    CHECK(g_part_loc[0] == 0 && g_part_loc[1] == 1 && g_part_loc[2] == 2 &&
          g_part_loc[3] == 3);
    styled.rect = {0, 0, 48, 48};
    styled.fill_rgb565 = 0xffffu;
    styled.alpha = 0;
    styled.ornamented = 0;
    CHECK(components->draw_container_styled(&styled).ok == kQolOk);
    CHECK(g_styled_container_draws == styled_draws_before_boundary + 4 &&
          g_last_styled_fill == 0xffffu &&
          g_last_styled_alpha == 0u && g_last_styled_container.w == 48 &&
          g_last_styled_container.h == 48);
    CHECK(components->draw_container_styled(nullptr).code == kQolErrInvalidArgument);
    styled.fill_rgb565 = 0x10000u;
    CHECK(components->draw_container_styled(&styled).code == kQolErrInvalidArgument);
    styled.fill_rgb565 = 0;
    styled.alpha = 101;
    CHECK(components->draw_container_styled(&styled).code == kQolErrInvalidArgument);
    styled.alpha = 100;
    styled.ornamented = 2;
    CHECK(components->draw_container_styled(&styled).code == kQolErrInvalidArgument);
    styled.ornamented = 0;
    styled.rect = {INT32_MAX, 0, 48, 48};
    CHECK(components->draw_container_styled(&styled).code == kQolErrInvalidArgument);
    CHECK(g_styled_container_draws == styled_draws_before_boundary + 4);

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
    const int clicks_before_text_interactions = g_clicks;
    const auto dispatch_text_event = [&](uint64_t event, int64_t x, int64_t y) {
        int64_t point[] = {x, y};
        uint32_t consumed = 0;
        CHECK(components->dispatch_native_event(text_button, event,
            reinterpret_cast<uintptr_t>(point), 0, &consumed).ok == kQolOk);
        return consumed;
    };
    auto* fake_text_button = static_cast<FakeControl*>(text_button);
    CHECK(components->text_button_set_enabled(text_button, 0).ok == kQolOk);
    CHECK(dispatch_text_event(0x17, 120, 80) == 1);
    CHECK(dispatch_text_event(0x18, 120, 80) == 1);
    CHECK(g_clicks == clicks_before_text_interactions && !fake_text_button->enabled);
    CHECK(components->text_button_set_enabled(text_button, 1).ok == kQolOk);
    CHECK(dispatch_text_event(0x17, 120, 80) == 1);
    CHECK(dispatch_text_event(0x18, 120, 80) == 1);
    CHECK(g_clicks == clicks_before_text_interactions + 1 && fake_text_button->enabled);

    // Active is selection styling; only the native event tree changes control_pressed.
    const size_t overlays_before_active_draws = g_overlay_calls.size();
    CHECK(components->text_button_set_active(text_button, 1).ok == kQolOk);
    CHECK(components->text_button_draw(text_button).ok == kQolOk);
    CHECK(!fake_text_button->pressed && g_native_button_pressed == 0 &&
          g_native_label_color == qol::contracts::ui_theme::kTextGold);
    CHECK(g_overlay_calls.size() == overlays_before_active_draws);
    CHECK(dispatch_text_event(0x17, 120, 80) == 1 && fake_text_button->pressed);
    CHECK(components->text_button_draw(text_button).ok == kQolOk);
    CHECK(g_native_button_pressed == 1 &&
          g_native_label_color == qol::contracts::ui_theme::kTextNormal);
    CHECK(dispatch_text_event(0x19, 500, 500) == 0 && !fake_text_button->pressed);
    CHECK(components->text_button_draw(text_button).ok == kQolOk);
    CHECK(g_native_button_pressed == 0 &&
          g_native_label_color == qol::contracts::ui_theme::kTextGold);
    CHECK(components->text_button_set_active(text_button, 0).ok == kQolOk);
    CHECK(components->text_button_draw(text_button).ok == kQolOk);
    CHECK(g_native_label_color == qol::contracts::ui_theme::kTextDisabled &&
          g_overlay_calls.size() == overlays_before_active_draws + 1);
    CHECK(g_overlay_calls.back().alpha == 28 &&
          g_overlay_calls.back().color == 0xFF000000u &&
          g_overlay_calls.back().rect.w == text_desc.rect.w &&
          g_overlay_calls.back().rect.h == text_desc.rect.h);
    CHECK(dispatch_text_event(0x17, 120, 80) == 1 && fake_text_button->pressed);
    CHECK(components->text_button_draw(text_button).ok == kQolOk);
    CHECK(g_native_button_pressed == 1 &&
          g_native_label_color == qol::contracts::ui_theme::kTextNormal);
    CHECK(dispatch_text_event(0x19, 500, 500) == 0 && !fake_text_button->pressed);
    CHECK(components->text_button_set_active(text_button, 1).ok == kQolOk);
    CHECK(components->text_button_draw(text_button).ok == kQolOk);
    CHECK(g_native_label_color == qol::contracts::ui_theme::kTextGold &&
          g_clicks == clicks_before_text_interactions + 1);

    int64_t touch_inside[] = {120, 80};
    int64_t touch_outside[] = {500, 500};
    uint32_t consumed = 0;
    CHECK(components->dispatch_native_event(text_button, 0x17,
        reinterpret_cast<uintptr_t>(touch_inside), 0, &consumed).ok == kQolOk && consumed == 1);
    CHECK(components->dispatch_native_event(text_button, 0x18,
        reinterpret_cast<uintptr_t>(touch_inside), 0, &consumed).ok == kQolOk && consumed == 1);
    CHECK(g_clicks == clicks_before_text_interactions + 2);
    CHECK(components->dispatch_native_event(text_button, 0x17,
        reinterpret_cast<uintptr_t>(touch_inside), 0, &consumed).ok == kQolOk);
    CHECK(components->dispatch_native_event(text_button, 0x18,
        reinterpret_cast<uintptr_t>(touch_outside), 0, &consumed).ok == kQolOk);
    CHECK(g_clicks == clicks_before_text_interactions + 2);
    CHECK(components->dispatch_native_event(text_button, 0x17,
        reinterpret_cast<uintptr_t>(touch_outside), 0, &consumed).ok == kQolOk);
    CHECK(components->dispatch_native_event(text_button, 0x18,
        reinterpret_cast<uintptr_t>(touch_inside), 0, &consumed).ok == kQolOk);
    CHECK(g_clicks == clicks_before_text_interactions + 2);
    CHECK(components->destroy(text_button).ok == kQolOk);
    CHECK(g_native_button_deletes == 4);
    CHECK(components->destroy(text_button).code == kQolErrNotFound);

    const auto assert_button_factory_bounds = [&](auto factory, int64_t minimum_width,
                                                  int64_t minimum_height) {
        qol::contracts::UiButtonDesc too_narrow{{10, 12, minimum_width - 1, minimum_height},
            "边界", 0, &on_click, &g_clicks};
        qol::contracts::QolUiHandle candidate = nullptr;
        CHECK(factory(&too_narrow, text_root, &candidate).code == kQolErrInvalidArgument);
        CHECK(candidate == nullptr);
        qol::contracts::UiButtonDesc too_short{{10, 12, minimum_width, minimum_height - 1},
            "边界", 0, &on_click, &g_clicks};
        CHECK(factory(&too_short, text_root, &candidate).code == kQolErrInvalidArgument);
        CHECK(candidate == nullptr);
        qol::contracts::UiButtonDesc at_minimum{{10, 12, minimum_width, minimum_height},
            "边界", 0, &on_click, &g_clicks};
        CHECK(factory(&at_minimum, text_root, &candidate).code == kQolOk);
        CHECK(candidate != nullptr);
        CHECK(components->destroy(candidate).code == kQolOk);
    };
    assert_button_factory_bounds(components->text_button_create, 204, 48);
    assert_button_factory_bounds(components->capsule_text_button_create, 204, 48);
    assert_button_factory_bounds(components->decorated_text_button_create, 204, 48);
    assert_button_factory_bounds(components->compact_capsule_text_button_create, 140, 48);
    assert_button_factory_bounds(components->flat_text_button_create, 80, 42);

    qol::contracts::UiButtonDesc flat_desc{{20, 20, 80, 42}, "平面", 0,
                                             &on_click, &g_clicks};
    qol::contracts::QolUiHandle flat_button = nullptr;
    CHECK(components->flat_text_button_create(&flat_desc, text_root, &flat_button).code == kQolOk);
    auto* fake_flat_button = static_cast<FakeControl*>(flat_button);
    const size_t flat_fill_start = g_fill_rect_calls.size();
    CHECK(components->text_button_set_active(flat_button, 1).code == kQolOk);
    CHECK(components->text_button_draw(flat_button).code == kQolOk);
    CHECK(g_fill_rect_calls.size() == flat_fill_start + 5);
    CHECK(g_fill_rect_calls[flat_fill_start].color == 0xFF383430u &&
          g_last_text_rgb == (qol::contracts::ui_theme::kTextGold & 0x00ffffffu));
    int64_t flat_x = 0;
    int64_t flat_y = 0;
    CHECK(control_absolute_origin(flat_button, &flat_x, &flat_y).code == kQolOk);
    const int flat_click_base = g_clicks;
    int64_t flat_inside[] = {flat_x + 40, flat_y + 21};
    int64_t flat_outside[] = {flat_x + 100, flat_y + 80};
    uint32_t flat_consumed = 0;
    CHECK(components->dispatch_native_event(flat_button, 0x17,
        reinterpret_cast<uintptr_t>(flat_inside), 0, &flat_consumed).code == kQolOk &&
        flat_consumed == 1 && fake_flat_button->pressed);
    const size_t pressed_fill_start = g_fill_rect_calls.size();
    CHECK(components->text_button_draw(flat_button).code == kQolOk);
    CHECK(g_fill_rect_calls[pressed_fill_start].color == 0xFF474139u &&
          g_last_text_rgb == (qol::contracts::ui_theme::kTextNormal & 0x00ffffffu));
    CHECK(components->dispatch_native_event(flat_button, 0x19,
        reinterpret_cast<uintptr_t>(flat_outside), 0, &flat_consumed).code == kQolOk &&
        !fake_flat_button->pressed);
    CHECK(components->text_button_set_active(flat_button, 0).code == kQolOk);
    const size_t unselected_fill_start = g_fill_rect_calls.size();
    CHECK(components->text_button_draw(flat_button).code == kQolOk);
    CHECK(g_fill_rect_calls[unselected_fill_start].color == 0xFF24201Cu &&
          g_last_text_rgb == (qol::contracts::ui_theme::kTextNormal & 0x00ffffffu));
    CHECK(components->text_button_set_enabled(flat_button, 0).code == kQolOk);
    CHECK(components->dispatch_native_event(flat_button, 0x17,
        reinterpret_cast<uintptr_t>(flat_inside), 0, &flat_consumed).code == kQolOk);
    CHECK(components->dispatch_native_event(flat_button, 0x18,
        reinterpret_cast<uintptr_t>(flat_inside), 0, &flat_consumed).code == kQolOk);
    CHECK(g_clicks == flat_click_base && !fake_flat_button->enabled);
    CHECK(components->text_button_draw(flat_button).code == kQolOk);
    CHECK(g_last_text_rgb == (qol::contracts::ui_theme::kTextDisabled & 0x00ffffffu));
    CHECK(components->text_button_set_enabled(flat_button, 1).code == kQolOk);
    CHECK(components->dispatch_native_event(flat_button, 0x17,
        reinterpret_cast<uintptr_t>(flat_inside), 0, &flat_consumed).code == kQolOk);
    CHECK(components->dispatch_native_event(flat_button, 0x18,
        reinterpret_cast<uintptr_t>(flat_inside), 0, &flat_consumed).code == kQolOk);
    CHECK(g_clicks == flat_click_base + 1);
    CHECK(components->destroy(flat_button).code == kQolOk);

    const auto create_arrow = [&](qol::contracts::UiRect rect, uint32_t direction,
                                  uint8_t enabled, qol::contracts::QolUiHandle* out) {
        const qol::contracts::UiArrowButtonDesc desc{
            rect, direction, enabled, &on_click, &g_clicks};
        return components->arrow_button_create(&desc, text_root, out);
    };
    g_scaled_image_width = 100;
    g_scaled_image_height = 100;
    qol::contracts::QolUiHandle arrow = nullptr;
    CHECK(create_arrow({0, 0, 1, 1}, qol::contracts::kUiArrowLeft, 1, &arrow).code == kQolOk);
    g_scaled_part_calls.clear();
    CHECK(components->arrow_button_draw(arrow).code == kQolOk);
    CHECK(g_scaled_part_calls.size() == 1 && g_scaled_part_calls[0].scale == 1 &&
          g_scaled_part_calls[0].group == &g_fake_arrow_group &&
          g_scaled_part_calls[0].part == &g_fake_arrow_locs[0]);
    CHECK(components->destroy(arrow).code == kQolOk);

    g_scaled_image_width = 40;
    g_scaled_image_height = 40;
    arrow = nullptr;
    CHECK(create_arrow({0, 0, 160, 160}, qol::contracts::kUiArrowRight, 1, &arrow).code == kQolOk);
    g_scaled_part_calls.clear();
    CHECK(components->arrow_button_draw(arrow).code == kQolOk);
    CHECK(g_scaled_part_calls.size() == 1 && g_scaled_part_calls[0].scale == 400 &&
          g_scaled_part_calls[0].part == &g_fake_arrow_locs[2]);
    CHECK(components->destroy(arrow).code == kQolOk);

    g_scaled_image_width = 101;
    g_scaled_image_height = 101;
    arrow = nullptr;
    CHECK(create_arrow({0, 0, 1, 1}, qol::contracts::kUiArrowLeft, 1, &arrow).code == kQolOk);
    g_scaled_part_calls.clear();
    CHECK(components->arrow_button_draw(arrow).code == kQolErrInvalidArgument);
    CHECK(g_scaled_part_calls.empty());
    CHECK(components->destroy(arrow).code == kQolOk);

    g_scaled_image_width = 40;
    g_scaled_image_height = 40;
    arrow = nullptr;
    CHECK(create_arrow({0, 0, 161, 161}, qol::contracts::kUiArrowLeft, 1, &arrow).code == kQolOk);
    g_scaled_part_calls.clear();
    CHECK(components->arrow_button_draw(arrow).code == kQolErrInvalidArgument);
    CHECK(g_scaled_part_calls.empty());
    CHECK(components->destroy(arrow).code == kQolOk);

    qol::contracts::UiArrowButtonDesc disabled_arrow_desc{{10, 10, 40, 40},
        qol::contracts::kUiArrowRight, 0, &on_click, &g_clicks};
    arrow = nullptr;
    CHECK(components->arrow_button_create(&disabled_arrow_desc, text_root, &arrow).code == kQolOk);
    auto* fake_arrow = static_cast<FakeControl*>(arrow);
    CHECK(components->arrow_button_set_enabled(arrow, 0).code == kQolOk);
    int64_t arrow_origin_x = 0, arrow_origin_y = 0;
    CHECK(control_absolute_origin(arrow, &arrow_origin_x, &arrow_origin_y).code == kQolOk);
    const int arrow_click_base = g_clicks;
    int64_t arrow_inside[] = {arrow_origin_x + 20, arrow_origin_y + 20};
    CHECK(components->dispatch_native_event(arrow, 0x17,
        reinterpret_cast<uintptr_t>(arrow_inside), 0, &flat_consumed).code == kQolOk);
    CHECK(components->dispatch_native_event(arrow, 0x18,
        reinterpret_cast<uintptr_t>(arrow_inside), 0, &flat_consumed).code == kQolOk);
    CHECK(g_clicks == arrow_click_base && !fake_arrow->enabled);
    const size_t arrow_overlays = g_overlay_calls.size();
    g_scaled_part_calls.clear();
    CHECK(components->arrow_button_draw(arrow).code == kQolOk);
    CHECK(g_scaled_part_calls.size() == 1 && g_scaled_part_calls[0].scale == 100 &&
          g_scaled_part_calls[0].part == &g_fake_arrow_locs[2]);
    CHECK(g_overlay_calls.size() == arrow_overlays + 1 &&
          g_overlay_calls.back().alpha == 35 &&
          g_overlay_calls.back().rect.w == 40 && g_overlay_calls.back().rect.h == 40);
    CHECK(components->arrow_button_set_enabled(arrow, 1).code == kQolOk);
    CHECK(components->dispatch_native_event(arrow, 0x17,
        reinterpret_cast<uintptr_t>(arrow_inside), 0, &flat_consumed).code == kQolOk);
    g_scaled_part_calls.clear();
    CHECK(components->arrow_button_draw(arrow).code == kQolOk);
    CHECK(g_scaled_part_calls.size() == 1 && g_scaled_part_calls[0].part == &g_fake_arrow_locs[3]);
    CHECK(components->dispatch_native_event(arrow, 0x18,
        reinterpret_cast<uintptr_t>(arrow_inside), 0, &flat_consumed).code == kQolOk);
    CHECK(g_clicks == arrow_click_base + 1);
    CHECK(components->destroy(arrow).code == kQolOk);

    qol::contracts::UiIconButtonDesc icon_desc{{20, 20, 1, 1}, 0x16, 0, 1, 1,
        nullptr, &on_click, &g_clicks, 0};
    qol::contracts::QolUiHandle icon = nullptr;
    g_scaled_image_width = 100;
    g_scaled_image_height = 100;
    CHECK(components->icon_button_create(&icon_desc, text_root, &icon).code == kQolOk);
    g_scaled_part_calls.clear();
    CHECK(components->icon_button_draw(icon).code == kQolOk);
    CHECK(g_scaled_part_calls.size() == 1 && g_scaled_part_calls[0].scale == 1 &&
          g_scaled_part_calls[0].group == &g_fake_icon_group &&
          g_scaled_part_calls[0].part == &g_fake_icon_loc);
    CHECK(components->destroy(icon).code == kQolOk);

    icon_desc.rect = {20, 20, 160, 160};
    g_scaled_image_width = 40;
    g_scaled_image_height = 40;
    icon = nullptr;
    CHECK(components->icon_button_create(&icon_desc, text_root, &icon).code == kQolOk);
    g_scaled_part_calls.clear();
    CHECK(components->icon_button_draw(icon).code == kQolOk);
    CHECK(g_scaled_part_calls.size() == 1 && g_scaled_part_calls[0].scale == 400);
    CHECK(components->destroy(icon).code == kQolOk);

    icon_desc.rect = {20, 20, 1, 1};
    g_scaled_image_width = 101;
    g_scaled_image_height = 101;
    icon = nullptr;
    CHECK(components->icon_button_create(&icon_desc, text_root, &icon).code == kQolOk);
    g_scaled_part_calls.clear();
    CHECK(components->icon_button_draw(icon).code == kQolOk);
    CHECK(g_scaled_part_calls.empty());
    CHECK(components->destroy(icon).code == kQolOk);

    icon_desc.rect = {20, 20, 161, 161};
    g_scaled_image_width = 40;
    g_scaled_image_height = 40;
    icon = nullptr;
    CHECK(components->icon_button_create(&icon_desc, text_root, &icon).code == kQolOk);
    g_scaled_part_calls.clear();
    CHECK(components->icon_button_draw(icon).code == kQolOk);
    CHECK(g_scaled_part_calls.empty());  // Icons beyond 400% are omitted, not distorted.
    CHECK(components->destroy(icon).code == kQolOk);

    icon_desc.rect = {20, 20, 40, 40};
    icon_desc.enabled = 0;
    icon = nullptr;
    CHECK(components->icon_button_create(&icon_desc, text_root, &icon).code == kQolOk);
    auto* fake_icon = static_cast<FakeControl*>(icon);
    CHECK(components->icon_button_set_enabled(icon, 0).code == kQolOk);
    int64_t icon_origin_x = 0, icon_origin_y = 0;
    CHECK(control_absolute_origin(icon, &icon_origin_x, &icon_origin_y).code == kQolOk);
    const int icon_click_base = g_clicks;
    int64_t icon_inside[] = {icon_origin_x + 20, icon_origin_y + 20};
    CHECK(components->dispatch_native_event(icon, 0x17,
        reinterpret_cast<uintptr_t>(icon_inside), 0, &flat_consumed).code == kQolOk);
    CHECK(components->dispatch_native_event(icon, 0x18,
        reinterpret_cast<uintptr_t>(icon_inside), 0, &flat_consumed).code == kQolOk);
    CHECK(g_clicks == icon_click_base && !fake_icon->enabled);
    const size_t icon_overlays = g_overlay_calls.size();
    g_scaled_part_calls.clear();
    CHECK(components->icon_button_draw(icon).code == kQolOk);
    CHECK(g_scaled_part_calls.size() == 1 && g_scaled_part_calls[0].scale == 100 &&
          g_scaled_part_calls[0].part == &g_fake_icon_loc);
    CHECK(g_overlay_calls.size() == icon_overlays + 1 &&
          g_overlay_calls.back().alpha == 32 &&
          g_overlay_calls.back().rect.w == 40 && g_overlay_calls.back().rect.h == 40);
    CHECK(components->icon_button_set_enabled(icon, 1).code == kQolOk);
    CHECK(components->dispatch_native_event(icon, 0x17,
        reinterpret_cast<uintptr_t>(icon_inside), 0, &flat_consumed).code == kQolOk);
    CHECK(components->dispatch_native_event(icon, 0x18,
        reinterpret_cast<uintptr_t>(icon_inside), 0, &flat_consumed).code == kQolOk);
    CHECK(g_clicks == icon_click_base + 1);
    CHECK(components->destroy(icon).code == kQolOk);
    g_scaled_image_width = 40;
    g_scaled_image_height = 40;
    g_scaled_part_calls.clear();

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
    const int clicks_before_main_menu = g_clicks;
    const int native_button_draws_before_main_menu = g_native_button_draws;
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
    CHECK(g_native_button_draws == native_button_draws_before_main_menu + 1);
    CHECK(g_native_label_text == "模块设置");
    CHECK(g_native_button_rect.w == replacement_control->rect.w);
    CHECK(g_native_button_rect.h == replacement_control->rect.w * 48 / 220);
    CHECK(g_native_label_rect.x == g_native_button_rect.x &&
          g_native_label_rect.y == g_native_button_rect.y &&
          g_native_label_rect.h == g_native_button_rect.h);
    CHECK(replacement_control->rect.h == menu_button.rect.h);
    replacement_control->click(replacement_control->click_user);
    CHECK(g_clicks == clicks_before_main_menu + 1);

    const int deletions_before_unknown_state = g_native_button_deletes;
    g_main_menu_identity_known = false;
    CHECK(components->main_menu_text_button_is_live(replacement_menu_button,
        &replacement_live).code == kQolErrNotReady);
    CHECK(components->destroy(replacement_menu_button).code == kQolErrNotReady);
    CHECK(g_native_button_deletes == deletions_before_unknown_state);
    CHECK(g_main_menu_click_owner == replacement_control->click_user);
    replacement_control->click(replacement_control->click_user);
    CHECK(g_clicks == clicks_before_main_menu + 2);

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
    CHECK(g_clicks == clicks_before_main_menu + 3);
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
