#include "host/ui_adapter.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "game/access/game_access.h"
#include "game/access/attr_range_ui.h"
#include "game/access/ui_kit.h"
#include "game/access/ui_native_close_button.h"
#include "game/access/ui_native_scroll.h"
#include "game/access/ui_native_toggle.h"
#include "game/access/ui_native_text_button.h"
#include "game/access/ui_components.h"
#include "game/access/popup_state.h"
#include "game/access/ui_state.h"
#include "game/adapter_ports/game_host_port.h"
#include "game/adapter_ports/craft_port.h"
#include "game/state/game_state.h"
#include "infra/log/qol_log.h"

namespace qol::host {
namespace {

using QolResult = ::QolResult;
using contracts::QolUiAdapter;
using contracts::QolUiDrawFn;
using contracts::QolUiHandle;
using contracts::UiRect;

constexpr const char* kInvalidArgument = "invalid argument";
constexpr const char* kNotReady = "ui adapter not ready";
constexpr const char* kUnsupported = "ui adapter unsupported";
// 控件按下态：原版按 data + CB_STATE（game_symbols.h CB_STATE）== 1 判定；
// 旧实现误读 data+0x08（内联文本区），导致按住亮光绘制永不出现。
constexpr size_t kControlPressedStateOffset = CB_STATE;

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
    return contracts::error(kQolErrNotReady, kNotReady, 20);
}

QolResult unsupported() {
    return contracts::error(kQolErrUnsupported, kUnsupported, 22);
}

bool game_owned_image_unit(int32_t unit) {
    // 0x0f 边框、0x59 option/返回、0x4f/0x50 标题背景由游戏共享。
    // UnitLoad 幂等且 UnitUnload 无引用计数；模块消费者归零不表示游戏已停止使用。
    return unit == 0x0f || unit == 0x59 || unit == 0x4f || unit == 0x50;
}

QolResult shared_image_unload_blocked() {
    static constexpr char message[] = "game-owned shared image unit cannot be unloaded";
    return contracts::error(kQolErrBlocked, message, sizeof(message) - 1);
}

QolResult insert_inline_color(void* builder, uint32_t max_text_bytes, uint8_t color_code) {
    constexpr uint32_t kMaxTextBytes = 4096;
    if (builder == nullptr || max_text_bytes == 0 || max_text_bytes > kMaxTextBytes ||
        color_code == 0) {
        return invalid_argument();
    }
    auto** text_slot = reinterpret_cast<char**>(builder);
    char* text = *text_slot;
    if (text == nullptr) return invalid_argument();
    const size_t length = strnlen(text, static_cast<size_t>(max_text_bytes) + 1);
    if (length > max_text_bytes) return contracts::error(kQolErrBlocked, "text exceeds bound", 18);
    if (!qol::game::attr_insert_value_color(text, text + length,
                                            static_cast<char>(color_code))) {
        return not_ready();
    }
    *text_slot = text + length + 4;
    return contracts::ok();
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

    // 0=root、1=button、2=label、3=group；组件描述符负责填充属性。
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
        case 3:
            if (parent == nullptr) return invalid_argument();
            if (!qol::game::game_host_is_main_thread() ||
                fn_ui_create_group_base_control == nullptr) return not_ready();
            *out = fn_ui_create_group_base_control(parent, 0, 0, 0, 0);
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
    // 原生 ControlButton_SetText（F_CONTROL_BUTTON_SET_TEXT_VMA=0xaa7dc）先把 data+0x00
    // 起 0x20 字节清零，再对 data+0x00 调 strcpy；之后 data+0x20 是 ExecuteProc 槽
    // （ControlButton_GetExecuteProc 读 data+0x20，ControlEventProc 释放时间接调用）。
    // strcpy 无界：文本含 NUL 超过 0x20 字节就会覆盖 ExecuteProc，点击即跳转到被文本
    // 污染的地址（现场 SIGBUS BUS_ADRALN）。按钮与标签都经 fn_ctrl_btn_create 创建、
    // 共用这段内联缓冲，因此这里统一 fail-closed，不调用原生、不静默截断。
    constexpr size_t kControlButtonInlineTextCapacity = 0x20;
    if (strnlen(text, kControlButtonInlineTextCapacity) >= kControlButtonInlineTextCapacity) {
        static constexpr char message[] = "control text exceeds inline buffer";
        return contracts::error(kQolErrBlocked, message, sizeof(message) - 1);
    }
    if (fn_ctrl_btn_set_text == nullptr) return not_ready();
    fn_ctrl_btn_set_text(control, const_cast<char*>(text));
    return contracts::ok();
}

QolResult set_draw_proc(QolUiHandle control, QolUiDrawFn draw_proc) {
    if (control == nullptr || draw_proc == nullptr) return invalid_argument();
    if (fn_ctrl_btn_set_draw_proc == nullptr) return not_ready();
    fn_ctrl_btn_set_draw_proc(control, draw_proc);
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
        const auto it = g_click_bindings.find(control);
        if (it != g_click_bindings.end() && it->second.callback == click_proc &&
            it->second.user == user) {
            g_click_bindings.erase(it);
        }
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
    if (game_owned_image_unit(unit)) return shared_image_unload_blocked();
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
    if (game_owned_image_unit(unit)) return shared_image_unload_blocked();
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

QolResult image_loc_size(int32_t unit, int32_t loc, int32_t* out_width, int32_t* out_height) {
    if (out_width == nullptr || out_height == nullptr) return invalid_argument();
    QolUiHandle image = nullptr;
    const QolResult result = image_loc(unit, loc, &image);
    if (!contracts::succeeded(result)) return result;

    const auto* bytes = static_cast<const uint8_t*>(image);
    std::memcpy(out_width, bytes + 0x8, sizeof(*out_width));
    std::memcpy(out_height, bytes + 0xc, sizeof(*out_height));
    return *out_width > 0 && *out_height > 0 ? contracts::ok() : not_ready();
}

QolResult load_option_images() {
    return ui_original::load_option_images() ? contracts::ok() : not_ready();
}

QolResult unload_option_images() {
    return shared_image_unload_blocked();
}

QolResult draw_back_button(QolUiHandle control, uint8_t pressed) {
    if (control == nullptr) return invalid_argument();
    return ui_original::draw_back_button(control, pressed != 0) ? contracts::ok() : not_ready();
}

QolResult draw_toggle(QolUiHandle control, uint8_t enabled) {
    if (control == nullptr) return invalid_argument();
    return ui_original::draw_toggle(control, enabled != 0) ? contracts::ok() : not_ready();
}

QolResult draw_native_toggle(UiRect rect, int32_t value, uint8_t pressed) {
    if ((value != 0 && value != 1) || pressed > 1) return invalid_argument();
    if (!qol::game::game_host_is_main_thread()) return not_ready();
    return qol::game::ui_draw_native_toggle(game_rect(rect), value, pressed)
               ? contracts::ok()
               : not_ready();
}

QolResult draw_native_text_button_background(UiRect rect, uint8_t pressed) {
    if (pressed > 1) return invalid_argument();
    if (!qol::game::game_host_is_main_thread()) return not_ready();
    return qol::game::ui_draw_native_text_button_background(game_rect(rect), pressed)
               ? contracts::ok()
               : not_ready();
}

QolResult set_button_enabled(QolUiHandle control, uint8_t enabled) {
    if (control == nullptr || enabled > 1) return invalid_argument();
    if (!qol::game::game_host_is_main_thread()) return not_ready();
    return ui_set_button_enabled(control, enabled != 0) ? contracts::ok() : not_ready();
}

QolResult draw_native_text_button_label(UiRect rect, const char* text, uint32_t color,
                                        int32_t font) {
    if (text == nullptr || font < 0) return invalid_argument();
    if (!qol::game::game_host_is_main_thread()) return not_ready();
    return qol::game::ui_draw_native_text_button_label(game_rect(rect), text, color, font)
               ? contracts::ok()
               : not_ready();
}

QolResult draw_native_close_button(UiRect rect, uint8_t pressed) {
    if (pressed > 1) return invalid_argument();
    if (!qol::game::game_host_is_main_thread()) return not_ready();
    return qol::game::ui_draw_native_close_button(game_rect(rect), pressed)
               ? contracts::ok()
               : not_ready();
}

QolResult craft_stuff_slot_index(void* transient_control, int32_t* out_index) {
    return qol::game::craft_stuff_slot_index(transient_control, out_index);
}

QolResult control_item_identity(QolUiHandle control, contracts::QolItemHandle* out_item) {
    if (control == nullptr || out_item == nullptr) return invalid_argument();
    contracts::QolItemHandle item{};
    if (!ui_control_item_identity(control, &item)) return not_ready();
    *out_item = item;
    return contracts::ok();
}

QolResult item_proc_source_control(QolUiHandle destination, uint64_t event, const void* param,
                                   QolUiHandle* out_source) {
    if (destination == nullptr || out_source == nullptr || param == nullptr) {
        return invalid_argument();
    }
    void* source = nullptr;
    if (!ui_item_proc_source_control(destination, event, param, &source)) {
        return not_ready();
    }
    *out_source = source;
    return contracts::ok();
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

QolResult find_original_bag_container(QolUiHandle* out) {
    if (out == nullptr) return invalid_argument();
    *out = ui_original_bag_container();
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult control_type(QolUiHandle control, int32_t* out) {
    if (control == nullptr || out == nullptr) return invalid_argument();
    return ui_control_type(control, out) ? contracts::ok() : not_ready();
}

QolResult control_active(QolUiHandle control, int32_t* out) {
    if (control == nullptr || out == nullptr) return invalid_argument();
    return ui_control_active(control, out) ? contracts::ok() : not_ready();
}

QolResult control_user_type(QolUiHandle control, int32_t* out) {
    if (control == nullptr || out == nullptr) return invalid_argument();
    return ui_control_user_type(control, out) ? contracts::ok() : not_ready();
}

QolResult control_data(QolUiHandle control, QolUiHandle* out) {
    if (control == nullptr || out == nullptr) return invalid_argument();
    *out = ui_control_data(control);
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult control_item(QolUiHandle control, QolUiHandle* out) {
    if (control == nullptr || out == nullptr) return invalid_argument();
    *out = ui_control_item(control);
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult control_item_slot(QolUiHandle control, int32_t* out) {
    if (control == nullptr || out == nullptr) return invalid_argument();
    return ui_control_item_slot(control, out) ? contracts::ok() : not_ready();
}

QolResult control_parent(QolUiHandle control, QolUiHandle* out) {
    if (control == nullptr || out == nullptr) return invalid_argument();
    *out = ui_control_parent(control);
    return contracts::ok();
}

QolResult find_root(contracts::QolUiRootKind kind, QolUiHandle* out) {
    if (out == nullptr) return invalid_argument();
    *out = ui_find_root(static_cast<uint32_t>(kind));
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult touch_state(QolUiHandle* out_moving, QolUiHandle* out_drop_source,
                     int64_t* out_x, int64_t* out_y) {
    if (out_moving == nullptr || out_drop_source == nullptr || out_x == nullptr || out_y == nullptr) {
        return invalid_argument();
    }
    return ui_touch_state(out_moving, out_drop_source, out_x, out_y) ? contracts::ok() : not_ready();
}

QolResult fill_rect(UiRect rect, uint32_t color) {
    ui_fill_rect(game_rect(rect), color);
    return contracts::ok();
}

QolResult draw_border_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (w < 48 || h < 48) return invalid_argument();
    if (!qol::game::game_host_is_main_thread() || fn_ui_draw_border_rect == nullptr) {
        return not_ready();
    }
    fn_ui_draw_border_rect(x, y, w, h);
    return contracts::ok();
}

QolResult set_clip(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    if (!qol::game::game_host_is_main_thread() || fn_grpx_set_clip == nullptr) {
        return not_ready();
    }
    fn_grpx_set_clip(x1, y1, x2, y2);
    return contracts::ok();
}

QolResult reset_clip() {
    if (!qol::game::game_host_is_main_thread() || fn_grpx_reset_clip == nullptr) {
        return not_ready();
    }
    fn_grpx_reset_clip();
    return contracts::ok();
}

QolResult scroll_create(QolUiHandle parent, const contracts::UiNativeScrollDesc* desc,
                        contracts::QolUiNativeScrollDrawFn draw_proc, QolUiHandle* out) {
    if (out == nullptr) return invalid_argument();
    *out = nullptr;
    if (parent == nullptr || desc == nullptr || draw_proc == nullptr ||
        desc->viewport.w <= 0 || desc->viewport.h <= 0 || desc->content_extent < 0 ||
        desc->max_drag_delta <= 0 || desc->drag_threshold < 0 || desc->show_bar > 1 ||
        (desc->show_bar && (desc->viewport.h < 4 ||
         desc->viewport.w > std::numeric_limits<int64_t>::max() - 6))) {
        return invalid_argument();
    }
    if (!qol::game::game_host_is_main_thread() || fn_ctrl_scroll_create == nullptr ||
        fn_ctrl_scroll_set_option == nullptr || fn_ctrl_scroll_set_min_variation == nullptr ||
        (desc->show_bar && (fn_ctrl_scroll_set_bar_data == nullptr ||
                           fn_ctrl_scroll_set_bar_draw_type == nullptr))) return not_ready();
    QolUiHandle scroll = fn_ctrl_scroll_create(parent, 1, draw_proc);
    if (scroll == nullptr) return not_ready();
    ui_set_rect(scroll, game_rect(desc->viewport));
    fn_ctrl_scroll_set_option(scroll, desc->content_extent, desc->max_drag_delta);
    fn_ctrl_scroll_set_min_variation(scroll, desc->drag_threshold);
    if (desc->show_bar) {
        // 原生帮助页使用 viewport 右侧 6px、顶部 2px、上下各缩 2px 的纯色 bar。
        fn_ctrl_scroll_set_bar_data(scroll, desc->viewport.w + 6, 2, desc->viewport.h - 4);
        fn_ctrl_scroll_set_bar_draw_type(scroll, 0, 0);
    }
    *out = scroll;
    return contracts::ok();
}

QolResult scroll_set_target(QolUiHandle scroll, QolUiHandle group) {
    if (scroll == nullptr || group == nullptr) return invalid_argument();
    if (!qol::game::game_host_is_main_thread() || fn_ctrl_scroll_set_target_group == nullptr) {
        return not_ready();
    }
    fn_ctrl_scroll_set_target_group(scroll, group);
    return contracts::ok();
}

QolResult button_set_scroll(QolUiHandle button, QolUiHandle scroll) {
    if (button == nullptr || scroll == nullptr) return invalid_argument();
    if (!qol::game::game_host_is_main_thread() || fn_ctrl_btn_set_scroll_control == nullptr) {
        return not_ready();
    }
    fn_ctrl_btn_set_scroll_control(button, scroll);
    return contracts::ok();
}

QolResult scroll_process(QolUiHandle scroll) {
    if (scroll == nullptr) return invalid_argument();
    if (!qol::game::game_host_is_main_thread() || fn_ctrl_scroll_process == nullptr) return not_ready();
    fn_ctrl_scroll_process(scroll);
    return contracts::ok();
}

QolResult scroll_draw(QolUiHandle scroll) {
    if (scroll == nullptr) return invalid_argument();
    if (!qol::game::game_host_is_main_thread() || fn_ctrl_scroll_draw == nullptr) return not_ready();
    fn_ctrl_scroll_draw(scroll);
    return contracts::ok();
}

QolResult scroll_get_offset(QolUiHandle scroll, int64_t* out) {
    if (scroll == nullptr || out == nullptr) return invalid_argument();
    *out = 0;
    if (!qol::game::game_host_is_main_thread()) return not_ready();
    qol::game::NativeScrollState state{};
    if (!qol::game::native_scroll_state(scroll, &state)) return not_ready();
    *out = state.offset;
    return contracts::ok();
}

QolResult scroll_set_offset(QolUiHandle scroll, int64_t offset) {
    if (scroll == nullptr || offset < 0) return invalid_argument();
    if (!qol::game::game_host_is_main_thread() || fn_ctrl_scroll_reset_result_point == nullptr ||
        fn_ctrl_scroll_set_key_step_variation == nullptr || fn_ctrl_scroll_scroll == nullptr) {
        return not_ready();
    }
    qol::game::NativeScrollState state{};
    if (!qol::game::native_scroll_state(scroll, &state)) return not_ready();
    if (offset > state.max_offset) return invalid_argument();
    fn_ctrl_scroll_reset_result_point(scroll);
    fn_ctrl_scroll_set_key_step_variation(scroll, offset);
    fn_ctrl_scroll_scroll(scroll, 1);
    fn_ctrl_scroll_set_key_step_variation(scroll, state.key_step);
    return contracts::ok();
}

QolResult control_tree_event(QolUiHandle root, uint64_t event, uint64_t param, uint64_t param2,
                             uint32_t* consumed) {
    if (root == nullptr || consumed == nullptr) return invalid_argument();
    *consumed = 0;
    if (!qol::game::game_host_is_main_thread() || fn_touch_handle_event == nullptr) return not_ready();
    *consumed = fn_touch_handle_event(root, event, param, param2);
    return contracts::ok();
}

QolResult draw_item_porting(QolUiHandle item, int32_t x, int32_t y, int32_t a, int32_t b) {
    return ui_draw_item_porting(item, x, y, a, b) ? contracts::ok() : not_ready();
}

QolResult execute_proc_matches(QolUiHandle control, contracts::QolUiClickFn expected,
                               int32_t* out) {
    if (control == nullptr || expected == nullptr || out == nullptr) return invalid_argument();
    *out = ui_execute_proc_matches(control, expected) ? 1 : 0;
    return contracts::ok();
}

QolResult control_absolute_origin(QolUiHandle control, int64_t* out_x, int64_t* out_y) {
    if (control == nullptr || out_x == nullptr || out_y == nullptr) return invalid_argument();
    return ui_control_absolute_origin(control, out_x, out_y) ? contracts::ok() : not_ready();
}

QolResult control_pressed(QolUiHandle control, int32_t* out_pressed) {
    if (control == nullptr || out_pressed == nullptr) return invalid_argument();
    *out_pressed = 0;
    void* data = ui_control_data(control);
    if (data == nullptr) return not_ready();
    *out_pressed = *reinterpret_cast<const uint8_t*>(static_cast<const uint8_t*>(data) +
                                                     kControlPressedStateOffset) == 1
                       ? 1
                       : 0;
    return contracts::ok();
}

QolResult touch_coordinates(uint64_t param, int64_t* out_x, int64_t* out_y) {
    if (param == 0 || out_x == nullptr || out_y == nullptr) return invalid_argument();
    const auto* point = reinterpret_cast<const int64_t*>(static_cast<uintptr_t>(param));
    *out_x = point[0];
    *out_y = point[1];
    return contracts::ok();
}

QolResult delete_control(QolUiHandle control) {
    if (control == nullptr || !qol::game::game_host_is_main_thread() ||
        fn_touch_handle_delete_control == nullptr || fn_ctrl_get_count == nullptr ||
        fn_ctrl_get_child == nullptr) return not_ready();
    // 删除整棵 native 子树前解绑所有点击回调；不得持绑定锁跨游戏调用。
    constexpr size_t kMaxTreeControls = 4096;
    std::vector<QolUiHandle> subtree{control};
    std::unordered_set<QolUiHandle> seen{control};
    for (size_t i = 0; i < subtree.size(); ++i) {
        const uint32_t count = fn_ctrl_get_count(subtree[i]);
        if (count > kMaxTreeControls - subtree.size()) return not_ready();
        for (uint32_t j = 0; j < count; ++j) {
            QolUiHandle child = fn_ctrl_get_child(subtree[i], j);
            if (child == nullptr || !seen.insert(child).second) return not_ready();
            subtree.push_back(child);
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_click_mtx);
        for (QolUiHandle handle : subtree) g_click_bindings.erase(handle);
    }
    if (!ui_delete_control(control)) return not_ready();
    return contracts::ok();
}

QolResult set_control_item(QolUiHandle control, QolUiHandle item) {
    if (control == nullptr || !ui_set_control_item(control, item)) return not_ready();
    return contracts::ok();
}

QolResult set_control_proc(QolUiHandle control, contracts::QolUiControlProcFn proc) {
    if (control == nullptr || proc == nullptr ||
        !ui_set_control_proc(control, reinterpret_cast<UiControlProc>(proc))) {
        return not_ready();
    }
    return contracts::ok();
}

QolResult set_control_user_type(QolUiHandle control, int32_t type) {
    if (control == nullptr || !ui_set_control_user_type(control, type)) return not_ready();
    return contracts::ok();
}

QolResult set_control_data(QolUiHandle control, QolUiHandle data) {
    if (control == nullptr || !ui_set_control_data(control, data)) return not_ready();
    return contracts::ok();
}

QolResult allocate_control_data(uint32_t size, QolUiHandle* out) {
    if (out == nullptr || size == 0) return invalid_argument();
    *out = ui_allocate_control_data(size);
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult set_touch_cursor(QolUiHandle control, QolUiHandle cursor) {
    if (control == nullptr || !ui_set_touch_cursor(control, cursor)) return not_ready();
    return contracts::ok();
}

QolResult reset_touch_moving() {
    return ui_reset_touch_moving() ? contracts::ok() : not_ready();
}

QolResult reset_touch_selected() {
    return ui_reset_touch_selected() ? contracts::ok() : not_ready();
}

QolResult control_is_moving(QolUiHandle control, int32_t* out_moving) {
    if (control == nullptr || out_moving == nullptr) return invalid_argument();
    return ui_control_is_moving(control, out_moving) ? contracts::ok() : not_ready();
}

QolResult clear_control_drag_flags(QolUiHandle control) {
    if (control == nullptr || !ui_clear_control_drag_flags(control)) return not_ready();
    return contracts::ok();
}

QolResult clear_touch_state() { return ui_clear_touch_state() ? contracts::ok() : not_ready(); }

QolResult set_touch_release_input(int64_t x, int64_t y, uint64_t param) {
    return ui_set_touch_release_input(x, y, param) ? contracts::ok() : not_ready();
}

QolResult clear_touch_drop_event() {
    return ui_clear_touch_drop_event() ? contracts::ok() : not_ready();
}

QolResult find_main_menu_more_games(QolUiHandle* out) {
    if (out == nullptr) return invalid_argument();
    *out = ui_main_menu_more_games_button();
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult set_control_show(QolUiHandle control, uint8_t show) {
    if (control == nullptr || show > 1) return invalid_argument();
    return ui_set_control_show(control, show != 0) ? contracts::ok() : not_ready();
}

QolResult main_menu_more_games_snapshot(QolUiHandle* out_button, QolUiHandle* out_parent,
                                       UiRect* out_rect) {
    if (out_button == nullptr || out_parent == nullptr || out_rect == nullptr) {
        return invalid_argument();
    }
    *out_button = nullptr;
    *out_parent = nullptr;
    *out_rect = UiRect{};
    if (!qol::game::game_host_is_main_thread()) return not_ready();

    QolUiHandle original = nullptr;
    QolUiHandle parent = nullptr;
    int32_t original_in_tree = 0;
    int32_t ignored_probe_in_tree = 0;
    if (!ui_main_menu_live_tree_snapshot(nullptr, &original, &parent, &original_in_tree,
                                         &ignored_probe_in_tree) ||
        original == nullptr || parent == nullptr || original_in_tree == 0) {
        return not_ready();
    }
    if (!game_memory_accessible(original, CO_SIZE, 'r')) return not_ready();
    ::UiRect rect{};
    if (!ui_get_rect(original, &rect)) return not_ready();
    *out_button = original;
    *out_parent = parent;
    *out_rect = contract_rect(rect);
    return contracts::ok();
}

QolResult main_menu_text_button_is_live(QolUiHandle original, QolUiHandle replacement,
                                        contracts::QolUiClickFn expected_execute, void* owner,
                                        int32_t* out_live) {
    if (original == nullptr || replacement == nullptr || expected_execute == nullptr ||
        owner == nullptr || out_live == nullptr) return invalid_argument();
    *out_live = 0;
    if (!qol::game::game_host_is_main_thread()) return not_ready();

    QolUiHandle current_original = nullptr;
    QolUiHandle parent = nullptr;
    int32_t original_in_tree = 0;
    int32_t replacement_in_tree = 0;
    if (!ui_main_menu_live_tree_snapshot(replacement, &current_original, &parent,
                                         &original_in_tree, &replacement_in_tree)) {
        return not_ready();
    }
    // The slot can already name a rebuilt original while this owned replacement still needs
    // deletion. Settings separately compares the snapshot's original handle when deciding to rebind.
    (void)current_original;
    (void)original_in_tree;
    if (replacement_in_tree == 0) return contracts::ok();

    // Native handles can be recycled. The current tree proves these nodes are readable; the
    // registry tuple proves this replacement still belongs to this component and callback.
    {
        std::lock_guard<std::mutex> lock(g_click_mtx);
        const auto binding = g_click_bindings.find(replacement);
        if (binding == g_click_bindings.end() || binding->second.callback != expected_execute ||
            binding->second.user != owner) return contracts::ok();
    }
    if (!game_memory_accessible(replacement, CO_DATA + sizeof(void*), 'r')) return not_ready();
    void* control_data = ui_control_data(replacement);
    if (control_data == nullptr ||
        !game_memory_accessible(control_data, CB_EXECUTE_PROC + sizeof(void*), 'r')) {
        return not_ready();
    }
    if (!ui_execute_proc_matches(replacement, &click_proc_trampoline)) return contracts::ok();
    *out_live = 1;
    return contracts::ok();
}

QolResult main_menu_text_button_invalidate_binding(QolUiHandle replacement, void* owner) {
    if (replacement == nullptr || owner == nullptr) return invalid_argument();
    if (!qol::game::game_host_is_main_thread()) return not_ready();
    ui_main_menu_observe_screen(nullptr);
    std::lock_guard<std::mutex> lock(g_click_mtx);
    const auto binding = g_click_bindings.find(replacement);
    if (binding != g_click_bindings.end() && binding->second.user == owner) {
        g_click_bindings.erase(binding);
    }
    return contracts::ok();
}

QolResult main_menu_more_games_set_active(QolUiHandle original, int32_t active) {
    if (original == nullptr || (active != 0 && active != 1)) return invalid_argument();
    if (!qol::game::game_host_is_main_thread()) return not_ready();

    QolUiHandle current_original = nullptr;
    QolUiHandle parent = nullptr;
    int32_t original_in_tree = 0;
    int32_t ignored_probe_in_tree = 0;
    if (!ui_main_menu_live_tree_snapshot(nullptr, &current_original, &parent,
                                         &original_in_tree, &ignored_probe_in_tree) ||
        current_original == nullptr || parent == nullptr || original_in_tree == 0) {
        return not_ready();
    }
    if (original != current_original) {
        static constexpr char message[] = "not the current More Games control";
        return contracts::error(kQolErrNotFound, message, sizeof(message) - 1);
    }
    if (fn_ctrl_set_active == nullptr || !game_memory_accessible(original, CO_SIZE, 'w')) {
        return not_ready();
    }

    // ControlObject_SetActive 的入参是 bool：原生内部把它编码为 Active 字段的 0x20（true）
    // / 0x21（false）并返回旧编码状态；EventProc 只在 Active == 0x20 时派发。因此这里直接传
    // bool 1/0，不传存储编码值（非 0 会被原生当成 true 而重新启用原按钮）。
    fn_ctrl_set_active(original, active != 0 ? 1u : 0u);
    return contracts::ok();
}

// 候选 hit rect 与兄弟条目的碰撞：坐标一律用绝对逻辑坐标做 int64 交集判定。
bool rects_overlap(int64_t ax, int64_t ay, int64_t aw, int64_t ah, int64_t bx, int64_t by,
                   int64_t bw, int64_t bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

// 菜单替换几何诊断：每个 stage 在固定 32 槽静态表里各存一份四值，只有该 stage 的数值变化
// 时才输出（不同 stage 交替调用不会互相顶掉）。表满后不再登记新 stage（不循环覆盖）。
// 只记录原因与矩形数值，不记录控件地址或用户文本；stage 均为内部字面量。
void log_menu_rect_stage(const char* stage, int64_t a, int64_t b, int64_t c, int64_t d) {
    struct Slot {
        const char* stage;
        int64_t v[4];
    };
    constexpr size_t kSlots = 32;
    static Slot slots[kSlots] = {};
    Slot* slot = nullptr;
    for (size_t i = 0; i < kSlots; ++i) {
        if (slots[i].stage == stage) {
            slot = &slots[i];
            break;
        }
    }
    if (slot == nullptr) {
        for (size_t i = 0; i < kSlots; ++i) {
            if (slots[i].stage == nullptr) {
                slots[i].stage = stage;
                slot = &slots[i];
                break;
            }
        }
    }
    if (slot == nullptr) return;  // 表满：只丢弃新 stage，不覆盖已有记录
    if (slot->v[0] == a && slot->v[1] == b && slot->v[2] == c && slot->v[3] == d) return;
    slot->v[0] = a;
    slot->v[1] = b;
    slot->v[2] = c;
    slot->v[3] = d;
    QOL_LOG_WARN(QolDomain::kUi,
                 "menu replacement check stage=%s a=%lld b=%lld c=%lld d=%lld int=int64", stage,
                 static_cast<long long>(a), static_cast<long long>(b),
                 static_cast<long long>(c), static_cast<long long>(d));
}

// ---- Minor 21 尾槽（additive）：显式外观布局/绘制、菜单替换几何检查 ----

// 只读布局预检：不绘制、不分配私有字体、不改全局渲染状态。渲染由 game 层实现持有。
QolResult layout_native_text_button(const contracts::UiTextButtonPaintDesc* desc,
                                    contracts::UiTextButtonPaintLayout* out) {
    if (desc == nullptr || out == nullptr) return invalid_argument();
    *out = contracts::UiTextButtonPaintLayout{};
    if (!qol::game::game_host_is_main_thread()) return not_ready();
    return qol::game::ui_layout_native_text_button(*desc, out);
}

QolResult draw_native_text_button(const contracts::UiTextButtonPaintDesc* desc, const char* text,
                                  uint32_t text_color, uint8_t pressed) {
    if (desc == nullptr || text == nullptr || pressed > 1) return invalid_argument();
    if (!qol::game::game_host_is_main_thread()) return not_ready();
    return qol::game::ui_draw_native_text_button(*desc, text, text_color, pressed);
}

QolResult main_menu_replacement_rect_available(QolUiHandle original,
                                               UiRect parent_relative_hit_rect,
                                               uint8_t* out_available) {
    if (original == nullptr || out_available == nullptr) return invalid_argument();
    *out_available = 0;
    if (parent_relative_hit_rect.w <= 0 || parent_relative_hit_rect.h <= 0) {
        return invalid_argument();
    }
    if (!qol::game::game_host_is_main_thread()) return not_ready();

    QolUiHandle current_original = nullptr;
    QolUiHandle parent = nullptr;
    int32_t original_in_tree = 0;
    int32_t ignored_probe_in_tree = 0;
    if (!ui_main_menu_live_tree_snapshot(nullptr, &current_original, &parent, &original_in_tree,
                                         &ignored_probe_in_tree) ||
        current_original == nullptr || parent == nullptr || original_in_tree == 0) {
        log_menu_rect_stage("tree-unknown", original_in_tree, current_original != nullptr,
                            parent != nullptr, 0);
        return not_ready();
    }
    // 身份未知（不是当前 More Games 原控件）→ NotReady，不改动任何控件。
    if (original != current_original) {
        log_menu_rect_stage("identity-unknown", 0, 0, 0, 0);
        return not_ready();
    }
    if (fn_ctrl_get_count == nullptr || fn_ctrl_get_child == nullptr) return not_ready();
    if (!game_memory_accessible(parent, CO_SIZE, 'r')) {
        log_menu_rect_stage("parent-unreadable", 0, 0, 0, 0);
        return not_ready();
    }

    ::UiRect parent_rect{};
    int64_t parent_abs_x = 0;
    int64_t parent_abs_y = 0;
    if (!ui_get_rect(parent, &parent_rect) ||
        !ui_control_absolute_origin(parent, &parent_abs_x, &parent_abs_y)) {
        log_menu_rect_stage("parent-invalid", parent_rect.w, parent_rect.h, parent_abs_x,
                            parent_abs_y);
        return not_ready();
    }
    if (parent_rect.w <= 0 || parent_rect.h <= 0) {
        log_menu_rect_stage("parent-invalid-size", parent_rect.w, parent_rect.h, 0, 0);
        return not_ready();
    }

    const int64_t cand_x = parent_abs_x + parent_relative_hit_rect.x;
    const int64_t cand_y = parent_abs_y + parent_relative_hit_rect.y;
    const int64_t cand_w = parent_relative_hit_rect.w;
    const int64_t cand_h = parent_relative_hit_rect.h;
    // 候选必须完全落在父范围内；越出父界是已证明不安全的布局 → out_available=0。
    if (cand_x < parent_abs_x || cand_y < parent_abs_y ||
        cand_x + cand_w > parent_abs_x + parent_rect.w ||
        cand_y + cand_h > parent_abs_y + parent_rect.h) {
        log_menu_rect_stage("outside-parent-cand", cand_x, cand_y, cand_w, cand_h);
        log_menu_rect_stage("outside-parent-ref", parent_abs_x, parent_abs_y, parent_rect.w,
                            parent_rect.h);
        return contracts::ok();
    }

    // 原控件自身命中区：候选 rect 与该区域的重叠属原生主菜单既有布局，不是本次替换
    // 新增的冲突；只有候选在原命中区之外新占用的区域才算新碰撞。同一层内存解释。
    ::UiRect original_rect{};
    int64_t original_abs_x = 0;
    int64_t original_abs_y = 0;
    if (!ui_get_rect(original, &original_rect) ||
        !ui_control_absolute_origin(original, &original_abs_x, &original_abs_y) ||
        original_rect.w <= 0 || original_rect.h <= 0) {
        log_menu_rect_stage("original-rect-invalid", original_rect.w, original_rect.h, 0, 0);
        return not_ready();
    }
    log_menu_rect_stage("original-rect", original_abs_x, original_abs_y, original_rect.w,
                        original_rect.h);

    // 兄弟交互碰撞：只考虑 native 按钮（CO_TYPE==3 且有 data）；排除将被替换的原控件
    // 与本模块自身绑定控件（ExecuteProc 是自有 trampoline）。内存解释只在本层。
    const uint32_t count = fn_ctrl_get_count(parent);
    for (uint32_t i = 0; i < count; ++i) {
        QolUiHandle child = fn_ctrl_get_child(parent, i);
        if (child == nullptr || child == original) continue;
        if (!game_memory_accessible(child, CO_SIZE, 'r')) {
            log_menu_rect_stage("child-unreadable", i, 0, 0, 0);
            return not_ready();
        }
        int32_t child_type = 0;
        if (!ui_control_type(child, &child_type) || child_type != 3) continue;
        if (ui_control_data(child) == nullptr) continue;
        if (ui_execute_proc_matches(child, &click_proc_trampoline)) continue;
        ::UiRect child_rect{};
        int64_t child_abs_x = 0;
        int64_t child_abs_y = 0;
        if (!ui_get_rect(child, &child_rect) ||
            !ui_control_absolute_origin(child, &child_abs_x, &child_abs_y)) {
            log_menu_rect_stage("child-invalid", i, child_rect.w, child_rect.h, 0);
            return not_ready();
        }
        if (child_rect.w <= 0 || child_rect.h <= 0) continue;
        if (!rects_overlap(cand_x, cand_y, cand_w, cand_h, child_abs_x, child_abs_y, child_rect.w,
                           child_rect.h)) {
            continue;
        }
        const int64_t overlap_x = cand_x > child_abs_x ? cand_x : child_abs_x;
        const int64_t overlap_y = cand_y > child_abs_y ? cand_y : child_abs_y;
        const int64_t cand_right = cand_x + cand_w;
        const int64_t child_right = child_abs_x + child_rect.w;
        const int64_t cand_bottom = cand_y + cand_h;
        const int64_t child_bottom = child_abs_y + child_rect.h;
        const int64_t overlap_right = cand_right < child_right ? cand_right : child_right;
        const int64_t overlap_bottom = cand_bottom < child_bottom ? cand_bottom : child_bottom;
        // 重叠完全落在原控件命中区内 → 原生既有布局（相邻按钮透明边距矩形可互叠），
        // 非本次替换新增，放行；否则为新增冲突，保持 fail-closed。
        const bool within_original =
            overlap_x >= original_abs_x && overlap_y >= original_abs_y &&
            overlap_right <= original_abs_x + original_rect.w &&
            overlap_bottom <= original_abs_y + original_rect.h;
        if (within_original) {
            log_menu_rect_stage("sibling-overlap-existing", overlap_x, overlap_y,
                                overlap_right - overlap_x, overlap_bottom - overlap_y);
            continue;
        }
        // 已证明新增碰撞 → out_available=0，不隐藏/不改动原控件。
        log_menu_rect_stage("sibling-overlap-new", overlap_x, overlap_y,
                            overlap_right - overlap_x, overlap_bottom - overlap_y);
        log_menu_rect_stage("sibling-overlap-cand", cand_x, cand_y, cand_w, cand_h);
        log_menu_rect_stage("sibling-overlap-child", child_abs_x, child_abs_y, child_rect.w,
                            child_rect.h);
        return contracts::ok();
    }
    log_menu_rect_stage("accepted", cand_x, cand_y, cand_w, cand_h);
    *out_available = 1;
    return contracts::ok();
}

QolResult bind_main_menu_more_games(contracts::QolUiClickFn replacement, uint64_t* out_token) {
    if (replacement == nullptr || out_token == nullptr) return invalid_argument();
    *out_token = 0;
    return unsupported();
}

QolResult unbind_main_menu_more_games(uint64_t token) {
    if (token == 0) return invalid_argument();
    return unsupported();
}

QolResult popup_stack_top_matches(contracts::QolUiPopupEnterFn enter, int32_t* out_matches) {
    if (enter == nullptr || out_matches == nullptr) return invalid_argument();
    *out_matches = ui_popup_stack_top_matches(enter) ? 1 : 0;
    return contracts::ok();
}

QolResult clear_control_proc(QolUiHandle control) {
    if (control == nullptr) return invalid_argument();
    ui_clear_control_proc(control);
    return contracts::ok();
}

QolResult screen_name(const char** out) {
    if (out == nullptr) return invalid_argument();
    *out = ui_screen();
    ui_main_menu_observe_screen(*out);
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult popup_stack_snapshot(uint32_t* out_count, uintptr_t* out_top_enter_vma) {
    if (out_count == nullptr || out_top_enter_vma == nullptr) return invalid_argument();
    if (!qol::game::game_host_is_main_thread()) {
        return contracts::error(kQolErrNotReady, "popup snapshot adapter_thread=0", 32);
    }
    if (!qol::game::popup_stack_snapshot(out_count, out_top_enter_vma)) {
        const char* reason = qol::game::popup_stack_snapshot_failure();
        return contracts::error(kQolErrNotReady, reason, reason == nullptr
            ? 0u : static_cast<uint32_t>(std::strlen(reason)));
    }
    return contracts::ok();
}

int32_t adapter_is_main_thread() {
    return qol::game::game_host_is_main_thread() ? 1 : 0;
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
     &find_original_bag_container,
     &control_type,
     &control_active,
     &control_data,
     &execute_proc_matches,
     &control_absolute_origin,
     &control_pressed,
     &touch_coordinates,
     &find_main_menu_more_games,
     &bind_main_menu_more_games,
     &unbind_main_menu_more_games,
     &popup_stack_top_matches,
     &clear_control_proc,
     &screen_name,
     &control_user_type,
     &control_item,
     &control_item_slot,
     &touch_state,
     &fill_rect,
     &draw_item_porting,
     &control_parent,
     &find_root,
     &insert_inline_color,
     &delete_control,
     &set_control_item,
     &set_control_proc,
     &set_control_user_type,
     &set_control_data,
     &allocate_control_data,
     &set_touch_cursor,
     &reset_touch_moving,
     &reset_touch_selected,
     &control_is_moving,
     &clear_control_drag_flags,
     &clear_touch_state,
     &set_touch_release_input,
      &clear_touch_drop_event,
       &popup_stack_snapshot,
       &adapter_is_main_thread,
       &draw_border_rect,
        &image_loc_size,
        &set_clip,
         &reset_clip,
         &scroll_create,
         &scroll_set_target,
         &button_set_scroll,
         &scroll_process,
         &scroll_draw,
         &scroll_get_offset,
        &scroll_set_offset,
         &control_tree_event,
         &draw_native_toggle,
        &control_item_identity,
        &item_proc_source_control,
        &draw_native_text_button_background,
        &set_button_enabled,
        &draw_native_text_button_label,
         &draw_native_close_button,
          &craft_stuff_slot_index,
          &set_control_show,
          &main_menu_more_games_snapshot,
          &main_menu_text_button_is_live,
          &main_menu_text_button_invalidate_binding,
          &main_menu_more_games_set_active,
          // Minor 21 additive tail: explicit text-button layout/draw and the menu
          // replacement geometry check.
          &layout_native_text_button,
          &draw_native_text_button,
          &main_menu_replacement_rect_available,
};

}  // namespace

const contracts::QolUiAdapter* ui_adapter() { return &g_ui_adapter; }

}  // namespace qol::host
