#include <cassert>
#include <cstdint>
#include <cstdio>

#include "contracts/ui.h"
#include "game_access.h"
#include "game/access/ui_kit.h"
#include "game/access/ui_components.h"
#include "host/ui_adapter.h"

namespace {

struct FakeControl {
    UiRect rect{};
};

int g_set_rect_calls = 0;
int g_get_rect_calls = 0;
int g_hit_test_calls = 0;
int g_fill_rect_calls = 0;
int g_set_text_calls = 0;
int g_set_draw_proc_calls = 0;
int g_draw_control_calls = 0;
UiClickProc g_click_proc = nullptr;
void* g_click_user = nullptr;

void draw_proc(void*) {}
void click_proc(void* user) { g_click_user = user; }

void fake_set_text(void*, char*) { ++g_set_text_calls; }
void fake_set_draw_proc(void*, void*) { ++g_set_draw_proc_calls; }
void fake_draw_control(void*) { ++g_draw_control_calls; }

}  // namespace

uintptr_t g_base = 0;

using ControlButtonSetTextFn = void (*)(void* ctrl, char* text);
using ControlButtonSetDrawProcFn = void (*)(void* ctrl, void* proc);
using ControlButtonDrawFn = void (*)(void* ctrl);

ControlButtonSetTextFn fn_ctrl_btn_set_text = &fake_set_text;
ControlButtonSetDrawProcFn fn_ctrl_btn_set_draw_proc = &fake_set_draw_proc;
ControlButtonDrawFn fn_ctrl_btn_draw = &fake_draw_control;

decltype(fn_ui_set_popup_process_info) fn_ui_set_popup_process_info = nullptr;
decltype(fn_ctrl_get_count) fn_ctrl_get_count = nullptr;
decltype(fn_ctrl_get_child) fn_ctrl_get_child = nullptr;
decltype(fn_calc_res_width) fn_calc_res_width = nullptr;
decltype(fn_calc_res_height) fn_calc_res_height = nullptr;
decltype(fn_touch_handle_unuse_control_event_move) fn_touch_handle_unuse_control_event_move = nullptr;
decltype(fn_popupstate_push) fn_popupstate_push = nullptr;
decltype(fn_uidesc_draw) fn_uidesc_draw = nullptr;
decltype(fn_grpx_set_font_color) fn_grpx_set_font_color = nullptr;
decltype(fn_grpx_set_font_color_rgb) fn_grpx_set_font_color_rgb = nullptr;
decltype(fn_grpx_draw_string_with_font) fn_grpx_draw_string_with_font = nullptr;
decltype(fn_grpx_draw_part) fn_grpx_draw_part = nullptr;
decltype(fn_imgsys_unit_load) fn_imgsys_unit_load = nullptr;
decltype(fn_imgsys_unit_unload) fn_imgsys_unit_unload = nullptr;
decltype(fn_imgsys_get_group) fn_imgsys_get_group = nullptr;
decltype(fn_imgsys_get_loc) fn_imgsys_get_loc = nullptr;

void* ui_create_root(UiRect) { return nullptr; }
void* ui_create_button(void*, UiRect, const char*, UiClickProc, UiDrawProc) { return nullptr; }
void* ui_create_label(void*, UiRect, const char*, UiDrawProc) { return nullptr; }
void* ui_find_child_in_area(void*, UiRect) { return nullptr; }
void* ui_main_menu_more_games_button() { return nullptr; }
bool ui_bind_main_menu_more_games(UiClickProc, uint64_t*) { return false; }
bool ui_unbind_main_menu_more_games(uint64_t) { return false; }
bool ui_popup_stack_top_matches(void (*)()) { return false; }
void ui_clear_control_proc(void*) {}
const char* data_ui_screen() { return "test"; }
bool ui_draw_control_title_image_part(void*, int32_t) { return true; }
void ui_show_text_popup(int32_t) {}

void ui_set_rect(void* ctrl, UiRect rect) {
    ++g_set_rect_calls;
    static_cast<FakeControl*>(ctrl)->rect = rect;
}

bool ui_get_rect(void* ctrl, UiRect* rect) {
    ++g_get_rect_calls;
    if (ctrl == nullptr || rect == nullptr) return false;
    *rect = static_cast<FakeControl*>(ctrl)->rect;
    return true;
}

bool ui_clone_button_style(void* target, void* source, UiClickProc on_click) {
    if (target == nullptr || source == nullptr || on_click == nullptr) return false;
    g_click_proc = on_click;
    return true;
}

bool ui_hit_test(void* ctrl, int64_t x, int64_t y, UiRect rect) {
    ++g_hit_test_calls;
    if (ctrl == nullptr) return false;
    const UiRect& origin = static_cast<FakeControl*>(ctrl)->rect;
    return x >= origin.x && x < origin.x + rect.w && y >= origin.y && y < origin.y + rect.h;
}

void* ui_original_bag_container() { return nullptr; }
void* ui_find_root(uint32_t) { return nullptr; }
bool ui_control_type(void*, int32_t*) { return false; }
bool ui_control_active(void*, int32_t*) { return false; }
bool ui_control_user_type(void*, int32_t*) { return false; }
void* ui_control_data(void*) { return nullptr; }
void* ui_control_item(void*) { return nullptr; }
bool ui_control_item_slot(void*, int32_t*) { return false; }
void* ui_control_parent(void*) { return nullptr; }
bool ui_execute_proc_matches(void*, UiClickProc) { return false; }
bool ui_control_absolute_origin(void*, int64_t*, int64_t*) { return false; }
bool ui_touch_state(void**, void**, int64_t*, int64_t*) { return false; }

