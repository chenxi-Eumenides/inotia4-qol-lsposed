// game_ui_autosell.cpp —— 自动出售 UI 原型（入口按钮 + 只读占位面板）。
//
// 机制来源（已确认）：
// - 入口按钮挂载/点击：与扩展背包页签同宿主（原版袋容器），ControlButton 的
//   ExecuteProc 由原生 TouchHandle 递归分发（扩展页签先例）。
// - 入口按钮绘制：Scene_Draw_POPUP_SC_EQUIP 内 `bl UIDesc_Draw` 调用点独立 BL
//   patch（扩展背包占用的是同函数 +0x210 的 `bl GRPX_End`，二者地址不同）。
// - 面板：IAP 死条目 F_PANEL_UNK3_ENTER 五回调改写 + UI_SetPopupProcessInfo。
//
// 线程模型：入口安装、绘制、面板 enter/process/event 全部运行在游戏主线程，
// 故状态无需加锁；安装仅由 nativeInit 在 bridge 就绪后调用一次。
//
// 本轮范围：不做任何配置读写、不做扫描接线；关闭按钮只写日志。

#include "game_ui_autosell.h"

#include "core/native/call_patch.h"
#include "data/native/game_symbols.h"
#include "game_access.h"
#include "game_ui_kit.h"

#include <android/log.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#define AUTOSELLUI_TAG "Inotia4AutoSellUI"
#define AUTOSELLUI_LOG(...) __android_log_print(ANDROID_LOG_INFO, AUTOSELLUI_TAG, __VA_ARGS__)

namespace {

// Scene_Draw_POPUP_SC_EQUIP +0x1cc 处原 `bl UIDesc_Draw` 指令字（llvm-objdump 核对）。
constexpr uint32_t kUidescDrawBlWord = 0x97fdaadb;

constexpr size_t kPopupStateSize = 0x40;
constexpr int kPopupStateCount = 27;

// 入口按钮：与原版袋列（袋 0）同列右移 68、扩展页签（行距 70）之后的行 5。
// 由扩展页签公式换算：x = 袋0绝对 + 68，y = 袋0绝对 + 2 + 5×70（尺寸 57×57）。
constexpr int64_t kEntryRelX = 68;
constexpr int64_t kEntryRelY = 2 + 5 * 70;
constexpr int64_t kEntrySize = 57;

// 面板几何（逻辑坐标基准 960×640；实际逻辑宽由 CalcResolution 推出，真机 1408）。
// 宽度基准：2 × 0x180（假定物品详情面板宽 384）= 0x300 = 768 ≤ 1408。
constexpr int64_t kBaseW = 0x3c0;
constexpr int64_t kBaseH = 0x280;
constexpr int64_t kPanelW = 0x300;
constexpr int64_t kPanelH = 0x240;
constexpr int64_t kRowStartY = 0x70;
constexpr int64_t kRowH = 0x30;
constexpr int64_t kCloseW = 0xc0;
constexpr int64_t kCloseH = 0x30;

// ABGR 配色，与 settings/savebackup 同值。
constexpr uint32_t kGold = 0xFF00B4D7;
constexpr uint32_t kText = 0xFFCB9EE2;
constexpr uint32_t kBorderGray = 0xFF606060;

struct PlaceholderRow {
    const char* label;
    const char* value;
};

// 占位行（静态显示；不读、不写配置）。分两列，每列 4 行。
constexpr PlaceholderRow kRows[] = {
    {"总开关", "关"},
    {"装备·品质", "关闭"},
    {"装备·强化次数", "关闭"},
    {"装备·镶嵌空位", "关闭"},
    {"宝石·等级", "关闭"},
    {"宝石·属性范围", "关闭"},
    {"特殊类型", "未启用"},
};
constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));
constexpr int kRowsPerColumn = 4;

