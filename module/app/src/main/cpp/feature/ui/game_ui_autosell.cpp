// game_ui_autosell.cpp —— 自动出售 UI（背包入口 + 按存档配置面板）。
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
// 配置面板只在关闭按钮释放时提交 draft：先写运行时，再按当前存档槽直写 sidecar。

#include "game_ui_autosell.h"

#include "core/native/call_patch.h"
#include "core/native/qol_log.h"
#include "data/native/game_symbols.h"
#include "feature/autosell/autosell_config.h"
#include "feature/autosell/autosell_scan.h"
#include "feature/autosell/autosell_store.h"
#include "game_access.h"
#include "game_state.h"
#include "game_ui_kit.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#define AUTOSELLUI_LOG(...) QOL_LOG_INFO(QolDomain::kAutosell, __VA_ARGS__)

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
constexpr int64_t kGearInset = 5;  // 齿轮贴图左上基准的内缩量（同扩展页签 47x47 图标内缩 5）
// 真机目视微调（逻辑像素；屏幕缩放约 0.606）：左移 ≈3 物理像素、上移 ≈1 物理像素。
constexpr int64_t kEntryNudgeX = -5;
constexpr int64_t kEntryNudgeY = -2;

// 面板几何（逻辑坐标基准 960×640；实际逻辑宽由 CalcResolution 推出，真机 1408）。
// 宽度基准：2 × 0x180（假定物品详情面板宽 384）= 0x300 = 768 ≤ 1408。
constexpr int64_t kBaseW = 0x3c0;
constexpr int64_t kBaseH = 0x280;
constexpr int64_t kPanelW = 0x300;
// 规则行提高到 0x32（原 0x26 的 1.3 倍取整），并同步放大纵向布局；
// 三个特殊类型 chip 改为一行后，面板高度 0x270（768×624）可容纳全部命中区。
// 最终布局（删除顶部居中标题与「出售规则」节标题后）：规则区起点 0x86，
// 关闭按钮相对面板底上移 0x3c，面板高度 0x250（768×592）。
constexpr int64_t kPanelH = 0x250;
constexpr int64_t kCloseW = 0xc0;
constexpr int64_t kCloseH = 0x30;

// world 左侧 HUD 齿轮：主代理真机探测与反汇编确认使用 unit 0x16 的 loc 0x00。
// 同图组 loc 0x01/0x02/0x03 分别为物品、商店和刷新图标；绘制必须 flip=1。
constexpr int32_t kGearImageUnit = 0x16;
constexpr int32_t kGearImageLoc = 0x00;

// 面板/控件填充沿用 GRPX_FillRectAlpha 的 RGB565 口径；全屏遮罩沿用 UI Kit 的
// ABGR 黑色口径，alpha 是 0..100 百分比。真机建议先验收：遮罩 0x40（64% 黑）、
// 面板 0x54（84% 深棕）。
constexpr uint32_t kMaskAlpha = 0x40;
constexpr uint32_t kPanelAlpha = 0x54;
constexpr uint32_t kPanelFill565 = 0x2104;
constexpr uint32_t kControlFill565 = 0x2945;
constexpr uint32_t kSelectedFill565 = 0x5182;

// ABGR 配色，与 settings/savebackup 同值。
constexpr uint32_t kGold = 0xFF00B4D7;
constexpr uint32_t kText = 0xFFCB9EE2;
constexpr uint32_t kBorderGray = 0xFF606060;

constexpr int kRuleCount = 5;
constexpr int kSpecialCount = 3;
// 最终布局：总开关 panel.y+0x14、说明行 panel.y+0x46；规则区起点 0x86。
constexpr int64_t kRuleTop = 0x86;
constexpr int64_t kRuleRowH = 0x32;
constexpr int64_t kRuleRowPitch = 0x3a;

constexpr const char* kRarityLabels[] = {"关", "≤白", "≤绿", "≤蓝", "≤黄", "≤紫"};
constexpr const char* kGemTierLabels[] = {"关", "≤低级", "≤中级", "≤高级", "≤顶级", "≤混沌"};
constexpr const char* kGemRangeLabels[] = {"关", "≤30%", "≤60%", "≤75%", "≤90%", "≤99%"};
constexpr const char* kSpecialLabels[] = {
    "背包", "普通徽章", "骰子",
};
constexpr uint32_t kSpecialBits[] = {
    autosell::kSpecialBackpack,
    autosell::kSpecialNormalSeal,
    autosell::kSpecialDice,
};

