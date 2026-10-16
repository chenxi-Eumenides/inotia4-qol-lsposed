#include <cassert>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

#include "contracts/ui.h"
#include "fake_shared_atlases.h"
#include "game/access/attr_range_ui.h"
#include "game/access/game_access.h"
#include "game/access/ui_kit.h"
#include "game/access/ui_components.h"
#include "host/ui_adapter.h"
#include "infra/log/qol_log.h"

namespace {

struct FakeControl {
    UiRect rect{};
    alignas(int64_t) uint8_t data[CS_SIZE]{};
    std::vector<FakeControl*> children;
    UiClickProc execute_proc = nullptr;
    FakeControl* parent = nullptr;  // 绝对原点按父链累加（ui_control_absolute_origin）
    int32_t type = 0;               // CO_TYPE；交互按钮 = 3
};

enum class NativeCall { kCreate, kRect, kOption, kThreshold, kBar, kBarType, kTarget,
                        kButtonScroll, kProcess, kDraw, kReset, kStep, kScroll, kEvent,
                        kGroup, kDelete };
struct Call {
    NativeCall kind;
    void* control;
    int64_t a = 0, b = 0, c = 0;
};
std::vector<Call> g_native_calls;
FakeControl g_scroll;
FakeControl g_group;
bool g_create_scroll_ok = true;
bool g_create_group_ok = true;
bool g_scroll_data_ok = true;
bool g_memory_accessible = true;
int g_data_reads = 0;
void* g_native_parent = nullptr;
void* g_target = nullptr;
NativeScrollDrawProc g_scroll_draw_proc = nullptr;
uint64_t g_event_args[3]{};
bool g_menu_tree_known = false;
void* g_menu_original = nullptr;
void* g_menu_parent = nullptr;
std::vector<void*> g_menu_children;
int g_menu_tree_reads = 0;
int g_menu_screen_observations = 0;
const char* g_last_menu_screen_observation = nullptr;
int g_execute_proc_reads = 0;
int g_control_parent_reads = 0;
int g_memory_accessible_reads = 0;
int g_active_calls = 0;
void* g_active_control = nullptr;
uint32_t g_last_set_active_argument = UINT32_MAX;  // callee 收到的入参（原生为 bool 0/1）
uint32_t g_active_value = UINT32_MAX;              // 编码后的存储值（0x20 激活 / 0x21 非激活）

void set_data(FakeControl& control, size_t field, int64_t value) {
    std::memcpy(control.data + field, &value, sizeof(value));
}
int64_t get_data(FakeControl& control, size_t field) {
    int64_t value;
    std::memcpy(&value, control.data + field, sizeof(value));
    return value;
}
void native_draw_proc(void*, int64_t) {}
void* fake_scroll_create(void* parent, uint8_t type, NativeScrollDrawProc draw) {
    g_native_calls.push_back({NativeCall::kCreate, parent, type});
    g_native_parent = parent;
    g_scroll_draw_proc = draw;
    g_scroll.data[CS_TYPE] = type;
    return g_create_scroll_ok ? &g_scroll : nullptr;
}
void fake_scroll_option(void* control, int64_t extent, int64_t delta) {
    g_native_calls.push_back({NativeCall::kOption, control, extent, delta});
    auto& scroll = *static_cast<FakeControl*>(control);
    set_data(scroll, CS_CONTENT_EXTENT, extent);
    set_data(scroll, CS_VIEW_EXTENT, scroll.rect.h);
}
void fake_scroll_threshold(void* control, int64_t threshold) {
    g_native_calls.push_back({NativeCall::kThreshold, control, threshold});
}
void fake_scroll_bar(void* control, int64_t x, int64_t y, int64_t length) {
    g_native_calls.push_back({NativeCall::kBar, control, x, y, length});
}
void fake_scroll_bar_type(void* control, uint8_t type, int64_t aux) {
    g_native_calls.push_back({NativeCall::kBarType, control, type, aux});
}
void fake_scroll_target(void* control, void* group) {
    g_native_calls.push_back({NativeCall::kTarget, control});
    g_target = group;
}
void fake_button_scroll(void* button, void* scroll) {
    g_native_calls.push_back({NativeCall::kButtonScroll, button});
    g_target = scroll;
}
void fake_scroll_process(void* control) { g_native_calls.push_back({NativeCall::kProcess, control}); }
void fake_scroll_draw(void* control) { g_native_calls.push_back({NativeCall::kDraw, control}); }
void fake_scroll_reset(void* control) {
    g_native_calls.push_back({NativeCall::kReset, control});
    auto& scroll = *static_cast<FakeControl*>(control);
    set_data(scroll, CS_CURRENT, get_data(scroll, CS_START));
}
void fake_scroll_step(void* control, int64_t step) {
    g_native_calls.push_back({NativeCall::kStep, control, step});
    set_data(*static_cast<FakeControl*>(control), CS_KEY_STEP, step);
}
void fake_scroll_scroll(void* control, int64_t dir) {
    g_native_calls.push_back({NativeCall::kScroll, control, dir});
    auto& scroll = *static_cast<FakeControl*>(control);
    set_data(scroll, CS_CURRENT, get_data(scroll, CS_CURRENT) - get_data(scroll, CS_KEY_STEP));
}
void* fake_group_create(void* parent, int64_t x, int64_t y, int64_t w, int64_t h) {
    g_native_calls.push_back({NativeCall::kGroup, parent, x, y, w});
    g_group.rect = UiRect{x, y, w, h};
    return g_create_group_ok ? &g_group : nullptr;
}
uint32_t fake_tree_event(void* root, uint64_t event, uint64_t param, uint64_t param2) {
    g_native_calls.push_back({NativeCall::kEvent, root});
    g_event_args[0] = event;
    g_event_args[1] = param;
    g_event_args[2] = param2;
    return 7;
}
void* fake_control_data(void* control) {
    ++g_data_reads;
    return g_scroll_data_ok ? static_cast<FakeControl*>(control)->data : nullptr;
}
uint32_t fake_child_count(void* control) {
    return static_cast<uint32_t>(static_cast<FakeControl*>(control)->children.size());
}
void* fake_child_at(void* control, uint32_t index) {
    return static_cast<FakeControl*>(control)->children[index];
}
void fake_tree_delete(void* control) { g_native_calls.push_back({NativeCall::kDelete, control}); }

int g_set_rect_calls = 0;
int g_get_rect_calls = 0;
int g_hit_test_calls = 0;
int g_fill_rect_calls = 0;
int g_set_text_calls = 0;
int g_set_draw_proc_calls = 0;
int g_draw_control_calls = 0;
bool g_popup_snapshot_ok = true;
int g_popup_snapshot_reads = 0;
bool g_game_main_thread = true;
alignas(16) char g_uidesc_test_buffer[G_UIDESC_TEXT_BUF_SIZE + 8]{};
bool g_uidesc_buffer_available = true;
int32_t g_border_rect[4]{};
int32_t g_clip_rect[4]{};
int g_set_clip_calls = 0;
int g_reset_clip_calls = 0;
UiClickProc g_click_proc = nullptr;
void* g_click_user = nullptr;

void draw_proc(void*) {}
void click_proc(void* user) { g_click_user = user; }

void fake_set_text(void*, char*) { ++g_set_text_calls; }
void fake_set_draw_proc(void*, NativeButtonDrawProc) { ++g_set_draw_proc_calls; }
void fake_draw_control(void*) { ++g_draw_control_calls; }
void fake_draw_border_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    g_border_rect[0] = x;
    g_border_rect[1] = y;
    g_border_rect[2] = w;
    g_border_rect[3] = h;
}
void fake_set_clip(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    ++g_set_clip_calls;
    g_clip_rect[0] = x1;
    g_clip_rect[1] = y1;
    g_clip_rect[2] = x2;
    g_clip_rect[3] = y2;
}
void fake_reset_clip() { ++g_reset_clip_calls; }

// 游戏 IMGSYS fake：UnitLoad 幂等（已加载不重载、不增引用），UnitUnload 无引用计数、
// 直接置失效并记录物理卸载次数。0x0f 边框、0x59 option/返回、0x4f/0x50 标题背景由
// 游戏共享；模块消费者归零不代表游戏已停用，受保护 unit 的物理卸载必须为 0，否则
// 仍存活的原生消费者（如人物信息页边框绘制）会使用失效贴图崩溃。
constexpr int32_t kProtectedImageUnits[] = {0x0f, 0x59, 0x4f, 0x50};
qol::tests::FakeSharedAtlases g_atlases;

bool image_unit_loaded(int32_t unit) { return g_atlases.unit(unit).loaded; }
int image_unit_generation(int32_t unit) {
    return static_cast<int>(g_atlases.unit(unit).generation);
}
int image_unit_unload_calls(int32_t unit) {
    return static_cast<int>(g_atlases.unit(unit).physical_unloads);
}
int protected_unload_calls() { return static_cast<int>(g_atlases.protected_unloads()); }
void fake_imgsys_unit_load(int32_t unit) { g_atlases.load(unit); }
void fake_imgsys_unit_unload(int32_t unit) { g_atlases.unload(unit); }
void fake_control_set_active(void* control, uint32_t active) {
    ++g_active_calls;
    g_active_control = control;
    // 模拟原生 ControlObject_SetActive：入参是 bool，内部编码为 Active 字段 0x20（true）/
    // 0x21（false）。分开记录 callee 入参与编码后的存储值，避免把两者混为一谈。
    g_last_set_active_argument = active;
    g_active_value = active != 0 ? 0x20u : 0x21u;
}

}  // namespace

uintptr_t g_base = 0;

// host/ui_adapter.cpp 的菜单几何/绑定诊断走统一日志面（infra/log/qol_log.h）。本 target 与
// 其它被替身的生产依赖一致，不链接 infra/log/qol_log.cpp，这里提供只计数的替身。
int g_qol_log_calls = 0;
void qol_log_write(QolLogLevel, QolDomain, const char*, int, const char*, ...) {
    ++g_qol_log_calls;
}