void ui_begin_frame() {}
void ui_fill_rect_alpha(UiRect, uint32_t, uint32_t) { ++g_fill_rect_calls; }
void ui_fill_rect(UiRect, uint32_t) { ++g_fill_rect_calls; }
void ui_end_frame() {}
void ui_draw_panel_decor(UiRect, const int64_t*, size_t, uint32_t) {}
void ui_draw_vertical_line(int64_t, int64_t, int64_t, uint32_t, int) {}
void ui_draw_text(void*, int, int, uint32_t) {}
void ui_draw_text_centered(void*, int, uint32_t) {}
bool ui_load_image_unit(int32_t) { return true; }
void ui_unload_image_unit(int32_t) {}
bool ui_draw_image_part(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) { return true; }
bool ui_draw_control_image_part(void*, int32_t, int32_t, int32_t, int32_t) { return true; }
bool ui_draw_control_image_part_centered(void*, int32_t, int32_t, int32_t, int32_t) { return true; }
bool ui_draw_item_porting(void*, int32_t, int32_t, int32_t, int32_t) { return true; }
void ui_draw_button_background(void*, UiRect, uint32_t) {}
void ui_draw_button_border(void*, UiRect, uint32_t, int) {}
void ui_draw_control_alpha_overlay(void*, UiRect, uint32_t, uint32_t) {}

namespace ui_original {
bool load_option_images() { return true; }
void unload_option_images() {}
bool draw_back_button(void*, bool) { return true; }
bool draw_toggle(void*, bool) { return true; }
bool draw_item_icon(void*, const ItemIcon&) { return true; }
}  // namespace ui_original

namespace ui_custom {
void draw_button(void*, UiRect, uint32_t, uint32_t, int, const char*, uint32_t, int, int) {}
}  // namespace ui_custom

int main() {
    int assertions = 0;
#define CHECK(condition) \
    do {                  \
        assert(condition); \
        ++assertions;     \
    } while (false)

    const qol::contracts::QolUiAdapter* adapter = qol::host::ui_adapter();
    CHECK(adapter != nullptr);
    CHECK(adapter->size == sizeof(qol::contracts::QolUiAdapter));
    CHECK(adapter->major == qol::contracts::kUiAdapterMajor);
    CHECK(adapter->minor == qol::contracts::kUiAdapterMinor);
    CHECK(adapter->find_child_in_area != nullptr);
    CHECK(adapter->set_popup_process_info != nullptr);
    CHECK(adapter->draw_custom_button != nullptr);
    CHECK(adapter->control_user_type != nullptr);
    CHECK(adapter->control_item != nullptr);
    CHECK(adapter->control_item_slot != nullptr);
    CHECK(adapter->touch_state != nullptr);
    CHECK(adapter->fill_rect != nullptr);
    CHECK(adapter->draw_item_porting != nullptr);
    CHECK(adapter->control_parent != nullptr);
    CHECK(adapter->find_root != nullptr);

    qol::contracts::QolUiComponents components{};
    components.size = sizeof(components);
    components.major = qol::contracts::kUiContractMajor;
    components.minor = qol::contracts::kUiContractMinor;
    CHECK(components.size == sizeof(qol::contracts::QolUiComponents));
    CHECK(components.major == 1 && components.minor == 0);

    const qol::contracts::UiRect rect{10, 20, 30, 40};
    FakeControl control{};
    CHECK(adapter->set_rect(&control, rect).ok == kQolOk);
    CHECK(g_set_rect_calls == 1);

    qol::contracts::UiRect out{};
    CHECK(adapter->get_rect(&control, &out).ok == kQolOk);
    CHECK(g_get_rect_calls == 1);
    CHECK(out.x == 10 && out.y == 20 && out.w == 30 && out.h == 40);

    CHECK(adapter->hit_test(&control, 10, 20, rect) == 1);
    CHECK(adapter->hit_test(&control, 40, 20, rect) == 0);
    CHECK(g_hit_test_calls == 2);

    CHECK(adapter->fill_rect_alpha(rect, 0x123456, 0x50).ok == kQolOk);
    CHECK(g_fill_rect_calls == 1);

    CHECK(adapter->set_rect(nullptr, rect).code == kQolErrInvalidArgument);
    CHECK(adapter->get_rect(&control, nullptr).code == kQolErrInvalidArgument);
    CHECK(adapter->draw_text(nullptr, 0, 0, 0).code == kQolErrInvalidArgument);
    qol::contracts::QolUiHandle created = nullptr;
    CHECK(adapter->create_control(1, nullptr, &created).code == kQolErrInvalidArgument);
    CHECK(adapter->set_text(&control, nullptr).code == kQolErrInvalidArgument);
    CHECK(adapter->set_text(&control, "text").ok == kQolOk);
    CHECK(g_set_text_calls == 1);
    CHECK(adapter->set_draw_proc(&control, &draw_proc).ok == kQolOk);
    CHECK(g_set_draw_proc_calls == 1);
    int click_marker = 0;
    CHECK(adapter->set_click_proc(&control, &click_proc, &click_marker).ok == kQolOk);
    CHECK(g_click_proc != nullptr);
    g_click_proc(&control);
    CHECK(g_click_user == &click_marker);
    CHECK(adapter->draw_control(&control).ok == kQolOk);
    CHECK(g_draw_control_calls == 1);

    std::printf("ui_adapter assertions: %d\n", assertions);
    return 0;
#undef CHECK
}