struct PressTarget {
    int kind = 0;      // 1=关闭，2=总开关，3=规则前，4=规则后，5=特殊类型
    int index = -1;
};

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
PressTarget g_pressed_target;       // 仅游戏主线程访问
autosell::Config g_draft;           // 面板草稿；打开时读取，关闭时一次性提交
void* g_hit_root = nullptr;         // 仅用于复用 ui_hit_test 的绝对区域
bool g_gear_image_loaded = false;    // 首次成功加载后不重复调用 unit loader
bool g_gear_failure_logged = false;  // 仅游戏主线程访问
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
    // 关闭按钮相对面板底上移 0x3c（原 0x48），与底部「特殊类型」chip 拉开约 0x05 逻辑像素。
    return {panel.x + panel.w / 2 - kCloseW / 2, panel.y + panel.h - 0x3c, kCloseW, kCloseH};
}

UiRect autosell_total_rect(const UiRect& panel) {
    // 「自动出售」总开关行：作为面板第一行（标题行），起点 panel.y + 0x14。
    return {panel.x + 0x20, panel.y + 0x14, panel.w - 0x40, 0x2a};
}

UiRect autosell_rule_row(const UiRect& panel, int index) {
    return {panel.x + 0x20, panel.y + kRuleTop + index * kRuleRowPitch, panel.w - 0x40,
            kRuleRowH};
}

UiRect autosell_rule_selector(const UiRect& panel, int index) {
    const UiRect row = autosell_rule_row(panel, index);
    return {panel.x + 0x140, row.y, 0x1a0, row.h};
}

UiRect autosell_rule_prev(const UiRect& panel, int index) {
    const UiRect selector = autosell_rule_selector(panel, index);
    return {selector.x, selector.y, 0x2c, selector.h};
}

UiRect autosell_rule_next(const UiRect& panel, int index) {
    const UiRect selector = autosell_rule_selector(panel, index);
    return {selector.x + selector.w - 0x2c, selector.y, 0x2c, selector.h};
}

UiRect autosell_special_chip(const UiRect& panel, int index) {
    const int column = index % 3;
    const int row = index / 3;
    const UiRect last_rule = autosell_rule_row(panel, kRuleCount - 1);
    const int64_t special_header_y = last_rule.y + last_rule.h + 0x0e;
    return {panel.x + 0x20 + column * 0xf0, special_header_y + 0x22 + row * 0x38, 0xd8,
            kRuleRowH};
}

int64_t autosell_special_header_y(const UiRect& panel) {
    const UiRect last_rule = autosell_rule_row(panel, kRuleCount - 1);
    return last_rule.y + last_rule.h + 0x0e;
}

bool autosell_hit_test(UiRect area, int64_t x, int64_t y) {
    if (g_hit_root != nullptr) {
        // ui_hit_test 按控件绝对原点 + 传入宽高判定；复用一个无父控件表示不同区域，
        // 不把这些临时区域挂入游戏控件树，也不会触发默认 DrawProc。
        ui_set_rect(g_hit_root, area);
        return ui_hit_test(g_hit_root, x, y, {0, 0, area.w, area.h});
    }
    return x >= area.x && x < area.x + area.w && y >= area.y && y < area.y + area.h;
}

bool autosell_touch_xy(uint64_t param, int64_t* x, int64_t* y) {
    // param 仅在 event 0x17/0x18 中是 {x, y, id} 坐标指针；其它事件不得解引用。
    if (param == 0 || x == nullptr || y == nullptr) return false;
    *x = *reinterpret_cast<const int64_t*>(param);
    *y = *reinterpret_cast<const int64_t*>(param + 8);
    return true;
}