bool g_installed = false;
// 开关：JVM 线程写、游戏主线程读；默认关闭。
std::atomic<bool> g_enabled{false};
// 面板是否激活：主线程写、JVM 线程读（关闭时判断是否需要关闭面板）。
std::atomic<bool> g_panel_active{false};
// 入口控件引用：主线程与 JVM 线程都可能写，用互斥保护。
std::mutex g_entry_mtx;
void* g_entry_ctrl = nullptr;       // 当前已挂载的入口按钮控件
void* g_entry_container = nullptr;  // 挂载入口按钮的容器句柄（变化即旧控件失效）
int g_close_delay = 0;              // 仅游戏主线程访问
bool g_close_pressed = false;       // 仅游戏主线程访问
uint8_t* g_state_entry = nullptr;   // 被改写的 PopupState 条目（仅主线程访问）
uint8_t g_state_backup[kPopupStateSize] = {};
int g_state_id = -1;

void autosell_equip_desc_draw_wrapper();
bool autosell_ensure_state_injected();
void autosell_panel_enter();
void autosell_panel_process();
void autosell_panel_f3();
void autosell_panel_f4();
uint32_t autosell_panel_event(uint64_t event, uint64_t param, uint64_t param2);

// 控件绝对坐标（父链累加，同扩展背包/自定义面板口径）。
void autosell_ctrl_abs(void* ctrl, int64_t* x, int64_t* y) {
    int64_t ax = 0;
    int64_t ay = 0;
    for (void* current = ctrl; current != nullptr;) {
        uint8_t* data = static_cast<uint8_t*>(current);
        ax += *reinterpret_cast<int64_t*>(data + CO_RECT_X);
        ay += *reinterpret_cast<int64_t*>(data + CO_RECT_Y);
        current = *reinterpret_cast<void**>(data + CO_PARENT);
    }
    *x = ax;
    *y = ay;
}

// 文本绘制（独立 PopupState 下 ui_draw_text_centered 不出字；用 RGB + 官方字体封装）。
// align：0=左 / 1=右 / 2=中。
void autosell_draw_text_at(int64_t x, int64_t y, const char* text, uint32_t color, int align) {
    if (text == nullptr || text[0] == 0) return;
    if (fn_grpx_set_font_color_rgb == nullptr || fn_grpx_draw_string_with_font == nullptr) return;
    fn_grpx_set_font_color_rgb(static_cast<int32_t>(color & 0xff),
                               static_cast<int32_t>((color >> 8) & 0xff),
                               static_cast<int32_t>((color >> 16) & 0xff));
    fn_grpx_draw_string_with_font(const_cast<char*>(text), static_cast<int>(x),
                                  static_cast<int>(y), align, 1);
}

UiRect autosell_panel_rect() {
    int64_t logical_w = kBaseW;
    if (fn_calc_res_width != nullptr && fn_calc_res_height != nullptr) {
        const int64_t screen_w = kBaseW + static_cast<int64_t>(fn_calc_res_width()) * 2;
        const int64_t screen_h = kBaseH + static_cast<int64_t>(fn_calc_res_height()) * 2;
        if (screen_w > 0 && screen_h > 0) logical_w = screen_w * kBaseH / screen_h;
    }
    const int64_t x = (logical_w - kPanelW) / 2;
    const int64_t y = (kBaseH - kPanelH) / 2;
    return {x, y, kPanelW, kPanelH};
}

UiRect autosell_mask_rect(const UiRect& panel) {
    // 全屏遮罩：逻辑屏宽 = 面板宽 + 面板左右边距×2（与 settings 同口径）。
    return {0, 0, panel.x * 2 + kPanelW, kBaseH};
}

UiRect autosell_close_rect(const UiRect& panel) {
    return {panel.x + panel.w / 2 - kCloseW / 2, panel.y + panel.h - 0x50, kCloseW, kCloseH};
}

bool autosell_close_hit(const UiRect& close, uint64_t param) {
    if (param == 0) return false;
    const int64_t tx = *reinterpret_cast<const int64_t*>(param);
    const int64_t ty = *reinterpret_cast<const int64_t*>(param + 8);
    return tx >= close.x && tx < close.x + close.w && ty >= close.y && ty < close.y + close.h;
}

// ---- 入口按钮 ----

void autosell_entry_clicked(void* ctrl) {
    (void)ctrl;
    // 关闭态残留控件（不销毁原版控件）不得再打开面板。
    if (!g_enabled.load(std::memory_order_acquire)) return;
    if (g_state_id < 0 || fn_ui_set_popup_process_info == nullptr) {
        AUTOSELLUI_LOG("entry clicked but state not injected");
        return;
    }
    AUTOSELLUI_LOG("entry clicked -> open panel state=%d", g_state_id);
    fn_ui_set_popup_process_info(1, g_state_id);
}

