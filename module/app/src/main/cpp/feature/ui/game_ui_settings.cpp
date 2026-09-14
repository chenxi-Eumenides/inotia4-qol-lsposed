#include "game_ui_settings.h"

#include "game_access.h"
#include "game_ops_common.h"
#include "feature/patch/game_patch.h"
#include "game_ptr_hook.h"
#include "game_state.h"
#include "game_symbols.h"
#include "game_ui.h"
#include "game_ui_kit.h"
#include "game_ui_components.h"
#include "game_ui_savebackup.h"

#include "core/native/qol_log.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#define SETTINGS_LOG(...) QOL_LOG_INFO(QolDomain::kUi, __VA_ARGS__)

#define POPUP_STATE_SIZE 0x40
#define CB_TEXT_SIZE 0x20

// 面板布局（逻辑坐标 0-960×0-640 空间，root 相对全屏居中；触摸坐标同空间）
#define ROOT_W 0x3c0
#define ROOT_H 0x280
#define SETTINGS_COLUMN_COUNT 2
#define VISIBLE_GRID_ROWS 4
#define SETTINGS_PAGE_CAPACITY (SETTINGS_COLUMN_COUNT * VISIBLE_GRID_ROWS)
#define PAGE_BUTTON_W 0x48
#define PAGE_BUTTON_H 0x48
#define PAGE_NAV_TEXT_GAP 0x28
// 保持分页前的原始网格宽度；2×4 后将翻页按钮移到网格下方，避免与网格重叠。
#define CONTENT_W 0x430
#define CONTENT_X ((ROOT_W - CONTENT_W) / 2)
#define CELL_W (CONTENT_W / SETTINGS_COLUMN_COUNT)
#define CELL_H 0x66
#define GRID_Y 0x60
#define GRID_H (VISIBLE_GRID_ROWS * CELL_H)
#define PAGE_INDICATOR_H 0x20
#define PAGE_NAV_Y (GRID_Y + GRID_H + 0x08)
#define PAGE_INDICATOR_Y (PAGE_NAV_Y + (PAGE_BUTTON_H - PAGE_INDICATOR_H) / 2)
#define ADDR_H 0x28
#define ROW_BTN_W 0xa8
#define ROW_BTN_H 0x28
// 存档备份格：按钮复用原版物品详情「使用」贴图（UIDesc_DrawMenuButton + loc 0x0e）。
// 该分片 149x75，现有绘制 API 只能按原始尺寸画；控制矩形高取开关同高 0x28，
// 宽度按贴图宽高比 149:75 同比得 0x50≈80（比其它开关窄）。
#define SAVEBACKUP_BTN_W 0x50
// 原版「使用」按钮分片：ButtonImgGroup(DrawID=0) 图组内 loc 0x0e
// （UIEquip_SetDescMenu 反汇编实证：使用/确认使用/佣兵徽章/骰子/开箱/解封共用）。
#define USE_BUTTON_LOC 0x0e
#define CELL_PADDING 0x20
#define ADDR_Y 0x18
#define BACK_BTN_W 0x4a
#define BACK_BTN_H 0x51
#define UI_GOLD 0xFF00B4D7
#define UI_OPTION_TEXT 0xFFCB9EE2
#define TITLE_BACKGROUND_LEFT_UNIT 0x4f
#define TITLE_BACKGROUND_RIGHT_UNIT 0x50
#define TITLE_BACKGROUND_LOC 0x0
// UIOption 的语言切换箭头：与原版 options 共用图像单元及箭头分片。
#define OPTION_IMAGE_UNIT 0x59
#define PAGE_PREV_ARROW_NORMAL_LOC 0x09
#define PAGE_PREV_ARROW_PRESSED_LOC 0x0a
#define PAGE_NEXT_ARROW_NORMAL_LOC 0x0b
#define PAGE_NEXT_ARROW_PRESSED_LOC 0x0c

