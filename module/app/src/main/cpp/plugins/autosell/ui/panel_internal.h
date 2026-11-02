#pragma once

#include "panel.h"

#include "../../../contracts/panels.h"
#include "../../../contracts/ui.h"
#include "../../../contracts/ui_theme.h"
#include "config.h"
#include "log.h"
#include "plugin.h"
#include "scan.h"
#include "store.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#define AUTOSELLUI_LOG(...) AUTOSELL_LOG_INFO(__VA_ARGS__)

namespace qol::features::autosell {

using UiRect = qol::contracts::UiRect;

// 入口按钮：中心对齐两处——横向=扩展背包页签列（原版袋 0 右侧 68、宽 57）的中心，
// 纵向=原版任务背包格（袋容器 child 5）的中心。齿轮本身 68×68。
constexpr int64_t kEntrySize = 68;
constexpr int64_t kBagTabSize = 57;       // 原版袋格/扩展页签边长
constexpr int64_t kExtTabOffsetX = 68;    // 扩展页签列相对袋 0 的偏移
constexpr int64_t kEntryOffsetY = 3;      // 视觉微调：在中心对齐基础上整体下移 3

// 面板几何（逻辑坐标基准 960×640；实际逻辑宽由 CalcResolution 推出，真机 1408）。
// 宽度基准：2 × 0x180（假定物品详情面板宽 384）= 0x300 = 768 ≤ 1408。
constexpr int64_t kBaseW = 0x3c0;
constexpr int64_t kBaseH = 0x280;
constexpr int64_t kPanelW = 805;
constexpr int64_t kPanelH = 590;
constexpr int64_t kToggleSize = 78;

// world 左侧 HUD 齿轮：主代理真机探测与反汇编确认使用 unit 0x16 的 loc 0x00。
constexpr int32_t kGearImageUnit = 0x16;
constexpr int32_t kGearImageLoc = 0x00;

// 面板/控件填充沿用 GRPX_FillRectAlpha 的 RGB565 口径；全屏遮罩沿用 UI Kit 的
// ABGR 黑色口径，alpha 是 0..100 百分比。真机建议先验收：遮罩 0x40（64% 黑）、
// 面板 0x54（84% 深棕）。
constexpr uint32_t kMaskAlpha = 0x40;
constexpr uint32_t kPanelAlpha = 0x54;
constexpr uint32_t kPanelFill565 = 0x2104;

// ABGR 配色，与 settings/savebackup 同值。
constexpr uint32_t kGold = 0xFF00B4D7;
constexpr uint32_t kText = 0xFFCB9EE2;
constexpr uint32_t kDisabledText = 0xFFAAAAAA;

constexpr int kRuleCount = 8;

constexpr const char* kRarityLabels[] = {"关", "≤白", "≤绿", "≤蓝", "≤黄", "≤紫"};
constexpr const char* kGemTierLabels[] = {"关", "≤低级", "≤中级", "≤高级", "≤顶级", "≤混沌"};
constexpr const char* kGemRangeLabels[] = {"关", "≤30%", "≤60%", "≤75%", "≤90%", "≤99%"};
constexpr const char* kDiceLabels[] = {"关", "≤低级", "≤中级", "≤高级", "≤顶级", "≤混沌"};
constexpr const char* kBackpackLabels[] = {"关", "≤手提包", "≤小", "≤中", "≤大"};
constexpr const char* kBadgeLabels[] = {"关", "≤普通", "≤被祝福", "≤教团", "≤英雄"};

struct PressTarget {
    int kind = 0;      // 1=返回，2=总开关，3=规则前，4=规则后
    int index = -1;
};

extern std::atomic<bool> g_enabled;
extern const qol::contracts::QolUiComponents* g_ui_components;
extern std::atomic<bool> g_panel_active;
extern std::mutex g_entry_mtx;
extern qol::contracts::QolUiAnchoredButton g_entry;
extern PressTarget g_pressed_target;
extern ::autosell::Config g_draft;
extern void* g_hit_root;
extern void* g_component_root;
extern void* g_orphan_component_root;
extern void* g_back_button;
extern void* g_arrows[kRuleCount][2];
extern bool g_toggle_pressed;

const qol::contracts::QolUiAdapter* ui_api();
bool ui_set_rect(void* ctrl, UiRect rect);
bool ui_get_rect(void* ctrl, UiRect* rect);
void* ui_create_root(UiRect rect);
bool ui_hit_test(void* ctrl, int64_t x, int64_t y, UiRect rect);
void ui_begin_frame();
void ui_end_frame();
void ui_fill_rect_alpha(UiRect rect, uint32_t color, uint32_t alpha);
bool ui_screen_inset_width(int32_t* out);
bool ui_screen_inset_height(int32_t* out);
bool ui_child_count(void* parent, uint32_t* out);
bool ui_child_at(void* parent, uint32_t index, void** out);
void ui_uidesc_draw();

const qol::contracts::UiPanelsApi* autosell_panels_api();
const qol::contracts::QolUiComponents* autosell_components();
void autosell_request_panel_close();

void autosell_ctrl_abs(void* ctrl, int64_t* x, int64_t* y);
int64_t autosell_screen_width();
UiRect autosell_panel_rect();
UiRect autosell_mask_rect(const UiRect& panel);
UiRect autosell_equipment_group(const UiRect& panel);
UiRect autosell_back_rect();
UiRect autosell_total_rect();
UiRect autosell_rule_row(const UiRect& panel, int index);
UiRect autosell_rule_prev(const UiRect& panel, int index);
UiRect autosell_rule_next(const UiRect& panel, int index);
bool autosell_hit_test(UiRect area, int64_t x, int64_t y);
bool autosell_touch_xy(uint64_t param, int64_t* x, int64_t* y);

int autosell_rule_value(int index);
int autosell_rule_max(int index);
void autosell_set_rule_value(int index, int value);
const char* autosell_rule_name(int index);
void autosell_rule_value_text(int index, char* out, size_t out_size);
void autosell_commit_draft();
void autosell_save_before_back(void*);

bool autosell_create_panel_components(const UiRect& panel);
bool autosell_destroy_panel_components();
void autosell_draw_component_text(const char* text, int32_t x, int32_t y, uint32_t color,
                                  int32_t align, int32_t font);
void autosell_draw_total_toggle(const UiRect& panel);

}  // namespace qol::features::autosell