void autosell_entry_draw(void* ctrl) {
    if (ctrl == nullptr) return;
    const UiRect size{0, 0, kEntrySize, kEntrySize};
    ui_draw_button_background(ctrl, size, 0xCC1A1008);
    ui_draw_button_border(ctrl, size, kGold, 2);
    int64_t ax = 0;
    int64_t ay = 0;
    autosell_ctrl_abs(ctrl, &ax, &ay);
    autosell_draw_text_at(ax + kEntrySize / 2, ay + 0x12, "自动", kText, 2);
    autosell_draw_text_at(ax + kEntrySize / 2, ay + 0x2e, "出售", kText, 2);
}

bool autosell_is_child(void* parent, void* child) {
    if (parent == nullptr || child == nullptr || fn_ctrl_get_count == nullptr ||
        fn_ctrl_get_child == nullptr) {
        return false;
    }
    const uint32_t count = fn_ctrl_get_count(parent);
    for (uint32_t i = 0; i < count; ++i) {
        if (fn_ctrl_get_child(parent, i) == child) return true;
    }
    return false;
}

// 入口控件有效性：必须是当前容器的子控件，且身份字段确为本模块入口按钮。
// 安全顺序：容器指针比对与子控件指针扫描都不解引用 g_entry_ctrl；只有确认它仍在
// 容器子链表中后才读取其字段（此时控件必然存活），避免对已失效控件解引用/跳转。
bool autosell_entry_valid(void* container) {
    if (container == nullptr || g_entry_ctrl == nullptr) return false;
    if (!autosell_is_child(container, g_entry_ctrl)) return false;
    uint8_t* ctrl = static_cast<uint8_t*>(g_entry_ctrl);
    if (*reinterpret_cast<uint32_t*>(ctrl + CO_TYPE) != 3) return false;    // 按钮类型
    if (*reinterpret_cast<uint32_t*>(ctrl + CO_ACTIVE) != 0x20) return false;  // 激活态
    uint8_t* data = *reinterpret_cast<uint8_t**>(ctrl + CO_DATA);
    if (data == nullptr) return false;
    return *reinterpret_cast<void**>(data + CB_EXECUTE_PROC) ==
           reinterpret_cast<void*>(&autosell_entry_clicked);
}

void autosell_install_entry(void* container) {
    // 文字必须传 nullptr：ControlButton_SetText 会把字符串写到 CO_DATA[0]，而游戏的
    // ControlItem_Draw 会把 GetData()[0] 当物品指针传给 ITEM_DrawPorting，导致野解引用崩溃。
    // 入口外观由 autosell_entry_draw 自绘，不使用控件内置文字。
    void* btn = ui_create_button(container, {kEntryRelX, kEntryRelY, kEntrySize, kEntrySize},
                                 nullptr, &autosell_entry_clicked, &autosell_entry_draw);
    if (btn == nullptr) {
        AUTOSELLUI_LOG("entry create failed container=%p", container);
        return;
    }
    if (fn_touch_handle_unuse_control_event_move != nullptr) {
        fn_touch_handle_unuse_control_event_move(btn);
    }
    g_entry_ctrl = btn;
    AUTOSELLUI_LOG("entry installed container=%p ctrl=%p", container, btn);
}