namespace {

std::mutex g_settings_mtx;
std::atomic<bool> g_thread_started{false};
jclass g_config_bridge_class = nullptr;
bool g_more_games_injected = false;
PtrHook g_more_games_hook;

bool g_panel_active = false;
uint8_t* g_state_entry = nullptr;
uint8_t g_state_backup[POPUP_STATE_SIZE] = {0};
int g_state_id = -1;
void* g_root = nullptr;
bool g_option_images_loaded = false;
bool g_title_background_images_loaded = false;
bool g_background_unavailable_logged = false;
bool g_page_arrow_unavailable_logged = false;
int g_close_delay_frames = 0;
bool g_back_pressed = false;

struct SettingsRow {
    void* btn;
    void* desc;
};

// 配置项唯一登记表：key、label、page。
// page 是逻辑页码；空出的逻辑页会在运行时按升序压缩，新增项只需在此表增加一行。
// 空 key 的项是非开关入口，当前用于存档备份。
struct SettingsItem {
    const char* key;
    const char* label;
    int page;
};

static const SettingsItem kSettingsItems[] = {
    {"", "存档备份", 1},
    {"extensionBagEnabled", "扩展背包", 1},
    {"stackLimitIncrease", "堆叠上限", 1},
    {"apiEnabled", "API服务", 1},
    {"autoSellEnabled", "自动出售", 1},
    {"moveMergeEnabled", "拖拽合并", 2},
    {"gemCraftOptimize", "宝石优化", 2},
    {"debugLogEnabled", "调试日志", 2},
    {"opEnabled", "OP能力", 3},
};
static constexpr int kSettingsItemCount =
    static_cast<int>(sizeof(kSettingsItems) / sizeof(kSettingsItems[0]));

SettingsRow g_rows[kSettingsItemCount];
void* g_prev_page_btn = nullptr;
void* g_next_page_btn = nullptr;
void* g_back_btn = nullptr;
void* g_addr_desc = nullptr;
int g_page_index = 0;
int g_total_pages = 1;

// 当前配置值缓存（面板打开时从 Kotlin 拉取，切换时本地翻转+上抛）
static char g_row_status[kSettingsItemCount][CB_TEXT_SIZE] = {
    "", "关", "关", "开", "关", "关", "关", "关", "关"
};
static char g_addr_text[CB_TEXT_SIZE] = "";

bool inject_state_entry_locked();
void settings_row_clicked(void* ctrl);
void settings_page_clicked(void* ctrl);
void settings_back_clicked(void* ctrl);
void settings_savebackup_clicked(void* ctrl);  // v0.7.x：存档备份按钮
void savebackup_btn_draw(void* ctrl);
void page_btn_draw(void* ctrl);

#include "game_ui_settings_geometry.inc"
#include "game_ui_settings_config.inc"
#include "game_ui_settings_render.inc"
#include "game_ui_settings_panel.inc"
#include "game_ui_settings_injection.inc"
}  // namespace
#include "game_ui_settings_api.inc"

// 全局命名空间代理：给 savebackup UI 使用的设置面板状态查询。
// 匿名命名空间成员在同 TU 内全局可见（using-directive 等价），故直接读 + 加锁即可。
bool settings_panel_active_for_savebackup() {
    // g_settings_mtx / g_panel_active 是 settings TU 的匿名命名空间成员，
    // 同 TU 内（settings.cpp）可见，无需 extern。
    std::lock_guard<std::mutex> lock(g_settings_mtx);
    return g_panel_active;
}

// 关闭设置面板（v0.7.x）：清 panel 状态 + 还原 state entry。
// 在切换式兜底路径中被 data_savebackup_ui_open_panel 调用（嵌套 push 失败后退化）。
// 仅清理 settings 模块自身状态；调用方需自行调 fn_ui_set_popup_process_info(3, 0)
// 让游戏主循环真正关闭 UI 显示（push state 0 + clear draw flag）。
void settings_ui_close_panel() {
    std::lock_guard<std::mutex> lock(g_settings_mtx);
    if (g_panel_active) {
        // 复用 settings_panel_f3 清理 panel 状态（清 g_panel_active、释放 option_images 等）。
        // f3 内不持锁，直接调用即可（g_settings_mtx 已持锁）。
        settings_panel_f3();
        SETTINGS_LOG("settings panel closed by external caller");
    }
    // 还原注入的 state entry（保持死条目可复用；离开主菜单的 ensure_inject_thread 也会还原，
    // 这里主动还原避免 settings_panel_enter 残留在 entry 上影响后续 game 内 IAP 检测路径）。
    if (g_state_entry != nullptr) {
        memcpy(g_state_entry, g_state_backup, POPUP_STATE_SIZE);
        g_state_entry = nullptr;
        g_state_id = -1;
    }
}