int autosell_rule_value(int index) {
    switch (index) {
        case 0: return g_draft.rarity;
        case 1: return g_draft.enhance;
        case 2: return g_draft.socket;
        case 3: return g_draft.gem_tier;
        case 4: return g_draft.gem_range;
        default: return 0;
    }
}

int autosell_rule_max(int index) {
    switch (index) {
        case 0: return 5;
        case 1: return 32;
        case 2: return 16;
        case 3: return 5;
        case 4: return 5;
        default: return 0;
    }
}

void autosell_set_rule_value(int index, int value) {
    const int clamped = value < 0 ? 0 : (value > autosell_rule_max(index) ? autosell_rule_max(index) : value);
    switch (index) {
        case 0: g_draft.rarity = clamped; break;
        case 1: g_draft.enhance = clamped; break;
        case 2: g_draft.socket = clamped; break;
        case 3: g_draft.gem_tier = clamped; break;
        case 4: g_draft.gem_range = clamped; break;
        default: break;
    }
}

const char* autosell_rule_name(int index) {
    switch (index) {
        case 0: return "装备品质";
        case 1: return "装备强化耐久度";
        case 2: return "装备总孔数";
        case 3: return "宝石档位";
        case 4: return "宝石属性范围";
        default: return "";
    }
}

void autosell_rule_value_text(int index, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) return;
    const int raw_value = autosell_rule_value(index);
    const int max_value = autosell_rule_max(index);
    const int value = raw_value < 0 ? 0 : (raw_value > max_value ? max_value : raw_value);
    if (value == 0) {
        std::snprintf(out, out_size, "%s", "关");
        return;
    }
    if (index == 0) {
        std::snprintf(out, out_size, "%s", kRarityLabels[value]);
    } else if (index == 1) {
        std::snprintf(out, out_size, "≤%d", value - 1);
    } else if (index == 2) {
        std::snprintf(out, out_size, "≤%d", value - 1);
    } else if (index == 3) {
        std::snprintf(out, out_size, "%s", kGemTierLabels[value]);
    } else {
        std::snprintf(out, out_size, "%s", kGemRangeLabels[value]);
    }
}

void autosell_draw_box(UiRect rect, uint32_t fill, uint32_t fill_alpha, uint32_t border,
                       int border_thickness) {
    ui_fill_rect_alpha(rect, fill, fill_alpha);
    ui_draw_panel_decor(rect, nullptr, 0, border);
    ui_draw_vertical_line(rect.x, rect.y, rect.h, border, border_thickness);
    ui_draw_vertical_line(rect.x + rect.w - border_thickness, rect.y, rect.h, border,
                          border_thickness);
}

// 居中：x 用 rect 中点；y 按字符近似高度 (kTextGlyphH=20) 反推，使基线落在
// rect 几何中点。Rect 高 < 字高时退化为沿用原 +6 偏移，避免飞出框外。
void autosell_draw_centered(UiRect rect, const char* text, uint32_t color) {
    constexpr int64_t kTextGlyphH = 20;
    const int64_t y_offset = rect.h >= kTextGlyphH ? (rect.h - kTextGlyphH) / 2 : 6;
    autosell_draw_text_at(rect.x + rect.w / 2, rect.y + y_offset, text, color, 2);
}

void autosell_draw_toggle(UiRect rect, bool enabled) {
    autosell_draw_box(rect, enabled ? kSelectedFill565 : kControlFill565, 0x50,
                      enabled ? kGold : kBorderGray, 2);
    autosell_draw_centered(rect, enabled ? "开" : "关", enabled ? kGold : kText);
}

void autosell_draw_selector(UiRect selector, const char* value, bool active) {
    autosell_draw_box(selector, active ? kSelectedFill565 : kControlFill565, 0x50,
                      active ? kGold : kBorderGray, 2);
    const UiRect prev{selector.x + 2, selector.y + 2, 0x28, selector.h - 4};
    const UiRect next{selector.x + selector.w - 0x2a, selector.y + 2, 0x28, selector.h - 4};
    const UiRect value_rect{selector.x + 0x2e, selector.y + 2, selector.w - 0x5c, selector.h - 4};
    autosell_draw_centered(prev, "<", active ? kGold : kText);
    autosell_draw_centered(value_rect, value, active ? kGold : kText);
    autosell_draw_centered(next, ">", active ? kGold : kText);
}