// 每帧（EQUIP 页）确保入口按钮存在并绘制。关闭态直接移除引用。
void autosell_on_equip_draw() {
    if (g_base == 0 || fn_ctrl_get_count == nullptr) return;
    std::lock_guard<std::mutex> lock(g_entry_mtx);
    if (!g_enabled.load(std::memory_order_acquire)) {
        g_entry_ctrl = nullptr;
        g_entry_container = nullptr;
        return;
    }
    // 入口按钮点击需要已注入的面板 state id；在面板打开前、能绘制入口时注入。
    autosell_ensure_state_injected();
    void* container = *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_BAG_CONTAINER_VMA);
    if (container == nullptr) {
        g_entry_ctrl = nullptr;
        g_entry_container = nullptr;
        return;
    }
    // 容器指针变化：旧控件随旧容器失效，仅移除引用（不销毁原版控件）。
    if (container != g_entry_container) {
        g_entry_ctrl = nullptr;
        g_entry_container = container;
    }
    // 容器就位守卫：面板打开初期为占位容器（无原版 6 袋标签），此时不挂载。
    if (fn_ctrl_get_count(container) < 6) return;
    // 强校验；任一不符即置空并重建。绝不绘制未通过校验的控件。
    if (!autosell_entry_valid(container)) {
        g_entry_ctrl = nullptr;
        autosell_install_entry(container);
    }
    if (g_entry_ctrl != nullptr && fn_ctrl_btn_draw != nullptr) {
        fn_ctrl_btn_draw(g_entry_ctrl);
    }
}

// ---- 面板 ----

bool autosell_ensure_state_injected() {
    if (g_state_entry != nullptr) return true;
    if (g_base == 0) return false;
    void** got = reinterpret_cast<void**>(g_base + G_POPUP_STATE_LIST_GOT_VMA);
    if (got == nullptr || *got == nullptr) return false;
    uint8_t* list = reinterpret_cast<uint8_t*>(*got);
    for (int i = 0; i < kPopupStateCount; ++i) {
        uint8_t* entry = list + i * kPopupStateSize;
        if (*reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_ENTER) !=
            g_base + F_PANEL_UNK3_ENTER) {
            continue;
        }
        std::memcpy(g_state_backup, entry, kPopupStateSize);
        *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_ENTER) =
            reinterpret_cast<uintptr_t>(&autosell_panel_enter);
        *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_PROCESS) =
            reinterpret_cast<uintptr_t>(&autosell_panel_process);
        *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_F3) =
            reinterpret_cast<uintptr_t>(&autosell_panel_f3);
        *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_F4) =
            reinterpret_cast<uintptr_t>(&autosell_panel_f4);
        *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_EVENT) =
            reinterpret_cast<uintptr_t>(&autosell_panel_event);
        g_state_entry = entry;
        g_state_id = *reinterpret_cast<int32_t*>(entry);
        AUTOSELLUI_LOG("state entry %d injected (INAPP_HOT dead entry)", g_state_id);
        return true;
    }
    return false;
}

void autosell_panel_enter() {
    AUTOSELLUI_LOG("panel enter");
    autosell_ensure_state_injected();
    g_panel_active.store(true, std::memory_order_release);
    g_close_delay = 0;
    g_close_pressed = false;
}

void autosell_panel_process() {
    if (!g_panel_active.load(std::memory_order_acquire) || g_base == 0) return;
    if (g_close_delay > 0) {
        --g_close_delay;
        if (g_close_delay == 0) {
            if (fn_ui_set_popup_process_info != nullptr) fn_ui_set_popup_process_info(3, 0);
            return;
        }
    }
    ui_begin_frame();
    const UiRect panel = autosell_panel_rect();
    const UiRect mask = autosell_mask_rect(panel);
    ui_fill_rect_alpha(mask, 0xFF000000, 0x60);
    ui_fill_rect_alpha(panel, 0xFF14100C, 0x64);
    // 金色外框。
    ui_draw_panel_decor(panel, nullptr, 0, kGold);
    ui_draw_vertical_line(panel.x, panel.y, panel.h, kGold, 3);
    ui_draw_vertical_line(panel.x + panel.w - 3, panel.y, panel.h, kGold, 3);
    autosell_draw_text_at(panel.x + panel.w / 2, panel.y + 0x14, "自动出售", kGold, 2);
    autosell_draw_text_at(panel.x + panel.w / 2, panel.y + 0x3c, "原型占位：不读取、不保存配置",
                          kText, 2);
    // 占位行两列。
    const int64_t col_w = (panel.w - 0x60) / 2;
    for (int i = 0; i < kRowCount; ++i) {
        const int col = i / kRowsPerColumn;
        const int row = i % kRowsPerColumn;
        const int64_t x = panel.x + 0x30 + col * (col_w + 0x20);
        const int64_t y = panel.y + kRowStartY + row * kRowH;
        autosell_draw_text_at(x, y + 6, kRows[i].label, kText, 0);
        const UiRect value{ x + col_w - 0x80, y + 2, 0x78, 0x24 };
        ui_fill_rect_alpha(value, 0xFF201810, 0x50);
        ui_draw_panel_decor(value, nullptr, 0, kBorderGray);
        ui_draw_vertical_line(value.x, value.y, value.h, kBorderGray, 1);
        ui_draw_vertical_line(value.x + value.w - 1, value.y, value.h, kBorderGray, 1);
        autosell_draw_text_at(value.x + value.w / 2, value.y + 5, kRows[i].value, kText, 2);
    }
    // 关闭按钮。
    const UiRect close = autosell_close_rect(panel);
    ui_fill_rect_alpha(close, 0xFF402010, 0x60);
    ui_draw_panel_decor(close, nullptr, 0, kGold);
    ui_draw_vertical_line(close.x, close.y, close.h, kGold, 2);
    ui_draw_vertical_line(close.x + close.w - 2, close.y, close.h, kGold, 2);
    autosell_draw_text_at(close.x + close.w / 2, close.y + 7, "关闭", kGold, 2);
    ui_end_frame();
}

