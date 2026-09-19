#include "host/ui_adapter.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "game_access.h"
#include "game/access/ui_kit.h"
#include "game/access/ui_components.h"

using ControlButtonSetTextFn = void (*)(void* ctrl, char* text);
using ControlButtonSetDrawProcFn = void (*)(void* ctrl, void* proc);
using ControlButtonDrawFn = void (*)(void* ctrl);

extern ControlButtonSetTextFn fn_ctrl_btn_set_text;
extern ControlButtonSetDrawProcFn fn_ctrl_btn_set_draw_proc;
extern ControlButtonDrawFn fn_ctrl_btn_draw;

namespace qol::host {
namespace {

using QolResult = ::QolResult;
using contracts::QolUiAdapter;
using contracts::QolUiDrawFn;
using contracts::QolUiHandle;
using contracts::UiRect;

constexpr const char* kInvalidArgument = "invalid argument";
constexpr const char* kNotReady = "ui primitive not ready";
constexpr const char* kUnsupported = "ui primitive unsupported";

struct ClickBinding {
    contracts::QolUiClickFn callback;
    void* user;
};

std::mutex g_click_mtx;
std::unordered_map<QolUiHandle, ClickBinding> g_click_bindings;

QolResult invalid_argument() {
    return contracts::error(kQolErrInvalidArgument, kInvalidArgument, 16);
}

QolResult not_ready() {
    return contracts::error(kQolErrNotReady, kNotReady, 22);
}

QolResult unsupported() {
    return contracts::error(kQolErrUnsupported, kUnsupported, 24);
}

::UiRect game_rect(UiRect rect) {
    return ::UiRect{rect.x, rect.y, rect.w, rect.h};
}

UiRect contract_rect(::UiRect rect) {
    return UiRect{rect.x, rect.y, rect.w, rect.h};
}

QolResult create_control(uint32_t kind, QolUiHandle parent, QolUiHandle* out) {
    if (out == nullptr) return invalid_argument();
    *out = nullptr;

    // 0=root、1=button、2=label；组件描述符由 core.ui 负责填充属性。
    ::UiRect rect{};
    switch (kind) {
        case 0:
            if (parent != nullptr) return invalid_argument();
            *out = ui_create_root(rect);
            break;
        case 1:
            if (parent == nullptr) return invalid_argument();
            *out = ui_create_button(parent, rect, nullptr, nullptr, nullptr);
            break;
        case 2:
            if (parent == nullptr) return invalid_argument();
            *out = ui_create_label(parent, rect, nullptr, nullptr);
            break;
        default:
            return unsupported();
    }
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult set_rect(QolUiHandle control, UiRect rect) {
    if (control == nullptr) return invalid_argument();
    ui_set_rect(control, game_rect(rect));
    return contracts::ok();
}

QolResult get_rect(QolUiHandle control, UiRect* out) {
    if (control == nullptr || out == nullptr) return invalid_argument();
    ::UiRect rect{};
    if (!ui_get_rect(control, &rect)) return not_ready();
    *out = contract_rect(rect);
    return contracts::ok();
}

QolResult set_text(QolUiHandle control, const char* text) {
    if (control == nullptr || text == nullptr) return invalid_argument();
    if (fn_ctrl_btn_set_text == nullptr) return not_ready();
    fn_ctrl_btn_set_text(control, const_cast<char*>(text));
    return contracts::ok();
}

QolResult set_draw_proc(QolUiHandle control, QolUiDrawFn draw_proc) {
    if (control == nullptr || draw_proc == nullptr) return invalid_argument();
    if (fn_ctrl_btn_set_draw_proc == nullptr) return not_ready();
    fn_ctrl_btn_set_draw_proc(control, reinterpret_cast<void*>(draw_proc));
    return contracts::ok();
}

void click_proc_trampoline(void* control) {
    ClickBinding binding{};
    {
        std::lock_guard<std::mutex> lock(g_click_mtx);
        const auto it = g_click_bindings.find(control);
        if (it == g_click_bindings.end()) return;
        binding = it->second;
    }
    binding.callback(binding.user);
}

QolResult set_click_proc(QolUiHandle control, contracts::QolUiClickFn click_proc, void* user) {
    if (control == nullptr || click_proc == nullptr) return invalid_argument();
    {
        std::lock_guard<std::mutex> lock(g_click_mtx);
        g_click_bindings[control] = ClickBinding{click_proc, user};
    }
    if (!ui_clone_button_style(control, control, &click_proc_trampoline)) {
        std::lock_guard<std::mutex> lock(g_click_mtx);
        g_click_bindings.erase(control);
        return not_ready();
    }
    return contracts::ok();
}

QolResult find_child_in_area(QolUiHandle parent, UiRect area, QolUiHandle* out) {
    if (parent == nullptr || out == nullptr) return invalid_argument();
    *out = ui_find_child_in_area(parent, game_rect(area));
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult clone_button_style(QolUiHandle target, QolUiHandle source,
                             contracts::QolUiClickFn click_proc, void* user) {
    if (target == nullptr || source == nullptr || click_proc == nullptr) return invalid_argument();
    if (!ui_clone_button_style(target, source, &click_proc_trampoline)) return not_ready();
    std::lock_guard<std::mutex> lock(g_click_mtx);
    g_click_bindings[target] = ClickBinding{click_proc, user};
    return contracts::ok();
}

int32_t hit_test(QolUiHandle control, int64_t x, int64_t y, UiRect rect) {
    if (control == nullptr) return -kQolErrInvalidArgument;
    return ui_hit_test(control, x, y, game_rect(rect)) ? 1 : 0;
}

QolResult draw_control(QolUiHandle control) {
    if (control == nullptr) return invalid_argument();
    if (fn_ctrl_btn_draw == nullptr) return not_ready();
    fn_ctrl_btn_draw(control);
    return contracts::ok();
}

QolResult begin_frame() {
    ui_begin_frame();
    return contracts::ok();
}

QolResult fill_rect_alpha(UiRect rect, uint32_t color, uint32_t alpha) {
    ui_fill_rect_alpha(game_rect(rect), color, alpha);
    return contracts::ok();
}

QolResult draw_panel_decor(UiRect panel, const int64_t* separators, uint32_t count, uint32_t color) {
    if (count > 0 && separators == nullptr) return invalid_argument();
    ui_draw_panel_decor(game_rect(panel), separators, static_cast<std::size_t>(count), color);
    return contracts::ok();
}

QolResult draw_vertical_line(int64_t x, int64_t y, int64_t h, uint32_t color, int32_t thickness) {
    ui_draw_vertical_line(x, y, h, color, thickness);
    return contracts::ok();
}

QolResult draw_text(QolUiHandle control, int32_t x, int32_t y, uint32_t color) {
    if (control == nullptr) return invalid_argument();
    ui_draw_text(control, x, y, color);
    return contracts::ok();
}

QolResult draw_text_centered(QolUiHandle control, int32_t y, uint32_t color) {
    if (control == nullptr) return invalid_argument();
    ui_draw_text_centered(control, y, color);
    return contracts::ok();
}

QolResult load_image_unit(int32_t unit) {
    return ui_load_image_unit(unit) ? contracts::ok() : not_ready();
}

QolResult unload_image_unit(int32_t unit) {
    ui_unload_image_unit(unit);
    return contracts::ok();
}

QolResult draw_image_part(int32_t unit, int32_t loc, int32_t x, int32_t y, int32_t type,
                          int32_t flip) {
    return ui_draw_image_part(unit, loc, x, y, type, flip) ? contracts::ok() : not_ready();
}

QolResult draw_control_image_part(QolUiHandle control, int32_t unit, int32_t loc, int32_t type,
                                  int32_t flip) {
    if (control == nullptr) return invalid_argument();
    return ui_draw_control_image_part(control, unit, loc, type, flip) ? contracts::ok() : not_ready();
}

QolResult draw_control_image_part_centered(QolUiHandle control, int32_t unit, int32_t loc,
                                           int32_t type, int32_t flip) {
    if (control == nullptr) return invalid_argument();
    return ui_draw_control_image_part_centered(control, unit, loc, type, flip) ? contracts::ok()
                                                                                : not_ready();
}

QolResult draw_button_background(QolUiHandle control, UiRect rect, uint32_t color) {
    if (control == nullptr) return invalid_argument();
    ui_draw_button_background(control, game_rect(rect), color);
    return contracts::ok();
}

QolResult draw_button_border(QolUiHandle control, UiRect rect, uint32_t color, int32_t thickness) {
    if (control == nullptr) return invalid_argument();
    ui_draw_button_border(control, game_rect(rect), color, thickness);
    return contracts::ok();
}

QolResult draw_control_alpha_overlay(QolUiHandle control, UiRect rect, uint32_t color, uint32_t alpha) {
    if (control == nullptr) return invalid_argument();
    ui_draw_control_alpha_overlay(control, game_rect(rect), color, alpha);
    return contracts::ok();
}

QolResult end_frame() {
    ui_end_frame();
    return contracts::ok();
}

QolResult draw_control_title_image_part(QolUiHandle control, int32_t loc) {
    if (control == nullptr) return invalid_argument();
    return ui_draw_control_title_image_part(control, loc) ? contracts::ok() : not_ready();
}

QolResult show_text_popup(int32_t text_id) {
    if (g_base == 0) return not_ready();
    ui_show_text_popup(text_id);
    return contracts::ok();
}

QolResult child_count(QolUiHandle control, uint32_t* out) {
    if (control == nullptr || out == nullptr) return invalid_argument();
    if (fn_ctrl_get_count == nullptr) return not_ready();
    *out = fn_ctrl_get_count(control);
    return contracts::ok();
}

QolResult child_at(QolUiHandle control, uint32_t index, QolUiHandle* out) {
    if (control == nullptr || out == nullptr) return invalid_argument();
    *out = nullptr;
    if (fn_ctrl_get_child == nullptr) return not_ready();
    *out = fn_ctrl_get_child(control, index);
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult resolution_width(int32_t* out) {
    if (out == nullptr) return invalid_argument();
    if (fn_calc_res_width == nullptr) return not_ready();
    *out = fn_calc_res_width();
    return contracts::ok();
}

QolResult resolution_height(int32_t* out) {
    if (out == nullptr) return invalid_argument();
    if (fn_calc_res_height == nullptr) return not_ready();
    *out = fn_calc_res_height();
    return contracts::ok();
}

QolResult unuse_control_event_move(QolUiHandle control) {
    if (control == nullptr) return invalid_argument();
    if (fn_touch_handle_unuse_control_event_move == nullptr) return not_ready();
    fn_touch_handle_unuse_control_event_move(control);
    return contracts::ok();
}

QolResult set_popup_process_info(int32_t process, int32_t state_id) {
    if (fn_ui_set_popup_process_info == nullptr) return not_ready();
    fn_ui_set_popup_process_info(process, state_id);
    return contracts::ok();
}

QolResult push_popup_state(uint32_t state_id) {
    if (fn_popupstate_push == nullptr) return not_ready();
    return fn_popupstate_push(state_id) != 0 ? contracts::ok() : not_ready();
}

QolResult uidesc_draw() {
    if (fn_uidesc_draw == nullptr) return not_ready();
    fn_uidesc_draw();
    return contracts::ok();
}

QolResult set_font_color(uint32_t color) {
    if (fn_grpx_set_font_color == nullptr) return not_ready();
    fn_grpx_set_font_color(color);
    return contracts::ok();
}

QolResult set_font_color_rgb(int32_t red, int32_t green, int32_t blue) {
    if (fn_grpx_set_font_color_rgb == nullptr) return not_ready();
    fn_grpx_set_font_color_rgb(red, green, blue);
    return contracts::ok();
}

QolResult draw_string_with_font(const char* text, int32_t x, int32_t y, int32_t align,
                                int32_t font) {
    if (text == nullptr) return invalid_argument();
    if (fn_grpx_draw_string_with_font == nullptr) return not_ready();
    fn_grpx_draw_string_with_font(const_cast<char*>(text), x, y, align, font);
    return contracts::ok();
}

QolResult draw_part(QolUiHandle group, int32_t x, int32_t y, QolUiHandle loc, int32_t type,
                    int32_t flip) {
    if (group == nullptr || loc == nullptr) return invalid_argument();
    if (fn_grpx_draw_part == nullptr) return not_ready();
    fn_grpx_draw_part(group, x, y, loc, type, flip, 0);
    return contracts::ok();
}

QolResult image_unit_load(int32_t unit) {
    if (fn_imgsys_unit_load == nullptr) return not_ready();
    fn_imgsys_unit_load(unit);
    return contracts::ok();
}

QolResult image_unit_unload(int32_t unit) {
    if (fn_imgsys_unit_unload == nullptr) return not_ready();
    fn_imgsys_unit_unload(unit);
    return contracts::ok();
}

QolResult image_group(int32_t unit, QolUiHandle* out) {
    if (out == nullptr) return invalid_argument();
    *out = nullptr;
    if (fn_imgsys_get_group == nullptr) return not_ready();
    *out = fn_imgsys_get_group(unit);
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult image_loc(int32_t unit, int32_t loc, QolUiHandle* out) {
    if (out == nullptr) return invalid_argument();
    *out = nullptr;
    if (fn_imgsys_get_loc == nullptr) return not_ready();
    *out = fn_imgsys_get_loc(unit, loc);
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult load_option_images() {
    return ui_original::load_option_images() ? contracts::ok() : not_ready();
}

QolResult unload_option_images() {
    ui_original::unload_option_images();
    return contracts::ok();
}

QolResult draw_back_button(QolUiHandle control, uint8_t pressed) {
    if (control == nullptr) return invalid_argument();
    return ui_original::draw_back_button(control, pressed != 0) ? contracts::ok() : not_ready();
}

QolResult draw_toggle(QolUiHandle control, uint8_t enabled) {
    if (control == nullptr) return invalid_argument();
    return ui_original::draw_toggle(control, enabled != 0) ? contracts::ok() : not_ready();
}

QolResult draw_item_icon(QolUiHandle control, contracts::QolUiItemIcon icon) {
    if (control == nullptr) return invalid_argument();
    const ui_original::ItemIcon game_icon{icon.image_unit, icon.image_loc, icon.draw_type, icon.flip,
                                          icon.centered != 0};
    return ui_original::draw_item_icon(control, game_icon) ? contracts::ok() : not_ready();
}

QolResult draw_custom_button(QolUiHandle control, UiRect size, uint32_t background,
                             uint32_t border, int32_t border_thickness, const char* text,
                             uint32_t text_color, int32_t text_x, int32_t text_y) {
    if (control == nullptr) return invalid_argument();
    ui_custom::draw_button(control, game_rect(size), background, border, border_thickness, text,
                           text_color, text_x, text_y);
    return contracts::ok();
}

const QolUiAdapter g_ui_adapter{
    sizeof(QolUiAdapter), contracts::kUiAdapterMajor, contracts::kUiAdapterMinor,
    &create_control,
    &set_rect,
    &get_rect,
    &set_text,
    &set_draw_proc,
    &set_click_proc,
    &hit_test,
    &draw_control,
    &begin_frame,
    &fill_rect_alpha,
    &draw_panel_decor,
    &draw_vertical_line,
    &draw_text,
    &draw_text_centered,
    &load_image_unit,
    &unload_image_unit,
    &draw_image_part,
    &draw_control_image_part,
    &draw_control_image_part_centered,
    &draw_button_background,
    &draw_button_border,
    &draw_control_alpha_overlay,
    &end_frame,
     &find_child_in_area,
     &clone_button_style,
     &draw_control_title_image_part,
     &show_text_popup,
     &child_count,
     &child_at,
     &resolution_width,
     &resolution_height,
     &unuse_control_event_move,
     &set_popup_process_info,
     &push_popup_state,
     &uidesc_draw,
     &set_font_color,
     &set_font_color_rgb,
     &draw_string_with_font,
     &draw_part,
     &image_unit_load,
     &image_unit_unload,
     &image_group,
     &image_loc,
     &load_option_images,
     &unload_option_images,
     &draw_back_button,
     &draw_toggle,
     &draw_item_icon,
     &draw_custom_button,
};

}  // namespace

const contracts::QolUiAdapter* ui_adapter() { return &g_ui_adapter; }

}  // namespace qol::host
