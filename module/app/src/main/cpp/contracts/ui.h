#pragma once

// UI 跨边界契约只放 POD 与函数指针，不放 STL、异常或虚函数。
// QolUiComponents 由 core.ui 实现并提供；QolUiAdapter 由 host 注入。
// 全部接口仅允许在游戏主线程调用；描述符字符串只需在调用期间有效，组件必须复制。
// 本头禁止出现 CO_*、CB_*、VMA、game_symbols.h 或游戏对象类型。

#include <cstdint>

#include "contracts/result.h"

namespace qol::contracts {

constexpr const char* kUiComponentsCapabilityId = "ui_components";
constexpr uint16_t kUiContractMajor = 1, kUiContractMinor = 0;

enum class UiAlign : uint8_t { kLeft = 0, kRight = 1, kCenter = 2 };
using QolUiHandle = void*;
using QolUiClickFn = void (*)(void* user);
using QolUiChangedFn = void (*)(void* user, int32_t value);
using QolUiScrollFn = void (*)(void* user, int32_t offset);

struct UiRect {
    int64_t x, y, w, h;
};
struct UiButtonDesc {
    UiRect rect;
    const char* text;
    uint32_t style;
    QolUiClickFn on_click;
    void* user;
};
struct UiPanelDesc {
    UiRect rect, mask_rect;
    uint32_t panel_color, flags;
};
struct UiLabelDesc {
    UiRect rect;
    const char* text;
    uint32_t color;
    UiAlign align;
};
struct UiToggleDesc {
    UiRect rect;
    const char* text;
    uint32_t style;
    int32_t value;
    QolUiChangedFn on_changed;
    void* user;
};
struct UiSelectDesc {
    UiRect rect;
    const char* const* options;
    uint32_t option_count;
    int32_t selected;
    uint32_t style;
    QolUiChangedFn on_changed;
    void* user;
};
struct UiScrollViewDesc {
    UiRect viewport;
    int32_t content_extent;
    int32_t offset;
    int32_t page_size;
    uint32_t flags;
    QolUiScrollFn on_scroll;
    void* user;
};

struct QolUiComponents {
    /* 由 core.ui 实现并提供（U11-2）；本 lane 只定义 */
    uint32_t size;
    uint16_t major, minor;
    QolResult (*panel_create)(const UiPanelDesc*, QolUiHandle parent, QolUiHandle* out);
    QolResult (*button_create)(const UiButtonDesc*, QolUiHandle parent, QolUiHandle* out);
    QolResult (*label_create)(const UiLabelDesc*, QolUiHandle parent, QolUiHandle* out);
    QolResult (*toggle_create)(const UiToggleDesc*, QolUiHandle parent, QolUiHandle* out);
    QolResult (*select_create)(const UiSelectDesc*, QolUiHandle parent, QolUiHandle* out);
    QolResult (*scroll_view_create)(const UiScrollViewDesc*, QolUiHandle parent, QolUiHandle* out);
    QolResult (*set_rect)(QolUiHandle, UiRect);
    QolResult (*button_set_text)(QolUiHandle, const char*);
    QolResult (*label_set_text)(QolUiHandle, const char*);
    QolResult (*toggle_set_value)(QolUiHandle, int32_t);
    QolResult (*select_set_selected)(QolUiHandle, int32_t);
    QolResult (*scroll_set_offset)(QolUiHandle, int32_t);
    QolResult (*dispatch_touch)(QolUiHandle root, uint32_t phase, int64_t x, int64_t y);
    QolResult (*destroy)(QolUiHandle);
};

using QolUiDrawFn = void (*)(void* control);
struct QolUiAdapter {
    /* host 注入，背后 game/access/ui_kit|ui_components */
    uint32_t size;
    uint16_t major, minor;
    QolResult (*create_control)(uint32_t kind, QolUiHandle parent, QolUiHandle* out);
    QolResult (*set_rect)(QolUiHandle, UiRect);
    QolResult (*get_rect)(const QolUiHandle, UiRect*);
    QolResult (*set_text)(QolUiHandle, const char*);
    QolResult (*set_draw_proc)(QolUiHandle, QolUiDrawFn);
    QolResult (*set_click_proc)(QolUiHandle, QolUiClickFn, void* user);
    int32_t (*hit_test)(const QolUiHandle, int64_t x, int64_t y, UiRect);
    QolResult (*draw_control)(QolUiHandle);
    QolResult (*begin_frame)();
    QolResult (*fill_rect_alpha)(UiRect, uint32_t color, uint32_t alpha);
    QolResult (*draw_panel_decor)(UiRect, const int64_t*, uint32_t, uint32_t);
    QolResult (*draw_vertical_line)(int64_t, int64_t, int64_t, uint32_t, int32_t);
    QolResult (*draw_text)(QolUiHandle, int32_t, int32_t, uint32_t);
    QolResult (*draw_text_centered)(QolUiHandle, int32_t, uint32_t);
    QolResult (*load_image_unit)(int32_t);
    QolResult (*unload_image_unit)(int32_t);
    QolResult (*draw_image_part)(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
    QolResult (*draw_control_image_part)(QolUiHandle, int32_t, int32_t, int32_t, int32_t);
    QolResult (*draw_control_image_part_centered)(QolUiHandle, int32_t, int32_t, int32_t, int32_t);
    QolResult (*draw_button_background)(QolUiHandle, UiRect, uint32_t);
    QolResult (*draw_button_border)(QolUiHandle, UiRect, uint32_t, int32_t);
    QolResult (*draw_control_alpha_overlay)(QolUiHandle, UiRect, uint32_t, uint32_t);
    QolResult (*end_frame)();
};

}  // namespace qol::contracts