void autosell_commit_draft() {
    autosell_apply_config(g_draft);
    const int slot = current_save_slot();
    if (slot < 0 || slot >= 3) {
        AUTOSELLUI_LOG("panel close applied config; no valid save slot=%d, skip persist", slot);
        return;
    }
    const bool persisted = autosell_store_persist(slot, g_draft);
    AUTOSELLUI_LOG("panel close applied config; persist slot=%d ok=%d", slot, persisted ? 1 : 0);
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
    // 首次绘制显式加载 unit；成功后保持加载，不调用 UnitUnload。
    if (!g_gear_image_loaded) {
        g_gear_image_loaded = ui_load_image_unit(kGearImageUnit);
    }
    void* gear_group = fn_imgsys_get_group != nullptr ? fn_imgsys_get_group(kGearImageUnit) : nullptr;
    void* gear_loc = fn_imgsys_get_loc != nullptr ? fn_imgsys_get_loc(kGearImageUnit, kGearImageLoc) : nullptr;
    // 贴图约定（同扩展背包页签 extension_bag_render.inc:59-72）：fn_grpx_draw_part 以控制
    // 左上为基准，需自行内缩居中（47x47 图标内缩 5）。ui_draw_control_image_part_centered
    // 会在绝对坐标上再加 w/2,h/2，导致绘制落在右下（约半个图标），故此处不用它。
    if (gear_group != nullptr && gear_loc != nullptr) {
        int64_t gx = 0;
        int64_t gy = 0;
        autosell_ctrl_abs(ctrl, &gx, &gy);
        if (ui_draw_image_part(kGearImageUnit, kGearImageLoc,
                               static_cast<int32_t>(gx + kGearInset),
                               static_cast<int32_t>(gy + kGearInset), 0, 1)) {
            return;
        }
    }
    if (!g_gear_failure_logged) {
        AUTOSELLUI_LOG("gear image draw failed unit=0x%x loc=0x%x load_ok=%d group=%p loc=%p loc_null=%d",
                       kGearImageUnit, kGearImageLoc, g_gear_image_loaded ? 1 : 0,
                       gear_group, gear_loc, gear_loc == nullptr ? 1 : 0);
        g_gear_failure_logged = true;
    }
    // 未找到图组/分片或图像绘制不可用时，使用可见的深色底、金边和「售」字 fallback。
    const UiRect size{0, 0, kEntrySize, kEntrySize};
    ui_draw_button_background(ctrl, size, 0xCC1A1008);
    ui_draw_button_border(ctrl, size, kGold, 2);
    int64_t ax = 0;
    int64_t ay = 0;
    autosell_ctrl_abs(ctrl, &ax, &ay);
    autosell_draw_text_at(ax + kEntrySize / 2, ay + 0x13, "售", kGold, 2);
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

// 动态定位入口：与扩展背包页签同一套算法（extension_bag_mix.inc:134-148）——
// 取袋容器 child 0（原版袋 0）的绝对位置，加上「袋列右侧 68 / 下移 2」，再减去挂载层
// （袋容器）绝对位置得到相对 rect；行号 5（与任务背包同行，扩展页签只占 0..4）。
// 不使用绝对坐标，也不依赖子控件顺序（袋 0 = child 0）。读取失败时退回常量兜底。
UiRect autosell_entry_rect(void* container) {
    UiRect out{kEntryRelX, kEntryRelY, kEntrySize, kEntrySize};
    if (container == nullptr || fn_ctrl_get_child == nullptr) return out;
    void* bag0 = fn_ctrl_get_child(container, 0);
    if (bag0 == nullptr || bag0 == g_entry_ctrl) return out;
    const UiRect bag0_rect = [&]() {
        UiRect r{};
        ui_get_rect(bag0, &r);
        return r;
    }();
    if (bag0_rect.w <= 0) return out;

    int64_t bag0_abs_x = 0;
    int64_t bag0_abs_y = 0;
    autosell_ctrl_abs(bag0, &bag0_abs_x, &bag0_abs_y);
    int64_t mount_abs_x = 0;
    int64_t mount_abs_y = 0;
    autosell_ctrl_abs(container, &mount_abs_x, &mount_abs_y);

    out.x = bag0_abs_x + 68 - mount_abs_x + kEntryNudgeX;
    // y 取「任务背包」实际 rect（袋列第 6 个 = child 5）：实测原版袋标签 y 为
    // 0/70/139/208/277/347，并非严格的 2+index*70（第 6 个差 5），故用实际值对齐。
    void* task_bag = fn_ctrl_get_child(container, 5);
    UiRect task{};
    if (task_bag != nullptr && task_bag != g_entry_ctrl && ui_get_rect(task_bag, &task) &&
        task.h > 0) {
        out.y = task.y + kEntryNudgeY;
    } else {
        out.y = bag0_abs_y + 2 - mount_abs_y + 5 * 70 + kEntryNudgeY;
    }
    return out;
}

void autosell_install_entry(void* container) {
    // 文字必须传 nullptr：ControlButton_SetText 会把字符串写到 CO_DATA[0]，而游戏的
    // ControlItem_Draw 会把 GetData()[0] 当物品指针传给 ITEM_DrawPorting，导致野解引用崩溃。
    // 入口外观由 autosell_entry_draw 自绘，不使用控件内置文字。
    const UiRect rect = autosell_entry_rect(container);
    void* btn = ui_create_button(container, rect, nullptr, &autosell_entry_clicked,
                                 &autosell_entry_draw);
    if (btn == nullptr) {
        AUTOSELLUI_LOG("entry create failed container=%p", container);
        return;
    }
    if (fn_touch_handle_unuse_control_event_move != nullptr) {
        fn_touch_handle_unuse_control_event_move(btn);
    }
    g_entry_ctrl = btn;
    AUTOSELLUI_LOG("entry installed container=%p ctrl=%p rect=(%lld,%lld)",
                   container, btn, static_cast<long long>(rect.x),
                   static_cast<long long>(rect.y));
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
    g_draft = autosell_get_runtime_config();
    if (g_hit_root == nullptr) g_hit_root = ui_create_root({0, 0, 1, 1});
    g_panel_active.store(true, std::memory_order_release);
    g_close_delay = 0;
    g_pressed_target = {};
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
    ui_fill_rect_alpha(mask, 0xFF000000, kMaskAlpha);
    ui_fill_rect_alpha(panel, kPanelFill565, kPanelAlpha);
    // 金色外框。
    ui_draw_panel_decor(panel, nullptr, 0, kGold);
    ui_draw_vertical_line(panel.x, panel.y, panel.h, kGold, 3);
    ui_draw_vertical_line(panel.x + panel.w - 3, panel.y, panel.h, kGold, 3);
    // 第一行：总开关 + 标题文字「自动出售」。
    const UiRect total = autosell_total_rect(panel);
    autosell_draw_text_at(total.x + 0x08, total.y + 6, "自动出售", kText, 0);
    const UiRect total_toggle{total.x + total.w - 0x88, total.y, 0x78, total.h};
    autosell_draw_toggle(total_toggle, g_draft.enabled);
    // 第二行：合并说明。占满整面板宽 (panel.x+0x10 .. panel.x+panel.w-0x10)
    // 避免单行截断；y 已按真机反馈下移 3 逻辑像素（≈2 物理像素）。
    autosell_draw_text_at(panel.x + panel.w / 2, panel.y + 0x49,
                          "按照规则定时出售物品。已强化/镶嵌/装备物品不会被出售。", kText, 2);

    for (int i = 0; i < kRuleCount; ++i) {
        const UiRect row = autosell_rule_row(panel, i);
        const bool active = autosell_rule_value(i) != 0;
        char value_text[32] = {};
        autosell_rule_value_text(i, value_text, sizeof(value_text));
        autosell_draw_text_at(row.x + 0x08,
                              row.y + (row.h >= 0x14 ? (row.h - 0x14) / 2 : 6),
                              autosell_rule_name(i), active ? kText : kBorderGray, 0);
        autosell_draw_selector(autosell_rule_selector(panel, i), value_text, active);
    }

    autosell_draw_text_at(panel.x + 0x20, autosell_special_header_y(panel), "特殊类型（可多选）", kGold,
                          0);
    for (int i = 0; i < kSpecialCount; ++i) {
        const UiRect chip = autosell_special_chip(panel, i);
        const bool selected = (g_draft.special_mask & kSpecialBits[i]) != 0;
        autosell_draw_box(chip, selected ? kSelectedFill565 : kControlFill565, 0x50,
                          selected ? kGold : kBorderGray, 2);
        autosell_draw_centered(chip, kSpecialLabels[i], selected ? kGold : kText);
    }

    // 关闭按钮。
    const UiRect close = autosell_close_rect(panel);
    autosell_draw_box(close, kSelectedFill565, 0x50, kGold, 2);
    autosell_draw_centered(close, "保存并关闭", kGold);
    ui_end_frame();
}

void autosell_panel_f3() {
    AUTOSELLUI_LOG("panel f3 (terminate)");
    g_panel_active.store(false, std::memory_order_release);
    g_close_delay = 0;
    g_pressed_target = {};
}

void autosell_panel_f4() {}

uint32_t autosell_panel_event(uint64_t event, uint64_t param, uint64_t param2) {
    (void)param2;
    if (!g_panel_active.load(std::memory_order_acquire) || param == 0) return 1;
    if (g_close_delay > 0) return 1;
    if (event != 0x17 && event != 0x18) return 1;
    const UiRect panel = autosell_panel_rect();
    int64_t tx = 0;
    int64_t ty = 0;
    if (!autosell_touch_xy(param, &tx, &ty)) return 1;
    if (event == 0x17) {
        g_pressed_target = {};
        if (autosell_hit_test(autosell_close_rect(panel), tx, ty)) {
            g_pressed_target = {1, -1};
        } else if (autosell_hit_test(autosell_total_rect(panel), tx, ty)) {
            g_pressed_target = {2, -1};
        } else {
            for (int i = 0; i < kRuleCount; ++i) {
                if (autosell_hit_test(autosell_rule_prev(panel, i), tx, ty)) {
                    g_pressed_target = {3, i};
                    break;
                }
                if (autosell_hit_test(autosell_rule_next(panel, i), tx, ty)) {
                    g_pressed_target = {4, i};
                    break;
                }
            }
            if (g_pressed_target.kind == 0) {
                for (int i = 0; i < kSpecialCount; ++i) {
                    if (autosell_hit_test(autosell_special_chip(panel, i), tx, ty)) {
                        g_pressed_target = {5, i};
                        break;
                    }
                }
            }
        }
    } else if (event == 0x18) {
        const PressTarget pressed = g_pressed_target;
        bool released_inside = false;
        if (pressed.kind == 1) {
            released_inside = autosell_hit_test(autosell_close_rect(panel), tx, ty);
        } else if (pressed.kind == 2) {
            released_inside = autosell_hit_test(autosell_total_rect(panel), tx, ty);
        } else if (pressed.kind == 3) {
            released_inside = autosell_hit_test(autosell_rule_prev(panel, pressed.index), tx, ty);
        } else if (pressed.kind == 4) {
            released_inside = autosell_hit_test(autosell_rule_next(panel, pressed.index), tx, ty);
        } else if (pressed.kind == 5) {
            released_inside = autosell_hit_test(autosell_special_chip(panel, pressed.index), tx, ty);
        }
        if (released_inside) {
            if (pressed.kind == 1) {
                autosell_commit_draft();
                AUTOSELLUI_LOG("close released -> save and close panel");
                g_close_delay = 2;
            } else if (pressed.kind == 2) {
                g_draft.enabled = !g_draft.enabled;
            } else if (pressed.kind == 3 || pressed.kind == 4) {
                const int delta = pressed.kind == 3 ? -1 : 1;
                autosell_set_rule_value(pressed.index, autosell_rule_value(pressed.index) + delta);
            } else if (pressed.kind == 5 && pressed.index >= 0 && pressed.index < kSpecialCount) {
                g_draft.special_mask ^= kSpecialBits[pressed.index];
            }
        }
        g_pressed_target = {};
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
