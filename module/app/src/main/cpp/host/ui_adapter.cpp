#include "host/ui_adapter.h"

#include <cstddef>
#include <cstdint>

#include "game/access/ui_kit.h"

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
    // ui_kit exposes text assignment only during control creation; U11-2 owns component text state.
    return unsupported();
}

QolResult set_draw_proc(QolUiHandle control, QolUiDrawFn draw_proc) {
    if (control == nullptr || draw_proc == nullptr) return invalid_argument();
    // ui_kit has no public post-creation draw-proc setter.
    return unsupported();
}

QolResult set_click_proc(QolUiHandle control, contracts::QolUiClickFn click_proc, void*) {
    if (control == nullptr || click_proc == nullptr) return invalid_argument();
    // ui_kit binds the callback only while creating a button and has no user-data setter.
    return unsupported();
}

int32_t hit_test(QolUiHandle control, int64_t x, int64_t y, UiRect rect) {
    if (control == nullptr) return -kQolErrInvalidArgument;
    return ui_hit_test(control, x, y, game_rect(rect)) ? 1 : 0;
}

QolResult draw_control(QolUiHandle control) {
    if (control == nullptr) return invalid_argument();
    // Drawing is callback-driven by the game control tree; no public imperative primitive exists.
    return unsupported();
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

const QolUiAdapter g_ui_adapter{
    sizeof(QolUiAdapter), contracts::kUiContractMajor, contracts::kUiContractMinor,
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
};

}  // namespace

const contracts::QolUiAdapter* ui_adapter() { return &g_ui_adapter; }

}  // namespace qol::host
