#pragma once

// UI 跨边界契约只放 POD 与函数指针，不放 STL、异常或虚函数。
// QolUiComponents 由 core.custom_ui 实现并提供；QolUiAdapter 由 host 注入。
// 全部接口仅允许在游戏主线程调用；描述符字符串只需在调用期间有效，组件必须复制。
// 本头禁止出现 CO_*、CB_*、VMA、game_symbols.h 或游戏对象类型。

#include <cstdint>

#include "result.h"
#include "item_handle.h"

namespace qol::contracts {

constexpr const char* kUiComponentsCapabilityId = "ui_components";
constexpr uint16_t kUiContractMajor = 1, kUiContractMinor = 11;
constexpr uint16_t kUiAdapterMajor = 1, kUiAdapterMinor = 16;

enum class UiAlign : uint8_t { kLeft = 0, kRight = 1, kCenter = 2 };
using QolUiHandle = void*;
using QolUiClickFn = void (*)(void* user);
using QolUiPopupEnterFn = void (*)();
using QolUiChangedFn = void (*)(void* user, int32_t value);
using QolUiScrollFn = void (*)(void* user, int32_t offset);
enum class QolUiRootKind : uint32_t {
    kEquipBagContainer = 0,
    kEquipItemRoot = 1,
    kStoreBagContainer = 2,
    kStoreItemRoot = 3,
    kMixBagContainer = 4,
    kMixItemRoot = 5,
};

struct UiRect {
    int64_t x, y, w, h;
};

// text_button_create interprets style as a native font choice; button_create keeps its own style.
enum UiTextButtonStyle : uint32_t {
    kUiTextButtonFontDefault = 0,
    kUiTextButtonFontLarger = 1,
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

using QolUiListItemDrawFn = void (*)(void* user, uint32_t index, QolUiHandle row, uint8_t selected);
using QolUiListSelectFn = void (*)(void* user, uint32_t index);
struct UiScrollListDesc {
    UiRect viewport;
    int32_t item_height;
    uint32_t item_count;
    int32_t initial_selected;  // -1 表示未选中；空列表必须为 -1。
    QolUiListItemDrawFn draw_item;
    QolUiListSelectFn on_select;
    void* user;
};

struct UiNativeToggleDesc {
    UiRect rect;
    int32_t value;
    uint8_t enabled;
    QolUiChangedFn on_changed;
    void* user;
};

// 自适应导航按钮视觉模式：auto 按面板栈深判定（栈顶仅一层显示 X，多层级显示返回箭头）。
enum UiNavButtonMode : uint32_t {
    kUiNavButtonAuto = 0,
    kUiNavButtonForceClose = 1,
    kUiNavButtonForceBack = 2,
};

// X 与返回箭头融合的导航按钮：点击一律请求面板栈弹栈顶（延迟安全；栈顶仅一层时即退出
// 本页，语义与返回一致）；on_click 为可空的消费者附加通知，不影响弹栈行为。
struct UiNavButtonDesc {
    UiRect rect;
    uint32_t mode;
    QolUiClickFn on_click;
    void* user;
};

using QolUiHelpDetailDrawFn = void (*)(void* user, UiRect right_viewport);
struct UiHelpOption {
    void* user;
    QolUiHelpDetailDrawFn draw_detail;
};
struct UiHelpPageDesc {
    UiRect panel_rect;       // 相对 parent；parent 为空时为屏幕坐标。
    UiRect left_viewport;    // 相对 panel；框向外扩 8，右侧另留 16 给原生滚动条。
    UiRect right_viewport;   // 相对 panel；框向外扩 8；右栏不滚动、不裁剪。
    int32_t item_height;
    const UiHelpOption* options;  // 创建时复制；user/回调须有效至 destroy/unbind。
    uint32_t option_count;
    int32_t initial_selected;    // -1 无选择；空页须为 -1。
    QolUiListItemDrawFn draw_item;
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
    QolResult (*draw_container)(UiRect);
    QolResult (*draw_container_ornamented)(UiRect);
    // 原生列表上限 1024 行、extent <= INT32_MAX；parent 可借用，但不得嵌套 scroll。
    // destroy 释放 owned native 子树；回调内 destroy 仅置 pending，下一 process 安全点执行。
    QolResult (*scroll_list_create)(const UiScrollListDesc*, QolUiHandle parent, QolUiHandle* out);
    QolResult (*scroll_list_process)(QolUiHandle list);
    QolResult (*scroll_list_draw)(QolUiHandle list);
    QolResult (*scroll_list_get_offset)(QolUiHandle list, int64_t* out);
    // root 中所有交互按钮须有 ExecuteProc；不能混用 dispatch_touch。
    QolResult (*dispatch_native_event)(QolUiHandle root, uint64_t event, uint64_t param,
                                        uint64_t param2, uint32_t* consumed);
    // help_page 拥有完整 native 子树；destroy/unbind 通过 Adapter 递归删除一次。
    // 两栏框均在 panel 内且不得重叠；绘制使用绝对坐标，detail 也收到绝对 viewport。
    // 消费者加载框图集 0x0f；回调不得同步创建/删除控件，destroy 仅排队。
    QolResult (*help_page_create)(const UiHelpPageDesc*, QolUiHandle parent, QolUiHandle* out);
    QolResult (*help_page_process)(QolUiHandle page);
    QolResult (*help_page_draw)(QolUiHandle page);
    QolResult (*help_page_set_selected)(QolUiHandle page, int32_t index);
    QolResult (*help_page_get_selected)(QolUiHandle page, int32_t* out);
    // 标准返回按钮助手：页面只负责把按钮放进自己的树并定位，返回/关闭语义归面板宿主
    // （嵌套返回上一级；最外层关闭并归还 popup；宿主负责去重、延迟 pop/restore 与取消）。
    // back_button_create 创建 0x59 样式按钮：原生事件树页面由助手绑定 ExecuteProc，
    // 组件/手工命中页面须经 back_button_touch 命中。助手负责加载并借用共享图集 0x59（不卸载）。
    QolResult (*back_button_create)(QolUiHandle parent, UiRect rect, QolUiHandle* out);
    // 绘制标准返回按钮外观：原生按钮读 control_pressed，组件按钮读 back_button_touch
    // 维护的按下态；0x59 不可用时退化自绘。
    QolResult (*back_button_draw)(QolUiHandle button);
    // 手工命中页的命中入口：phase 1=按下 / 2=抬起；按下落于按钮内记录按下态，抬起仍在
    // 按钮内则请求宿主 back，移出取消。仅供组件按钮使用。
    QolResult (*back_button_touch)(QolUiHandle button, uint32_t phase, int64_t x, int64_t y);
    // 直接请求宿主 back（f3/f4 等游戏驱动的关闭入口）：与按钮激活同语义，可安全在回调中调用。
    QolResult (*back_request)();
    // 不经控件 data 的文本绘制原语：以 control 绝对原点为基准，直接绘制传入字符串。
    // 原生按钮/标签的文本内联缓冲只有 0x20 字节（含 NUL），ControlButton_SetText 用
    // strcpy 无界拷贝；超过 31 字节会覆盖 ExecuteProc 槽。长文本必须走此入口自绘，
    // 不得再写入行按钮内联文本。
    QolResult (*draw_control_text)(QolUiHandle control, const char* text, int32_t x_offset,
                                   int32_t y_offset, uint32_t color, int32_t align, int32_t font);
    // 可缩放原生风格开关（自绘，不依赖原生控件；任意 rect，仅主线程、当前 frame 内绘制）。
    // rect 为绝对坐标；value != 0 为开（亮），0 为关（暗）。颜色取自 contracts/ui_theme.h。
    QolResult (*switch_draw)(UiRect rect, int32_t value);
    // 开关命中：phase 1=按下 / 2=抬起。按下点落在 rect 内时翻转 *inout_value（与组件的
    // kToggle 一致，按下即切换），并仅作用于 rect 自身，不改变父行其它区域的点击语义。
    QolResult (*switch_touch)(UiRect rect, uint32_t phase, int64_t x, int64_t y,
                              int32_t* inout_value);
    // Native options-style capsule button. The parent may be a borrowed native list row;
    // that row remains owned by its list and must not be destroyed by the toggle consumer.
    QolResult (*native_toggle_create)(const UiNativeToggleDesc*, QolUiHandle parent,
                                      QolUiHandle* out);
    QolResult (*native_toggle_set_value)(QolUiHandle, int32_t value);
    QolResult (*native_toggle_get_value)(QolUiHandle, int32_t* out);
    QolResult (*native_toggle_set_enabled)(QolUiHandle, uint8_t enabled);
    QolResult (*native_toggle_draw)(QolUiHandle);
    // Returns a borrowed row button handle for creating list-owned child controls only.
    QolResult (*scroll_list_get_row)(QolUiHandle list, uint32_t index, QolUiHandle* out_row);
    // Selection is enabled by default. Disabled lists still dispatch genuine row child buttons,
    // but never update selected or invoke the list's selection callback.
    QolResult (*scroll_list_set_selection_enabled)(QolUiHandle list, uint8_t enabled);
    // Creates a separately hit-tested native button child owned by the scroll list. rect is
    // relative to the row; text must fit the native inline buffer, so custom labels are drawn
    // separately with draw_control_text.
    QolResult (*scroll_row_button_create)(QolUiHandle list, uint32_t row, UiRect rect,
                                          QolUiClickFn on_click, void* user,
                                          QolUiHandle* out_button);
    // 原生底图按钮的公共自定义文本封装；组件持有 UTF-8 标签。parent 也可为列表借用行，
    // 此时列表拥有按钮子树，工厂将原生按钮绑定到所属 scroll。
    QolResult (*text_button_create)(const UiButtonDesc*, QolUiHandle parent,
                                    QolUiHandle* out);
    QolResult (*text_button_set_enabled)(QolUiHandle, uint8_t enabled);
    QolResult (*text_button_draw)(QolUiHandle);
    // 自适应导航按钮：X（unit 0x0f loc 0x3a / 按下叠加 0x34）与返回箭头（unit 0x59 loc 3 /
    // 按下叠加 loc 2）融合。auto 模式绘制时按面板栈深选图；点击一律延迟请求弹面板栈顶，
    // on_click 仅作附加通知。模式强制用于特殊页面与演示。
    QolResult (*nav_button_create)(const UiNavButtonDesc*, QolUiHandle parent,
                                   QolUiHandle* out);
    QolResult (*nav_button_set_enabled)(QolUiHandle, uint8_t enabled);
    QolResult (*nav_button_draw)(QolUiHandle);
};

using QolUiDrawFn = void (*)(void* control);
using QolUiNativeScrollDrawFn = void (*)(QolUiHandle scroll, int64_t displacement);
struct UiNativeScrollDesc {
    UiRect viewport;
    int64_t content_extent;
    int64_t max_drag_delta;
    int64_t drag_threshold;
    uint8_t show_bar;
};
using QolUiControlProcFn = uint64_t (*)(void*, uint64_t, void*, void*);
struct QolUiItemIcon {
    int32_t image_unit;
    int32_t image_loc;
    int32_t draw_type;
    int32_t flip;
    uint8_t centered;
};
using QolUiLoadOptionImagesFn = QolResult (*)();
using QolUiUnloadOptionImagesFn = QolResult (*)();
using QolUiDrawBackButtonFn = QolResult (*)(QolUiHandle, uint8_t pressed);
using QolUiDrawToggleFn = QolResult (*)(QolUiHandle, uint8_t enabled);
using QolUiDrawItemIconFn = QolResult (*)(QolUiHandle, QolUiItemIcon);
using QolUiCustomButtonFn = QolResult (*)(QolUiHandle, UiRect, uint32_t, uint32_t, int32_t,
                                          const char*, uint32_t, int32_t, int32_t);
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
    // 游戏拥有的共享图集 0x0f/0x59/0x4f/0x50 只能确保加载与借用绘制；
    // 两种 unit 卸载入口及 unload_option_images 均返回 kQolErrBlocked，不释放贴图。
    QolResult (*unload_image_unit)(int32_t);
    QolResult (*draw_image_part)(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
    QolResult (*draw_control_image_part)(QolUiHandle, int32_t, int32_t, int32_t, int32_t);
    QolResult (*draw_control_image_part_centered)(QolUiHandle, int32_t, int32_t, int32_t, int32_t);
    QolResult (*draw_button_background)(QolUiHandle, UiRect, uint32_t);
    QolResult (*draw_button_border)(QolUiHandle, UiRect, uint32_t, int32_t);
    QolResult (*draw_control_alpha_overlay)(QolUiHandle, UiRect, uint32_t, uint32_t);
    QolResult (*end_frame)();

    // U11-3：面板消费者仍需使用的控件树、原版绘制和弹窗原语。
    QolResult (*find_child_in_area)(QolUiHandle, UiRect, QolUiHandle*);
    QolResult (*clone_button_style)(QolUiHandle, QolUiHandle, QolUiClickFn, void*);
    QolResult (*draw_control_title_image_part)(QolUiHandle, int32_t);
    QolResult (*show_text_popup)(int32_t);
    QolResult (*child_count)(QolUiHandle, uint32_t*);
    QolResult (*child_at)(QolUiHandle, uint32_t, QolUiHandle*);
    QolResult (*screen_inset_width)(int32_t*);
    QolResult (*screen_inset_height)(int32_t*);
    QolResult (*unuse_control_event_move)(QolUiHandle);
    QolResult (*set_popup_process_info)(int32_t, int32_t);
    QolResult (*push_popup_state)(uint32_t);
    QolResult (*uidesc_draw)();
    QolResult (*set_font_color)(uint32_t);
    QolResult (*set_font_color_rgb)(int32_t, int32_t, int32_t);
    QolResult (*draw_string_with_font)(const char*, int32_t, int32_t, int32_t, int32_t);
    QolResult (*draw_part)(QolUiHandle, int32_t, int32_t, QolUiHandle, int32_t, int32_t);
    QolResult (*image_unit_load)(int32_t);
    QolResult (*image_unit_unload)(int32_t);
    QolResult (*image_group)(int32_t, QolUiHandle*);
    QolResult (*image_loc)(int32_t, int32_t, QolUiHandle*);
    QolUiLoadOptionImagesFn load_option_images;
    QolUiUnloadOptionImagesFn unload_option_images;
    QolUiDrawBackButtonFn draw_back_button;
    QolUiDrawToggleFn draw_toggle;
    QolUiDrawItemIconFn draw_item_icon;
    QolUiCustomButtonFn draw_custom_button;

    // U11-3：原版背包入口需要的容器定位与控件身份查询。
    QolResult (*find_original_bag_container)(QolUiHandle*);
    QolResult (*control_type)(QolUiHandle, int32_t*);
    QolResult (*control_active)(QolUiHandle, int32_t*);
    QolResult (*control_data)(QolUiHandle, QolUiHandle*);
    QolResult (*execute_proc_matches)(QolUiHandle, QolUiClickFn, int32_t*);
    QolResult (*control_absolute_origin)(QolUiHandle, int64_t*, int64_t*);
    QolResult (*control_pressed)(QolUiHandle, int32_t*);
    QolResult (*touch_coordinates)(uint64_t, int64_t*, int64_t*);

    // 运行时控件实例面：宿主负责定位、校验、绑定和还原动态 ExecuteProc 槽。
    QolResult (*find_main_menu_more_games)(QolUiHandle*);
    QolResult (*bind_main_menu_more_games)(QolUiClickFn, uint64_t*);
    QolResult (*unbind_main_menu_more_games)(uint64_t);
    QolResult (*popup_stack_top_matches)(QolUiPopupEnterFn, int32_t*);
    QolResult (*clear_control_proc)(QolUiHandle);
    QolResult (*screen_name)(const char**);

    // G3：扩展背包使用的控件事实与原版物品绘制原语。
    QolResult (*control_user_type)(QolUiHandle, int32_t*);
    QolResult (*control_item)(QolUiHandle, QolUiHandle*);
    QolResult (*control_item_slot)(QolUiHandle, int32_t*);
    QolResult (*touch_state)(QolUiHandle*, QolUiHandle*, int64_t*, int64_t*);
    QolResult (*fill_rect)(UiRect, uint32_t);
    QolResult (*draw_item_porting)(QolUiHandle, int32_t, int32_t, int32_t, int32_t);
    QolResult (*control_parent)(QolUiHandle, QolUiHandle*);
    QolResult (*find_root)(QolUiRootKind, QolUiHandle*);
    QolResult (*insert_inline_color)(void* builder, uint32_t max_text_bytes, uint8_t color_code);

    // 扩展背包运行时控件操作；宿主负责定位游戏控件函数和游戏内存分配器。
    QolResult (*delete_control)(QolUiHandle);
    QolResult (*set_control_item)(QolUiHandle, QolUiHandle);
    QolResult (*set_control_proc)(QolUiHandle, QolUiControlProcFn);
    QolResult (*set_control_user_type)(QolUiHandle, int32_t);
    QolResult (*set_control_data)(QolUiHandle, QolUiHandle);
    QolResult (*allocate_control_data)(uint32_t, QolUiHandle*);
    QolResult (*set_touch_cursor)(QolUiHandle, QolUiHandle);
    QolResult (*reset_touch_moving)();
    QolResult (*reset_touch_selected)();
    QolResult (*control_is_moving)(QolUiHandle, int32_t*);
    QolResult (*clear_control_drag_flags)(QolUiHandle);
    QolResult (*clear_touch_state)();
    QolResult (*set_touch_release_input)(int64_t, int64_t, uint64_t);
    QolResult (*clear_touch_drop_event)();

    // Read-only game popup stack snapshot; main-thread only.
    QolResult (*popup_stack_snapshot)(uint32_t*, uintptr_t*);
    // 真实 pthread 主线程判定（minor 7 追加）；游戏 popup 读写的权威线程门，
    // 消费者不得以编译期常量替代。
    int32_t (*is_main_thread)();
    QolResult (*draw_border_rect)(int32_t x, int32_t y, int32_t w, int32_t h);
    QolResult (*image_loc_size)(int32_t unit, int32_t loc, int32_t* out_width,
                                int32_t* out_height);
    // 裁剪不支持嵌套；绘制后 reset_clip 复原整屏，不恢复上一层。
    QolResult (*set_clip)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);
    QolResult (*reset_clip)();
    // 原生纵向 scroll：挂树不自动 process/draw；裁剪不可嵌套。
    QolResult (*scroll_create)(QolUiHandle parent, const UiNativeScrollDesc*,
                               QolUiNativeScrollDrawFn, QolUiHandle* out);
    QolResult (*scroll_set_target)(QolUiHandle scroll, QolUiHandle group);
    QolResult (*button_set_scroll)(QolUiHandle button, QolUiHandle scroll);
    QolResult (*scroll_process)(QolUiHandle scroll);
    QolResult (*scroll_draw)(QolUiHandle scroll);
    QolResult (*scroll_get_offset)(QolUiHandle scroll, int64_t* out);
    QolResult (*scroll_set_offset)(QolUiHandle scroll, int64_t offset);
    // 原生事件树中的交互按钮须绑定 ExecuteProc；不得并行手工派发点击。
    QolResult (*control_tree_event)(QolUiHandle root, uint64_t event, uint64_t param,
                                    uint64_t param2, uint32_t* consumed);
    // 使用 Options 原生亮/暗开关贴图等比绘制；rect 是可用范围，pressed 仅叠原生按压态。
    QolResult (*draw_native_toggle)(UiRect rect, int32_t value, uint8_t pressed);
    // Returned item handles are registry identities; control handles are frame-local ABI values.
    QolResult (*control_item_identity)(QolUiHandle control, QolItemHandle* out_item);
    QolResult (*item_proc_source_control)(QolUiHandle destination, uint64_t event,
                                          const void* param, QolUiHandle* out_source);
    // 仅绘制已确认的原版文本按钮背景；不管理标签、事件或业务状态。
    QolResult (*draw_native_text_button_background)(UiRect rect, uint8_t pressed);
    QolResult (*set_button_enabled)(QolUiHandle, uint8_t enabled);
    // 游戏字体测量、字宽预算与居中由 Adapter 执行；文本仍由组件持有。
    QolResult (*draw_native_text_button_label)(UiRect rect, const char* text, uint32_t color,
                                              int32_t font);
    // 仅绘制原生 X 关闭贴图（unit 0x0f loc 0x3a，pressed 叠加 loc 0x34 中心对齐）；
    // 不管理命中、事件或业务状态。图集由游戏拥有，只确保加载，不卸载。
    QolResult (*draw_native_close_button)(UiRect rect, uint8_t pressed);
};

}  // namespace qol::contracts