ControlButtonSetTextFn fn_ctrl_btn_set_text = &fake_set_text;
ControlButtonSetDrawProcFn fn_ctrl_btn_set_draw_proc = &fake_set_draw_proc;
ControlButtonDrawFn fn_ctrl_btn_draw = &fake_draw_control;
UiDrawBorderRectFn fn_ui_draw_border_rect = &fake_draw_border_rect;
decltype(fn_grpx_set_clip) fn_grpx_set_clip = &fake_set_clip;
decltype(fn_grpx_reset_clip) fn_grpx_reset_clip = &fake_reset_clip;
decltype(fn_ctrl_scroll_create) fn_ctrl_scroll_create = &fake_scroll_create;
decltype(fn_ctrl_scroll_set_option) fn_ctrl_scroll_set_option = &fake_scroll_option;
decltype(fn_ctrl_scroll_set_min_variation) fn_ctrl_scroll_set_min_variation = &fake_scroll_threshold;
decltype(fn_ctrl_scroll_set_bar_data) fn_ctrl_scroll_set_bar_data = &fake_scroll_bar;
decltype(fn_ctrl_scroll_set_bar_draw_type) fn_ctrl_scroll_set_bar_draw_type = &fake_scroll_bar_type;
decltype(fn_ctrl_scroll_set_target_group) fn_ctrl_scroll_set_target_group = &fake_scroll_target;
decltype(fn_ctrl_btn_set_scroll_control) fn_ctrl_btn_set_scroll_control = &fake_button_scroll;
decltype(fn_ctrl_scroll_process) fn_ctrl_scroll_process = &fake_scroll_process;
decltype(fn_ctrl_scroll_draw) fn_ctrl_scroll_draw = &fake_scroll_draw;
decltype(fn_ctrl_scroll_reset_result_point) fn_ctrl_scroll_reset_result_point = &fake_scroll_reset;
decltype(fn_ctrl_scroll_set_key_step_variation) fn_ctrl_scroll_set_key_step_variation = &fake_scroll_step;
decltype(fn_ctrl_scroll_scroll) fn_ctrl_scroll_scroll = &fake_scroll_scroll;
decltype(fn_ui_create_group_base_control) fn_ui_create_group_base_control = &fake_group_create;
decltype(fn_touch_handle_event) fn_touch_handle_event = &fake_tree_event;
decltype(fn_touch_handle_delete_control) fn_touch_handle_delete_control = &fake_tree_delete;
decltype(fn_ctrl_get_data) fn_ctrl_get_data = &fake_control_data;
decltype(fn_ctrl_set_active) fn_ctrl_set_active = &fake_control_set_active;

decltype(fn_ui_set_popup_process_info) fn_ui_set_popup_process_info = nullptr;
decltype(fn_ctrl_get_count) fn_ctrl_get_count = &fake_child_count;
decltype(fn_ctrl_get_child) fn_ctrl_get_child = &fake_child_at;
decltype(fn_calc_res_width) fn_calc_res_width = nullptr;
decltype(fn_calc_res_height) fn_calc_res_height = nullptr;
decltype(fn_touch_handle_unuse_control_event_move) fn_touch_handle_unuse_control_event_move = nullptr;
decltype(fn_popupstate_push) fn_popupstate_push = nullptr;
decltype(fn_uidesc_draw) fn_uidesc_draw = nullptr;
decltype(fn_grpx_set_font_color) fn_grpx_set_font_color = nullptr;
decltype(fn_grpx_set_font_color_rgb) fn_grpx_set_font_color_rgb = nullptr;
decltype(fn_grpx_draw_string_with_font) fn_grpx_draw_string_with_font = nullptr;
uint64_t fake_font_height(uint32_t) { return 16; }
decltype(fn_grpx_get_font_height) fn_grpx_get_font_height = &fake_font_height;
decltype(fn_grpx_draw_part) fn_grpx_draw_part = nullptr;
decltype(fn_imgsys_unit_load) fn_imgsys_unit_load = &fake_imgsys_unit_load;
decltype(fn_imgsys_unit_unload) fn_imgsys_unit_unload = &fake_imgsys_unit_unload;
decltype(fn_imgsys_get_group) fn_imgsys_get_group = nullptr;
decltype(fn_imgsys_get_loc) fn_imgsys_get_loc = nullptr;
decltype(fn_get_group_title_img_type) fn_get_group_title_img_type = nullptr;
decltype(fn_sgl_draw_texture_part_ex) fn_sgl_draw_texture_part_ex = nullptr;

void* ui_create_root(UiRect) { return nullptr; }
void* ui_create_button(void*, UiRect, const char*, UiClickProc, UiDrawProc) { return nullptr; }
void* ui_create_label(void*, UiRect, const char*, UiDrawProc) { return nullptr; }
void* ui_find_child_in_area(void*, UiRect) { return nullptr; }
void* ui_main_menu_more_games_button() { return nullptr; }
bool ui_set_control_show(void*, bool) { return false; }
void ui_main_menu_observe_screen(const char* screen) {
    ++g_menu_screen_observations;
    g_last_menu_screen_observation = screen;
}
bool ui_main_menu_live_tree_snapshot(void* probe, void** out_original, void** out_parent,
                                     int32_t* out_original_in_tree,
                                     int32_t* out_probe_in_tree) {
    ++g_menu_tree_reads;
    if (out_original == nullptr || out_parent == nullptr || out_original_in_tree == nullptr ||
        out_probe_in_tree == nullptr) return false;
    *out_original = nullptr;
    *out_parent = nullptr;
    *out_original_in_tree = 0;
    *out_probe_in_tree = 0;
    if (!g_menu_tree_known) return false;
    *out_original = g_menu_original;
    *out_parent = g_menu_parent;
    *out_original_in_tree = std::find(g_menu_children.begin(), g_menu_children.end(),
        g_menu_original) != g_menu_children.end() ? 1 : 0;
    *out_probe_in_tree = probe != nullptr &&
        std::find(g_menu_children.begin(), g_menu_children.end(), probe) != g_menu_children.end()
        ? 1 : 0;
    return true;
}
bool ui_popup_stack_top_matches(void (*)()) { return false; }
void ui_clear_control_proc(void*) {}
bool ui_delete_control(void* control) {
    fn_touch_handle_delete_control(control);
    return true;
}
bool game_memory_accessible(const void* address, size_t, char) {
    ++g_memory_accessible_reads;
    return address != nullptr && g_memory_accessible;
}
bool ui_set_control_item(void*, void*) { return false; }
bool ui_set_button_enabled(void*, bool) { return true; }
bool ui_set_control_proc(void*, UiControlProc) { return false; }
bool ui_set_control_user_type(void*, int32_t) { return false; }
bool ui_set_control_data(void*, void*) { return false; }
void* ui_allocate_control_data(size_t) { return nullptr; }
bool ui_set_touch_cursor(void*, void*) { return false; }
bool ui_reset_touch_moving() { return false; }
bool ui_reset_touch_selected() { return false; }
const char* ui_screen() { return "test"; }
bool ui_draw_control_title_image_part(void*, int32_t) { return true; }
void ui_show_text_popup(int32_t) {}
bool ui_control_is_moving(void*, int32_t* out_moving) {
    if (out_moving != nullptr) *out_moving = 0;
    return true;
}
bool ui_clear_control_drag_flags(void*) { return true; }
bool ui_clear_touch_state() { return true; }
bool ui_set_touch_release_input(int64_t, int64_t, uint64_t) { return true; }
bool ui_clear_touch_drop_event() { return true; }

namespace qol::game {
bool g_attr_insert_value_color_ok = true;
bool attr_uidesc_text_buffer_base(char** out_buffer) {
    if (out_buffer == nullptr) return false;
    *out_buffer = nullptr;
    if (!g_uidesc_buffer_available) return false;
    *out_buffer = g_uidesc_test_buffer;
    return true;
}
bool attr_insert_value_color(char* before, char* after, char code) {
    if (!g_attr_insert_value_color_ok || before == nullptr || after <= before || code == 0 ||
        reinterpret_cast<uintptr_t>(after) + 4 >
            reinterpret_cast<uintptr_t>(g_uidesc_test_buffer) + G_UIDESC_TEXT_BUF_SIZE) {
        return false;
    }

    char* insert_at = nullptr;
    for (char* p = before; p < after; ++p) {
        if (*p == ':') {
            insert_at = p + 1;
            while (insert_at < after && *insert_at == ' ') ++insert_at;
            break;
        }
    }
    if (insert_at == nullptr) {
        for (char* p = before; p < after; ++p) {
            if (*p >= '0' && *p <= '9') {
                insert_at = p;
                break;
            }
        }
    }
    if (insert_at == nullptr) return false;
    if (insert_at > before && (insert_at[-1] == '-' || insert_at[-1] == '+')) --insert_at;
    std::memmove(insert_at + 4, insert_at, static_cast<size_t>(after - insert_at));
    insert_at[0] = '$';
    insert_at[1] = 'B';
    insert_at[2] = '$';
    insert_at[3] = code;
    return true;
}
bool game_host_is_main_thread() { return g_game_main_thread; }
QolResult craft_stuff_slot_index(void* transient_control, int32_t* out_index) {
    if (out_index == nullptr) return contracts::error(kQolErrInvalidArgument, "invalid", 7);
    *out_index = -1;
    if (transient_control != reinterpret_cast<void*>(0x55)) {
        return contracts::error(kQolErrNotFound, "not found", 9);
    }
    *out_index = 2;
    return contracts::ok();
}
bool popup_stack_snapshot(uint32_t* out_count, uintptr_t* out_top_enter_vma) {
    ++g_popup_snapshot_reads;
    if (!g_popup_snapshot_ok || out_count == nullptr || out_top_enter_vma == nullptr) return false;
    *out_count = 0;
    *out_top_enter_vma = 0;
    return true;
}
const char* popup_stack_snapshot_failure() { return "fake snapshot failure"; }
}