void autosell_panel_f3() {
    AUTOSELLUI_LOG("panel f3 (terminate)");
    g_panel_active.store(false, std::memory_order_release);
    g_close_delay = 0;
    g_close_pressed = false;
}

void autosell_panel_f4() {}

uint32_t autosell_panel_event(uint64_t event, uint64_t param, uint64_t param2) {
    (void)param2;
    if (!g_panel_active.load(std::memory_order_acquire) || param == 0) return 1;
    if (g_close_delay > 0) return 1;
    const UiRect close = autosell_close_rect(autosell_panel_rect());
    if (event == 0x17) {
        if (autosell_close_hit(close, param)) g_close_pressed = true;
    } else if (event == 0x18) {
        if (g_close_pressed && autosell_close_hit(close, param)) {
            // 关闭即保存的接线点（本轮只记日志，不写配置）。
            AUTOSELLUI_LOG("close released -> close panel (no save in prototype)");
            g_close_delay = 2;
        }
        g_close_pressed = false;
    }
    return 1;
}

// 复刻原 `bl UIDesc_Draw`，随后绘制入口按钮（独立于扩展背包绘制宿主）。
void autosell_equip_desc_draw_wrapper() {
    if (fn_uidesc_draw != nullptr) fn_uidesc_draw();
    autosell_on_equip_draw();
}

}  // namespace

bool autosell_ui_install_if_ready() {
    if (g_installed) return true;
    if (!bridge_ready() || g_base == 0) return false;
    const uintptr_t call_addr =
        g_base + fn_resolve("F_SCENE_DRAW_EQUIP_VMA", F_SCENE_DRAW_EQUIP_VMA) +
        F_SCENE_DRAW_EQUIP_DESC_CALL_OFF;
    if (!call_patch_install_bl(call_addr, kUidescDrawBlWord,
                               reinterpret_cast<void*>(&autosell_equip_desc_draw_wrapper))) {
        AUTOSELLUI_LOG("scene draw patch install failed call=%p expected=0x%08x",
                       reinterpret_cast<void*>(call_addr), kUidescDrawBlWord);
        return false;
    }
    g_installed = true;
    AUTOSELLUI_LOG("scene draw patch installed call=%p", reinterpret_cast<void*>(call_addr));
    return true;
}

bool autosell_ui_enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

void autosell_ui_set_enabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    if (enabled) {
        // 开启：不在此线程触碰游戏控件树；下一次 EQUIP 页绘制时安装按钮。
        return;
    }
    // 关闭：移除入口引用（不销毁原版控件）。
    {
        std::lock_guard<std::mutex> lock(g_entry_mtx);
        g_entry_ctrl = nullptr;
        g_entry_container = nullptr;
    }
    // 面板正打开则请求关闭（弹栈后游戏会回调 f3 清理面板状态）。
    if (g_panel_active.exchange(false, std::memory_order_acq_rel)) {
        if (fn_ui_set_popup_process_info != nullptr) fn_ui_set_popup_process_info(3, 0);
        AUTOSELLUI_LOG("disabled -> panel close requested");
    }
}
