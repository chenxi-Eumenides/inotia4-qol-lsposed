// panel.cpp —— 自动出售 UI（背包入口 + 按存档配置面板；原 feature/ui/game_ui_autosell.cpp）。
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
// 配置面板通过左上返回按钮提交 draft：运行时更新与 slot-data 写请求共用同一 revision 路径。

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
namespace {

using UiRect = qol::contracts::UiRect;

const qol::contracts::QolUiAdapter* ui_api() {
    const qol::contracts::QolHostSdk* sdk = host_sdk();
    return sdk == nullptr ? nullptr : sdk->ui;
}

bool ui_set_rect(void* ctrl, UiRect rect) {
    const auto* ui = ui_api();
    return ui != nullptr && ui->set_rect != nullptr && ui->set_rect(ctrl, rect).ok == kQolOk;
}

bool ui_get_rect(void* ctrl, UiRect* rect) {
    const auto* ui = ui_api();
    return ui != nullptr && ui->get_rect != nullptr && ui->get_rect(ctrl, rect).ok == kQolOk;
}

void* ui_create_root(UiRect rect) {
    const auto* ui = ui_api();
    if (ui == nullptr || ui->create_control == nullptr) return nullptr;
    void* ctrl = nullptr;
    if (ui->create_control(0, nullptr, &ctrl).ok != kQolOk || !ui_set_rect(ctrl, rect)) {
        return nullptr;
    }
    return ctrl;
}

bool ui_hit_test(void* ctrl, int64_t x, int64_t y, UiRect rect) {
    const auto* ui = ui_api();
    return ui != nullptr && ui->hit_test != nullptr && ui->hit_test(ctrl, x, y, rect) != 0;
}

void ui_begin_frame() {
    const auto* ui = ui_api();
    if (ui != nullptr && ui->begin_frame != nullptr) (void)ui->begin_frame();
}

void ui_end_frame() {
    const auto* ui = ui_api();
    if (ui != nullptr && ui->end_frame != nullptr) (void)ui->end_frame();
}

void ui_fill_rect_alpha(UiRect rect, uint32_t color, uint32_t alpha) {
    const auto* ui = ui_api();
    if (ui != nullptr && ui->fill_rect_alpha != nullptr) {
        (void)ui->fill_rect_alpha(rect, color, alpha);
    }
}

bool ui_screen_inset_width(int32_t* out) {
    const auto* ui = ui_api();
    return ui != nullptr && ui->screen_inset_width != nullptr &&
           ui->screen_inset_width(out).ok == kQolOk;
}

bool ui_screen_inset_height(int32_t* out) {
    const auto* ui = ui_api();
    return ui != nullptr && ui->screen_inset_height != nullptr &&
           ui->screen_inset_height(out).ok == kQolOk;
}

bool ui_child_count(void* parent, uint32_t* out) {
    const auto* ui = ui_api();
    return ui != nullptr && ui->child_count != nullptr && ui->child_count(parent, out).ok == kQolOk;
}

bool ui_child_at(void* parent, uint32_t index, void** out) {
    const auto* ui = ui_api();
    return ui != nullptr && ui->child_at != nullptr && ui->child_at(parent, index, out).ok == kQolOk;
}

void ui_set_popup_process_info(int32_t mode, int32_t state_id) {
    const auto* ui = ui_api();
    if (ui != nullptr && ui->set_popup_process_info != nullptr) {
        (void)ui->set_popup_process_info(mode, state_id);
    }
}

void ui_uidesc_draw() {
    const auto* ui = ui_api();
    if (ui != nullptr && ui->uidesc_draw != nullptr) (void)ui->uidesc_draw();
}

// 入口按钮：与原版袋列（袋 0）同列右移 68、扩展页签（行距 70）之后的行 5。
// 由扩展页签公式换算：x = 袋0绝对 + 68，y = 袋0绝对 + 2 + 5×70（尺寸 57×57）。
constexpr int64_t kEntryRelX = 68;
constexpr int64_t kEntryRelY = 2 + 5 * 70;
constexpr int64_t kEntrySize = 68;
constexpr int64_t kEntryCenterAdjust = -5;
// 保持齿轮中心对齐袋列图标，并整体向右、向下各微移 2 逻辑单位。
constexpr int64_t kEntryNudgeX = -2;
constexpr int64_t kEntryNudgeY = 2;

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

constexpr int kRuleCount = 5;
constexpr int kSpecialCount = 3;

constexpr const char* kRarityLabels[] = {"关", "≤白", "≤绿", "≤蓝", "≤黄", "≤紫"};
constexpr const char* kGemTierLabels[] = {"关", "≤低级", "≤中级", "≤高级", "≤顶级", "≤混沌"};
constexpr const char* kGemRangeLabels[] = {"关", "≤30%", "≤60%", "≤75%", "≤90%", "≤99%"};
constexpr const char* kSpecialLabels[] = {"背包", "骰子", "普通徽章"};
constexpr uint32_t kSpecialBits[] = {
    ::autosell::kSpecialBackpack,
    ::autosell::kSpecialDice,
    ::autosell::kSpecialNormalSeal,
};

struct PressTarget {
    int kind = 0;      // 1=返回，2=总开关，3=规则前，4=规则后，5=特殊类型
    int index = -1;
};

// 开关：JVM 线程写、游戏主线程读；默认关闭。
std::atomic<bool> g_enabled{false};
const qol::contracts::QolUiComponents* g_ui_components = nullptr;
// 面板是否激活：主线程写、JVM 线程读（关闭时判断是否需要关闭面板）。
std::atomic<bool> g_panel_active{false};
// 入口控件引用：主线程与 JVM 线程都可能写，用互斥保护。
std::mutex g_entry_mtx;
void* g_entry_ctrl = nullptr;       // 当前已挂载的入口按钮控件
void* g_entry_container = nullptr;  // 挂载入口按钮的容器句柄（变化即旧控件失效）
bool g_entry_attempted = false;
int g_close_delay = 0;              // 仅游戏主线程访问
PressTarget g_pressed_target;       // 仅游戏主线程访问
::autosell::Config g_draft;           // 面板草稿；打开时读取，关闭时一次性提交
void* g_hit_root = nullptr;         // 仅用于复用 ui_hit_test 的绝对区域
void* g_component_root = nullptr;
void* g_orphan_component_root = nullptr;
void* g_back_button = nullptr;
void* g_category_buttons[kSpecialCount]{};
void* g_arrows[kRuleCount][2]{};
bool g_toggle_pressed = false;
// own:popup.autosell 的 PopupState 占用条目（资源事务 acquire 写入、release 写回）。
qol::contracts::QolPopupStateEntry g_popup_entry{};
int g_state_id = -1;

const qol::contracts::QolUiComponents* autosell_components() {
    if (g_ui_components != nullptr) return g_ui_components;
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    if (sdk->capabilities.require(qol::contracts::kUiComponentsCapabilityId,
            {qol::contracts::kUiContractMajor, qol::contracts::kUiContractMinor},
            &iface).ok != kQolOk) return nullptr;
    g_ui_components = static_cast<const qol::contracts::QolUiComponents*>(iface);
    return g_ui_components;
}

void autosell_panel_enter();
void autosell_panel_process();
void autosell_panel_f3();
void autosell_panel_f4();
uint32_t autosell_panel_event(uint64_t event, uint64_t param, uint64_t param2);

// 控件绝对坐标（父链累加，同扩展背包/自定义面板口径）。
void autosell_ctrl_abs(void* ctrl, int64_t* x, int64_t* y) {
    const auto* ui = ui_api();
    if (ui == nullptr || ui->control_absolute_origin == nullptr || x == nullptr || y == nullptr ||
        ui->control_absolute_origin(ctrl, x, y).ok != kQolOk) {
        if (x != nullptr) *x = 0;
        if (y != nullptr) *y = 0;
    }
}

int64_t autosell_screen_width() {
    int64_t logical_w = kBaseW;
    int32_t inset_w = 0;
    int32_t inset_h = 0;
    if (ui_screen_inset_width(&inset_w) && ui_screen_inset_height(&inset_h)) {
        const int64_t screen_w = kBaseW + static_cast<int64_t>(inset_w) * 2;
        const int64_t screen_h = kBaseH + static_cast<int64_t>(inset_h) * 2;
        if (screen_w > 0 && screen_h > 0) logical_w = screen_w * kBaseH / screen_h;
    }
    return logical_w;
}

UiRect autosell_panel_rect() {
    const int64_t logical_w = autosell_screen_width();
    const int64_t x = (logical_w - kPanelW) / 2;
    const int64_t y = (kBaseH - kPanelH) / 2;
    return {x, y, kPanelW, kPanelH};
}

UiRect autosell_mask_rect(const UiRect& panel) {
    // 全屏遮罩：逻辑屏宽 = 面板宽 + 面板左右边距×2（与 settings 同口径）。
    return {0, 0, panel.x * 2 + kPanelW, kBaseH};
}

UiRect autosell_equipment_group(const UiRect& panel) {
    return {panel.x + 24, panel.y + 128, 366, 240};
}

UiRect autosell_gem_group(const UiRect& panel) {
    return {panel.x + 415, panel.y + 128, 366, 240};
}

UiRect autosell_back_rect() {
    return {qol::contracts::ui_theme::kNavBoxAbsX, qol::contracts::ui_theme::kNavBoxAbsY,
        qol::contracts::ui_theme::kNavBoxW, qol::contracts::ui_theme::kNavBoxH};
}

UiRect autosell_total_rect() {
    return {autosell_screen_width() - qol::contracts::ui_theme::kNavBoxAbsX - kToggleSize,
        qol::contracts::ui_theme::kNavBoxAbsY, kToggleSize, kToggleSize};
}

UiRect autosell_rule_row(const UiRect& panel, int index) {
    if (index < 3) return {panel.x + 40, panel.y + 184 + index * 52, 334, 44};
    return {panel.x + 433, panel.y + 196 + (index - 3) * 72, 334, 48};
}

UiRect autosell_rule_prev(const UiRect& panel, int index) {
    const UiRect row = autosell_rule_row(panel, index);
    return index < 3 ? UiRect{row.x + 142, row.y + 1, 38, 42}
                     : UiRect{row.x + 112, row.y + 5, 34, 38};
}

UiRect autosell_rule_next(const UiRect& panel, int index) {
    const UiRect row = autosell_rule_row(panel, index);
    return index < 3 ? UiRect{row.x + 292, row.y + 1, 38, 42}
                     : UiRect{row.x + 286, row.y + 5, 34, 38};
}

UiRect autosell_special_chip(const UiRect& panel, int index) {
    if (index == 0) return {panel.x + 176, panel.y + 408, 177, 62};
    if (index == 1) return {panel.x + 176, panel.y + 478, 177, 62};
    return {panel.x + 371, panel.y + 408, 177, 62};
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
        case 0: return "品质";
        case 1: return "耐久度";
        case 2: return "总孔数";
        case 3: return "等级";
        case 4: return "品质";
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

void autosell_commit_draft() {
    const int slot = autosell_active_save_slot();
    const bool persisted = autosell_store_commit_config(g_draft);
    AUTOSELLUI_LOG("panel close applied config; persist slot=%d ok=%d", slot, persisted ? 1 : 0);
}

void autosell_save_before_back(void*) { autosell_commit_draft(); }

// ---- 入口按钮 ----

void autosell_entry_clicked(void* ctrl) {
    (void)ctrl;
    // 关闭态残留控件（不销毁原版控件）不得再打开面板；门禁/state 校验在 provider_open 内。
    autosell_provider_open();
}

bool autosell_is_child(void* parent, void* child) {
    if (parent == nullptr || child == nullptr) {
        return false;
    }
    uint32_t count = 0;
    if (!ui_child_count(parent, &count)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        void* candidate = nullptr;
        if (ui_child_at(parent, i, &candidate) && candidate == child) return true;
    }
    return false;
}

// 入口控件有效性：必须是当前容器的子控件，且身份字段确为本模块入口按钮。
// 安全顺序：容器指针比对与子控件指针扫描都不解引用 g_entry_ctrl；只有确认它仍在
// 容器子链表中后才读取其字段（此时控件必然存活），避免对已失效控件解引用/跳转。
bool autosell_entry_valid(void* container) {
    if (container == nullptr || g_entry_ctrl == nullptr) return false;
    // 子链表成员关系是存活证明；控件身份字段在部分构建/控件状态下不可靠，
    // 继续强校验会导致每帧重建入口控件。
    return autosell_is_child(container, g_entry_ctrl);
}

// 动态定位入口：与扩展背包页签同一套算法（ui_mix.cpp）——
// 取袋容器 child 0（原版袋 0）的绝对位置，加上「袋列右侧 68 / 下移 2」，再减去挂载层
// （袋容器）绝对位置得到相对 rect；行号 5（与任务背包同行，扩展页签只占 0..4）。
// 不使用绝对坐标，也不依赖子控件顺序（袋 0 = child 0）。读取失败时退回常量兜底。
UiRect autosell_entry_rect(void* container) {
    UiRect out{kEntryRelX + 2, kEntryRelY + 2, kEntrySize, kEntrySize};
    if (container == nullptr) return out;
    void* bag0 = nullptr;
    if (!ui_child_at(container, 0, &bag0)) return out;
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

    out.x = bag0_abs_x + 68 - mount_abs_x + kEntryNudgeX + kEntryCenterAdjust;
    // y 取「任务背包」实际 rect（袋列第 6 个 = child 5）：实测原版袋标签 y 为
    // 0/70/139/208/277/347，并非严格的 2+index*70（第 6 个差 5），故用实际值对齐。
    void* task_bag = nullptr;
    (void)ui_child_at(container, 5, &task_bag);
    UiRect task{};
    if (task_bag != nullptr && task_bag != g_entry_ctrl && ui_get_rect(task_bag, &task) &&
        task.h > 0) {
        out.y = task.y + kEntryNudgeY + kEntryCenterAdjust;
    } else {
        out.y = bag0_abs_y + 2 - mount_abs_y + 5 * 70 + kEntryNudgeY +
            kEntryCenterAdjust;
    }
    return out;
}

void autosell_install_entry(void* container) {
    if (g_entry_attempted) return;
    g_entry_attempted = true;
    const UiRect rect = autosell_entry_rect(container);
    const auto* components = autosell_components();
    if (components == nullptr || components->icon_button_create == nullptr) {
        AUTOSELLUI_LOG("entry create failed: icon button component unavailable");
        return;
    }
    const qol::contracts::UiIconButtonDesc desc{rect, kGearImageUnit, kGearImageLoc, 1, 1,
        "售", &autosell_entry_clicked, nullptr, 0};
    void* btn = nullptr;
    if (components->icon_button_create(&desc, container, &btn).ok != kQolOk || btn == nullptr) {
        AUTOSELLUI_LOG("entry create failed container=%p", container);
        return;
    }
    g_entry_ctrl = btn;
    AUTOSELLUI_LOG("entry installed container=%p ctrl=%p rect=(%lld,%lld)",
                   container, btn, static_cast<long long>(rect.x),
                   static_cast<long long>(rect.y));
}

// 每帧（EQUIP 页）确保入口按钮存在并绘制。关闭态直接移除引用。
void autosell_on_equip_draw() {
    std::lock_guard<std::mutex> lock(g_entry_mtx);
    const auto* ui = ui_api();
    void* container = nullptr;
    if (ui == nullptr || ui->find_original_bag_container == nullptr ||
        ui->find_original_bag_container(&container).ok != kQolOk || container == nullptr) {
        g_entry_ctrl = nullptr;
        g_entry_container = nullptr;
        g_entry_attempted = false;
        return;
    }
    // 容器指针变化：旧控件随旧容器失效，仅移除引用（不销毁原版控件）。
    if (container != g_entry_container) {
        g_entry_ctrl = nullptr;
        g_entry_container = container;
        g_entry_attempted = false;
    }
    if (!g_enabled.load(std::memory_order_acquire)) {
        if (g_entry_ctrl != nullptr && autosell_entry_valid(container)) {
            const auto* components = autosell_components();
            if (components != nullptr && components->icon_button_set_enabled != nullptr)
                (void)components->icon_button_set_enabled(g_entry_ctrl, 0);
        }
        return;
    }
    // 容器就位守卫：面板打开初期为占位容器（无原版 6 袋标签），此时不挂载。
    uint32_t child_count = 0;
    if (!ui_child_count(container, &child_count) || child_count < 6) return;
    if (g_entry_ctrl != nullptr && !autosell_entry_valid(container)) {
        g_entry_ctrl = nullptr;
        g_entry_attempted = true;  // 该容器本帧不再重建已失联的子控件。
    }
    if (g_entry_ctrl == nullptr) autosell_install_entry(container);
    if (g_entry_ctrl != nullptr && autosell_entry_valid(container)) {
        const auto* components = autosell_components();
        if (components != nullptr && components->icon_button_set_enabled != nullptr)
            (void)components->icon_button_set_enabled(g_entry_ctrl, 1);
        if (components != nullptr && components->icon_button_draw != nullptr)
            (void)components->icon_button_draw(g_entry_ctrl);
    }
}

// ---- 面板 ----

void autosell_component_noop(void*) {}

bool autosell_create_panel_components(const UiRect& panel) {
    if (g_ui_components == nullptr || g_ui_components->panel_create == nullptr ||
        g_ui_components->compact_capsule_text_button_create == nullptr ||
        g_ui_components->text_button_set_active == nullptr ||
        g_ui_components->text_button_draw == nullptr ||
        g_ui_components->arrow_button_create == nullptr ||
        g_ui_components->arrow_button_draw == nullptr ||
        g_ui_components->nav_button_create == nullptr ||
        g_ui_components->nav_button_draw == nullptr || g_ui_components->destroy == nullptr) {
        return false;
    }
    const qol::contracts::UiPanelDesc root_desc{{panel.x, panel.y, panel.w, panel.h},
        {0, 0, kBaseW, kBaseH}, kPanelFill565, 0};
    if (g_ui_components->panel_create(&root_desc, nullptr, &g_component_root).ok != kQolOk ||
        g_component_root == nullptr) return false;
    const auto local = [&panel](UiRect rect) {
        rect.x -= panel.x;
        rect.y -= panel.y;
        return rect;
    };
    const auto create_text_button = [&](UiRect rect, const char* label, void** out) {
        const qol::contracts::UiButtonDesc desc{local(rect), label,
            qol::contracts::kUiTextButtonFontDefault, &autosell_component_noop, nullptr};
        return g_ui_components->compact_capsule_text_button_create(&desc, g_component_root, out).ok ==
            kQolOk && *out != nullptr;
    };
    const UiRect back_rect = autosell_back_rect();
    const qol::contracts::UiNavButtonDesc back_desc{local(back_rect),
        qol::contracts::kUiNavButtonForceBack, nullptr, nullptr};
    if (g_ui_components->nav_button_create(&back_desc, g_component_root,
            &g_back_button).ok != kQolOk || g_back_button == nullptr) return false;
    for (int i = 0; i < kSpecialCount; ++i) {
        if (!create_text_button(autosell_special_chip(panel, i), kSpecialLabels[i],
                                &g_category_buttons[i])) return false;
    }
    for (int i = 0; i < kRuleCount; ++i) {
        const UiRect left = local(autosell_rule_prev(panel, i));
        const UiRect right = local(autosell_rule_next(panel, i));
        const qol::contracts::UiArrowButtonDesc left_desc{left, qol::contracts::kUiArrowLeft,
            1, &autosell_component_noop, nullptr};
        const qol::contracts::UiArrowButtonDesc right_desc{right, qol::contracts::kUiArrowRight,
            1, &autosell_component_noop, nullptr};
        if (g_ui_components->arrow_button_create(&left_desc, g_component_root,
                &g_arrows[i][0]).ok != kQolOk ||
            g_ui_components->arrow_button_create(&right_desc, g_component_root,
                &g_arrows[i][1]).ok != kQolOk) return false;
    }
    return true;
}

bool autosell_destroy_panel_components() {
    if (g_orphan_component_root != nullptr) {
        const auto* ui = ui_api();
        if (ui == nullptr || ui->delete_control == nullptr ||
            ui->delete_control(g_orphan_component_root).ok != kQolOk) return false;
        g_orphan_component_root = nullptr;
    }
    if (g_component_root == nullptr) return true;
    void* const native_root = g_component_root;
    if (g_ui_components == nullptr || g_ui_components->destroy == nullptr ||
        g_ui_components->destroy(g_component_root).ok != kQolOk) return false;
    g_component_root = nullptr;
    g_back_button = nullptr;
    std::memset(g_category_buttons, 0, sizeof(g_category_buttons));
    std::memset(g_arrows, 0, sizeof(g_arrows));
    const auto* ui = ui_api();
    if (ui == nullptr || ui->delete_control == nullptr ||
        ui->delete_control(native_root).ok != kQolOk) {
        g_orphan_component_root = native_root;
        return false;
    }
    return true;
}

void autosell_draw_component_text(const char* text, int32_t x, int32_t y, uint32_t color,
                                  int32_t align, int32_t font) {
    if (g_ui_components != nullptr && g_component_root != nullptr &&
        g_ui_components->draw_control_text != nullptr) {
        (void)g_ui_components->draw_control_text(g_component_root, text, x, y, color, align, font);
    }
}

void autosell_draw_total_toggle(const UiRect& panel) {
    const auto* ui = ui_api();
    if (g_component_root == nullptr || ui == nullptr || ui->fill_rect == nullptr) return;
    const UiRect rect = autosell_total_rect();
    if (g_ui_components != nullptr && g_ui_components->draw_container != nullptr) {
        (void)g_ui_components->draw_container(rect);
    } else {
        constexpr int64_t edge = 2;
        const uint32_t border_color = g_toggle_pressed ? kGold : 0xFF766A58u;
        const UiRect borders[] = {
            {rect.x, rect.y, rect.w, edge}, {rect.x, rect.y + rect.h - edge, rect.w, edge},
            {rect.x, rect.y, edge, rect.h}, {rect.x + rect.w - edge, rect.y, edge, rect.h}};
        for (UiRect border : borders) (void)ui->fill_rect(border, border_color);
    }
    if (g_toggle_pressed) {
        constexpr int64_t edge = 2;
        const UiRect borders[] = {
            {rect.x, rect.y, rect.w, edge}, {rect.x, rect.y + rect.h - edge, rect.w, edge},
            {rect.x, rect.y, edge, rect.h}, {rect.x + rect.w - edge, rect.y, edge, rect.h}};
        for (UiRect border : borders) (void)ui->fill_rect(border, kGold);
    }
    const char* glyph = g_draft.enabled ? "√" : "X";
    const int32_t glyph_x = static_cast<int32_t>(rect.x - panel.x + rect.w / 2);
    const int32_t glyph_y = static_cast<int32_t>(rect.y - panel.y + (rect.h - 38) / 2);
    const uint32_t glyph_color = g_draft.enabled ? kGold : 0xFFAAAAAAu;
    const int32_t offsets[][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (const auto& offset : offsets) {
        autosell_draw_component_text(glyph, glyph_x + offset[0], glyph_y + offset[1],
                                     glyph_color, 2, 2);
    }
}

void autosell_panel_enter() {
    AUTOSELLUI_LOG("panel enter");
    g_panel_active.store(false, std::memory_order_release);
    g_draft = autosell_get_runtime_config();
    (void)autosell_components();
    if (const auto* ui = ui_api(); ui != nullptr && ui->load_image_unit != nullptr)
        (void)ui->load_image_unit(0x0f);
    if (!autosell_destroy_panel_components()) {
        AUTOSELLUI_LOG("panel component cleanup deferred");
        ui_set_popup_process_info(3, 0);
        return;
    }
    if (!autosell_create_panel_components(autosell_panel_rect())) {
        (void)autosell_destroy_panel_components();
        AUTOSELLUI_LOG("panel component creation failed");
        ui_set_popup_process_info(3, 0);
        return;
    }
    if (g_hit_root == nullptr) g_hit_root = ui_create_root({0, 0, 1, 1});
    g_panel_active.store(true, std::memory_order_release);
    g_close_delay = 0;
    g_pressed_target = {};
    g_toggle_pressed = false;
}

void autosell_panel_process() {
    if (!g_panel_active.load(std::memory_order_acquire) || ui_api() == nullptr) return;
    if (g_close_delay > 0) {
        --g_close_delay;
        if (g_close_delay == 0) {
            ui_set_popup_process_info(3, 0);
            return;
        }
    }
    ui_begin_frame();
    const UiRect panel = autosell_panel_rect();
    const UiRect mask = autosell_mask_rect(panel);
    ui_fill_rect_alpha(mask, 0xFF000000, kMaskAlpha);
    ui_fill_rect_alpha(panel, kPanelFill565, kPanelAlpha);
    if (g_ui_components != nullptr && g_ui_components->draw_container_ornamented != nullptr) {
        (void)g_ui_components->draw_container_ornamented(panel);
        if (g_ui_components->draw_container != nullptr) {
            (void)g_ui_components->draw_container(autosell_equipment_group(panel));
            (void)g_ui_components->draw_container(autosell_gem_group(panel));
        }
    }

    autosell_draw_component_text("自动出售", static_cast<int32_t>(panel.w / 2), 20,
                                 kGold, 2, 2);
    autosell_draw_total_toggle(panel);
    if (g_ui_components != nullptr && g_back_button != nullptr)
        (void)g_ui_components->nav_button_draw(g_back_button);

    autosell_draw_component_text("装备", 207, 150, kGold, 2, 1);
    autosell_draw_component_text("宝石", 598, 150, kGold, 2, 1);
    for (int i = 0; i < kRuleCount; ++i) {
        const UiRect row = autosell_rule_row(panel, i);
        const char* label = autosell_rule_name(i);
        const uint32_t color = autosell_rule_value(i) == 0 ? kText : kGold;
        const int32_t label_x = static_cast<int32_t>(row.x - panel.x + (i < 3 ? 8 : 4));
        const int32_t label_y = static_cast<int32_t>(row.y - panel.y + 10);
        autosell_draw_component_text(label, label_x, label_y, color, 0, 1);
        char value_text[32] = {};
        autosell_rule_value_text(i, value_text, sizeof(value_text));
        const int32_t value_x = static_cast<int32_t>(row.x - panel.x + (i < 3 ? 234 : 220));
        autosell_draw_component_text(value_text, value_x,
            static_cast<int32_t>(row.y - panel.y + 10), color, 2, 1);
        if (g_ui_components != nullptr && g_arrows[i][0] != nullptr && g_arrows[i][1] != nullptr) {
            (void)g_ui_components->arrow_button_draw(g_arrows[i][0]);
            (void)g_ui_components->arrow_button_draw(g_arrows[i][1]);
        }
    }

    autosell_draw_component_text("已强化、镶嵌的物品会被自动保护", 402, 80,
                                 kText, 2, 1);
    for (int c = 0; c < 4; ++c) {
        static const char* kCategoryChars[] = {"物", "品", "类", "别"};
        autosell_draw_component_text(kCategoryChars[c], 127,
            408 + c * 35, kGold, 2, 1);
    }
    for (int i = 0; i < kSpecialCount; ++i) {
        const bool selected = (g_draft.special_mask & kSpecialBits[i]) != 0;
        if (g_ui_components != nullptr && g_category_buttons[i] != nullptr) {
            (void)g_ui_components->text_button_set_active(g_category_buttons[i], selected ? 1 : 0);
            (void)g_ui_components->text_button_draw(g_category_buttons[i]);
        }
    }
    ui_end_frame();
}

void autosell_panel_f3() {
    AUTOSELLUI_LOG("panel f3 (terminate)");
    (void)autosell_destroy_panel_components();
    g_panel_active.store(false, std::memory_order_release);
    g_close_delay = 0;
    g_toggle_pressed = false;
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
        if (autosell_hit_test(autosell_back_rect(), tx, ty)) {
            g_pressed_target = {1, -1};
        } else if (autosell_hit_test(autosell_total_rect(), tx, ty)) {
            g_pressed_target = {2, -1};
            g_toggle_pressed = true;
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
        if (g_pressed_target.kind != 0 && g_pressed_target.kind != 2 &&
            g_component_root != nullptr && g_ui_components != nullptr &&
            g_ui_components->dispatch_native_event != nullptr) {
            uint32_t consumed = 0;
            (void)g_ui_components->dispatch_native_event(g_component_root, event, param,
                                                          param2, &consumed);
        }
    } else if (event == 0x18) {
        const PressTarget pressed = g_pressed_target;
        bool released_inside = false;
        if (pressed.kind == 1) {
            released_inside = autosell_hit_test(autosell_back_rect(), tx, ty);
        } else if (pressed.kind == 2) {
            released_inside = autosell_hit_test(autosell_total_rect(), tx, ty);
        } else if (pressed.kind == 3) {
            released_inside = autosell_hit_test(autosell_rule_prev(panel, pressed.index), tx, ty);
        } else if (pressed.kind == 4) {
            released_inside = autosell_hit_test(autosell_rule_next(panel, pressed.index), tx, ty);
        } else if (pressed.kind == 5) {
            released_inside = autosell_hit_test(autosell_special_chip(panel, pressed.index), tx, ty);
        }
        if (released_inside) {
            if (pressed.kind == 1) {
                autosell_save_before_back(nullptr);
                AUTOSELLUI_LOG("back released -> save and close panel");
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
        if (pressed.kind != 0 && pressed.kind != 2 &&
            !(pressed.kind == 1 && released_inside) && g_component_root != nullptr &&
            g_ui_components != nullptr && g_ui_components->dispatch_native_event != nullptr) {
            uint32_t consumed = 0;
            (void)g_ui_components->dispatch_native_event(g_component_root, event, param,
                                                          param2, &consumed);
        }
        if (pressed.kind == 2) g_toggle_pressed = false;
        g_pressed_target = {};
    }
    return 1;
}

}  // namespace

// 复刻原 `bl UIDesc_Draw`，随后绘制入口按钮（独立于扩展背包绘制宿主）。
// kCallPatch 站点 `patch:SCENE_DRAW_EQUIP+0x1cc` 的 handler（宿主安装时写入 BL）。
void autosell_equip_desc_draw_wrapper() {
    ui_uidesc_draw();
    autosell_on_equip_draw();
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
    // 不从非游戏线程触碰入口控件；下次装备页绘制时禁用原有组件，不重建。
    // 面板正打开则请求关闭（弹栈后游戏会回调 f3 清理面板状态）。
    if (g_panel_active.exchange(false, std::memory_order_acq_rel)) {
        ui_set_popup_process_info(3, 0);
        AUTOSELLUI_LOG("disabled -> panel close requested");
    }
}

// ============== PanelProviderSpec（panel_id="autosell"）==============

bool autosell_provider_open() {
    // 与旧入口按钮点击同门禁：关闭态不得打开面板（含经 core.ui 面板栈打开）。
    if (!g_enabled.load(std::memory_order_acquire)) return false;
    if (g_state_id < 0 || ui_api() == nullptr) {
        AUTOSELLUI_LOG("provider open rejected: state not injected");
        return false;
    }
    AUTOSELLUI_LOG("provider open panel state=%d", g_state_id);
    ui_set_popup_process_info(1, g_state_id);
    return true;
}

void autosell_provider_close() {
    if (!g_panel_active.exchange(false, std::memory_order_acq_rel)) return;  // 幂等
    ui_set_popup_process_info(3, 0);
    AUTOSELLUI_LOG("provider close requested");
}

bool autosell_provider_active() {
    return g_panel_active.load(std::memory_order_acquire);
}

const char* autosell_provider_block_reason() {
    return nullptr;  // 现有实现无占用文案
}

// ============== own:popup.autosell（kOwnership）==============

int32_t autosell_popup_acquire(const char* id) {
    (void)id;
    if (g_popup_entry.slot != nullptr) return 0;
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->hooks.popup_resolve == nullptr ||
        sdk->hooks.popup_acquire == nullptr) {
        AUTOSELLUI_LOG("popup.autosell: hook API unavailable");
        return 1;
    }
    qol::contracts::QolPopupStateEntry entry{};
    if (sdk->hooks.popup_resolve("F_PANEL_UNK3_ENTER", &entry).ok != kQolOk) {
        AUTOSELLUI_LOG("popup.autosell: state entry (INAPP_HOT) not found");
        return 1;
    }
    const qol::contracts::QolPopupStateHooks popup_hooks{
        &autosell_panel_enter,
        &autosell_panel_process,
        &autosell_panel_f3,
        &autosell_panel_f4,
        &autosell_panel_event,
    };
    if (sdk->hooks.popup_acquire(&entry, &popup_hooks).ok != kQolOk) {
        AUTOSELLUI_LOG("popup.autosell: acquire rejected");
        return 1;
    }
    g_popup_entry = entry;
    g_state_id = entry.state_id;
    AUTOSELLUI_LOG("popup.autosell acquired: state id=%d", g_state_id);
    return 0;
}

void autosell_popup_release(const char* id) {
    (void)id;
    if (g_popup_entry.slot != nullptr) {
        const auto* sdk = host_sdk();
        if (sdk != nullptr && sdk->hooks.popup_restore != nullptr) {
            (void)sdk->hooks.popup_restore(&g_popup_entry);
        }
    }
    g_state_id = -1;
    AUTOSELLUI_LOG("popup.autosell released");
}

}  // namespace qol::features::autosell