void ui_set_rect(void* ctrl, UiRect rect) {
    ++g_set_rect_calls;
    g_native_calls.push_back({NativeCall::kRect, ctrl});
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
    static_cast<FakeControl*>(target)->execute_proc = on_click;
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
bool ui_control_type(void* control, int32_t* out_type) {
    if (control == nullptr || out_type == nullptr) return false;
    *out_type = static_cast<FakeControl*>(control)->type;
    return true;
}
bool ui_control_active(void*, int32_t*) { return false; }
bool ui_control_user_type(void*, int32_t*) { return false; }
void* ui_control_data(void* control) {
    if (control == nullptr) return nullptr;
    return static_cast<FakeControl*>(control)->data;
}
void* ui_control_item(void*) { return nullptr; }
bool ui_control_item_slot(void*, int32_t*) { return false; }
bool ui_control_item_identity(void*, qol::contracts::QolItemHandle*) { return false; }
bool ui_item_proc_source_control(void* destination, uint64_t event, const void* param,
                                 void** out_source) {
    if (destination == nullptr || param == nullptr || out_source == nullptr ||
        (event != 0x02 && event != 0x04) || !g_game_main_thread) return false;
    struct FakeDropParam { uint64_t reserved; void* source; };
    const auto* field = static_cast<const FakeDropParam*>(param);
    if (field->source == nullptr) return false;
    *out_source = field->source;
    return true;
}
void* ui_control_parent(void* control) {
    ++g_control_parent_reads;
    return control == nullptr ? nullptr : static_cast<FakeControl*>(control)->parent;
}
bool ui_execute_proc_matches(void* control, UiClickProc expected) {
    ++g_execute_proc_reads;
    return control != nullptr && expected != nullptr &&
        static_cast<FakeControl*>(control)->execute_proc == expected;
}
bool ui_control_absolute_origin(void* control, int64_t* out_x, int64_t* out_y) {
    if (control == nullptr || out_x == nullptr || out_y == nullptr) return false;
    int64_t x = 0;
    int64_t y = 0;
    for (FakeControl* current = static_cast<FakeControl*>(control); current != nullptr;
         current = current->parent) {
        x += current->rect.x;
        y += current->rect.y;
    }
    *out_x = x;
    *out_y = y;
    return true;
}
bool ui_touch_state(void**, void**, int64_t*, int64_t*) { return false; }
void ui_begin_frame() {}
void ui_fill_rect_alpha(UiRect, uint32_t, uint32_t) { ++g_fill_rect_calls; }
void ui_fill_rect(UiRect, uint32_t) { ++g_fill_rect_calls; }
void ui_end_frame() {}
void ui_draw_panel_decor(UiRect, const int64_t*, size_t, uint32_t) {}
void ui_draw_vertical_line(int64_t, int64_t, int64_t, uint32_t, int) {}
void ui_draw_text(void*, int, int, uint32_t) {}
void ui_draw_text_centered(void*, int, uint32_t) {}
bool ui_load_image_unit(int32_t unit) {
    fake_imgsys_unit_load(unit);
    return true;
}
void ui_unload_image_unit(int32_t unit) { fake_imgsys_unit_unload(unit); }
bool ui_draw_image_part(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) { return true; }
bool ui_draw_control_image_part(void*, int32_t, int32_t, int32_t, int32_t) { return true; }
bool ui_draw_control_image_part_centered(void*, int32_t, int32_t, int32_t, int32_t) { return true; }
bool ui_draw_item_porting(void*, int32_t, int32_t, int32_t, int32_t) { return true; }
void ui_draw_button_background(void*, UiRect, uint32_t) {}
void ui_draw_button_border(void*, UiRect, uint32_t, int) {}
void ui_draw_control_alpha_overlay(void*, UiRect, uint32_t, uint32_t) {}

namespace ui_original {
bool load_option_images() { return ui_load_image_unit(0x59); }
void unload_option_images() { ui_unload_image_unit(0x59); }
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
    CHECK(adapter->popup_stack_snapshot != nullptr);
    CHECK(adapter->is_main_thread != nullptr);
    CHECK(adapter->is_main_thread() == 1);
    g_game_main_thread = false;
    CHECK(adapter->is_main_thread() == 0);
    g_game_main_thread = true;
    const char* current_screen = nullptr;
    const int screen_observations_before = g_menu_screen_observations;
    CHECK(adapter->screen_name(&current_screen).ok == kQolOk);
    CHECK(current_screen != nullptr && std::strcmp(current_screen, "test") == 0 &&
          g_menu_screen_observations == screen_observations_before + 1 &&
          g_last_menu_screen_observation == current_screen);
    CHECK(adapter->draw_native_toggle != nullptr);
    CHECK(adapter->control_item_identity != nullptr);
    CHECK(adapter->item_proc_source_control != nullptr);
    CHECK(adapter->draw_native_text_button_background != nullptr);
    CHECK(adapter->set_button_enabled != nullptr);
    CHECK(adapter->draw_native_text_button_label != nullptr);
    CHECK(adapter->draw_native_close_button != nullptr);
    CHECK(adapter->craft_stuff_slot_index != nullptr);
    CHECK(adapter->set_control_show != nullptr);
    CHECK(adapter->main_menu_more_games_set_active != nullptr);
    CHECK(adapter->layout_native_text_button != nullptr);
    CHECK(adapter->draw_native_text_button != nullptr);
    CHECK(adapter->main_menu_replacement_rect_available != nullptr);
    CHECK(adapter->main_menu_more_games_snapshot != nullptr);
    CHECK(adapter->main_menu_text_button_is_live != nullptr);
    CHECK(adapter->main_menu_text_button_invalidate_binding != nullptr);
    uint64_t retired_menu_binding_token = 99;
    CHECK(adapter->bind_main_menu_more_games(+[](void*) {}, &retired_menu_binding_token).code ==
          kQolErrUnsupported);
    CHECK(retired_menu_binding_token == 0);
    CHECK(adapter->unbind_main_menu_more_games(1).code == kQolErrUnsupported);
    int32_t craft_slot = -1;
    CHECK(adapter->craft_stuff_slot_index(reinterpret_cast<void*>(0x55), &craft_slot).ok == kQolOk &&
          craft_slot == 2);
    CHECK(adapter->craft_stuff_slot_index(reinterpret_cast<void*>(0x56), &craft_slot).code ==
          kQolErrNotFound);

    // control_pressed 必须读原版 CB_STATE（game_symbols.h），而非内联文本区 0x08：
    // 仅 0x08 置位不算按下。
    {
        static FakeControl pressed_control;
        pressed_control.data[0x08] = 1;
        pressed_control.data[CB_STATE] = 0;
        int32_t state = -1;
        CHECK(adapter->control_pressed(&pressed_control, &state).ok == kQolOk && state == 0);
        pressed_control.data[CB_STATE] = 1;
        CHECK(adapter->control_pressed(&pressed_control, &state).ok == kQolOk && state == 1);
        pressed_control.data[0x08] = 0;
        pressed_control.data[CB_STATE] = 0;
        CHECK(adapter->control_pressed(&pressed_control, &state).ok == kQolOk && state == 0);
        pressed_control.data[CB_STATE] = 2;  // 非 1 的值不算按下（原版 ==1 判定）。
        CHECK(adapter->control_pressed(&pressed_control, &state).ok == kQolOk && state == 0);
    }

    static_assert(offsetof(qol::contracts::QolUiAdapter, control_item_identity) +
                      sizeof(adapter->control_item_identity) ==
                   offsetof(qol::contracts::QolUiAdapter, item_proc_source_control),
                   "ui adapter append order changed");
    static_assert(offsetof(qol::contracts::QolUiAdapter, item_proc_source_control) +
                      sizeof(adapter->item_proc_source_control) ==
                  offsetof(qol::contracts::QolUiAdapter, draw_native_text_button_background),
                  "ui adapter tail append order changed");
    static_assert(offsetof(qol::contracts::QolUiAdapter, set_button_enabled) +
                      sizeof(adapter->set_button_enabled) ==
                  offsetof(qol::contracts::QolUiAdapter, draw_native_text_button_label),
                  "ui adapter label helper must remain a tail append");
    static_assert(offsetof(qol::contracts::QolUiAdapter, draw_native_text_button_label) +
                      sizeof(adapter->draw_native_text_button_label) ==
                   offsetof(qol::contracts::QolUiAdapter, draw_native_close_button),
                   "ui adapter close draw must append the table");
    static_assert(offsetof(qol::contracts::QolUiAdapter, draw_native_close_button) +
                       sizeof(adapter->draw_native_close_button) ==
                    offsetof(qol::contracts::QolUiAdapter, craft_stuff_slot_index),
                    "craft slot identity must append the UI adapter table");
    static_assert(offsetof(qol::contracts::QolUiAdapter, craft_stuff_slot_index) +
                       sizeof(adapter->craft_stuff_slot_index) ==
                    offsetof(qol::contracts::QolUiAdapter, set_control_show),
                    "menu visibility helper must append the UI adapter table");
    static_assert(offsetof(qol::contracts::QolUiAdapter, set_control_show) +
                       sizeof(adapter->set_control_show) ==
                    offsetof(qol::contracts::QolUiAdapter, main_menu_more_games_snapshot),
                    "live menu snapshot must append the UI adapter table");
    static_assert(offsetof(qol::contracts::QolUiAdapter, main_menu_more_games_snapshot) +
                       sizeof(adapter->main_menu_more_games_snapshot) ==
                    offsetof(qol::contracts::QolUiAdapter, main_menu_text_button_is_live),
                    "menu owner identity check must follow the live tree snapshot");
    static_assert(offsetof(qol::contracts::QolUiAdapter, main_menu_text_button_is_live) +
                       sizeof(adapter->main_menu_text_button_is_live) ==
                    offsetof(qol::contracts::QolUiAdapter, main_menu_text_button_invalidate_binding),
                    "menu binding invalidation must append the UI adapter tail");
    static_assert(offsetof(qol::contracts::QolUiAdapter,
                           main_menu_text_button_invalidate_binding) +
                        sizeof(adapter->main_menu_text_button_invalidate_binding) ==
                     offsetof(qol::contracts::QolUiAdapter, main_menu_more_games_set_active),
                     "native More Games active setter must append the adapter tail");
    static_assert(offsetof(qol::contracts::QolUiAdapter, main_menu_replacement_rect_available) +
                      sizeof(adapter->main_menu_replacement_rect_available) ==
                   offsetof(qol::contracts::QolUiAdapter, ui_desc_text_cursor),
                   "UIDesc cursor operation must append the UI adapter table");
    static_assert(offsetof(qol::contracts::QolUiAdapter, ui_desc_text_cursor) +
                      sizeof(adapter->ui_desc_text_cursor) ==
                   offsetof(qol::contracts::QolUiAdapter, insert_inline_color_span),
                   "UIDesc span operation must append the UI adapter tail");
    uint64_t param[2]{};
    int source_control = 0;
    std::memcpy(&param[1], &source_control, sizeof(source_control));
    void* source_out = reinterpret_cast<void*>(static_cast<uintptr_t>(0x44));
    CHECK(adapter->item_proc_source_control(nullptr, 0x02, param, &source_out).code ==
          kQolErrInvalidArgument);
    CHECK(source_out == reinterpret_cast<void*>(static_cast<uintptr_t>(0x44)));
    CHECK(adapter->item_proc_source_control(reinterpret_cast<void*>(0x1), 0x80, param,
                                            &source_out).code == kQolErrNotReady);
    CHECK(source_out == reinterpret_cast<void*>(static_cast<uintptr_t>(0x44)));
    CHECK(adapter->item_proc_source_control(reinterpret_cast<void*>(0x1), 0x02, param,
                                            &source_out).code == kQolErrNotReady);
    CHECK(source_out == reinterpret_cast<void*>(static_cast<uintptr_t>(0x44)));
    g_game_main_thread = false;
    param[1] = reinterpret_cast<uintptr_t>(&source_control);
    CHECK(adapter->item_proc_source_control(reinterpret_cast<void*>(0x1), 0x02, param,
                                            &source_out).code == kQolErrNotReady);
    CHECK(source_out == reinterpret_cast<void*>(static_cast<uintptr_t>(0x44)));
    g_game_main_thread = true;
    param[1] = 0;
    CHECK(adapter->item_proc_source_control(reinterpret_cast<void*>(0x1), 0x04, param,
                                            &source_out).code == kQolErrNotReady);
    CHECK(source_out == reinterpret_cast<void*>(static_cast<uintptr_t>(0x44)));
    param[1] = reinterpret_cast<uintptr_t>(&source_control);
    CHECK(adapter->item_proc_source_control(reinterpret_cast<void*>(0x1), 0x02, param,
                                            &source_out).ok == kQolOk);
    CHECK(source_out == &source_control);
    CHECK(adapter->draw_native_toggle(qol::contracts::UiRect{0, 0, 20, 20}, 2, 0).code ==
          kQolErrInvalidArgument);
    CHECK(adapter->draw_native_text_button_background({0, 0, 20, 20}, 2).code ==
          kQolErrInvalidArgument);
    CHECK(adapter->set_button_enabled(reinterpret_cast<void*>(0x1), 2).code ==
          kQolErrInvalidArgument);
    CHECK(adapter->draw_native_text_button_label({0, 0, 40, 20}, nullptr, 0, 0).code ==
          kQolErrInvalidArgument);
    g_game_main_thread = false;
    CHECK(adapter->draw_native_toggle(qol::contracts::UiRect{0, 0, 20, 20}, 1, 0).code ==
          kQolErrNotReady);
    g_game_main_thread = true;
    CHECK(adapter->draw_custom_button != nullptr);
    CHECK(adapter->control_user_type != nullptr);
    CHECK(adapter->control_item != nullptr);
    CHECK(adapter->control_item_slot != nullptr);
    CHECK(adapter->touch_state != nullptr);
    CHECK(adapter->fill_rect != nullptr);
    CHECK(adapter->draw_item_porting != nullptr);
    CHECK(adapter->control_parent != nullptr);
    CHECK(adapter->find_root != nullptr);
    CHECK(adapter->insert_inline_color != nullptr);
    CHECK(adapter->ui_desc_text_cursor != nullptr);
    CHECK(adapter->insert_inline_color_span != nullptr);
    qol::contracts::QolUiAdapter adapter_before_minor_22{};
    adapter_before_minor_22.size = offsetof(qol::contracts::QolUiAdapter, ui_desc_text_cursor);
    adapter_before_minor_22.major = qol::contracts::kUiAdapterMajor;
    adapter_before_minor_22.minor = qol::contracts::kUiAdapterMinor - 1;
    const bool old_adapter_has_span_api =
        adapter_before_minor_22.minor >= qol::contracts::kUiAdapterMinor &&
        adapter_before_minor_22.ui_desc_text_cursor != nullptr &&
        adapter_before_minor_22.insert_inline_color_span != nullptr;
    CHECK(!old_adapter_has_span_api);

    uint32_t popup_count = 42;
    uintptr_t popup_top = 0x42;
    CHECK(adapter->popup_stack_snapshot(&popup_count, &popup_top).ok == kQolOk);
    CHECK(popup_count == 0 && popup_top == 0);
    g_popup_snapshot_ok = false;
    CHECK(adapter->popup_stack_snapshot(&popup_count, &popup_top).code == kQolErrNotReady);
    g_popup_snapshot_ok = true;
    g_game_main_thread = false;
    const int reads_before_non_main_thread_call = g_popup_snapshot_reads;
    CHECK(adapter->popup_stack_snapshot(&popup_count, &popup_top).code == kQolErrNotReady);
    CHECK(g_popup_snapshot_reads == reads_before_non_main_thread_call);
    g_game_main_thread = true;

    qol::contracts::QolUiComponents components{};
    components.size = sizeof(components);
    components.major = qol::contracts::kUiContractMajor;
    components.minor = qol::contracts::kUiContractMinor;
    CHECK(components.size == sizeof(qol::contracts::QolUiComponents));
    // Phase1C L0：contracts UI minor 13→14（additive 尾槽）；此行随版本常量机械同步。
    CHECK(components.major == 1 && components.minor == 14);

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
    CHECK(adapter->draw_border_rect(12, 34, 320, 140).ok == kQolOk);
    CHECK(g_border_rect[0] == 12 && g_border_rect[1] == 34 && g_border_rect[2] == 320 &&
          g_border_rect[3] == 140);
    CHECK(adapter->draw_border_rect(0, 0, 47, 140).code == kQolErrInvalidArgument);

    // Minor 21 slots remain in place; minor 22 appends the UIDesc cursor and span operations.
    CHECK(adapter->minor == 22);
    CHECK(adapter->set_clip != nullptr && adapter->reset_clip != nullptr);
    CHECK(adapter->set_clip(-12, 34, 320, 140).ok == kQolOk);
    CHECK(g_set_clip_calls == 1);
    CHECK(g_clip_rect[0] == -12 && g_clip_rect[1] == 34 && g_clip_rect[2] == 320 &&
          g_clip_rect[3] == 140);
    CHECK(adapter->reset_clip().ok == kQolOk);
    CHECK(g_reset_clip_calls == 1);

    g_game_main_thread = false;
    CHECK(adapter->set_clip(1, 2, 3, 4).code == kQolErrNotReady);
    CHECK(adapter->reset_clip().code == kQolErrNotReady);
    CHECK(g_set_clip_calls == 1 && g_reset_clip_calls == 1);
    g_game_main_thread = true;

    fn_grpx_set_clip = nullptr;
    CHECK(adapter->set_clip(1, 2, 3, 4).code == kQolErrNotReady);
    CHECK(g_set_clip_calls == 1);
    CHECK(adapter->reset_clip().ok == kQolOk);
    CHECK(g_reset_clip_calls == 2);
    fn_grpx_set_clip = &fake_set_clip;

    fn_grpx_reset_clip = nullptr;
    CHECK(adapter->reset_clip().code == kQolErrNotReady);
    CHECK(g_reset_clip_calls == 2);
    CHECK(adapter->set_clip(-1, -1, -1, -1).ok == kQolOk);
    CHECK(g_set_clip_calls == 2);
    CHECK(g_clip_rect[0] == -1 && g_clip_rect[1] == -1 && g_clip_rect[2] == -1 &&
          g_clip_rect[3] == -1);
    fn_grpx_reset_clip = &fake_reset_clip;

    CHECK(adapter->set_rect(nullptr, rect).code == kQolErrInvalidArgument);
    CHECK(adapter->get_rect(&control, nullptr).code == kQolErrInvalidArgument);
    CHECK(adapter->draw_text(nullptr, 0, 0, 0).code == kQolErrInvalidArgument);
    qol::contracts::QolUiHandle created = nullptr;
    CHECK(adapter->create_control(1, nullptr, &created).code == kQolErrInvalidArgument);
    const QolResult unsupported = adapter->create_control(99, &control, &created);
    CHECK(unsupported.code == kQolErrUnsupported);
    CHECK(unsupported.message != nullptr);
    CHECK(unsupported.message_len == std::strlen(unsupported.message));
    CHECK(adapter->set_text(&control, nullptr).code == kQolErrInvalidArgument);
    CHECK(adapter->set_text(&control, "text").ok == kQolOk);
    CHECK(g_set_text_calls == 1);
    // 内联文本缓冲上界 0x20（含 NUL）：31 字节通过，>=32 字节 fail-closed，不触达原生、
    // 不静默截断（否则会覆盖 data+0x20 ExecuteProc，点击时跳垃圾地址）。
    char at_limit[0x20]{};
    std::memset(at_limit, 'a', 0x1f);
    CHECK(adapter->set_text(&control, at_limit).ok == kQolOk);
    CHECK(g_set_text_calls == 2);
    char over_limit[0x21]{};
    std::memset(over_limit, 'b', 0x20);
    const QolResult overflow = adapter->set_text(&control, over_limit);
    CHECK(overflow.code == kQolErrBlocked);
    CHECK(adapter->set_rect(&control, rect).ok == kQolOk);
    CHECK(overflow.message != nullptr &&
          overflow.message_len == sizeof("control text exceeds inline buffer") - 1 &&
          std::strcmp(overflow.message, "control text exceeds inline buffer") == 0);
    CHECK(g_set_text_calls == 2);
    CHECK(adapter->set_draw_proc(&control, &draw_proc).ok == kQolOk);
    CHECK(g_set_draw_proc_calls == 1);
    int click_marker = 0;
    CHECK(adapter->set_click_proc(&control, &click_proc, &click_marker).ok == kQolOk);
    CHECK(g_click_proc != nullptr);
    g_click_proc(&control);
    CHECK(g_click_user == &click_marker);

    // A non-main-menu screen is unknown, not proof that a retained menu component is dead.
    FakeControl menu_parent{}, original_menu_button{}, replacement_menu_button{};
    original_menu_button.rect = UiRect{612, 488, 220, 52};
    int menu_owner = 1;
    qol::contracts::QolUiHandle menu_button = &control;
    qol::contracts::QolUiHandle menu_parent_out = &control;
    qol::contracts::UiRect menu_rect{9, 8, 7, 6};
    const int rect_reads_before_unknown = g_get_rect_calls;
    const int execute_reads_before_unknown = g_execute_proc_reads;
    g_menu_tree_known = false;
    CHECK(adapter->main_menu_more_games_snapshot(&menu_button, &menu_parent_out, &menu_rect).code ==
          kQolErrNotReady);
    CHECK(menu_button == nullptr && menu_parent_out == nullptr && menu_rect.x == 0 &&
          menu_rect.y == 0 && menu_rect.w == 0 && menu_rect.h == 0);
    int32_t menu_live = 1;
    CHECK(adapter->main_menu_text_button_is_live(&original_menu_button, &replacement_menu_button,
          &click_proc, &menu_owner, &menu_live).code == kQolErrNotReady);
    CHECK(menu_live == 0 && g_get_rect_calls == rect_reads_before_unknown &&
          g_execute_proc_reads == execute_reads_before_unknown && g_control_parent_reads == 0);

    // A stale slot address is returned as opaque data by the fake tree reader; because it is not
    // present among the trusted root's children, neither snapshot nor live checking reads its fields.
    const auto stale_slot = reinterpret_cast<void*>(static_cast<uintptr_t>(0xdead));
    g_menu_tree_known = true;
    g_menu_original = stale_slot;
    g_menu_parent = &menu_parent;
    g_menu_children = {&replacement_menu_button};
    const int rect_reads_before_stale = g_get_rect_calls;
    const int execute_reads_before_stale = g_execute_proc_reads;
    CHECK(adapter->main_menu_more_games_snapshot(&menu_button, &menu_parent_out, &menu_rect).code ==
          kQolErrNotReady);
    CHECK(menu_button == nullptr && menu_parent_out == nullptr &&
          g_get_rect_calls == rect_reads_before_stale);
    CHECK(adapter->main_menu_text_button_is_live(stale_slot, &replacement_menu_button,
          &click_proc, &menu_owner, &menu_live).ok == kQolOk);
    CHECK(menu_live == 0 && g_execute_proc_reads == execute_reads_before_stale &&
          g_control_parent_reads == 0);

    // Only a tree-proven original may be inspected for its rectangle. A replacement is live only
    // when it is also a current child and its host binding matches both callback and owner.
    g_menu_original = &original_menu_button;
    g_menu_children = {&original_menu_button, &replacement_menu_button};
    const int rect_reads_before_live = g_get_rect_calls;
    CHECK(adapter->main_menu_more_games_snapshot(&menu_button, &menu_parent_out, &menu_rect).ok ==
          kQolOk);
    CHECK(menu_button == &original_menu_button && menu_parent_out == &menu_parent &&
          menu_rect.x == 612 && menu_rect.y == 488 && menu_rect.w == 220 && menu_rect.h == 52 &&
          g_get_rect_calls == rect_reads_before_live + 1);

    // Native ControlObject_SetActive takes a bool and encodes it internally into the Active
    // field (0x20 active / 0x21 inactive); the adapter must pass the bool, not the encoded
    // value. The settings reader guard only accepts the encoded 0x20/0x21, and EventProc
    // dispatches only when the stored value is exactly 0x20.
    int active_calls_before = g_active_calls;
    int tree_reads_before_invalid_active = g_menu_tree_reads;
    int memory_reads_before_invalid_active = g_memory_accessible_reads;
    CHECK(adapter->main_menu_more_games_set_active(&original_menu_button, 2).code ==
          kQolErrInvalidArgument);
    CHECK(g_active_calls == active_calls_before && g_menu_tree_reads == tree_reads_before_invalid_active &&
          g_memory_accessible_reads == memory_reads_before_invalid_active);
    CHECK(adapter->main_menu_more_games_set_active(&original_menu_button, 0).ok == kQolOk);
    CHECK(g_active_calls == active_calls_before + 1 && g_active_control == &original_menu_button &&
          g_last_set_active_argument == 0 && g_active_value == 0x21);
    CHECK(g_active_value != 0x20);  // 停用后读回不满足 ==0x20 激活门
    CHECK(adapter->main_menu_more_games_set_active(&original_menu_button, 1).ok == kQolOk);
    CHECK(g_active_calls == active_calls_before + 2 && g_active_control == &original_menu_button &&
          g_last_set_active_argument == 1 && g_active_value == 0x20);
    CHECK(g_last_set_active_argument != 0x20);  // callee 收 bool，不是存储编码值

    // Reject before any native memory access when the caller is off-thread, the menu is unknown,
    // the slot is stale, or a live but different button is supplied.
    active_calls_before = g_active_calls;
    int memory_reads_before_rejects = g_memory_accessible_reads;
    tree_reads_before_invalid_active = g_menu_tree_reads;
    g_game_main_thread = false;
    CHECK(adapter->main_menu_more_games_set_active(&original_menu_button, 0).code ==
          kQolErrNotReady);
    CHECK(g_menu_tree_reads == tree_reads_before_invalid_active &&
          g_memory_accessible_reads == memory_reads_before_rejects &&
          g_active_calls == active_calls_before);
    g_game_main_thread = true;

    g_menu_tree_known = false;
    CHECK(adapter->main_menu_more_games_set_active(&original_menu_button, 0).code ==
          kQolErrNotReady);
    CHECK(g_memory_accessible_reads == memory_reads_before_rejects &&
          g_active_calls == active_calls_before);

    g_menu_tree_known = true;
    g_menu_original = stale_slot;
    g_menu_children = {&replacement_menu_button};
    CHECK(adapter->main_menu_more_games_set_active(stale_slot, 0).code == kQolErrNotReady);
    CHECK(g_memory_accessible_reads == memory_reads_before_rejects &&
          g_active_calls == active_calls_before);

    g_menu_original = &original_menu_button;
    g_menu_children = {&original_menu_button, &replacement_menu_button};
    CHECK(adapter->main_menu_more_games_set_active(&replacement_menu_button, 0).code ==
          kQolErrNotFound);
    CHECK(g_memory_accessible_reads == memory_reads_before_rejects &&
          g_active_calls == active_calls_before);
    g_memory_accessible = false;
    CHECK(adapter->main_menu_more_games_set_active(&original_menu_button, 0).code ==
          kQolErrNotReady);
    CHECK(g_active_calls == active_calls_before);
    g_memory_accessible = true;

    int old_menu_owner = 2;
    int reused_menu_owner = 3;
    CHECK(adapter->set_click_proc(&replacement_menu_button, &click_proc, &old_menu_owner).ok ==
          kQolOk);
    CHECK(adapter->main_menu_text_button_is_live(&original_menu_button,
          &replacement_menu_button, &click_proc, &old_menu_owner, &menu_live).ok == kQolOk);
    CHECK(menu_live == 1);
    const int execute_reads_before_wrong_owner = g_execute_proc_reads;
    CHECK(adapter->main_menu_text_button_is_live(&original_menu_button,
          &replacement_menu_button, &click_proc, &menu_owner, &menu_live).ok == kQolOk);
    CHECK(menu_live == 0 && g_execute_proc_reads == execute_reads_before_wrong_owner);
    CHECK(adapter->main_menu_text_button_is_live(&original_menu_button,
          &replacement_menu_button, &draw_proc, &old_menu_owner, &menu_live).ok == kQolOk);
    CHECK(menu_live == 0 && g_execute_proc_reads == execute_reads_before_wrong_owner);

    // The original slot may change while our replacement remains in the current root. It must
    // still report live so the component layer can safely delete this owned native child.
    g_menu_original = stale_slot;
    g_menu_children = {&replacement_menu_button};
    CHECK(adapter->main_menu_text_button_is_live(&original_menu_button,
          &replacement_menu_button, &click_proc, &old_menu_owner, &menu_live).ok == kQolOk);
    CHECK(menu_live == 1);
    g_native_calls.clear();
    CHECK(adapter->delete_control(&replacement_menu_button).ok == kQolOk);
    CHECK(g_native_calls.size() == 1 && g_native_calls[0].kind == NativeCall::kDelete &&
          g_native_calls[0].control == &replacement_menu_button);
    g_click_user = &menu_owner;
    g_click_proc(&replacement_menu_button);
    CHECK(g_click_user == &menu_owner);  // deletion removed the old owner's trampoline binding

    // Reusing the native address for a different component cannot inherit the old owner's live
    // identity, and invalidating the stale owner must leave the new registry binding intact.
    g_menu_original = &original_menu_button;
    g_menu_children = {&original_menu_button, &replacement_menu_button};
    CHECK(adapter->set_click_proc(&replacement_menu_button, &click_proc, &reused_menu_owner).ok ==
          kQolOk);
    CHECK(adapter->main_menu_text_button_is_live(&original_menu_button,
          &replacement_menu_button, &click_proc, &old_menu_owner, &menu_live).ok == kQolOk);
    CHECK(menu_live == 0);
    const int tree_reads_before_invalidate = g_menu_tree_reads;
    const int rect_reads_before_invalidate = g_get_rect_calls;
    const int execute_reads_before_invalidate = g_execute_proc_reads;
    const int screen_observations_before_invalidate = g_menu_screen_observations;
    g_menu_tree_known = false;
    CHECK(adapter->main_menu_text_button_invalidate_binding(&replacement_menu_button,
          &old_menu_owner).ok == kQolOk);
    CHECK(g_menu_tree_reads == tree_reads_before_invalidate &&
          g_get_rect_calls == rect_reads_before_invalidate &&
          g_execute_proc_reads == execute_reads_before_invalidate &&
          g_menu_screen_observations == screen_observations_before_invalidate + 1 &&
          g_last_menu_screen_observation == nullptr);
    g_click_user = nullptr;
    g_click_proc(&replacement_menu_button);
    CHECK(g_click_user == &reused_menu_owner);
    CHECK(adapter->main_menu_text_button_invalidate_binding(&replacement_menu_button,
          &reused_menu_owner).ok == kQolOk);
    g_click_user = &menu_owner;
    g_click_proc(&replacement_menu_button);
    CHECK(g_click_user == &menu_owner);
    g_menu_tree_known = false;

    // ---- Minor 21 尾槽：菜单替换几何 / 显式文本按钮参数与线程门 ----

    // 菜单替换几何：身份未知 → NotReady；越父界/兄弟碰撞 → 明确拒绝(out 0)；安全才 1.
    {
        FakeControl geom_parent{}, geom_original{}, geom_sibling{}, geom_self{};
        FakeControl geom_neighbor{};
        geom_parent.rect = UiRect{0, 0, 300, 400};
        geom_original.rect = UiRect{10, 10, 100, 50};
        geom_sibling.rect = UiRect{10, 100, 100, 50};
        geom_sibling.type = 3;
        geom_self.rect = UiRect{10, 200, 100, 50};
        geom_self.type = 3;
        // 相邻条目：其 rect 与原控件命中区本身就相交，属原生主菜单既有布局。
        geom_neighbor.rect = UiRect{95, 20, 100, 40};
        geom_neighbor.type = 3;
        geom_parent.children = {&geom_original, &geom_sibling, &geom_self, &geom_neighbor};
        g_menu_tree_known = true;
        g_menu_original = &geom_original;
        g_menu_parent = &geom_parent;
        g_menu_children = {&geom_original, &geom_sibling, &geom_self, &geom_neighbor};

        uint8_t available = 9;
        // 自己绑定的控件不算兄弟碰撞（ExecuteProc == 自有 trampoline）。
        CHECK(adapter->set_click_proc(&geom_self, &click_proc, &click_marker).ok == kQolOk);
        CHECK(adapter->main_menu_replacement_rect_available(&geom_original,
              qol::contracts::UiRect{10, 200, 100, 50}, &available).ok == kQolOk);
        CHECK(available == 1);
        // 与兄弟条目重叠 → 明确拒绝。
        available = 9;
        CHECK(adapter->main_menu_replacement_rect_available(&geom_original,
              qol::contracts::UiRect{10, 100, 100, 50}, &available).ok == kQolOk);
        CHECK(available == 0);
        // 候选与原控件命中区之外新增的重叠 → 仍拒绝（重叠右沿 195 超出原控件右沿 110）。
        available = 9;
        CHECK(adapter->main_menu_replacement_rect_available(&geom_original,
              qol::contracts::UiRect{5, 5, 200, 60}, &available).ok == kQolOk);
        CHECK(available == 0);
        // 候选整体落在原控件命中区内时，与相邻条目的重叠属既有布局 → 放行。
        available = 9;
        CHECK(adapter->main_menu_replacement_rect_available(&geom_original,
              qol::contracts::UiRect{12, 12, 96, 46}, &available).ok == kQolOk);
        CHECK(available == 1);
        // 越出父范围 → 明确拒绝。
        available = 9;
        CHECK(adapter->main_menu_replacement_rect_available(&geom_original,
              qol::contracts::UiRect{250, 0, 100, 50}, &available).ok == kQolOk);
        CHECK(available == 0);
        // 身份未知（不是当前 More Games 原控件）→ NotReady，输出归零。
        FakeControl other_button{};
        available = 9;
        CHECK(adapter->main_menu_replacement_rect_available(&other_button,
              qol::contracts::UiRect{10, 300, 100, 50}, &available).code == kQolErrNotReady);
        CHECK(available == 0);
        // 空尺寸 / 空指针参数。
        CHECK(adapter->main_menu_replacement_rect_available(&geom_original,
              qol::contracts::UiRect{10, 300, 0, 50}, &available).code == kQolErrInvalidArgument);
        CHECK(adapter->main_menu_replacement_rect_available(nullptr,
              qol::contracts::UiRect{10, 300, 100, 50}, &available).code == kQolErrInvalidArgument);
        CHECK(adapter->main_menu_replacement_rect_available(&geom_original,
              qol::contracts::UiRect{10, 300, 100, 50}, nullptr).code == kQolErrInvalidArgument);
        // 树未知 / 非主线程 → NotReady（身份不可证明）。
        g_menu_tree_known = false;
        available = 9;
        CHECK(adapter->main_menu_replacement_rect_available(&geom_original,
              qol::contracts::UiRect{10, 300, 100, 50}, &available).code == kQolErrNotReady);
        CHECK(available == 0);
        g_menu_tree_known = true;
        g_game_main_thread = false;
        CHECK(adapter->main_menu_replacement_rect_available(&geom_original,
              qol::contracts::UiRect{10, 300, 100, 50}, &available).code == kQolErrNotReady);
        g_game_main_thread = true;
        g_menu_tree_known = false;
        g_menu_original = nullptr;
        g_menu_parent = nullptr;
        g_menu_children.clear();
    }


    // 显式文本按钮两个新槽：参数与线程门先于任何渲染调用；输出布局先归零。
    {
        qol::contracts::UiTextButtonPaintDesc paint{};
        paint.visual_rect = qol::contracts::UiRect{0, 0, 220, 80};
        paint.appearance = qol::contracts::kUiTextButtonDecorated;
        qol::contracts::UiTextButtonPaintLayout layout{};
        layout.normal_bounds = qol::contracts::UiRect{9, 9, 9, 9};
        g_game_main_thread = false;
        CHECK(adapter->layout_native_text_button(&paint, &layout).code == kQolErrNotReady);
        CHECK(adapter->draw_native_text_button(&paint, "x", 0xFFFFFFFFu, 0).code == kQolErrNotReady);
        g_game_main_thread = true;
        CHECK(adapter->layout_native_text_button(nullptr, &layout).code == kQolErrInvalidArgument);
        CHECK(adapter->layout_native_text_button(&paint, nullptr).code == kQolErrInvalidArgument);
        CHECK(adapter->draw_native_text_button(nullptr, "x", 0, 0).code == kQolErrInvalidArgument);
        CHECK(adapter->draw_native_text_button(&paint, nullptr, 0, 0).code == kQolErrInvalidArgument);
        CHECK(adapter->draw_native_text_button(&paint, "x", 0, 2).code == kQolErrInvalidArgument);
    }

    CHECK(adapter->draw_control(&control).ok == kQolOk);
    CHECK(g_draw_control_calls == 1);
    char bounded_text[] = "value";
    char* bounded_builder = bounded_text;
    const QolResult bounded = adapter->insert_inline_color(&bounded_builder, 4, 'Y');
    CHECK(bounded.code == kQolErrBlocked);
    CHECK(bounded.message != nullptr);
    CHECK(bounded.message_len == std::strlen(bounded.message));
    CHECK(bounded_builder == bounded_text);

    const QolResult invalid = adapter->insert_inline_color(nullptr, 4, 'Y');
    CHECK(invalid.code == kQolErrInvalidArgument);
    CHECK(invalid.message != nullptr);
    CHECK(invalid.message_len == std::strlen(invalid.message));

    char ready_text[] = "value";
    char* ready_builder = ready_text;
    qol::game::g_attr_insert_value_color_ok = false;
    const QolResult not_ready = adapter->insert_inline_color(&ready_builder, 4096, 'Y');
    qol::game::g_attr_insert_value_color_ok = true;
    CHECK(not_ready.code == kQolErrNotReady);
    CHECK(not_ready.message != nullptr);
    CHECK(not_ready.message_len == std::strlen(not_ready.message));

    // Minor 22 exposes builder cursors as offsets into the bounded UIDesc text buffer.
    {
        constexpr size_t kCanarySize = 8;
        std::memset(g_uidesc_test_buffer, 0, sizeof(g_uidesc_test_buffer));
        std::memset(g_uidesc_test_buffer + G_UIDESC_TEXT_BUF_SIZE, 0xA5, kCanarySize);
        const char prefix[] = "$WPrevious: 77$B";
        const char row[] = "$GCurrent: -42$B";
        const uint32_t before_offset = static_cast<uint32_t>(sizeof(prefix) - 1);
        std::memcpy(g_uidesc_test_buffer, prefix, sizeof(prefix) - 1);
        char* cursor = g_uidesc_test_buffer + before_offset;
        uint32_t observed_offset = UINT32_MAX;
        CHECK(adapter->ui_desc_text_cursor(&cursor, &observed_offset).ok == kQolOk);
        CHECK(observed_offset == before_offset);

        std::memcpy(cursor, row, sizeof(row) - 1);
        cursor += sizeof(row) - 1;
        const uint32_t after_offset = static_cast<uint32_t>(cursor - g_uidesc_test_buffer);
        CHECK(adapter->ui_desc_text_cursor(&cursor, &observed_offset).ok == kQolOk);
        CHECK(observed_offset == after_offset);
        CHECK(adapter->insert_inline_color_span(&cursor, before_offset, after_offset, 'Y').ok ==
              kQolOk);
        CHECK(cursor == g_uidesc_test_buffer + after_offset + 4);
        CHECK(std::strcmp(g_uidesc_test_buffer,
                          "$WPrevious: 77$B$GCurrent: $B$Y-42$B") == 0);
        for (size_t i = 0; i < kCanarySize; ++i) {
            CHECK(static_cast<uint8_t>(g_uidesc_test_buffer[G_UIDESC_TEXT_BUF_SIZE + i]) == 0xA5);
        }

        char snapshot[sizeof(g_uidesc_test_buffer)];
        std::memcpy(snapshot, g_uidesc_test_buffer, sizeof(snapshot));
        char* saved_cursor = cursor;
        CHECK(adapter->insert_inline_color_span(&cursor, after_offset, after_offset, 'Y').code ==
              kQolErrInvalidArgument);
        CHECK(std::memcmp(snapshot, g_uidesc_test_buffer, sizeof(snapshot)) == 0 &&
              cursor == saved_cursor);
        CHECK(adapter->insert_inline_color_span(&cursor, after_offset + 1, after_offset, 'Y').code ==
              kQolErrInvalidArgument);
        CHECK(std::memcmp(snapshot, g_uidesc_test_buffer, sizeof(snapshot)) == 0 &&
              cursor == saved_cursor);
        CHECK(adapter->insert_inline_color_span(&cursor, before_offset, after_offset, 0).code ==
              kQolErrInvalidArgument);
        CHECK(std::memcmp(snapshot, g_uidesc_test_buffer, sizeof(snapshot)) == 0 &&
              cursor == saved_cursor);

        cursor = g_uidesc_test_buffer + G_UIDESC_TEXT_BUF_SIZE - 2;
        CHECK(adapter->insert_inline_color_span(&cursor, G_UIDESC_TEXT_BUF_SIZE - 12,
                                                G_UIDESC_TEXT_BUF_SIZE - 2, 'Y').code ==
              kQolErrBlocked);
        CHECK(cursor == g_uidesc_test_buffer + G_UIDESC_TEXT_BUF_SIZE - 2);
        CHECK(std::memcmp(snapshot, g_uidesc_test_buffer, sizeof(snapshot)) == 0);

        cursor = g_uidesc_test_buffer + G_UIDESC_TEXT_BUF_SIZE + 1;
        observed_offset = UINT32_MAX;
        CHECK(adapter->ui_desc_text_cursor(&cursor, &observed_offset).code == kQolErrBlocked);
        CHECK(observed_offset == 0);
        CHECK(adapter->insert_inline_color_span(&cursor, 1,
                                                G_UIDESC_TEXT_BUF_SIZE + 1, 'Y').code ==
              kQolErrBlocked);
        CHECK(std::memcmp(snapshot, g_uidesc_test_buffer, sizeof(snapshot)) == 0);

        char* empty_cursor = nullptr;
        CHECK(adapter->ui_desc_text_cursor(&empty_cursor, &observed_offset).code ==
              kQolErrNotReady);
        CHECK(adapter->insert_inline_color_span(&empty_cursor, 1, 2, 'Y').code ==
              kQolErrBlocked);
        CHECK(adapter->ui_desc_text_cursor(nullptr, &observed_offset).code ==
              kQolErrInvalidArgument);
        CHECK(adapter->ui_desc_text_cursor(&cursor, nullptr).code == kQolErrInvalidArgument);
        CHECK(std::memcmp(snapshot, g_uidesc_test_buffer, sizeof(snapshot)) == 0);

        g_uidesc_buffer_available = false;
        CHECK(adapter->ui_desc_text_cursor(&saved_cursor, &observed_offset).code ==
              kQolErrNotReady);
        g_uidesc_buffer_available = true;
        for (size_t i = 0; i < kCanarySize; ++i) {
            CHECK(static_cast<uint8_t>(g_uidesc_test_buffer[G_UIDESC_TEXT_BUF_SIZE + i]) == 0xA5);
        }
    }

    // 共享图集保护门：模拟游戏已加载共享图集（原生消费者存活），模块侧三种卸载入口
    // 对受保护 unit 必须拒绝执行、不触达底层物理卸载；加载保持幂等。
    for (const int32_t unit : kProtectedImageUnits) fake_imgsys_unit_load(unit);
    const int frame_generation = image_unit_generation(0x0f);
    CHECK(adapter->image_unit_load(0x0f).ok == kQolOk);
    CHECK(image_unit_loaded(0x0f) && image_unit_generation(0x0f) == frame_generation);
    for (const int32_t unit : kProtectedImageUnits) {
        const QolResult direct = adapter->image_unit_unload(unit);
        CHECK(direct.code == kQolErrBlocked);
        CHECK(direct.message != nullptr && direct.message_len == std::strlen(direct.message));
        CHECK(image_unit_loaded(unit));
        CHECK(adapter->unload_image_unit(unit).code == kQolErrBlocked);
        CHECK(image_unit_loaded(unit));
    }
    const QolResult option_unload = adapter->unload_option_images();
    CHECK(option_unload.code == kQolErrBlocked);
    CHECK(option_unload.message != nullptr);
    CHECK(option_unload.message_len == std::strlen(option_unload.message));
    CHECK(image_unit_loaded(0x59));
    CHECK(protected_unload_calls() == 0);
    CHECK(adapter->load_option_images().ok == kQolOk);
    CHECK(image_unit_loaded(0x59) && image_unit_generation(0x59) == 1);
    // 非保护 unit 不受门限制：两个卸载入口真实触达底层并置失效。
    fake_imgsys_unit_load(0x01);
    CHECK(adapter->unload_image_unit(0x01).ok == kQolOk);
    CHECK(!image_unit_loaded(0x01) && image_unit_unload_calls(0x01) == 1);
    CHECK(adapter->image_unit_unload(0x01).ok == kQolOk);
    CHECK(!image_unit_loaded(0x01) && image_unit_unload_calls(0x01) == 2);

    // v1.10 前缀原样保留；原生滚动入口只在尾部追加。
    CHECK(offsetof(qol::contracts::QolUiAdapter, scroll_create) ==
          offsetof(qol::contracts::QolUiAdapter, reset_clip) + sizeof(adapter->reset_clip));
    static_assert(std::is_same_v<ControlObjectSetRectFn,
        NativeControlRect (*)(void*, int64_t, int64_t, int64_t, int64_t)>);
    static_assert(std::is_same_v<ControlScrollCreateFn,
        void* (*)(void*, uint8_t, qol::contracts::QolUiNativeScrollDrawFn)>);
    qol::contracts::UiNativeScrollDesc desc{{47, 27, 259, 376}, 1200, 56, 12, 1};
    g_native_calls.clear();
    CHECK(adapter->scroll_create(&control, &desc, &native_draw_proc, &created).ok == kQolOk);
    CHECK(created == &g_scroll && g_native_parent == &control && g_scroll_draw_proc == &native_draw_proc);
    CHECK(g_native_calls.size() == 6);
    CHECK(g_native_calls[0].kind == NativeCall::kCreate && g_native_calls[0].a == 1);
    CHECK(g_native_calls[1].kind == NativeCall::kRect);
    CHECK(g_scroll.rect.x == 47 && g_scroll.rect.y == 27 && g_scroll.rect.w == 259 && g_scroll.rect.h == 376);
    CHECK(g_native_calls[2].kind == NativeCall::kOption && g_native_calls[2].a == 1200 && g_native_calls[2].b == 56);
    CHECK(g_native_calls[3].kind == NativeCall::kThreshold && g_native_calls[3].a == 12);
    CHECK(g_native_calls[4].kind == NativeCall::kBar && g_native_calls[4].a == 265 &&
          g_native_calls[4].b == 2 && g_native_calls[4].c == 372);
    CHECK(g_native_calls[5].kind == NativeCall::kBarType && g_native_calls[5].a == 0 && g_native_calls[5].b == 0);
    for (size_t i = 1; i < g_native_calls.size(); ++i) CHECK(g_native_calls[i].control == &g_scroll);

    const auto check_create_missing = [&](auto& function) {
        const auto saved = function;
        function = nullptr;
        g_native_calls.clear();
        created = &control;
        CHECK(adapter->scroll_create(&control, &desc, &native_draw_proc, &created).code == kQolErrNotReady);
        CHECK(created == nullptr && g_native_calls.empty());
        function = saved;
    };
    check_create_missing(fn_ctrl_scroll_create);
    check_create_missing(fn_ctrl_scroll_set_option);
    check_create_missing(fn_ctrl_scroll_set_min_variation);
    check_create_missing(fn_ctrl_scroll_set_bar_data);
    check_create_missing(fn_ctrl_scroll_set_bar_draw_type);
    desc.show_bar = 0;
    fn_ctrl_scroll_set_bar_data = nullptr;
    fn_ctrl_scroll_set_bar_draw_type = nullptr;
    g_native_calls.clear();
    CHECK(adapter->scroll_create(&control, &desc, &native_draw_proc, &created).ok == kQolOk);
    CHECK(g_native_calls.size() == 4);
    fn_ctrl_scroll_set_bar_data = &fake_scroll_bar;
    fn_ctrl_scroll_set_bar_draw_type = &fake_scroll_bar_type;
    g_create_scroll_ok = false;
    g_native_calls.clear();
    CHECK(adapter->scroll_create(&control, &desc, &native_draw_proc, &created).code == kQolErrNotReady);
    CHECK(created == nullptr && g_native_calls.size() == 1);
    g_create_scroll_ok = true;

    g_native_calls.clear();
    CHECK(adapter->create_control(3, nullptr, &created).code == kQolErrInvalidArgument);
    CHECK(adapter->create_control(3, &control, &created).ok == kQolOk);
    CHECK(created == &g_group && g_native_calls.size() == 1 && g_native_calls[0].control == &control);
    CHECK(g_native_calls[0].a == 0 && g_native_calls[0].b == 0 && g_native_calls[0].c == 0 && g_group.rect.h == 0);
    fn_ui_create_group_base_control = nullptr;
    CHECK(adapter->create_control(3, &control, &created).code == kQolErrNotReady && created == nullptr);
    fn_ui_create_group_base_control = &fake_group_create;
    g_create_group_ok = false;
    CHECK(adapter->create_control(3, &control, &created).code == kQolErrNotReady && created == nullptr);
    g_create_group_ok = true;

    g_native_calls.clear();
    CHECK(adapter->scroll_set_target(&g_scroll, &g_group).ok == kQolOk);
    CHECK(g_target == &g_group && g_native_calls.back().control == &g_scroll);
    CHECK(adapter->button_set_scroll(&control, &g_scroll).ok == kQolOk);
    CHECK(g_target == &g_scroll && g_native_calls.back().control == &control);
    const int button_draw_calls = g_draw_control_calls;
    CHECK(adapter->scroll_process(&g_scroll).ok == kQolOk);
    CHECK(adapter->scroll_draw(&g_scroll).ok == kQolOk);
    CHECK(g_native_calls[2].kind == NativeCall::kProcess && g_native_calls[3].kind == NativeCall::kDraw);
    CHECK(g_draw_control_calls == button_draw_calls);
    uint32_t consumed = 0;
    CHECK(adapter->control_tree_event(&control, UINT64_MAX, UINT64_MAX - 1, UINT64_MAX - 2, &consumed).ok == kQolOk);
    CHECK(consumed == 7 && g_native_calls.back().control == &control);
    CHECK(g_event_args[0] == UINT64_MAX && g_event_args[1] == UINT64_MAX - 1 && g_event_args[2] == UINT64_MAX - 2);

    set_data(g_scroll, CS_START, 20);
    set_data(g_scroll, CS_CURRENT, -103);
    set_data(g_scroll, CS_KEY_STEP, 64);
    int64_t offset = -1;
    CHECK(adapter->scroll_get_offset(&g_scroll, &offset).ok == kQolOk && offset == 123);
    g_native_calls.clear();
    CHECK(adapter->scroll_set_offset(&g_scroll, 824).ok == kQolOk);
    CHECK(g_native_calls.size() == 4 && g_native_calls[0].kind == NativeCall::kReset);
    CHECK(g_native_calls[1].kind == NativeCall::kStep && g_native_calls[1].a == 824);
    CHECK(g_native_calls[2].kind == NativeCall::kScroll && g_native_calls[2].a == 1);
    CHECK(g_native_calls[3].kind == NativeCall::kStep && g_native_calls[3].a == 64);
    CHECK(get_data(g_scroll, CS_KEY_STEP) == 64);
    CHECK(adapter->scroll_get_offset(&g_scroll, &offset).ok == kQolOk && offset == 824);
    g_native_calls.clear();
    CHECK(adapter->scroll_set_offset(&g_scroll, 825).code == kQolErrInvalidArgument);
    CHECK(adapter->scroll_set_offset(&g_scroll, -1).code == kQolErrInvalidArgument);
    CHECK(g_native_calls.empty());
    CHECK(adapter->scroll_set_offset(&g_scroll, 0).ok == kQolOk);
    CHECK(adapter->scroll_get_offset(&g_scroll, &offset).ok == kQolOk && offset == 0);
    set_data(g_scroll, CS_CONTENT_EXTENT, 12);
    g_native_calls.clear();
    CHECK(adapter->scroll_set_offset(&g_scroll, 1).code == kQolErrInvalidArgument);
    CHECK(g_native_calls.empty());
    CHECK(adapter->scroll_set_offset(&g_scroll, 0).ok == kQolOk);
    set_data(g_scroll, CS_CONTENT_EXTENT, 376);
    CHECK(adapter->scroll_set_offset(&g_scroll, 1).code == kQolErrInvalidArgument);
    set_data(g_scroll, CS_CONTENT_EXTENT, 1200);

    // 全部新入口都在任何 native 调用和 data 读取前通过真实线程门。
    g_game_main_thread = false;
    g_native_calls.clear();
    const int reads_before_thread_gate = g_data_reads;
    CHECK(adapter->create_control(3, &control, &created).code == kQolErrNotReady);
    CHECK(adapter->scroll_create(&control, &desc, &native_draw_proc, &created).code == kQolErrNotReady);
    CHECK(adapter->scroll_set_target(&g_scroll, &g_group).code == kQolErrNotReady);
    CHECK(adapter->button_set_scroll(&control, &g_scroll).code == kQolErrNotReady);
    CHECK(adapter->scroll_process(&g_scroll).code == kQolErrNotReady);
    CHECK(adapter->scroll_draw(&g_scroll).code == kQolErrNotReady);
    CHECK(adapter->scroll_get_offset(&g_scroll, &offset).code == kQolErrNotReady && offset == 0);
    CHECK(adapter->scroll_set_offset(&g_scroll, 3).code == kQolErrNotReady);
    consumed = 99;
    CHECK(adapter->control_tree_event(&control, 1, 2, 3, &consumed).code == kQolErrNotReady && consumed == 0);
    CHECK(g_native_calls.empty() && g_data_reads == reads_before_thread_gate);
    g_game_main_thread = true;

    const auto check_missing = [&](auto& function, auto operation) {
        const auto saved = function;
        function = nullptr;
        g_native_calls.clear();
        CHECK(operation().code == kQolErrNotReady);
        CHECK(g_native_calls.empty());
        function = saved;
    };
    check_missing(fn_ctrl_scroll_set_target_group, [&] { return adapter->scroll_set_target(&g_scroll, &g_group); });
    check_missing(fn_ctrl_btn_set_scroll_control, [&] { return adapter->button_set_scroll(&control, &g_scroll); });
    check_missing(fn_ctrl_scroll_process, [&] { return adapter->scroll_process(&g_scroll); });
    check_missing(fn_ctrl_scroll_draw, [&] { return adapter->scroll_draw(&g_scroll); });
    check_missing(fn_touch_handle_event, [&] { return adapter->control_tree_event(&control, 1, 2, 3, &consumed); });
    check_missing(fn_ctrl_scroll_reset_result_point, [&] { return adapter->scroll_set_offset(&g_scroll, 3); });
    check_missing(fn_ctrl_scroll_set_key_step_variation, [&] { return adapter->scroll_set_offset(&g_scroll, 3); });
    check_missing(fn_ctrl_scroll_scroll, [&] { return adapter->scroll_set_offset(&g_scroll, 3); });
    check_missing(fn_ctrl_get_data, [&] { return adapter->scroll_get_offset(&g_scroll, &offset); });
    check_missing(fn_ctrl_get_data, [&] { return adapter->scroll_set_offset(&g_scroll, 3); });
    g_scroll_data_ok = false;
    CHECK(adapter->scroll_get_offset(&g_scroll, &offset).code == kQolErrNotReady);
    CHECK(adapter->scroll_set_offset(&g_scroll, 3).code == kQolErrNotReady);
    g_scroll_data_ok = true;
    g_memory_accessible = false;
    CHECK(adapter->scroll_get_offset(&g_scroll, &offset).code == kQolErrNotReady);
    g_memory_accessible = true;
    set_data(g_scroll, CS_START, INT64_MAX);
    set_data(g_scroll, CS_CURRENT, INT64_MIN);
    CHECK(adapter->scroll_get_offset(&g_scroll, &offset).code == kQolErrNotReady);
    set_data(g_scroll, CS_START, 0);
    set_data(g_scroll, CS_CURRENT, 1);
    CHECK(adapter->scroll_get_offset(&g_scroll, &offset).code == kQolErrNotReady);
    set_data(g_scroll, CS_CURRENT, 0);

    g_native_calls.clear();
    CHECK(adapter->scroll_create(nullptr, &desc, &native_draw_proc, &created).code == kQolErrInvalidArgument);
    CHECK(adapter->scroll_create(&control, nullptr, &native_draw_proc, &created).code == kQolErrInvalidArgument);
    CHECK(adapter->scroll_create(&control, &desc, nullptr, &created).code == kQolErrInvalidArgument);
    CHECK(adapter->scroll_create(&control, &desc, &native_draw_proc, nullptr).code == kQolErrInvalidArgument);
    auto bad_desc = desc;
    bad_desc.viewport.h = 0;
    CHECK(adapter->scroll_create(&control, &bad_desc, &native_draw_proc, &created).code == kQolErrInvalidArgument);
    bad_desc = desc;
    bad_desc.content_extent = -1;
    CHECK(adapter->scroll_create(&control, &bad_desc, &native_draw_proc, &created).code == kQolErrInvalidArgument);
    CHECK(adapter->scroll_set_target(nullptr, &g_group).code == kQolErrInvalidArgument);
    CHECK(adapter->scroll_set_target(&g_scroll, nullptr).code == kQolErrInvalidArgument);
    CHECK(adapter->button_set_scroll(nullptr, &g_scroll).code == kQolErrInvalidArgument);
    CHECK(adapter->button_set_scroll(&control, nullptr).code == kQolErrInvalidArgument);
    CHECK(adapter->scroll_process(nullptr).code == kQolErrInvalidArgument);
    CHECK(adapter->scroll_draw(nullptr).code == kQolErrInvalidArgument);
    CHECK(adapter->scroll_get_offset(nullptr, &offset).code == kQolErrInvalidArgument);
    CHECK(adapter->scroll_get_offset(&g_scroll, nullptr).code == kQolErrInvalidArgument);
    CHECK(adapter->scroll_set_offset(nullptr, 0).code == kQolErrInvalidArgument);
    CHECK(adapter->control_tree_event(nullptr, 1, 2, 3, &consumed).code == kQolErrInvalidArgument);
    CHECK(adapter->control_tree_event(&control, 1, 2, 3, nullptr).code == kQolErrInvalidArgument);
    CHECK(g_native_calls.empty());

    FakeControl row{};
    g_scroll.children = {&g_group};
    g_group.children = {&row};
    CHECK(adapter->set_click_proc(&row, &click_proc, &click_marker).ok == kQolOk);
    CHECK(adapter->set_click_proc(&g_group, &click_proc, &click_marker).ok == kQolOk);
    check_missing(fn_ctrl_get_child, [&] { return adapter->delete_control(&g_scroll); });
    check_missing(fn_touch_handle_delete_control, [&] { return adapter->delete_control(&g_scroll); });
    g_native_calls.clear();
    CHECK(adapter->delete_control(&g_scroll).ok == kQolOk);
    CHECK(g_native_calls.size() == 1 && g_native_calls[0].kind == NativeCall::kDelete && g_native_calls[0].control == &g_scroll);
    g_click_user = nullptr;
    g_click_proc(&row);
    g_click_proc(&g_group);
    CHECK(g_click_user == nullptr);
    g_click_proc(&control);
    CHECK(g_click_user == &click_marker);

    std::printf("ui_adapter assertions: %d\n", assertions);
    return 0;
#undef CHECK
}
